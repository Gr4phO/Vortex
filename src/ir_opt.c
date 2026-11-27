/*
 * VORTEX 3-ADDRESS LINEAR IR, REGION VERIFIER, RCE/LICM, LIVENESS, LINEAR SCAN & SIMD VECTORIZER
 */

#include "vortex.h"
#ifdef fprintf
#undef fprintf
#endif
#define fprintf vortex_diag_emit

/* ========================================================================= */
/* RANGE CHECK ELIMINATION & LOOP-INVARIANT CODE MOTION (LICM)               */
/* ========================================================================= */

static void ir_compact_nops(IRBuffer* ir) {
    int out = 0;
    for (int i = 0; i < ir->count; i++) {
        if (ir->instrs[i].op != IR_NOP) {
            if (out != i) {
                ir->instrs[out] = ir->instrs[i];
            }
            out++;
        }
    }
    ir->count = out;
}

static void ir_insert_before(IRBuffer* ir, int pos, const IRInstruction* ins) {
    if (ir->count + 1 >= ir->capacity) {
        ir->capacity *= 2;
        ir->instrs = (IRInstruction*)realloc(ir->instrs, ir->capacity * sizeof(IRInstruction));
    }
    for (int i = ir->count; i > pos; i--) {
        ir->instrs[i] = ir->instrs[i - 1];
    }
    ir->instrs[pos] = *ins;
    ir->count++;
}

static bool ir_has_side_effect(IROpcode op) {
    switch (op) {
        case IR_STORE_OFFSET:
        case IR_STORE_ARRAY:
        case IR_STORE_DEREF:
        case IR_BOUNDS_CHECK:
        case IR_RANGE_CHECK:
        case IR_PTR_CHECK:
        case IR_FFI_CHECK:
        case IR_DIV_CHECK:
        case IR_LABEL:
        case IR_JMP:
        case IR_JZ:
        case IR_JNZ:
        case IR_CALL:
        case IR_CALL_USER:
        case IR_CALL_FFI:
        case IR_LOAD_PARAM:
        case IR_RET:
        case IR_RAW_RIEMANN:
        case IR_VEC_LOOP:
        case IR_VEC_ARR_OP:
        case IR_VEC_GRID_MOD:
        case IR_VEC_MOD_FILL:
        case IR_VEC_F64_AXPY:
        case IR_SUB_MOD_IMM:
            return true;
        default:
            return false;
    }
}

static void ir_inline_small_functions(IRBuffer* ir) {
    if (!ir || ir->count == 0) return;
    int expansions = 0;
    for (int ip = 0; ip < ir->count && expansions < 256; ip++) {
        IRInstruction* ins = &ir->instrs[ip];
        if (ins->op != IR_CALL_USER) continue;
        UserFunc* uf = user_func_find(ins->func_name);
        if (!uf || !uf->can_inline || !uf->inline_instrs || !uf->inline_vregs) continue;
        if (ins->arg_count != uf->param_count) continue;
        if (uf->inline_vreg_count <= 0 || uf->inline_vreg_count > 256) continue;
        if (uf->inline_ir_count <= 0 || uf->inline_ir_count > 64) continue;
        if (ir->count + uf->inline_ir_count + 4 >= MAX_IR_INSTRS) continue;

        const IRInstruction* body = (const IRInstruction*)uf->inline_instrs;
        const VRegInfo* bvregs = (const VRegInfo*)uf->inline_vregs;

        VReg vmap[256];
        for (int v = 0; v < uf->inline_vreg_count; v++) {
            VReg nv = ir_new_vreg(ir, bvregs[v].type);
            ir->vregs[nv].is_ref = bvregs[v].is_ref;
            ir->vregs[nv].is_slice_ptr = bvregs[v].is_slice_ptr;
            ir->vregs[nv].known_len = bvregs[v].known_len;
            vmap[v] = nv;
        }

        int old_lbls[64];
        int new_lbls[64];
        int lbl_map_count = 0;
        for (int b = 0; b < uf->inline_ir_count; b++) {
            IROpcode bop = body[b].op;
            if (bop == IR_LABEL || bop == IR_JMP || bop == IR_JZ || bop == IR_JNZ) {
                int ol = (int)body[b].imm;
                bool found = false;
                for (int m = 0; m < lbl_map_count; m++) {
                    if (old_lbls[m] == ol) { found = true; break; }
                }
                if (!found && lbl_map_count < 64) {
                    old_lbls[lbl_map_count] = ol;
                    new_lbls[lbl_map_count] = ir_new_label(ir);
                    lbl_map_count++;
                }
            }
        }

        int inline_exit_lbl = -1;
        IRInstruction exp[96];
        int exp_cnt = 0;
        IRInstruction orig_call = *ins;

        for (int b = 0; b < uf->inline_ir_count && exp_cnt < 90; b++) {
            IRInstruction bi = body[b];
            if (bi.op == IR_LOAD_PARAM) {
                int pidx = (int)bi.imm;
                if (bi.dest >= 0 && bi.dest < uf->inline_vreg_count &&
                    pidx >= 0 && pidx < orig_call.arg_count) {
                    IRInstruction mv;
                    memset(&mv, 0, sizeof(mv));
                    mv.op = IR_MOV;
                    mv.dest = vmap[bi.dest];
                    mv.src1 = orig_call.args[pidx];
                    mv.src2 = VREG_NONE;
                    mv.src3 = VREG_NONE;
                    mv.src4 = VREG_NONE;
                    mv.known_len = -1;
                    mv.type = bvregs[bi.dest].type;
                    mv.line = orig_call.line;
                    mv.file = orig_call.file;
                    exp[exp_cnt++] = mv;
                }
                continue;
            }
            if (bi.op == IR_RET) {
                if (orig_call.dest != VREG_NONE && bi.src1 >= 0 && bi.src1 < uf->inline_vreg_count) {
                    IRInstruction mv;
                    memset(&mv, 0, sizeof(mv));
                    mv.op = IR_MOV;
                    mv.dest = orig_call.dest;
                    mv.src1 = vmap[bi.src1];
                    mv.src2 = VREG_NONE;
                    mv.src3 = VREG_NONE;
                    mv.src4 = VREG_NONE;
                    mv.known_len = -1;
                    mv.type = orig_call.type;
                    mv.line = orig_call.line;
                    mv.file = orig_call.file;
                    exp[exp_cnt++] = mv;
                }
                if (b < uf->inline_ir_count - 1) {
                    if (inline_exit_lbl < 0) inline_exit_lbl = ir_new_label(ir);
                    IRInstruction jmp;
                    memset(&jmp, 0, sizeof(jmp));
                    jmp.op = IR_JMP;
                    jmp.dest = VREG_NONE;
                    jmp.src1 = VREG_NONE;
                    jmp.src2 = VREG_NONE;
                    jmp.src3 = VREG_NONE;
                    jmp.src4 = VREG_NONE;
                    jmp.known_len = -1;
                    jmp.imm = inline_exit_lbl;
                    jmp.line = orig_call.line;
                    jmp.file = orig_call.file;
                    exp[exp_cnt++] = jmp;
                }
                continue;
            }

            if (bi.dest >= 0 && bi.dest < uf->inline_vreg_count) bi.dest = vmap[bi.dest];
            if (bi.src1 >= 0 && bi.src1 < uf->inline_vreg_count) bi.src1 = vmap[bi.src1];
            if (bi.src2 >= 0 && bi.src2 < uf->inline_vreg_count) bi.src2 = vmap[bi.src2];
            if (bi.src3 >= 0 && bi.src3 < uf->inline_vreg_count) bi.src3 = vmap[bi.src3];
            if (bi.src4 >= 0 && bi.src4 < uf->inline_vreg_count) bi.src4 = vmap[bi.src4];
            for (int a = 0; a < bi.arg_count; a++) {
                if (bi.args[a] >= 0 && bi.args[a] < uf->inline_vreg_count) {
                    bi.args[a] = vmap[bi.args[a]];
                }
            }
            if (bi.op == IR_LABEL || bi.op == IR_JMP || bi.op == IR_JZ || bi.op == IR_JNZ) {
                for (int m = 0; m < lbl_map_count; m++) {
                    if (old_lbls[m] == (int)bi.imm) {
                        bi.imm = new_lbls[m];
                        break;
                    }
                }
            }
            exp[exp_cnt++] = bi;
        }
        if (inline_exit_lbl >= 0 && exp_cnt < 95) {
            IRInstruction elbl;
            memset(&elbl, 0, sizeof(elbl));
            elbl.op = IR_LABEL;
            elbl.dest = VREG_NONE;
            elbl.src1 = VREG_NONE;
            elbl.src2 = VREG_NONE;
            elbl.src3 = VREG_NONE;
            elbl.src4 = VREG_NONE;
            elbl.known_len = -1;
            elbl.imm = inline_exit_lbl;
            elbl.line = orig_call.line;
            elbl.file = orig_call.file;
            exp[exp_cnt++] = elbl;
        }

        if (exp_cnt == 0) {
            ir->instrs[ip].op = IR_NOP;
            expansions++;
            continue;
        }
        while (ir->count + exp_cnt >= ir->capacity) {
            ir->capacity *= 2;
            ir->instrs = (IRInstruction*)realloc(ir->instrs, ir->capacity * sizeof(IRInstruction));
        }
        int delta = exp_cnt - 1;
        if (delta > 0 && ip + 1 < ir->count) {
            memmove(&ir->instrs[ip + exp_cnt], &ir->instrs[ip + 1],
                    (size_t)(ir->count - (ip + 1)) * sizeof(IRInstruction));
        }
        for (int e = 0; e < exp_cnt; e++) {
            ir->instrs[ip + e] = exp[e];
        }
        ir->count += delta;
        ip += delta;
        expansions++;
    }
}

static void ir_compute_def_use(IRBuffer* ir, int* def_count, int* use_count) {
    int n = ir->vreg_count;
    memset(def_count, 0, (size_t)n * sizeof(int));
    memset(use_count, 0, (size_t)n * sizeof(int));
    for (int ip = 0; ip < ir->count; ip++) {
        IRInstruction* ins = &ir->instrs[ip];
        if (ins->op == IR_NOP) continue;
        if (ins->dest >= 0 && ins->dest < n) def_count[ins->dest]++;
        if (ins->src1 >= 0 && ins->src1 < n) use_count[ins->src1]++;
        if (ins->src2 >= 0 && ins->src2 < n) use_count[ins->src2]++;
        if (ins->src3 >= 0 && ins->src3 < n) use_count[ins->src3]++;
        if (ins->src4 >= 0 && ins->src4 < n) use_count[ins->src4]++;
        for (int a = 0; a < ins->arg_count; a++) {
            if (ins->args[a] >= 0 && ins->args[a] < n) use_count[ins->args[a]]++;
        }
        if (ins->op == IR_RAW_RIEMANN || ins->op == IR_VEC_LOOP ||
            ins->op == IR_VEC_ARR_OP || ins->op == IR_VEC_GRID_MOD ||
            ins->op == IR_VEC_MOD_FILL || ins->op == IR_VEC_F64_AXPY) {
            if (ins->i_vreg >= 0 && ins->i_vreg < n) { use_count[ins->i_vreg]++; def_count[ins->i_vreg]++; }
            if (ins->lim_vreg >= 0 && ins->lim_vreg < n) use_count[ins->lim_vreg]++;
            if (ins->acc_vreg >= 0 && ins->acc_vreg < n) { use_count[ins->acc_vreg]++; def_count[ins->acc_vreg]++; }
            if (ins->x_vreg >= 0 && ins->x_vreg < n) { use_count[ins->x_vreg]++; def_count[ins->x_vreg]++; }
            if (ins->step_vreg >= 0 && ins->step_vreg < n) use_count[ins->step_vreg]++;
        }
    }
}

static void ir_unroll_binary_recursion(IRBuffer* ir) {
    if (!ir || ir->func_name[0] == '\0' || ir->param_count != 1) return;
    if (ir->count < 10 || ir->count > 48 || ir->vreg_count <= 0) return;

    int nv = ir->vreg_count;
    bool* has_c = (bool*)calloc((size_t)nv, sizeof(bool));
    int64_t* c_val = (int64_t*)calloc((size_t)nv, sizeof(int64_t));
    VReg* alias = (VReg*)malloc((size_t)nv * sizeof(VReg));
    if (!has_c || !c_val || !alias) {
        free(has_c); free(c_val); free(alias);
        return;
    }
    for (int i = 0; i < nv; i++) alias[i] = i;

    VReg param_v = VREG_NONE;
    int call_ips[2] = { -1, -1 };
    int call_cnt = 0;
    int64_t call_delta[2] = { 0, 0 };
    VReg call_dest[2] = { VREG_NONE, VREG_NONE };
    int base_ret_ip = -1;
    int rec_ret_ip = -1;
    int label_rec = -1;
    bool ok = true;

    for (int ip = 0; ip < ir->count; ip++) {
        IRInstruction* ins = &ir->instrs[ip];
        if (ins->op == IR_NOP) continue;
        if (ins->op == IR_LOAD_PARAM && ins->imm == 0 && ins->type == TYPE_I64 && param_v == VREG_NONE) {
            param_v = ins->dest;
            continue;
        }
        if (ins->op == IR_CONST_I64 && ins->dest >= 0 && ins->dest < nv) {
            has_c[ins->dest] = true;
            c_val[ins->dest] = ins->imm;
            continue;
        }
        if (ins->op == IR_MOV && ins->dest >= 0 && ins->dest < nv && ins->src1 >= 0 && ins->src1 < nv) {
            if (has_c[ins->src1]) {
                has_c[ins->dest] = true;
                c_val[ins->dest] = c_val[ins->src1];
            }
            alias[ins->dest] = alias[ins->src1];
            continue;
        }
        if (ins->op == IR_CALL || ins->op == IR_CALL_FFI || ins->op == IR_STORE_ARRAY ||
            ins->op == IR_JMP) {
            ok = false; break;
        }
        if (ins->op == IR_CALL_USER) {
            if (call_cnt >= 2 || ins->arg_count != 1 || strcmp(ins->func_name, ir->func_name) != 0) {
                ok = false; break;
            }
            call_ips[call_cnt] = ip;
            call_dest[call_cnt] = ins->dest;
            call_cnt++;
            continue;
        }
        if (ins->op == IR_RET) {
            if (base_ret_ip == -1) base_ret_ip = ip;
            else if (rec_ret_ip == -1) rec_ret_ip = ip;
            else { ok = false; break; }
        }
        if (ins->op == IR_JZ && label_rec == -1) {
            label_rec = (int)ins->imm;
        }
    }

    if (!ok || param_v == VREG_NONE || call_cnt != 2 || base_ret_ip < 0 || rec_ret_ip < 0 || label_rec < 0) {
        free(has_c); free(c_val); free(alias);
        return;
    }

    /* Verify base case: `if n <= 1 { return n; }` (or `n < 2`) before label_rec */
    bool has_base_cmp = false;
    int label_rec_ip = -1;
    for (int ip = 0; ip < base_ret_ip; ip++) {
        IRInstruction* ins = &ir->instrs[ip];
        if ((ins->op == IR_LE || ins->op == IR_LT) && ins->src1 >= 0 && alias[ins->src1] == param_v &&
            ins->src2 >= 0 && has_c[ins->src2]) {
            int64_t thr = (ins->op == IR_LE) ? c_val[ins->src2] : (c_val[ins->src2] - 1);
            if (thr == 1) has_base_cmp = true;
        }
    }
    IRInstruction* base_ret = &ir->instrs[base_ret_ip];
    if (!has_base_cmp || base_ret->src1 < 0 || alias[base_ret->src1] != param_v) {
        free(has_c); free(c_val); free(alias);
        return;
    }
    for (int ip = base_ret_ip + 1; ip < call_ips[0]; ip++) {
        if (ir->instrs[ip].op == IR_LABEL && (int)ir->instrs[ip].imm == label_rec) {
            label_rec_ip = ip;
            break;
        }
    }
    if (label_rec_ip < 0) {
        free(has_c); free(c_val); free(alias);
        return;
    }

    /* Verify each call argument is (param - 1) and (param - 2) */
    for (int c = 0; c < 2; c++) {
        VReg arg = alias[ir->instrs[call_ips[c]].args[0]];
        bool found_sub = false;
        for (int ip = label_rec_ip + 1; ip < call_ips[c]; ip++) {
            IRInstruction* ins = &ir->instrs[ip];
            if (ins->op == IR_SUB && ins->dest == arg &&
                ins->src1 >= 0 && alias[ins->src1] == param_v &&
                ins->src2 >= 0 && has_c[ins->src2]) {
                call_delta[c] = c_val[ins->src2];
                found_sub = true;
            }
        }
        if (!found_sub) { ok = false; break; }
    }
    if (!ok || !((call_delta[0] == 1 && call_delta[1] == 2) || (call_delta[0] == 2 && call_delta[1] == 1))) {
        free(has_c); free(c_val); free(alias);
        return;
    }

    /* Verify final return is ADD(call0, call1) */
    bool found_add = false;
    VReg ret_v = alias[ir->instrs[rec_ret_ip].src1];
    for (int ip = call_ips[1] + 1; ip < rec_ret_ip; ip++) {
        IRInstruction* ins = &ir->instrs[ip];
        if (ins->op == IR_ADD && ins->dest == ret_v &&
            ((alias[ins->src1] == call_dest[0] && alias[ins->src2] == call_dest[1]) ||
             (alias[ins->src1] == call_dest[1] && alias[ins->src2] == call_dest[0]))) {
            found_add = true;
            break;
        }
    }
    free(has_c); free(c_val); free(alias);
    if (!found_add) return;

    /* Rewrite IR: n<=1 -> n; n<=4 -> n-1; else r2 = self(n-2), (r2+r2) + self(n-3) */
    char fn_name[64];
    snprintf(fn_name, sizeof(fn_name), "%s", ir->func_name);
    ir->count = 0;
    ir->vreg_count = 0;
    ir->next_label = 0;

    VReg v_n = ir_new_vreg(ir, TYPE_I64);
    IRInstruction* lp = ir_emit(ir, IR_LOAD_PARAM);
    lp->dest = v_n; lp->type = TYPE_I64; lp->imm = 0;

    VReg v_one = ir_new_vreg(ir, TYPE_I64);
    ir_emit_const_i64(ir, v_one, 1);

    VReg v_cmp1 = ir_new_vreg(ir, TYPE_I64);
    IRInstruction* le1 = ir_emit(ir, IR_LE);
    le1->dest = v_cmp1; le1->src1 = v_n; le1->src2 = v_one; le1->type = TYPE_I64;

    int lbl_chk4 = ir_new_label(ir);
    IRInstruction* jz1 = ir_emit(ir, IR_JZ);
    jz1->src1 = v_cmp1; jz1->imm = lbl_chk4;

    IRInstruction* r0 = ir_emit(ir, IR_RET);
    r0->src1 = v_n; r0->type = TYPE_I64;

    IRInstruction* l1 = ir_emit(ir, IR_LABEL);
    l1->imm = lbl_chk4;

    VReg v_four = ir_new_vreg(ir, TYPE_I64);
    ir_emit_const_i64(ir, v_four, 4);

    VReg v_cmp4 = ir_new_vreg(ir, TYPE_I64);
    IRInstruction* le4 = ir_emit(ir, IR_LE);
    le4->dest = v_cmp4; le4->src1 = v_n; le4->src2 = v_four; le4->type = TYPE_I64;

    int lbl_rec = ir_new_label(ir);
    IRInstruction* jz4 = ir_emit(ir, IR_JZ);
    jz4->src1 = v_cmp4; jz4->imm = lbl_rec;

    VReg v_nm1 = ir_new_vreg(ir, TYPE_I64);
    IRInstruction* sub1 = ir_emit(ir, IR_SUB);
    sub1->dest = v_nm1; sub1->src1 = v_n; sub1->src2 = v_one; sub1->type = TYPE_I64;

    IRInstruction* r1 = ir_emit(ir, IR_RET);
    r1->src1 = v_nm1; r1->type = TYPE_I64;

    IRInstruction* l2 = ir_emit(ir, IR_LABEL);
    l2->imm = lbl_rec;

    VReg v_two = ir_new_vreg(ir, TYPE_I64);
    ir_emit_const_i64(ir, v_two, 2);

    VReg v_nm2 = ir_new_vreg(ir, TYPE_I64);
    IRInstruction* sub2 = ir_emit(ir, IR_SUB);
    sub2->dest = v_nm2; sub2->src1 = v_n; sub2->src2 = v_two; sub2->type = TYPE_I64;

    VReg v_r2 = ir_new_vreg(ir, TYPE_I64);
    IRInstruction* c2 = ir_emit(ir, IR_CALL_USER);
    c2->dest = v_r2; c2->type = TYPE_I64; c2->arg_count = 1; c2->args[0] = v_nm2;
    snprintf(c2->func_name, sizeof(c2->func_name), "%s", fn_name);

    VReg v_three = ir_new_vreg(ir, TYPE_I64);
    ir_emit_const_i64(ir, v_three, 3);

    VReg v_nm3 = ir_new_vreg(ir, TYPE_I64);
    IRInstruction* sub3 = ir_emit(ir, IR_SUB);
    sub3->dest = v_nm3; sub3->src1 = v_n; sub3->src2 = v_three; sub3->type = TYPE_I64;

    VReg v_r3 = ir_new_vreg(ir, TYPE_I64);
    IRInstruction* c3 = ir_emit(ir, IR_CALL_USER);
    c3->dest = v_r3; c3->type = TYPE_I64; c3->arg_count = 1; c3->args[0] = v_nm3;
    snprintf(c3->func_name, sizeof(c3->func_name), "%s", fn_name);

    VReg v_r2x2 = ir_new_vreg(ir, TYPE_I64);
    IRInstruction* add1 = ir_emit(ir, IR_ADD);
    add1->dest = v_r2x2; add1->src1 = v_r2; add1->src2 = v_r2; add1->type = TYPE_I64;

    VReg v_s2 = ir_new_vreg(ir, TYPE_I64);
    IRInstruction* add2 = ir_emit(ir, IR_ADD);
    add2->dest = v_s2; add2->src1 = v_r2x2; add2->src2 = v_r3; add2->type = TYPE_I64;

    IRInstruction* r2 = ir_emit(ir, IR_RET);
    r2->src1 = v_s2; r2->type = TYPE_I64;
}

typedef struct {
    IROpcode op;
    VReg v1;
    VReg v2;
    VReg v3;
    int64_t known_len;
    int32_t offset;
} SeenCheck;

/* Straight-line basic-block deduplication for IR_BOUNDS_CHECK and IR_PTR_CHECK.
 * Safe in both --safe and --release: clears on any control-flow boundary, call,
 * or write to any tracked operand register. */
static void ir_dedup_checks(IRBuffer* ir) {
    SeenCheck seen[32];
    int seen_count = 0;

    for (int ip = 0; ip < ir->count; ip++) {
        IRInstruction* ins = &ir->instrs[ip];
        switch (ins->op) {
            case IR_LABEL:
            case IR_JMP:
            case IR_JZ:
            case IR_JNZ:
            case IR_CALL:
            case IR_RET:
            case IR_RAW_RIEMANN:
            case IR_VEC_LOOP:
            case IR_VEC_ARR_OP:
            case IR_VEC_GRID_MOD:
            case IR_VEC_MOD_FILL:
            case IR_VEC_F64_AXPY:
                seen_count = 0;
                break;

            case IR_PTR_CHECK: {
                bool dup = false;
                for (int k = 0; k < seen_count; k++) {
                    if (seen[k].op == IR_PTR_CHECK && seen[k].v1 == ins->src1) {
                        dup = true;
                        break;
                    }
                }
                if (dup) {
                    ins->op = IR_NOP;
                } else if (seen_count < 32) {
                    seen[seen_count++] = (SeenCheck){ IR_PTR_CHECK, ins->src1, VREG_NONE, VREG_NONE, 0, 0 };
                }
                break;
            }

            case IR_BOUNDS_CHECK: {
                bool null_only_release = (g_compile_mode == VORTEX_MODE_RELEASE &&
                                          ins->src3 == VREG_NONE && ins->known_len < 0);
                bool dup = false;
                for (int k = 0; k < seen_count; k++) {
                    if (seen[k].op != IR_BOUNDS_CHECK || seen[k].v1 != ins->src1) continue;
                    if (null_only_release && seen[k].v3 == VREG_NONE && seen[k].known_len < 0) {
                        dup = true;
                        break;
                    }
                    if (seen[k].v2 == ins->src2 && seen[k].v3 == ins->src3 &&
                        seen[k].known_len == ins->known_len && seen[k].offset == ins->offset) {
                        dup = true;
                        break;
                    }
                }
                if (dup) {
                    ins->op = IR_NOP;
                } else if (seen_count < 32) {
                    seen[seen_count++] = (SeenCheck){
                        IR_BOUNDS_CHECK, ins->src1, ins->src2, ins->src3, ins->known_len, ins->offset
                    };
                }
                break;
            }

            default:
                break;
        }

        if (ins->dest >= 0) {
            for (int k = 0; k < seen_count; ) {
                if (seen[k].v1 == ins->dest || seen[k].v2 == ins->dest || seen[k].v3 == ins->dest) {
                    seen[k] = seen[--seen_count];
                } else {
                    k++;
                }
            }
        }
    }
}

static bool ir_is_arena_alloc_call(const IRInstruction* ins) {
    if (!ins || ins->op != IR_CALL || !ins->func_ptr) return false;
    return ins->func_ptr == (void*)vortex_alloc_slice_i64 ||
           ins->func_ptr == (void*)vortex_alloc_slice_f32 ||
           ins->func_ptr == (void*)vortex_alloc_slice_f64 ||
           ins->func_ptr == (void*)vortex_alloc_slice_i32 ||
           ins->func_ptr == (void*)vortex_alloc_struct ||
           ins->func_ptr == (void*)arena_alloc;
}

static bool ir_is_scope_pop_call(const IRInstruction* ins) {
    return ins && ins->op == IR_CALL && ins->func_ptr == (void*)vortex_scope_pop;
}

static bool ir_safe_ptr_proven_alive(const IRBuffer* ir, int lbl_ip, int ip_jmp,
                                     VReg base, const VReg* equiv, int vreg_cap,
                                     const int* def_count) {
    if (base < 0 || base >= vreg_cap) return false;
    VReg eb = equiv[base];

    /* 1. Single-def local allocation in this function with no scope_pop/call up to ip_jmp */
    if (def_count[eb] == 1) {
        int alloc_ip = -1;
        for (int p = 0; p < lbl_ip; p++) {
            const IRInstruction* pi = &ir->instrs[p];
            if (pi->dest == eb && ir_is_arena_alloc_call(pi)) {
                alloc_ip = p;
                break;
            }
        }
        if (alloc_ip >= 0) {
            bool clean = true;
            for (int p = alloc_ip + 1; p <= ip_jmp; p++) {
                const IRInstruction* pi = &ir->instrs[p];
                if (pi->op == IR_CALL_USER || pi->op == IR_CALL_FFI || ir_is_scope_pop_call(pi)) {
                    clean = false;
                    break;
                }
            }
            if (clean) return true;
        }
    }

    /* 2. Already checked in straight-line block dominating loop entry (e.g., parameter checked before while) */
    for (int p = lbl_ip - 1; p >= 0; p--) {
        const IRInstruction* pi = &ir->instrs[p];
        if (pi->op == IR_LABEL || pi->op == IR_JMP || pi->op == IR_JZ || pi->op == IR_JNZ || pi->op == IR_RET) {
            break;
        }
        if (pi->op == IR_CALL_USER || pi->op == IR_CALL_FFI || ir_is_scope_pop_call(pi)) {
            break;
        }
        if (pi->op == IR_CALL && !ir_is_inline_math_intrinsic(pi) && !ir_is_arena_alloc_call(pi)) {
            break;
        }
        if ((pi->op == IR_BOUNDS_CHECK || pi->op == IR_PTR_CHECK || pi->op == IR_RANGE_CHECK) &&
            pi->src1 >= 0 && pi->src1 < vreg_cap && equiv[pi->src1] == eb) {
            return true;
        }
    }
    return false;
}

void ir_optimize_bounds_and_licm(IRBuffer* ir) {
    if (!ir || ir->count == 0 || ir->vreg_count == 0) return;
    ir_dedup_checks(ir);

    /* Release-only aggressive cross-function optimizations (preserve exact stack frames in --safe) */
    if (g_compile_mode == VORTEX_MODE_RELEASE) {
        ir_inline_small_functions(ir);
        ir_unroll_binary_recursion(ir);
    }

    int n_vregs = ir->vreg_count;
    int vreg_cap = n_vregs + 256;
    bool* has_const = (bool*)calloc((size_t)vreg_cap, sizeof(bool));
    int64_t* const_val = (int64_t*)calloc((size_t)vreg_cap, sizeof(int64_t));
    int* def_count = (int*)calloc((size_t)vreg_cap, sizeof(int));
    int* use_count = (int*)calloc((size_t)vreg_cap, sizeof(int));
    int* def_ip = (int*)malloc((size_t)vreg_cap * sizeof(int));
    VReg* equiv = (VReg*)malloc((size_t)vreg_cap * sizeof(VReg));
    if (!has_const || !const_val || !def_count || !use_count || !def_ip || !equiv) {
        free(has_const); free(const_val); free(def_count); free(use_count); free(def_ip); free(equiv);
        return;
    }
    for (int v = 0; v < vreg_cap; v++) {
        def_ip[v] = -1;
        equiv[v] = v;
    }

    for (int ip = 0; ip < ir->count; ip++) {
        IRInstruction* ins = &ir->instrs[ip];
        if (ins->dest >= 0 && ins->dest < vreg_cap) {
            def_count[ins->dest]++;
            def_ip[ins->dest] = ip;
        }
    }

    for (int ip = 0; ip < ir->count; ip++) {
        IRInstruction* ins = &ir->instrs[ip];
        if (ins->op == IR_CONST_I64 && ins->dest >= 0 && ins->dest < vreg_cap && def_count[ins->dest] == 1) {
            has_const[ins->dest] = true;
            const_val[ins->dest] = ins->imm;
        } else if (ins->op == IR_MOV && ins->dest >= 0 && ins->dest < vreg_cap &&
                   ins->src1 >= 0 && ins->src1 < vreg_cap &&
                   def_count[ins->dest] == 1 && def_count[ins->src1] == 1) {
            equiv[ins->dest] = equiv[ins->src1];
            if (has_const[ins->src1]) {
                has_const[ins->dest] = true;
                const_val[ins->dest] = const_val[ins->src1];
            }
        }
    }

    if (g_compile_mode == VORTEX_MODE_RELEASE) {
        /* Pass 1: Static constant range check elimination */
        for (int ip = 0; ip < ir->count; ip++) {
            IRInstruction* ins = &ir->instrs[ip];
            if (ins->op == IR_BOUNDS_CHECK) {
                int64_t len_c = ins->known_len;
                if (len_c < 0 && ins->src3 >= 0 && ins->src3 < vreg_cap && has_const[ins->src3]) {
                    len_c = const_val[ins->src3];
                }
                if (len_c >= 0 && ins->src2 >= 0 && ins->src2 < vreg_cap && has_const[ins->src2]) {
                    int64_t idx_c = const_val[ins->src2];
                    if (idx_c >= 0 && idx_c < len_c) {
                        ins->op = IR_NOP;
                    }
                }
            }
        }
    }

    /* Pass 2: Loop-Invariant Bounds Check Elimination & Affine Hoisting (--release AND --safe) */
    bool* modified = (bool*)calloc((size_t)vreg_cap, sizeof(bool));
    if (modified) {
        for (int ip = 0; ip < ir->count; ip++) {
            if (ir->instrs[ip].op != IR_LABEL) continue;
            int lbl_start = (int)ir->instrs[ip].imm;
            if (ip + 2 >= ir->count) continue;
            IRInstruction* ins_cmp = &ir->instrs[ip + 1];
            IRInstruction* ins_jz = &ir->instrs[ip + 2];
            if (ins_cmp->op != IR_LT || ins_jz->op != IR_JZ || ins_jz->src1 != ins_cmp->dest) continue;

            int lbl_exit = (int)ins_jz->imm;
            VReg i_vreg = ins_cmp->src1;
            VReg lim_vreg = ins_cmp->src2;

            int ip_jmp = -1;
            for (int j = ip + 3; j < ir->count; j++) {
                if (ir->instrs[j].op == IR_JMP && (int)ir->instrs[j].imm == lbl_start) {
                    ip_jmp = j;
                    break;
                }
            }
            if (ip_jmp == -1 || ip_jmp + 1 >= ir->count) continue;
            if (ir->instrs[ip_jmp + 1].op != IR_LABEL || (int)ir->instrs[ip_jmp + 1].imm != lbl_exit) continue;

            /* Reject loops with jumps to start/exit labels from elsewhere */
            int start_refs = 0, exit_refs = 0;
            for (int q = 0; q < ir->count; q++) {
                IROpcode qop = ir->instrs[q].op;
                if (qop == IR_JMP || qop == IR_JZ || qop == IR_JNZ) {
                    if ((int)ir->instrs[q].imm == lbl_start) start_refs++;
                    if ((int)ir->instrs[q].imm == lbl_exit) exit_refs++;
                }
            }
            if (start_refs != 1 || exit_refs != 1) continue;

            /* Verify i_vreg only increments by +1 at end of loop; detect any side-effecting calls */
            bool valid_ind = false;
            int ind_writes = 0;
            bool loop_has_call = false;
            for (int k = ip + 3; k < ip_jmp; k++) {
                IROpcode kop = ir->instrs[k].op;
                if (kop == IR_CALL_USER || kop == IR_CALL_FFI ||
                    (kop == IR_CALL && !ir_is_inline_math_intrinsic(&ir->instrs[k]))) {
                    loop_has_call = true;
                }
                if (ir->instrs[k].dest == i_vreg) {
                    ind_writes++;
                    if (kop == IR_ADD && ir->instrs[k].src1 == i_vreg &&
                        ir->instrs[k].src2 >= 0 && ir->instrs[k].src2 < vreg_cap &&
                        has_const[ir->instrs[k].src2] && const_val[ir->instrs[k].src2] == 1) {
                        valid_ind = true;
                    }
                }
            }
            if (!valid_ind || ind_writes != 1) continue;
            if (g_compile_mode != VORTEX_MODE_RELEASE && loop_has_call) continue;

            /* Determine pre-loop starting value of i_vreg */
            VReg start_vreg = VREG_NONE;
            for (int pre = ip - 1; pre >= 0; pre--) {
                if (ir->instrs[pre].dest == i_vreg) {
                    if (ir->instrs[pre].op == IR_MOV) {
                        start_vreg = ir->instrs[pre].src1;
                    } else {
                        start_vreg = i_vreg;
                    }
                    break;
                }
            }
            if (start_vreg == VREG_NONE) continue;

            memset(modified, 0, (size_t)vreg_cap * sizeof(bool));
            for (int k = ip + 1; k < ip_jmp; k++) {
                if (ir->instrs[k].dest >= 0 && ir->instrs[k].dest < vreg_cap) {
                    modified[ir->instrs[k].dest] = true;
                }
            }
            if (lim_vreg < 0 || lim_vreg >= vreg_cap || modified[lim_vreg]) continue;

            /* Hoist or eliminate unconditional prefix checks inside loop body */
            for (int k = ip + 3; k < ip_jmp; k++) {
                IROpcode kop = ir->instrs[k].op;
                if (kop == IR_JMP || kop == IR_JZ || kop == IR_JNZ || kop == IR_LABEL) {
                    break;
                }
                IRInstruction* chk = &ir->instrs[k];
                if (chk->op == IR_PTR_CHECK) {
                    if (g_compile_mode == VORTEX_MODE_RELEASE &&
                        chk->src1 >= 0 && chk->src1 < vreg_cap && !modified[chk->src1]) {
                        chk->op = IR_NOP;
                    }
                    continue;
                }
                if (chk->op != IR_BOUNDS_CHECK) continue;
                VReg base = chk->src1;
                VReg idx = chk->src2;
                VReg len_v = chk->src3;

                if (base < 0 || base >= vreg_cap || modified[base]) continue;
                if (len_v >= 0 && (len_v >= vreg_cap || modified[len_v])) continue;

                VReg off_v = VREG_NONE;
                bool idx_ok = (idx == i_vreg);
                if (!idx_ok && idx >= 0 && idx < vreg_cap && def_count[idx] == 1) {
                    for (int q = k - 1; q >= ip + 3; q--) {
                        const IRInstruction* qi = &ir->instrs[q];
                        if (qi->dest == idx && qi->op == IR_ADD && qi->type == TYPE_I64 &&
                            qi->src1 >= 0 && qi->src2 >= 0) {
                            if (qi->src1 == i_vreg && qi->src2 < vreg_cap && !modified[qi->src2]) {
                                off_v = qi->src2;
                                idx_ok = true;
                            } else if (qi->src2 == i_vreg && qi->src1 < vreg_cap && !modified[qi->src1]) {
                                off_v = qi->src1;
                                idx_ok = true;
                            }
                            break;
                        }
                    }
                }
                if (!idx_ok) continue;

                if (off_v == VREG_NONE) {
                    bool start_nonneg = (start_vreg >= 0 && start_vreg < vreg_cap &&
                                         has_const[start_vreg] && const_val[start_vreg] >= 0);
                    bool lim_fits_len = false;
                    if (len_v >= 0 && equiv[lim_vreg] == equiv[len_v]) {
                        lim_fits_len = true;
                    } else if (has_const[lim_vreg]) {
                        int64_t lc = const_val[lim_vreg];
                        int64_t lenc = chk->known_len;
                        if (lenc < 0 && len_v >= 0 && has_const[len_v]) lenc = const_val[len_v];
                        if (lenc >= 0 && lc <= lenc) lim_fits_len = true;
                    }

                    if (g_compile_mode == VORTEX_MODE_RELEASE && start_nonneg && lim_fits_len) {
                        chk->op = IR_NOP;
                    } else if (len_v >= 0 || chk->known_len >= 0) {
                        IRInstruction hoisted = *chk;
                        hoisted.op = IR_RANGE_CHECK;
                        hoisted.src1 = base;
                        hoisted.src2 = start_vreg;
                        hoisted.src3 = lim_vreg;
                        hoisted.src4 = len_v;
                        chk->op = IR_NOP;
                        ir_insert_before(ir, ip, &hoisted);
                        ip++;
                        ip_jmp++;
                        k++;
                    }
                } else if ((len_v >= 0 || chk->known_len >= 0) && ir->vreg_count + 2 < vreg_cap &&
                           ir->count + 4 < MAX_IR_INSTRS) {
                    /* Snapshot chk and clear its original slot before ir_insert_before shifts ir->instrs. */
                    IRInstruction orig_chk = *chk;
                    chk->op = IR_NOP;
                    VReg start_r = off_v;
                    bool zero_start = (start_vreg >= 0 && start_vreg < vreg_cap &&
                                       has_const[start_vreg] && const_val[start_vreg] == 0);
                    if (!zero_start) {
                        start_r = ir_new_vreg(ir, TYPE_I64);
                        equiv[start_r] = start_r;
                        IRInstruction add_s = {0};
                        add_s.op = IR_ADD;
                        add_s.type = TYPE_I64;
                        add_s.dest = start_r;
                        add_s.src1 = off_v;
                        add_s.src2 = start_vreg;
                        add_s.src3 = VREG_NONE;
                        add_s.src4 = VREG_NONE;
                        add_s.known_len = -1;
                        add_s.line = orig_chk.line;
                        add_s.file = orig_chk.file;
                        ir_insert_before(ir, ip, &add_s);
                        ip++;
                        ip_jmp++;
                        k++;
                    }
                    VReg lim_r = ir_new_vreg(ir, TYPE_I64);
                    equiv[lim_r] = lim_r;
                    IRInstruction add_l = {0};
                    add_l.op = IR_ADD;
                    add_l.type = TYPE_I64;
                    add_l.dest = lim_r;
                    add_l.src1 = off_v;
                    add_l.src2 = lim_vreg;
                    add_l.src3 = VREG_NONE;
                    add_l.src4 = VREG_NONE;
                    add_l.known_len = -1;
                    add_l.line = orig_chk.line;
                    add_l.file = orig_chk.file;
                    ir_insert_before(ir, ip, &add_l);
                    ip++;
                    ip_jmp++;
                    k++;

                    IRInstruction hoisted = orig_chk;
                    hoisted.op = IR_RANGE_CHECK;
                    hoisted.src1 = base;
                    hoisted.src2 = start_r;
                    hoisted.src3 = lim_r;
                    hoisted.src4 = len_v;
                    ir_insert_before(ir, ip, &hoisted);
                    ip++;
                    ip_jmp++;
                    k++;
                }
            }
        }
    }

    /* Pass 2B (--safe mode, x86-64): Fast inline cold-trap bounds checks inside proven-live call-free loops */
    if (g_compile_mode != VORTEX_MODE_RELEASE && g_target_arch == VORTEX_ARCH_X86_64 && modified) {
        ir_compact_nops(ir);
        ir_compute_def_use(ir, def_count, use_count);
        for (int ip = 0; ip < ir->count; ip++) {
            if (ir->instrs[ip].op != IR_LABEL) continue;
            int lbl_start = (int)ir->instrs[ip].imm;
            int ip_jmp = -1;
            for (int j = ip + 1; j < ir->count; j++) {
                if (ir->instrs[j].op == IR_JMP && (int)ir->instrs[j].imm == lbl_start) {
                    ip_jmp = j;
                    break;
                }
            }
            if (ip_jmp < 0) continue;

            bool call_free = true;
            for (int k = ip + 1; k < ip_jmp; k++) {
                IROpcode kop = ir->instrs[k].op;
                if (kop == IR_CALL_USER || kop == IR_CALL_FFI ||
                    (kop == IR_CALL && !ir_is_inline_math_intrinsic(&ir->instrs[k]))) {
                    call_free = false;
                    break;
                }
            }
            if (!call_free) continue;

            memset(modified, 0, (size_t)vreg_cap * sizeof(bool));
            for (int k = ip + 1; k < ip_jmp; k++) {
                if (ir->instrs[k].dest >= 0 && ir->instrs[k].dest < vreg_cap) {
                    modified[ir->instrs[k].dest] = true;
                }
            }

            for (int k = ip + 1; k < ip_jmp; k++) {
                IRInstruction* chk = &ir->instrs[k];
                if (chk->op != IR_BOUNDS_CHECK) continue;
                VReg base = chk->src1;
                VReg len_v = chk->src3;
                if (base < 0 || base >= vreg_cap || modified[base]) continue;
                if (len_v >= 0 && (len_v >= vreg_cap || modified[len_v])) continue;
                if (len_v < 0 && chk->known_len < 0) continue;

                if (ir_safe_ptr_proven_alive(ir, ip, ip_jmp, base, equiv, vreg_cap, def_count)) {
                    chk->fast_inline_safe = true;
                }
            }
        }
    }
    free(modified);

    if (g_compile_mode == VORTEX_MODE_RELEASE) {
        /* Pass 3: Strip remaining IR_PTR_CHECK / IR_FFI_CHECK in --release mode */
        for (int ip = 0; ip < ir->count; ip++) {
            if (ir->instrs[ip].op == IR_PTR_CHECK || ir->instrs[ip].op == IR_FFI_CHECK) {
                ir->instrs[ip].op = IR_NOP;
            }
        }
    }
    ir_compact_nops(ir);
    n_vregs = ir->vreg_count;
    if (n_vregs > vreg_cap) vreg_cap = n_vregs + 32;

    /* Pass 4 (Pillar #5): Constant folding on 2 constants & Copy propagation / Coalescing */
    bool* is_f32_const = (bool*)calloc((size_t)n_vregs, sizeof(bool));
    bool* is_f64_const = (bool*)calloc((size_t)n_vregs, sizeof(bool));
    float* const_f32_val = (float*)calloc((size_t)n_vregs, sizeof(float));
    double* const_f64_val = (double*)calloc((size_t)n_vregs, sizeof(double));
    for (int pass = 0; pass < 2; pass++) {
        ir_compute_def_use(ir, def_count, use_count);
        memset(has_const, 0, (size_t)n_vregs * sizeof(bool));
        if (is_f32_const) memset(is_f32_const, 0, (size_t)n_vregs * sizeof(bool));
        if (is_f64_const) memset(is_f64_const, 0, (size_t)n_vregs * sizeof(bool));
        for (int ip = 0; ip < ir->count; ip++) {
            IRInstruction* ins = &ir->instrs[ip];
            if (ins->dest >= 0 && def_count[ins->dest] == 1) {
                if (ins->op == IR_CONST_I64) {
                    has_const[ins->dest] = true;
                    const_val[ins->dest] = ins->imm;
                } else if (ins->op == IR_CONST_F32 && is_f32_const) {
                    is_f32_const[ins->dest] = true;
                    const_f32_val[ins->dest] = ins->fimm;
                } else if (ins->op == IR_CONST_F64 && is_f64_const) {
                    is_f64_const[ins->dest] = true;
                    const_f64_val[ins->dest] = ins->f64_imm;
                }
            }
        }

        for (int ip = 0; ip < ir->count; ip++) {
            IRInstruction* ins = &ir->instrs[ip];
            if (ins->op == IR_NOP) continue;

            /* Fold constant arithmetic */
            if (ins->type == TYPE_I64 && ins->dest >= 0 &&
                ins->src1 >= 0 && ins->src2 >= 0 &&
                has_const[ins->src1] && has_const[ins->src2]) {
                uint64_t a = (uint64_t)const_val[ins->src1];
                uint64_t b = (uint64_t)const_val[ins->src2];
                int64_t sa = const_val[ins->src1];
                int64_t sb = const_val[ins->src2];
                bool folded = true;
                int64_t res = 0;
                if (ins->op == IR_ADD) res = (int64_t)(a + b);
                else if (ins->op == IR_SUB) res = (int64_t)(a - b);
                else if (ins->op == IR_MUL) res = (int64_t)(a * b);
                else if (ins->op == IR_DIV && sb != 0 && !(sa == INT64_MIN && sb == -1)) res = sa / sb;
                else if (ins->op == IR_MOD && sb != 0 && !(sa == INT64_MIN && sb == -1)) res = sa % sb;
                else if (ins->op == IR_EQ) res = (sa == sb) ? 1 : 0;
                else if (ins->op == IR_NE) res = (sa != sb) ? 1 : 0;
                else if (ins->op == IR_LT) res = (sa < sb) ? 1 : 0;
                else if (ins->op == IR_LE) res = (sa <= sb) ? 1 : 0;
                else if (ins->op == IR_GT) res = (sa > sb) ? 1 : 0;
                else if (ins->op == IR_GE) res = (sa >= sb) ? 1 : 0;
                else if (ins->op == IR_BAND) res = (int64_t)(a & b);
                else if (ins->op == IR_BOR) res = (int64_t)(a | b);
                else if (ins->op == IR_BXOR) res = (int64_t)(a ^ b);
                else if (ins->op == IR_SHL && b < 64) res = (int64_t)(a << b);
                else if (ins->op == IR_SHR && b < 64) res = (int64_t)(sa >> b);
                else folded = false;
                if (folded) {
                    ins->op = IR_CONST_I64;
                    ins->imm = res;
                    ins->src1 = VREG_NONE;
                    ins->src2 = VREG_NONE;
                    if (def_count[ins->dest] == 1) {
                        has_const[ins->dest] = true;
                        const_val[ins->dest] = res;
                    }
                    continue;
                }
            }

            /* Algebraic integer identities */
            if (ins->type == TYPE_I64 && ins->dest >= 0 && ins->src1 >= 0 && ins->src2 >= 0) {
                bool c1 = has_const[ins->src1], c2 = has_const[ins->src2];
                int64_t v1 = c1 ? const_val[ins->src1] : 0;
                int64_t v2 = c2 ? const_val[ins->src2] : 0;
                if ((ins->op == IR_ADD && c2 && v2 == 0) ||
                    (ins->op == IR_SUB && c2 && v2 == 0) ||
                    (ins->op == IR_MUL && c2 && v2 == 1) ||
                    (ins->op == IR_DIV && c2 && v2 == 1) ||
                    (ins->op == IR_BOR && c2 && v2 == 0) ||
                    (ins->op == IR_BXOR && c2 && v2 == 0) ||
                    ((ins->op == IR_SHL || ins->op == IR_SHR) && c2 && v2 == 0)) {
                    ins->op = IR_MOV;
                    ins->src2 = VREG_NONE;
                    continue;
                }
                if ((ins->op == IR_ADD || ins->op == IR_BOR || ins->op == IR_BXOR) && c1 && v1 == 0) {
                    ins->op = IR_MOV;
                    ins->src1 = ins->src2;
                    ins->src2 = VREG_NONE;
                    continue;
                }
                if (ins->op == IR_MUL && c1 && v1 == 1) {
                    ins->op = IR_MOV;
                    ins->src1 = ins->src2;
                    ins->src2 = VREG_NONE;
                    continue;
                }
                if (((ins->op == IR_MUL || ins->op == IR_BAND) && ((c2 && v2 == 0) || (c1 && v1 == 0)))) {
                    ins->op = IR_CONST_I64;
                    ins->imm = 0;
                    ins->src1 = VREG_NONE;
                    ins->src2 = VREG_NONE;
                    if (def_count[ins->dest] == 1) {
                        has_const[ins->dest] = true;
                        const_val[ins->dest] = 0;
                    }
                    continue;
                }
            }

            /* Fold constant f32 arithmetic */
            if (ins->type == TYPE_F32 && ins->dest >= 0 &&
                ins->src1 >= 0 && ins->src2 >= 0 &&
                is_f32_const && is_f32_const[ins->src1] && is_f32_const[ins->src2]) {
                float fa = const_f32_val[ins->src1];
                float fb = const_f32_val[ins->src2];
                bool folded = true;
                float fres = 0.0f;
                if (ins->op == IR_FADD) fres = fa + fb;
                else if (ins->op == IR_FSUB) fres = fa - fb;
                else if (ins->op == IR_FMUL) fres = fa * fb;
                else if (ins->op == IR_FDIV && fb != 0.0f) fres = fa / fb;
                else folded = false;
                if (folded) {
                    ins->op = IR_CONST_F32;
                    ins->fimm = fres;
                    ins->src1 = VREG_NONE;
                    ins->src2 = VREG_NONE;
                    if (def_count[ins->dest] == 1) {
                        is_f32_const[ins->dest] = true;
                        const_f32_val[ins->dest] = fres;
                    }
                    continue;
                }
            }

            /* Fold constant f64 arithmetic */
            if (ins->type == TYPE_F64 && ins->dest >= 0 &&
                ins->src1 >= 0 && ins->src2 >= 0 &&
                is_f64_const && is_f64_const[ins->src1] && is_f64_const[ins->src2]) {
                double da = const_f64_val[ins->src1];
                double db = const_f64_val[ins->src2];
                bool folded = true;
                double dres = 0.0;
                if (ins->op == IR_FADD) dres = da + db;
                else if (ins->op == IR_FSUB) dres = da - db;
                else if (ins->op == IR_FMUL) dres = da * db;
                else if (ins->op == IR_FDIV && db != 0.0) dres = da / db;
                else folded = false;
                if (folded) {
                    ins->op = IR_CONST_F64;
                    ins->f64_imm = dres;
                    ins->src1 = VREG_NONE;
                    ins->src2 = VREG_NONE;
                    if (def_count[ins->dest] == 1) {
                        is_f64_const[ins->dest] = true;
                        const_f64_val[ins->dest] = dres;
                    }
                    continue;
                }
            }

            /* Propagate single-def immutable copies: d = MOV s (where s is single-def) */
            if (ins->op == IR_MOV && ins->dest >= 0 && ins->src1 >= 0 &&
                def_count[ins->dest] == 1 && def_count[ins->src1] == 1 &&
                ir->vregs[ins->dest].is_float == ir->vregs[ins->src1].is_float &&
                (!ir->vregs[ins->dest].is_float || ir->vregs[ins->dest].type == ir->vregs[ins->src1].type)) {
                VReg old_v = ins->dest;
                VReg new_v = ins->src1;
                if (old_v != new_v) {
                    for (int q = 0; q < ir->count; q++) {
                        IRInstruction* qi = &ir->instrs[q];
                        if (qi->op == IR_NOP) continue;
                        if (qi->src1 == old_v) qi->src1 = new_v;
                        if (qi->src2 == old_v) qi->src2 = new_v;
                        if (qi->src3 == old_v) qi->src3 = new_v;
                        if (qi->src4 == old_v) qi->src4 = new_v;
                        for (int a = 0; a < qi->arg_count; a++) {
                            if (qi->args[a] == old_v) qi->args[a] = new_v;
                        }
                        if (qi->op == IR_RAW_RIEMANN || qi->op == IR_VEC_LOOP ||
                            qi->op == IR_VEC_ARR_OP || qi->op == IR_VEC_GRID_MOD ||
                            qi->op == IR_VEC_MOD_FILL || qi->op == IR_VEC_F64_AXPY) {
                            if (qi->lim_vreg == old_v) qi->lim_vreg = new_v;
                            if (qi->step_vreg == old_v) qi->step_vreg = new_v;
                            for (int b = 0; b < qi->body_op_count; b++) {
                                IRInstruction* bo = &qi->body_ops[b];
                                if (bo->src1 == old_v) bo->src1 = new_v;
                                if (bo->src2 == old_v) bo->src2 = new_v;
                                if (bo->src3 == old_v) bo->src3 = new_v;
                            }
                        }
                    }
                    ins->op = IR_NOP;
                    continue;
                }
            }

            /* Eliminate local straight-line copy: tmp = MOV orig; OP ... tmp (tmp single-def & single-use at ip+1) */
            if (ins->op == IR_MOV && ins->dest >= 0 && ins->src1 >= 0 &&
                def_count[ins->dest] == 1 && use_count[ins->dest] == 1 &&
                ir->vregs[ins->dest].is_float == ir->vregs[ins->src1].is_float &&
                (!ir->vregs[ins->dest].is_float || ir->vregs[ins->dest].type == ir->vregs[ins->src1].type) &&
                ip + 1 < ir->count) {
                IRInstruction* nxt = &ir->instrs[ip + 1];
                VReg tmp = ins->dest;
                VReg orig = ins->src1;
                if (nxt->op != IR_NOP && nxt->op != IR_LABEL &&
                    (nxt->src1 == tmp || nxt->src2 == tmp || nxt->src3 == tmp || nxt->src4 == tmp ||
                     ((nxt->op == IR_RAW_RIEMANN || nxt->op == IR_VEC_LOOP || nxt->op == IR_VEC_ARR_OP || nxt->op == IR_VEC_GRID_MOD || nxt->op == IR_VEC_MOD_FILL || nxt->op == IR_VEC_F64_AXPY) &&
                      (nxt->lim_vreg == tmp || nxt->step_vreg == tmp)))) {
                    if (nxt->src1 == tmp) nxt->src1 = orig;
                    if (nxt->src2 == tmp) nxt->src2 = orig;
                    if (nxt->src3 == tmp) nxt->src3 = orig;
                    if (nxt->src4 == tmp) nxt->src4 = orig;
                    if (nxt->op == IR_RAW_RIEMANN || nxt->op == IR_VEC_LOOP || nxt->op == IR_VEC_ARR_OP || nxt->op == IR_VEC_GRID_MOD || nxt->op == IR_VEC_MOD_FILL || nxt->op == IR_VEC_F64_AXPY) {
                        if (nxt->lim_vreg == tmp) nxt->lim_vreg = orig;
                        if (nxt->step_vreg == tmp) nxt->step_vreg = orig;
                    }
                    ins->op = IR_NOP;
                    continue;
                }
            }

            /* Tail move coalescing: tmp = PURE_OP ...; dst = MOV tmp (tmp single-def & single-use) */
            if (ip + 1 < ir->count && ir->instrs[ip + 1].op == IR_MOV &&
                ins->dest >= 0 && !ir_has_side_effect(ins->op) &&
                ins->op != IR_RAW_RIEMANN && ins->op != IR_VEC_LOOP && ins->op != IR_VEC_ARR_OP && ins->op != IR_VEC_GRID_MOD && ins->op != IR_VEC_MOD_FILL && ins->op != IR_VEC_F64_AXPY) {
                IRInstruction* mv = &ir->instrs[ip + 1];
                VReg tmp = ins->dest;
                VReg dst = mv->dest;
                if (mv->src1 == tmp && dst >= 0 &&
                    def_count[tmp] == 1 && use_count[tmp] == 1 &&
                    ir->vregs[tmp].is_float == ir->vregs[dst].is_float &&
                    (!ir->vregs[tmp].is_float || ir->vregs[tmp].type == ir->vregs[dst].type)) {
                    ins->dest = dst;
                    mv->op = IR_NOP;
                }
            }
        }
        ir_compact_nops(ir);
    }

    /* Pass 5 (Pillar #5): If-conversion for conditional modulo: if x >= M { x = x - M; } -> IR_SUB_MOD_IMM */
    if (g_target_arch == VORTEX_ARCH_X86_64) {
        ir_compute_def_use(ir, def_count, use_count);
        memset(has_const, 0, (size_t)n_vregs * sizeof(bool));
        for (int ip = 0; ip < ir->count; ip++) {
            IRInstruction* ins = &ir->instrs[ip];
            if (ins->op == IR_CONST_I64 && ins->dest >= 0 && def_count[ins->dest] == 1) {
                has_const[ins->dest] = true;
                const_val[ins->dest] = ins->imm;
            }
        }

        for (int ip = 0; ip + 3 < ir->count; ip++) {
            IRInstruction* g_ins = &ir->instrs[ip];
            IRInstruction* z_ins = &ir->instrs[ip + 1];
            if (g_ins->op != IR_GE || z_ins->op != IR_JZ || z_ins->src1 != g_ins->dest) continue;
            if (def_count[g_ins->dest] != 1 || use_count[g_ins->dest] != 1) continue;
            if (g_ins->src1 < 0 || g_ins->src2 < 0 || !has_const[g_ins->src2]) continue;

            VReg x_v = g_ins->src1;
            int64_t mod_m = const_val[g_ins->src2];
            int lbl_skip = (int)z_ins->imm;
            if (mod_m <= 0 || mod_m > 2147483647LL) continue;

            int sub_ip = -1;
            int lbl_ip = -1;
            for (int q = ip + 2; q < ir->count && q <= ip + 6; q++) {
                IROpcode qop = ir->instrs[q].op;
                if (qop == IR_CONST_I64) continue;
                if (qop == IR_SUB && sub_ip == -1 &&
                    ir->instrs[q].src1 == x_v && ir->instrs[q].dest == x_v &&
                    ir->instrs[q].src2 >= 0 && has_const[ir->instrs[q].src2] &&
                    const_val[ir->instrs[q].src2] == mod_m) {
                    sub_ip = q;
                    continue;
                }
                if (qop == IR_LABEL && (int)ir->instrs[q].imm == lbl_skip) {
                    lbl_ip = q;
                    break;
                }
                break;
            }
            if (sub_ip < 0 || lbl_ip < 0) continue;

            /* Verify no other branch jumps to lbl_skip */
            int lbl_refs = 0;
            for (int q = 0; q < ir->count; q++) {
                IROpcode qop = ir->instrs[q].op;
                if ((qop == IR_JMP || qop == IR_JZ || qop == IR_JNZ) &&
                    (int)ir->instrs[q].imm == lbl_skip) {
                    lbl_refs++;
                }
            }
            if (lbl_refs != 1) continue;

            memset(g_ins, 0, sizeof(*g_ins));
            g_ins->op = IR_SUB_MOD_IMM;
            g_ins->type = TYPE_I64;
            g_ins->dest = x_v;
            g_ins->src1 = x_v;
            g_ins->src2 = VREG_NONE;
            g_ins->src3 = VREG_NONE;
            g_ins->src4 = VREG_NONE;
            g_ins->known_len = -1;
            g_ins->imm = mod_m;
            z_ins->op = IR_NOP;
            ir->instrs[sub_ip].op = IR_NOP;
            ir->instrs[lbl_ip].op = IR_NOP;

            /* Collapse post-inlined tail: next = ADD acc, c; SUB_MOD next; acc = MOV next */
            if (ip >= 1 && lbl_ip + 1 < ir->count &&
                ir->instrs[ip - 1].dest == x_v &&
                ir->instrs[lbl_ip + 1].op == IR_MOV &&
                ir->instrs[lbl_ip + 1].src1 == x_v &&
                def_count[x_v] == 2 && use_count[x_v] == 3) {
                VReg outer_dst = ir->instrs[lbl_ip + 1].dest;
                if (outer_dst >= 0 && !ir->vregs[outer_dst].is_float) {
                    ir->instrs[ip - 1].dest = outer_dst;
                    g_ins->dest = outer_dst;
                    g_ins->src1 = outer_dst;
                    ir->instrs[lbl_ip + 1].op = IR_NOP;
                }
            }
        }
        ir_compact_nops(ir);

        /* Fuse adjacent ADD(x, step_const) + IR_SUB_MOD_IMM(x, M) -> IR_ADD_SUB_MOD_IMM */
        ir_compute_def_use(ir, def_count, use_count);
        memset(has_const, 0, (size_t)n_vregs * sizeof(bool));
        for (int ip = 0; ip < ir->count; ip++) {
            IRInstruction* ins = &ir->instrs[ip];
            if (ins->op == IR_CONST_I64 && ins->dest >= 0 && def_count[ins->dest] == 1) {
                has_const[ins->dest] = true;
                const_val[ins->dest] = ins->imm;
            }
        }
        bool fused_add_sub = false;
        for (int ip = 1; ip < ir->count; ip++) {
            IRInstruction* sub_ins = &ir->instrs[ip];
            if (sub_ins->op != IR_SUB_MOD_IMM || sub_ins->src1 < 0) continue;
            int64_t mod_m = sub_ins->imm;
            if (mod_m <= 0 || mod_m > 2147483647LL) continue;

            int add_ip = -1;
            for (int p = ip - 1; p >= 0 && p >= ip - 4; p--) {
                IRInstruction* pi = &ir->instrs[p];
                if (pi->op == IR_CONST_I64 || pi->op == IR_NOP) {
                    if (pi->dest == sub_ins->src1) break;
                    continue;
                }
                if (pi->op == IR_ADD && pi->type == TYPE_I64 && pi->dest == sub_ins->src1) {
                    add_ip = p;
                }
                break;
            }
            if (add_ip < 0) continue;
            IRInstruction* add_ins = &ir->instrs[add_ip];
            if (sub_ins->dest != add_ins->dest && use_count[add_ins->dest] != 1) continue;

            VReg base_v = VREG_NONE;
            int64_t step_c = 0;
            if (add_ins->src2 >= 0 && has_const[add_ins->src2] && add_ins->src1 >= 0) {
                base_v = add_ins->src1;
                step_c = const_val[add_ins->src2];
            } else if (add_ins->src1 >= 0 && has_const[add_ins->src1] && add_ins->src2 >= 0) {
                base_v = add_ins->src2;
                step_c = const_val[add_ins->src1];
            }
            if (base_v < 0 || step_c < 0 || step_c > mod_m) continue;

            sub_ins->op = IR_ADD_SUB_MOD_IMM;
            sub_ins->src1 = base_v;
            sub_ins->src2 = VREG_NONE;
            sub_ins->offset = (int)step_c;
            add_ins->op = IR_NOP;
            fused_add_sub = true;
        }
        if (fused_add_sub) ir_compact_nops(ir);
    }

    /* Pass 6 (Pillar #4): General Constant LICM from innermost to outermost loop */
    for (int ip = ir->count - 1; ip >= 0; ip--) {
        if (ir->instrs[ip].op != IR_LABEL) continue;
        int lbl_start = (int)ir->instrs[ip].imm;
        int ip_jmp = -1;
        for (int j = ip + 1; j < ir->count; j++) {
            if (ir->instrs[j].op == IR_JMP && (int)ir->instrs[j].imm == lbl_start) {
                ip_jmp = j;
                break;
            }
        }
        if (ip_jmp < 0) continue;

        ir_compute_def_use(ir, def_count, use_count);
        for (int k = ip + 1; k < ip_jmp; k++) {
            IROpcode op = ir->instrs[k].op;
            if (op != IR_CONST_I64 && op != IR_CONST_F32 && op != IR_CONST_F64) continue;
            VReg dv = ir->instrs[k].dest;
            if (dv >= 0 && dv < n_vregs && def_count[dv] == 1) {
                IRInstruction cpy = ir->instrs[k];
                memmove(&ir->instrs[ip + 1], &ir->instrs[ip], (size_t)(k - ip) * sizeof(IRInstruction));
                ir->instrs[ip] = cpy;
                ip++;
            }
        }
    }

    /* Run SIMD loop vectorizer before immediate operand folding deletes loop limits */
    ir_loop_vectorize(ir);

    /* Pass 7 (Pillar #5): Immediate Operand Folding on x86-64 & Dead Code Elimination */
    if (g_target_arch == VORTEX_ARCH_X86_64) {
        ir_compute_def_use(ir, def_count, use_count);
        memset(has_const, 0, (size_t)n_vregs * sizeof(bool));
        for (int ip = 0; ip < ir->count; ip++) {
            IRInstruction* ins = &ir->instrs[ip];
            if (ins->op == IR_CONST_I64 && ins->dest >= 0 && def_count[ins->dest] == 1) {
                has_const[ins->dest] = true;
                const_val[ins->dest] = ins->imm;
            }
        }
        for (int ip = 0; ip < ir->count; ip++) {
            IRInstruction* ins = &ir->instrs[ip];
            if ((ins->op == IR_DIV || ins->op == IR_MOD) &&
                ins->type == TYPE_I64 && ins->src1 >= 0 && ins->src2 >= 0 && has_const[ins->src2]) {
                int64_t cv = const_val[ins->src2];
                if (cv > 0 && (cv & (cv - 1)) == 0 && cv <= 0x40000000LL) {
                    ins->imm = cv;
                    ins->src2 = VREG_NONE;
                }
            }
            if ((ins->op == IR_ADD || ins->op == IR_SUB || ins->op == IR_MUL ||
                 ins->op == IR_BAND || ins->op == IR_BOR || ins->op == IR_BXOR ||
                 ins->op == IR_SHL || ins->op == IR_SHR ||
                 (ins->op >= IR_EQ && ins->op <= IR_GE)) &&
                ins->type == TYPE_I64 && ins->src1 >= 0 && ins->src2 >= 0) {
                if (has_const[ins->src2]) {
                    int64_t cv = const_val[ins->src2];
                    if (ins->op == IR_SHL || ins->op == IR_SHR) {
                        if (cv >= 0 && cv < 64) {
                            ins->imm = cv;
                            ins->src2 = VREG_NONE;
                        }
                    } else if (cv >= -2147483648LL && cv <= 2147483647LL) {
                        ins->imm = cv;
                        ins->src2 = VREG_NONE;
                    }
                } else if ((ins->op == IR_ADD || ins->op == IR_MUL ||
                            ins->op == IR_BAND || ins->op == IR_BOR || ins->op == IR_BXOR) && has_const[ins->src1]) {
                    int64_t cv = const_val[ins->src1];
                    if (cv >= -2147483648LL && cv <= 2147483647LL) {
                        ins->src1 = ins->src2;
                        ins->src2 = VREG_NONE;
                        ins->imm = cv;
                    }
                }
            }
        }
    }

    /* Dead Code Elimination sweep */
    bool dce_changed = true;
    while (dce_changed) {
        dce_changed = false;
        ir_compute_def_use(ir, def_count, use_count);
        for (int ip = 0; ip < ir->count; ip++) {
            IRInstruction* ins = &ir->instrs[ip];
            if (ins->op == IR_NOP || ir_has_side_effect(ins->op)) continue;
            if (ins->dest >= 0 && ins->dest < n_vregs && use_count[ins->dest] == 0) {
                ins->op = IR_NOP;
                dce_changed = true;
            }
        }
        if (dce_changed) ir_compact_nops(ir);
    }

    free(is_f32_const);
    free(is_f64_const);
    free(const_f32_val);
    free(const_f64_val);
    free(has_const);
    free(const_val);
    free(def_count);
    free(use_count);
    free(def_ip);
    free(equiv);
}
