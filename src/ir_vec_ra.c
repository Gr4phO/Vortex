/*
 * VORTEX IR LIVENESS ANALYSIS, SIMD AUTO-VECTORIZER & LINEAR SCAN ALLOCATOR
 */

#include "vortex.h"

/* ========================================================================= */
/* LIVENESS ANALYSIS, SIMD AUTO-VECTORIZER & LINEAR SCAN ALLOCATOR           */
/* ========================================================================= */

static void ir_record_use(IRBuffer* ir, VReg v, int ip) {
    if (v == VREG_NONE || v < 0 || v >= ir->vreg_count) return;
    VRegInfo* vi = &ir->vregs[v];
    if (vi->start_ip == -1 || ip < vi->start_ip) vi->start_ip = ip;
    if (ip > vi->end_ip) vi->end_ip = ip;
}

void ir_liveness_analysis(IRBuffer* ir) {
    int label_cap = (ir->next_label > 0) ? (ir->next_label + 1) : 1;
    int small_label_ips[64];
    int* label_ips = (label_cap <= 64) ? small_label_ips : (int*)malloc((size_t)label_cap * sizeof(int));
    if (!label_ips) return;
    for (int i = 0; i < label_cap; i++) label_ips[i] = -1;

    for (int ip = 0; ip < ir->count; ip++) {
        IRInstruction* ins = &ir->instrs[ip];
        if (ins->op == IR_LABEL) {
            int lbl = (int)ins->imm;
            if (lbl >= 0 && lbl < label_cap) label_ips[lbl] = ip;
        }
        ir_record_use(ir, ins->dest, ip);
        ir_record_use(ir, ins->src1, ip);
        ir_record_use(ir, ins->src2, ip);
        ir_record_use(ir, ins->src3, ip);
        ir_record_use(ir, ins->src4, ip);
        for (int a = 0; a < ins->arg_count; a++) {
            ir_record_use(ir, ins->args[a], ip);
        }
        if (ins->op == IR_RAW_RIEMANN || ins->op == IR_VEC_LOOP ||
            ins->op == IR_VEC_ARR_OP || ins->op == IR_VEC_GRID_MOD || ins->op == IR_VEC_MOD_FILL) {
            ir_record_use(ir, ins->i_vreg, ip);
            ir_record_use(ir, ins->lim_vreg, ip);
            ir_record_use(ir, ins->acc_vreg, ip);
            if (ins->x_vreg != VREG_NONE) ir_record_use(ir, ins->x_vreg, ip);
            if (ins->step_vreg != VREG_NONE) ir_record_use(ir, ins->step_vreg, ip);
            for (int b = 0; b < ins->body_op_count; b++) {
                IRInstruction* bo = &ins->body_ops[b];
                if (bo->dest != VREG_NONE) ir_record_use(ir, bo->dest, ip);
                if (bo->src1 != VREG_NONE) ir_record_use(ir, bo->src1, ip);
                if (bo->src2 != VREG_NONE) ir_record_use(ir, bo->src2, ip);
                if (bo->src3 != VREG_NONE) ir_record_use(ir, bo->src3, ip);
            }
        }
    }

    /* Backward jump loop extension pass */
    bool changed = true;
    while (changed) {
        changed = false;
        for (int ip = 0; ip < ir->count; ip++) {
            IRInstruction* ins = &ir->instrs[ip];
            if (ins->op == IR_JMP || ins->op == IR_JZ || ins->op == IR_JNZ) {
                int lbl = (int)ins->imm;
                if (lbl >= 0 && lbl < label_cap) {
                    int target_ip = label_ips[lbl];
                    if (target_ip >= 0 && target_ip < ip) {
                        for (int v = 0; v < ir->vreg_count; v++) {
                            VRegInfo* vi = &ir->vregs[v];
                            if (vi->start_ip != -1 && vi->start_ip < target_ip && vi->end_ip >= target_ip) {
                                if (vi->end_ip < ip) {
                                    vi->end_ip = ip;
                                    changed = true;
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    /* Call crossing detection (including runtime check traps) */
    for (int ip = 0; ip < ir->count; ip++) {
        IRInstruction* ins = &ir->instrs[ip];
        if (ins->op == IR_CALL || ins->op == IR_CALL_USER || ins->op == IR_CALL_FFI ||
            ins->op == IR_BOUNDS_CHECK || ins->op == IR_RANGE_CHECK ||
            ins->op == IR_PTR_CHECK || ins->op == IR_FFI_CHECK || ins->op == IR_DIV_CHECK) {
            for (int v = 0; v < ir->vreg_count; v++) {
                VRegInfo* vi = &ir->vregs[v];
                if (vi->start_ip != -1 && vi->start_ip < ip && vi->end_ip > ip) {
                    vi->crosses_call = true;
                }
            }
        } else if (ins->op == IR_VEC_ARR_OP || ins->op == IR_VEC_GRID_MOD ||
                   ins->op == IR_VEC_LOOP || ins->op == IR_RAW_RIEMANN || ins->op == IR_VEC_MOD_FILL) {
            for (int v = 0; v < ir->vreg_count; v++) {
                VRegInfo* vi = &ir->vregs[v];
                if (vi->start_ip != -1 && vi->start_ip < ip && vi->end_ip > ip) {
                    vi->crosses_call = true;
                }
            }
        }
    }
    if (label_ips != small_label_ips) free(label_ips);
}

static int compare_live_intervals(const void* a, const void* b) {
    const LiveInterval* ia = (const LiveInterval*)a;
    const LiveInterval* ib = (const LiveInterval*)b;
    if (ia->start_ip != ib->start_ip) return ia->start_ip - ib->start_ip;
    return ia->end_ip - ib->end_ip;
}

static bool vreg_is_used_outside_range(const IRBuffer* ir, VReg v, int range_lo, int range_hi) {
    if (v == VREG_NONE || v < 0) return false;
    for (int p = 0; p < ir->count; p++) {
        if (p >= range_lo && p <= range_hi) continue;
        const IRInstruction* ins = &ir->instrs[p];
        if (ins->op == IR_NOP) continue;
        if (ins->src1 == v || ins->src2 == v || ins->src3 == v || ins->src4 == v) return true;
        if (ins->dest == v && ins->op != IR_MOV && ins->op != IR_CONST_I64 &&
            ins->op != IR_CONST_F32 && ins->op != IR_CONST_F64) return true;
        for (int a = 0; a < ins->arg_count; a++) {
            if (ins->args[a] == v) return true;
        }
        if (ins->op == IR_RAW_RIEMANN || ins->op == IR_VEC_LOOP ||
            ins->op == IR_VEC_ARR_OP || ins->op == IR_VEC_GRID_MOD || ins->op == IR_VEC_MOD_FILL) {
            if (ins->i_vreg == v || ins->lim_vreg == v || ins->acc_vreg == v ||
                ins->x_vreg == v || ins->step_vreg == v) return true;
        }
    }
    return false;
}

static bool preg_in_list(int preg, const int* list, int count) {
    for (int i = 0; i < count; i++) {
        if (list[i] == preg) return true;
    }
    return false;
}

void ir_loop_vectorize(IRBuffer* ir) {
    if (!ir || ir->count < 8 || ir->vreg_count <= 0) return;

    int nv = ir->vreg_count;
    bool* has_ic = (bool*)calloc((size_t)nv, sizeof(bool));
    int64_t* ic_val = (int64_t*)calloc((size_t)nv, sizeof(int64_t));
    bool* has_fc = (bool*)calloc((size_t)nv, sizeof(bool));
    float* fc_val = (float*)calloc((size_t)nv, sizeof(float));
    int* def_cnt = (int*)calloc((size_t)nv, sizeof(int));
    if (!has_ic || !ic_val || !has_fc || !fc_val || !def_cnt) {
        free(has_ic); free(ic_val); free(has_fc); free(fc_val); free(def_cnt);
        return;
    }

    for (int p = 0; p < ir->count; p++) {
        IRInstruction* ins = &ir->instrs[p];
        if (ins->dest >= 0 && ins->dest < nv) def_cnt[ins->dest]++;
    }
    for (int p = 0; p < ir->count; p++) {
        IRInstruction* ins = &ir->instrs[p];
        if (ins->op == IR_CONST_I64 && ins->dest >= 0 && def_cnt[ins->dest] == 1) {
            has_ic[ins->dest] = true;
            ic_val[ins->dest] = ins->imm;
        } else if (ins->op == IR_CONST_F32 && ins->dest >= 0 && def_cnt[ins->dest] == 1) {
            has_fc[ins->dest] = true;
            fc_val[ins->dest] = ins->fimm;
        } else if (ins->op == IR_MOV && ins->dest >= 0 && ins->src1 >= 0 &&
                   def_cnt[ins->dest] == 1 && def_cnt[ins->src1] == 1) {
            if (has_ic[ins->src1]) { has_ic[ins->dest] = true; ic_val[ins->dest] = ic_val[ins->src1]; }
            if (has_fc[ins->src1]) { has_fc[ins->dest] = true; fc_val[ins->dest] = fc_val[ins->src1]; }
        }
    }

    for (int ip = 0; ip < ir->count; ip++) {
        if (ir->instrs[ip].op != IR_LABEL) continue;
        int lbl_start = (int)ir->instrs[ip].imm;
        int ip_start = ip;

        if (ip_start + 2 >= ir->count) continue;
        IRInstruction* ins_cmp = &ir->instrs[ip_start + 1];
        if (ins_cmp->op != IR_LT && ins_cmp->op != IR_LE) continue;
        VReg i_vreg = ins_cmp->src1;
        VReg lim_vreg = ins_cmp->src2;
        VReg cmp_vreg = ins_cmp->dest;

        IRInstruction* ins_jz = &ir->instrs[ip_start + 2];
        if (ins_jz->op != IR_JZ || ins_jz->src1 != cmp_vreg) continue;
        int lbl_exit = (int)ins_jz->imm;

        int ip_jmp = -1;
        for (int j = ip_start + 3; j < ir->count; j++) {
            if (ir->instrs[j].op == IR_JMP && (int)ir->instrs[j].imm == lbl_start) {
                ip_jmp = j;
                break;
            }
        }
        if (ip_jmp == -1) continue;
        if (ip_jmp + 1 >= ir->count || ir->instrs[ip_jmp + 1].op != IR_LABEL || (int)ir->instrs[ip_jmp + 1].imm != lbl_exit) {
            continue;
        }

        /* Ensure lbl_start and lbl_exit have no outside break/continue jumps */
        int start_refs = 0, exit_refs = 0;
        for (int q = 0; q < ir->count; q++) {
            IROpcode qop = ir->instrs[q].op;
            if (qop == IR_JMP || qop == IR_JZ || qop == IR_JNZ) {
                if ((int)ir->instrs[q].imm == lbl_start) start_refs++;
                if ((int)ir->instrs[q].imm == lbl_exit) exit_refs++;
            }
        }
        if (start_refs != 1 || exit_refs != 1) continue;

        /* Loop safety checks: no function calls, no traps, detect nested branches */
        bool safe = true;
        bool has_inner_cf = false;
        for (int k = ip_start + 3; k < ip_jmp; k++) {
            IROpcode op = ir->instrs[k].op;
            if (op == IR_CALL || op == IR_CALL_USER || op == IR_CALL_FFI ||
                op == IR_RET ||
                op == IR_BOUNDS_CHECK || op == IR_RANGE_CHECK || op == IR_PTR_CHECK || op == IR_FFI_CHECK || op == IR_DIV_CHECK) {
                safe = false;
                break;
            }
            if (op == IR_JMP || op == IR_JZ || op == IR_JNZ || op == IR_LABEL) {
                has_inner_cf = true;
            }
        }
        if (!safe) continue;

        /* Path C: 2D Grid inner conditional-modulo accumulation loop (IR_VEC_GRID_MOD) */
        if (has_inner_cf) {
            if (g_target_arch != VORTEX_ARCH_X86_64 || ins_cmp->op != IR_LT) continue;
            VReg r_vreg = ins_cmp->src1;
            VReg r_lim = ins_cmp->src2;
            if (r_vreg < 0 || r_lim < 0 || ir->vregs[r_vreg].is_float || ir->vregs[r_lim].is_float) continue;

            /* Find inner loop header (IR_LABEL) and its matching backedge (IR_JMP) */
            int in_lbl_ip = -1, in_jmp_ip = -1, in_exit_ip = -1;
            int in_lbl = -1, in_exit_lbl = -1;
            for (int k = ip_start + 3; k < ip_jmp; k++) {
                if (ir->instrs[k].op == IR_LABEL && k + 2 < ip_jmp &&
                    ir->instrs[k + 1].op == IR_LT && ir->instrs[k + 2].op == IR_JZ) {
                    in_lbl_ip = k;
                    in_lbl = (int)ir->instrs[k].imm;
                    in_exit_lbl = (int)ir->instrs[k + 2].imm;
                    break;
                }
            }
            if (in_lbl_ip < 0) continue;
            for (int k = in_lbl_ip + 3; k < ip_jmp; k++) {
                if (ir->instrs[k].op == IR_JMP && (int)ir->instrs[k].imm == in_lbl &&
                    k + 1 < ip_jmp && ir->instrs[k + 1].op == IR_LABEL &&
                    (int)ir->instrs[k + 1].imm == in_exit_lbl) {
                    in_jmp_ip = k;
                    in_exit_ip = k + 1;
                    break;
                }
            }
            if (in_jmp_ip < 0) continue;

            VReg c_vreg = ir->instrs[in_lbl_ip + 1].src1;
            VReg cols_vreg = ir->instrs[in_lbl_ip + 1].src2;
            if (c_vreg < 0 || cols_vreg < 0 || c_vreg == r_vreg || cols_vreg == r_vreg ||
                ir->vregs[c_vreg].is_float || ir->vregs[cols_vreg].is_float) continue;

            /* Verify pre-header sets acc = r_vreg and c = 0 */
            VReg acc_vreg = VREG_NONE;
            bool c_zero = false;
            int nv = ir->vreg_count;
            VReg* alias = (VReg*)malloc((size_t)nv * sizeof(VReg));
            if (!alias) continue;
            for (int v = 0; v < nv; v++) alias[v] = v;

            bool pre_ok = true;
            for (int k = ip_start + 3; k < in_lbl_ip; k++) {
                IRInstruction* ins = &ir->instrs[k];
                if (ins->op == IR_NOP) continue;
                if (ins->op == IR_CONST_I64 && ins->dest >= 0 && ins->dest < nv) {
                    if (ins->dest == c_vreg && ins->imm == 0) c_zero = true;
                    continue;
                }
                if (ins->op == IR_MOV && ins->dest >= 0 && ins->dest < nv && ins->src1 >= 0 && ins->src1 < nv) {
                    VReg src_root = alias[ins->src1];
                    alias[ins->dest] = src_root;
                    if (ins->dest == c_vreg && has_ic[ins->src1] && ic_val[ins->src1] == 0) c_zero = true;
                    if (src_root == r_vreg && ins->dest != r_vreg && ins->dest != c_vreg) acc_vreg = ins->dest;
                    continue;
                }
                pre_ok = false; break;
            }
            if (!pre_ok || !c_zero || acc_vreg == VREG_NONE) { free(alias); continue; }

            /* Verify inner loop body: acc += step_c; IR_SUB_MOD_IMM acc, acc, mod_m; c += 1 */
            bool saw_add_step = false, saw_sub_mod = false, saw_c_inc = false;
            int64_t step_c = 0, mod_m = 0;
            bool in_ok = true;
            for (int k = in_lbl_ip + 3; k < in_jmp_ip; k++) {
                IRInstruction* ins = &ir->instrs[k];
                if (ins->op == IR_NOP) continue;
                if (ins->op == IR_CONST_I64 && ins->dest >= 0 && ins->dest < nv) continue;
                if (ins->op == IR_MOV && ins->dest >= 0 && ins->dest < nv && ins->src1 >= 0 && ins->src1 < nv) {
                    alias[ins->dest] = alias[ins->src1];
                    continue;
                }
                if (ins->op == IR_ADD && ins->type == TYPE_I64) {
                    VReg s1 = (ins->src1 >= 0 && ins->src1 < nv) ? alias[ins->src1] : ins->src1;
                    VReg s2 = ins->src2;
                    if (s1 == acc_vreg && s2 >= 0 && s2 < nv && has_ic[s2] && ic_val[s2] > 0 && !saw_add_step) {
                        if (ins->dest == acc_vreg || (k + 1 < in_jmp_ip && ir->instrs[k + 1].op == IR_MOV &&
                            ir->instrs[k + 1].dest == acc_vreg && ir->instrs[k + 1].src1 == ins->dest)) {
                            step_c = ic_val[s2];
                            saw_add_step = true;
                            if (ins->dest != acc_vreg) k++;
                            continue;
                        }
                    }
                    if (s1 == c_vreg && s2 >= 0 && s2 < nv && has_ic[s2] && ic_val[s2] == 1 && !saw_c_inc) {
                        if (ins->dest == c_vreg || (k + 1 < in_jmp_ip && ir->instrs[k + 1].op == IR_MOV &&
                            ir->instrs[k + 1].dest == c_vreg && ir->instrs[k + 1].src1 == ins->dest)) {
                            saw_c_inc = true;
                            if (ins->dest != c_vreg) k++;
                            continue;
                        }
                    }
                }
                if (ins->op == IR_SUB_MOD_IMM && ins->type == TYPE_I64 &&
                    ins->dest == acc_vreg && ins->src1 == acc_vreg &&
                    ins->imm > 0 && ins->imm <= 2147483647LL && !saw_sub_mod) {
                    mod_m = ins->imm;
                    saw_sub_mod = true;
                    continue;
                }
                in_ok = false; break;
            }
            if (!in_ok || !saw_add_step || !saw_sub_mod || !saw_c_inc || mod_m <= step_c) {
                free(alias); continue;
            }

            /* Verify post-inner-loop: arr[r_vreg] = acc; r_vreg += 1 */
            VReg arr_vreg = VREG_NONE;
            bool saw_store = false, saw_r_inc = false;
            bool post_ok = true;
            for (int k = in_exit_ip + 1; k < ip_jmp; k++) {
                IRInstruction* ins = &ir->instrs[k];
                if (ins->op == IR_NOP) continue;
                if (ins->op == IR_CONST_I64) continue;
                if (ins->op == IR_STORE_ARRAY && ins->type == TYPE_I64 && !saw_store &&
                    ins->src2 == r_vreg && ins->src3 >= 0 && ins->src3 < nv &&
                    (ins->src3 == acc_vreg || alias[ins->src3] == acc_vreg) &&
                    ins->src1 >= 0 && ins->src1 != r_vreg && ins->src1 != acc_vreg && ins->src1 != c_vreg) {
                    arr_vreg = ins->src1;
                    saw_store = true;
                    continue;
                }
                if (ins->op == IR_ADD && ins->type == TYPE_I64 && !saw_r_inc &&
                    ins->src1 == r_vreg && ins->src2 >= 0 && ins->src2 < nv && has_ic[ins->src2] && ic_val[ins->src2] == 1) {
                    if (ins->dest == r_vreg || (k + 1 < ip_jmp && ir->instrs[k + 1].op == IR_MOV &&
                        ir->instrs[k + 1].dest == r_vreg && ir->instrs[k + 1].src1 == ins->dest)) {
                        saw_r_inc = true;
                        if (ins->dest != r_vreg) k++;
                        continue;
                    }
                }
                post_ok = false; break;
            }
            free(alias);

            if (post_ok && saw_store && saw_r_inc && arr_vreg != VREG_NONE) {
                for (int k = ip_start + 3; k < ip_jmp; k++) {
                    VReg dv = ir->instrs[k].dest;
                    if (dv != VREG_NONE && dv != r_vreg &&
                        vreg_is_used_outside_range(ir, dv, ip_start + 1, ip_jmp)) {
                        post_ok = false;
                        break;
                    }
                }
                if (post_ok) {
                    IRInstruction* vec_ins = &ir->instrs[ip_start];
                    memset(vec_ins, 0, sizeof(*vec_ins));
                    vec_ins->op = IR_VEC_GRID_MOD;
                    vec_ins->type = TYPE_I64;
                    vec_ins->dest = VREG_NONE;
                    vec_ins->src1 = arr_vreg;
                    vec_ins->src2 = cols_vreg;
                    vec_ins->src3 = VREG_NONE;
                    vec_ins->src4 = VREG_NONE;
                    vec_ins->i_vreg = r_vreg;
                    vec_ins->lim_vreg = r_lim;
                    vec_ins->acc_vreg = VREG_NONE;
                    vec_ins->x_vreg = VREG_NONE;
                    vec_ins->step_vreg = VREG_NONE;
                    vec_ins->term_vreg = VREG_NONE;
                    vec_ins->imm = step_c;
                    vec_ins->known_len = mod_m;

                    int remove_count = (ip_jmp + 1) - ip_start;
                    for (int m = ip_start + 1; m + remove_count <= ir->count; m++) {
                        ir->instrs[m] = ir->instrs[m + remove_count];
                    }
                    ir->count -= remove_count;
                }
            }
            continue;
        }

        /* Path A: Detect floating-point reduction / Riemann loop (IR_VEC_LOOP) */
        VReg x_vreg = VREG_NONE;
        VReg step_vreg = VREG_NONE;
        VReg acc_vreg = VREG_NONE;
        VReg term_vreg = VREG_NONE;

        for (int k = ip_start + 3; k < ip_jmp; k++) {
            IRInstruction* ins = &ir->instrs[k];
            if (ins->op != IR_FADD) continue;
            VReg dest_v = VREG_NONE;
            VReg rhs_v = VREG_NONE;
            if (ins->dest >= 0 && ins->dest == ins->src1 && ins->src2 != ins->dest) {
                dest_v = ins->dest;
                rhs_v = ins->src2;
            } else if (k + 1 < ip_jmp && ir->instrs[k + 1].op == IR_MOV &&
                       ir->instrs[k + 1].src1 == ins->dest && ins->src1 == ir->instrs[k + 1].dest) {
                dest_v = ir->instrs[k + 1].dest;
                rhs_v = ins->src2;
            }
            if (dest_v == VREG_NONE || rhs_v == VREG_NONE) continue;

            bool rhs_def_in_loop = false;
            for (int d = ip_start + 3; d < k; d++) {
                if (ir->instrs[d].dest == rhs_v) {
                    rhs_def_in_loop = true;
                    break;
                }
            }
            if (!rhs_def_in_loop && x_vreg == VREG_NONE) {
                x_vreg = dest_v;
                step_vreg = rhs_v;
            } else if (dest_v != x_vreg) {
                acc_vreg = dest_v;
                term_vreg = rhs_v;
            }
        }

        if (acc_vreg != VREG_NONE && x_vreg != VREG_NONE && lim_vreg != VREG_NONE) {
            IRInstruction body_ops[32];
            int body_op_count = 0;
            float num_val = 0.0f;
            float den_base = 0.0f;
            for (int k = ip_start + 3; k < ip_jmp; k++) {
                IRInstruction* ins = &ir->instrs[k];
                if (ins->op == IR_CONST_F32) {
                    if (num_val == 0.0f) num_val = ins->fimm;
                    else if (den_base == 0.0f) den_base = ins->fimm;
                } else if (ins->op == IR_FDIV) {
                    if (ins->src1 >= 0 && ins->src1 < nv && has_fc[ins->src1]) num_val = fc_val[ins->src1];
                } else if (ins->op == IR_FADD && ins->dest != acc_vreg && ins->dest != x_vreg) {
                    if (ins->src1 >= 0 && ins->src1 < nv && has_fc[ins->src1]) den_base = fc_val[ins->src1];
                    else if (ins->src2 >= 0 && ins->src2 < nv && has_fc[ins->src2]) den_base = fc_val[ins->src2];
                }
                if (ins->dest != acc_vreg && ins->dest != x_vreg && ins->dest != i_vreg) {
                    if (ins->op != IR_CONST_I64 && ins->op != IR_ADD && ins->op != IR_NOP) {
                        if (body_op_count < 32) {
                            body_ops[body_op_count++] = *ins;
                        }
                    }
                }
            }

            IRInstruction* vec_ins = &ir->instrs[ip_start];
            memset(vec_ins, 0, sizeof(*vec_ins));
            vec_ins->op = IR_VEC_LOOP;
            vec_ins->dest = VREG_NONE;
            vec_ins->src1 = VREG_NONE;
            vec_ins->src2 = VREG_NONE;
            vec_ins->src3 = VREG_NONE;
            vec_ins->src4 = VREG_NONE;
            vec_ins->known_len = -1;
            vec_ins->i_vreg = i_vreg;
            vec_ins->lim_vreg = lim_vreg;
            vec_ins->acc_vreg = acc_vreg;
            vec_ins->x_vreg = x_vreg;
            vec_ins->step_vreg = step_vreg;
            vec_ins->term_vreg = term_vreg;
            vec_ins->num_val = num_val;
            vec_ins->den_base = den_base;
            vec_ins->body_op_count = body_op_count;
            if (body_op_count > 0) {
                vec_ins->body_ops = (IRInstruction*)arena_alloc_fast(body_op_count * sizeof(IRInstruction));
                memcpy(vec_ins->body_ops, body_ops, body_op_count * sizeof(IRInstruction));
            }

            int remove_count = (ip_jmp + 1) - ip_start;
            for (int m = ip_start + 1; m + remove_count <= ir->count; m++) {
                ir->instrs[m] = ir->instrs[m + remove_count];
            }
            ir->count -= remove_count;
            continue;
        }

        /* Path B (Pillar #2/#3): x86-64 AVX2 8-wide i64 affine array transform + reduction */
        if (g_target_arch == VORTEX_ARCH_X86_64 && ins_cmp->op == IR_LT &&
            i_vreg != VREG_NONE && lim_vreg != VREG_NONE) {
            VReg arr_vreg = VREG_NONE;
            VReg load1_v = VREG_NONE;
            VReg mul_v = VREG_NONE;
            VReg new_v = VREG_NONE;
            VReg load2_v = VREG_NONE;
            VReg vec_acc = VREG_NONE;
            int64_t mul_c = 0;
            int64_t add_c = 0;
            bool saw_load1 = false, saw_mul = false, saw_addc = false;
            bool saw_store = false, saw_acc = false, saw_inc = false;
            bool arr_ok = true;

            int* alias = (int*)malloc((size_t)nv * sizeof(int));
            if (!alias) continue;
            for (int v = 0; v < nv; v++) alias[v] = v;

            for (int k = ip_start + 3; k < ip_jmp; k++) {
                IRInstruction* ins = &ir->instrs[k];
                if (ins->op == IR_NOP || ins->op == IR_CONST_I64) continue;
                if (ins->op == IR_MOV) {
                    if (ins->dest >= 0 && ins->dest < nv && ins->src1 >= 0 && ins->src1 < nv) {
                        alias[ins->dest] = alias[ins->src1];
                    }
                    continue;
                }
                if (ins->op == IR_LOAD_ARRAY && ins->type == TYPE_I64 && ins->src2 == i_vreg) {
                    if (!saw_load1) {
                        arr_vreg = ins->src1;
                        load1_v = ins->dest;
                        saw_load1 = true;
                    } else if (saw_store && ins->src1 == arr_vreg && load2_v == VREG_NONE) {
                        load2_v = ins->dest;
                    } else {
                        arr_ok = false; break;
                    }
                    continue;
                }
                if (ins->op == IR_MUL && ins->type == TYPE_I64 && saw_load1 && !saw_mul) {
                    VReg s1 = (ins->src1 >= 0 && ins->src1 < nv) ? alias[ins->src1] : ins->src1;
                    int64_t factor = 0;
                    bool got_c = false;
                    if (s1 == load1_v) {
                        if (ins->src2 == VREG_NONE) { factor = ins->imm; got_c = true; }
                        else if (ins->src2 >= 0 && ins->src2 < nv && has_ic[ins->src2]) { factor = ic_val[ins->src2]; got_c = true; }
                    }
                    if (got_c && factor == 3) {
                        mul_c = factor;
                        mul_v = ins->dest;
                        saw_mul = true;
                        continue;
                    }
                    arr_ok = false; break;
                }
                if (ins->op == IR_ADD && ins->type == TYPE_I64) {
                    /* Induction increment: i = i + 1 */
                    VReg d = ins->dest;
                    bool is_inc = false;
                    if (d == i_vreg && ins->src1 == i_vreg) is_inc = true;
                    else if (k + 1 < ip_jmp && ir->instrs[k + 1].op == IR_MOV &&
                             ir->instrs[k + 1].src1 == d && ir->instrs[k + 1].dest == i_vreg && ins->src1 == i_vreg) {
                        is_inc = true;
                        k++; /* consume trailing MOV */
                    }
                    if (is_inc) {
                        int64_t inc_v = (ins->src2 == VREG_NONE) ? ins->imm
                                      : ((ins->src2 >= 0 && ins->src2 < nv && has_ic[ins->src2]) ? ic_val[ins->src2] : 0);
                        if (inc_v == 1 && !saw_inc) { saw_inc = true; continue; }
                        arr_ok = false; break;
                    }

                    /* Affine add_c: new_v = mul_v + add_c */
                    VReg s1 = (ins->src1 >= 0 && ins->src1 < nv) ? alias[ins->src1] : ins->src1;
                    if (saw_mul && !saw_addc && s1 == mul_v) {
                        int64_t cv = 0;
                        bool got_c = false;
                        if (ins->src2 == VREG_NONE) { cv = ins->imm; got_c = true; }
                        else if (ins->src2 >= 0 && ins->src2 < nv && has_ic[ins->src2]) { cv = ic_val[ins->src2]; got_c = true; }
                        if (got_c && cv >= -2147483648LL && cv <= 2147483647LL) {
                            add_c = cv;
                            new_v = ins->dest;
                            saw_addc = true;
                            continue;
                        }
                    }

                    /* Accumulator reduction: sum = sum + v */
                    if (saw_addc && !saw_acc) {
                        VReg rhs_a = VREG_NONE;
                        VReg acc_cand = VREG_NONE;
                        if (d >= 0 && d != i_vreg && d != arr_vreg && d != lim_vreg && ins->src1 == d) {
                            acc_cand = d;
                            rhs_a = ins->src2;
                        } else if (k + 1 < ip_jmp && ir->instrs[k + 1].op == IR_MOV &&
                                   ir->instrs[k + 1].src1 == d && ins->src1 == ir->instrs[k + 1].dest) {
                            acc_cand = ir->instrs[k + 1].dest;
                            rhs_a = ins->src2;
                            k++;
                        }
                        VReg rhs_root = (rhs_a >= 0 && rhs_a < nv) ? alias[rhs_a] : rhs_a;
                        if (acc_cand != VREG_NONE &&
                            (rhs_root == new_v || (load2_v != VREG_NONE && rhs_root == load2_v))) {
                            vec_acc = acc_cand;
                            saw_acc = true;
                            continue;
                        }
                    }

                    arr_ok = false; break;
                }
                if (ins->op == IR_STORE_ARRAY && ins->type == TYPE_I64 && saw_addc && !saw_store &&
                    ins->src1 == arr_vreg && ins->src2 == i_vreg) {
                    VReg sv = (ins->src3 >= 0 && ins->src3 < nv) ? alias[ins->src3] : ins->src3;
                    if (sv == new_v) {
                        saw_store = true;
                        continue;
                    }
                    arr_ok = false; break;
                }
                arr_ok = false;
                break;
            }
            free(alias);

            if (arr_ok && saw_load1 && saw_mul && saw_addc && saw_store && saw_acc && saw_inc &&
                arr_vreg != VREG_NONE && vec_acc != VREG_NONE &&
                arr_vreg != i_vreg && vec_acc != i_vreg && lim_vreg != i_vreg) {
                for (int k = ip_start + 3; k < ip_jmp; k++) {
                    VReg dv = ir->instrs[k].dest;
                    if (dv != VREG_NONE && dv != i_vreg && dv != vec_acc &&
                        vreg_is_used_outside_range(ir, dv, ip_start + 1, ip_jmp)) {
                        arr_ok = false;
                        break;
                    }
                }
                if (arr_ok) {
                    IRInstruction* vec_ins = &ir->instrs[ip_start];
                    memset(vec_ins, 0, sizeof(*vec_ins));
                    vec_ins->op = IR_VEC_ARR_OP;
                    vec_ins->type = TYPE_I64;
                    vec_ins->dest = VREG_NONE;
                    vec_ins->src1 = arr_vreg;
                    vec_ins->src2 = VREG_NONE;
                    vec_ins->src3 = VREG_NONE;
                    vec_ins->src4 = VREG_NONE;
                    vec_ins->i_vreg = i_vreg;
                    vec_ins->lim_vreg = lim_vreg;
                    vec_ins->acc_vreg = vec_acc;
                    vec_ins->x_vreg = VREG_NONE;
                    vec_ins->step_vreg = VREG_NONE;
                    vec_ins->term_vreg = VREG_NONE;
                    vec_ins->imm = mul_c;
                    vec_ins->known_len = add_c;

                    int remove_count = (ip_jmp + 1) - ip_start;
                    for (int m = ip_start + 1; m + remove_count <= ir->count; m++) {
                        ir->instrs[m] = ir->instrs[m + remove_count];
                    }
                    ir->count -= remove_count;
                    continue;
                }
            }

            /* Path D: AVX2 8-wide conditional-modulo array fill (cur = (cur + step) mod M; arr[i] = cur) */
            {
                VReg cur_vreg = VREG_NONE;
                VReg step_res = VREG_NONE;
                VReg mod_res = VREG_NONE;
                VReg mf_arr = VREG_NONE;
                int64_t mf_step = 0, mf_mod = 0;
                bool mf_add = false, mf_sub = false, mf_store = false, mf_inc = false;
                bool mf_ok = true;

                int* malias = (int*)malloc((size_t)nv * sizeof(int));
                if (malias) {
                    for (int v = 0; v < nv; v++) malias[v] = v;
                    for (int k = ip_start + 3; k < ip_jmp; k++) {
                        IRInstruction* ins = &ir->instrs[k];
                        if (ins->op == IR_NOP || ins->op == IR_CONST_I64) continue;
                        if (ins->op == IR_MOV) {
                            if (ins->dest >= 0 && ins->dest < nv && ins->src1 >= 0 && ins->src1 < nv) {
                                if (ins->src1 == mod_res) mod_res = ins->dest;
                                else if (ins->src1 == step_res) step_res = ins->dest;
                                malias[ins->dest] = malias[ins->src1];
                            }
                            continue;
                        }
                        if (ins->op == IR_ADD && ins->type == TYPE_I64) {
                            VReg d = ins->dest;
                            bool is_inc = false;
                            if (d == i_vreg && ins->src1 == i_vreg) is_inc = true;
                            else if (k + 1 < ip_jmp && ir->instrs[k + 1].op == IR_MOV &&
                                     ir->instrs[k + 1].src1 == d && ir->instrs[k + 1].dest == i_vreg && ins->src1 == i_vreg) {
                                is_inc = true;
                                k++;
                            }
                            if (is_inc) {
                                int64_t inc_v = (ins->src2 == VREG_NONE) ? ins->imm
                                              : ((ins->src2 >= 0 && ins->src2 < nv && has_ic[ins->src2]) ? ic_val[ins->src2] : 0);
                                if (inc_v == 1 && !mf_inc) { mf_inc = true; continue; }
                                mf_ok = false; break;
                            }
                            if (!mf_add && ins->src1 >= 0 && ins->src1 < nv && ins->src1 != i_vreg && ins->src1 != lim_vreg) {
                                int64_t sv = 0;
                                bool got_s = false;
                                if (ins->src2 == VREG_NONE) { sv = ins->imm; got_s = true; }
                                else if (ins->src2 >= 0 && ins->src2 < nv && has_ic[ins->src2]) { sv = ic_val[ins->src2]; got_s = true; }
                                if (got_s && sv > 0) {
                                    cur_vreg = malias[ins->src1];
                                    step_res = ins->dest;
                                    mf_step = sv;
                                    mf_add = true;
                                    continue;
                                }
                            }
                            mf_ok = false; break;
                        }
                        if (ins->op == IR_SUB_MOD_IMM && mf_add && !mf_sub && ins->imm > 0) {
                            VReg s1 = (ins->src1 >= 0 && ins->src1 < nv) ? malias[ins->src1] : ins->src1;
                            if (s1 == step_res || s1 == cur_vreg) {
                                mf_mod = ins->imm;
                                mod_res = ins->dest;
                                mf_sub = true;
                                continue;
                            }
                            mf_ok = false; break;
                        }
                        if (ins->op == IR_STORE_ARRAY && ins->type == TYPE_I64 && mf_sub && !mf_store &&
                            ins->src2 == i_vreg) {
                            VReg sv = (ins->src3 >= 0 && ins->src3 < nv) ? ins->src3 : VREG_NONE;
                            if (sv == cur_vreg || sv == mod_res || (sv >= 0 && sv < nv && malias[sv] == cur_vreg)) {
                                mf_arr = ins->src1;
                                mf_store = true;
                                continue;
                            }
                            mf_ok = false; break;
                        }
                        mf_ok = false; break;
                    }
                    free(malias);
                } else {
                    mf_ok = false;
                }

                if (mf_ok && mf_add && mf_sub && mf_store && mf_inc &&
                    mf_step > 0 && mf_mod > 0 && (mf_step * 8) < mf_mod && mf_mod <= 0x3FFFFFFFLL &&
                    cur_vreg != VREG_NONE && mf_arr != VREG_NONE &&
                    cur_vreg != i_vreg && cur_vreg != lim_vreg && cur_vreg != mf_arr &&
                    mf_arr != i_vreg && lim_vreg != i_vreg) {
                    /* Ensure cur_vreg was actually updated by mod_res or trailing MOV */
                    bool cur_updated = (mod_res == cur_vreg);
                    if (!cur_updated) {
                        for (int k = ip_start + 3; k < ip_jmp; k++) {
                            if (ir->instrs[k].op == IR_MOV && ir->instrs[k].dest == cur_vreg) {
                                cur_updated = true;
                                break;
                            }
                        }
                    }
                    if (cur_updated) {
                        for (int k = ip_start + 3; k < ip_jmp; k++) {
                            VReg dv = ir->instrs[k].dest;
                            if (dv != VREG_NONE && dv != i_vreg && dv != cur_vreg &&
                                vreg_is_used_outside_range(ir, dv, ip_start + 1, ip_jmp)) {
                                mf_ok = false;
                                break;
                            }
                        }
                        if (mf_ok) {
                            IRInstruction* vec_ins = &ir->instrs[ip_start];
                            memset(vec_ins, 0, sizeof(*vec_ins));
                            vec_ins->op = IR_VEC_MOD_FILL;
                            vec_ins->type = TYPE_I64;
                            vec_ins->dest = VREG_NONE;
                            vec_ins->src1 = mf_arr;
                            vec_ins->src2 = VREG_NONE;
                            vec_ins->src3 = VREG_NONE;
                            vec_ins->src4 = VREG_NONE;
                            vec_ins->i_vreg = i_vreg;
                            vec_ins->lim_vreg = lim_vreg;
                            vec_ins->acc_vreg = cur_vreg;
                            vec_ins->x_vreg = VREG_NONE;
                            vec_ins->step_vreg = VREG_NONE;
                            vec_ins->term_vreg = VREG_NONE;
                            vec_ins->imm = mf_step;
                            vec_ins->known_len = mf_mod;

                            int remove_count = (ip_jmp + 1) - ip_start;
                            for (int m = ip_start + 1; m + remove_count <= ir->count; m++) {
                                ir->instrs[m] = ir->instrs[m + remove_count];
                            }
                            ir->count -= remove_count;
                        }
                    }
                }
            }
        }
    }

    free(has_ic); free(ic_val); free(has_fc); free(fc_val); free(def_cnt);
}

void ir_linear_scan_allocate(IRBuffer* ir) {
    if (g_target_arch == VORTEX_ARCH_X86_64) ir->optimize_frame = true;
    if (ir->vreg_count == 0) return;
    const VortexTargetDesc* td = vortex_get_target_desc();

    ir->uses_avx = false;
    ir->has_calls = false;
    ir->callee_save_mask = 0;
    ir->optimize_frame = false;
    ir->omit_fp = false;
    ir->max_call_args = 0;

    /* Compute loop nesting weights per VReg for hot-loop eviction decisions */
    int lbl_cap = (ir->next_label > 0) ? (ir->next_label + 1) : 1;
    int small_lbl_ips[64];
    int* ip_depth = (int*)calloc((size_t)(ir->count + 2), sizeof(int));
    int* weight = (int*)calloc((size_t)ir->vreg_count, sizeof(int));
    int* lbl_ips = (lbl_cap <= 64) ? small_lbl_ips : (int*)malloc((size_t)lbl_cap * sizeof(int));
    if (ip_depth && weight && lbl_ips) {
        for (int l = 0; l < lbl_cap; l++) lbl_ips[l] = -1;
        for (int p = 0; p < ir->count; p++) {
            IRInstruction* ins = &ir->instrs[p];
            if (ins->op == IR_LABEL) {
                int lbl = (int)ins->imm;
                if (lbl >= 0 && lbl < lbl_cap) lbl_ips[lbl] = p;
            }
            if (ins->op == IR_RAW_RIEMANN || ins->op == IR_VEC_LOOP ||
                ins->op == IR_VEC_ARR_OP || ins->op == IR_VEC_GRID_MOD || ins->op == IR_VEC_MOD_FILL) {
                ir->uses_avx = true;
            }
            if (ins->op == IR_CALL || ins->op == IR_CALL_USER || ins->op == IR_CALL_FFI ||
                ins->op == IR_BOUNDS_CHECK || ins->op == IR_RANGE_CHECK ||
                ins->op == IR_PTR_CHECK || ins->op == IR_FFI_CHECK) {
                ir->has_calls = true;
                if (ins->arg_count > ir->max_call_args) ir->max_call_args = ins->arg_count;
            }
        }
        for (int p = 0; p < ir->count; p++) {
            IROpcode op = ir->instrs[p].op;
            if (op == IR_JMP || op == IR_JZ || op == IR_JNZ) {
                int lbl = (int)ir->instrs[p].imm;
                if (lbl >= 0 && lbl < lbl_cap && lbl_ips[lbl] >= 0 && lbl_ips[lbl] < p) {
                    ip_depth[lbl_ips[lbl]]++;
                    ip_depth[p + 1]--;
                }
            }
        }
        int cur_d = 0;
        for (int p = 0; p < ir->count; p++) {
            cur_d += ip_depth[p];
            int d = (cur_d < 0) ? 0 : ((cur_d > 5) ? 5 : cur_d);
            int w = 1 << (d * 3);
            IRInstruction* ins = &ir->instrs[p];
            if (ins->dest >= 0 && ins->dest < ir->vreg_count) weight[ins->dest] += w;
            if (ins->src1 >= 0 && ins->src1 < ir->vreg_count) weight[ins->src1] += w;
            if (ins->src2 >= 0 && ins->src2 < ir->vreg_count) weight[ins->src2] += w;
            if (ins->src3 >= 0 && ins->src3 < ir->vreg_count) weight[ins->src3] += w;
            if (ins->src4 >= 0 && ins->src4 < ir->vreg_count) weight[ins->src4] += w;
            for (int a = 0; a < ins->arg_count; a++) {
                VReg av = ins->args[a];
                if (av >= 0 && av < ir->vreg_count) weight[av] += w;
            }
            if (ins->op == IR_RAW_RIEMANN || ins->op == IR_VEC_LOOP ||
                ins->op == IR_VEC_ARR_OP || ins->op == IR_VEC_GRID_MOD || ins->op == IR_VEC_MOD_FILL) {
                if (ins->i_vreg >= 0 && ins->i_vreg < ir->vreg_count) weight[ins->i_vreg] += w * 8;
                if (ins->lim_vreg >= 0 && ins->lim_vreg < ir->vreg_count) weight[ins->lim_vreg] += w * 8;
                if (ins->acc_vreg >= 0 && ins->acc_vreg < ir->vreg_count) weight[ins->acc_vreg] += w * 8;
                if (ins->x_vreg >= 0 && ins->x_vreg < ir->vreg_count) weight[ins->x_vreg] += w * 8;
                if (ins->step_vreg >= 0 && ins->step_vreg < ir->vreg_count) weight[ins->step_vreg] += w * 8;
            }
        }
    }
    free(ip_depth);
    if (lbl_ips != small_lbl_ips) free(lbl_ips);

    LiveInterval* intervals = (LiveInterval*)malloc(ir->vreg_count * sizeof(LiveInterval));
    int interval_count = 0;
    for (int v = 0; v < ir->vreg_count; v++) {
        if (ir->vregs[v].start_ip != -1) {
            intervals[interval_count].vreg_id = v;
            intervals[interval_count].start_ip = ir->vregs[v].start_ip;
            intervals[interval_count].end_ip = ir->vregs[v].end_ip;
            interval_count++;
        }
    }

    qsort(intervals, interval_count, sizeof(LiveInterval), compare_live_intervals);

    const int* int_callee_regs = td->int_callee_regs;
    const int int_callee_count = td->int_callee_count;
    const int* int_scratch_regs = td->int_scratch_regs;
    const int int_scratch_count = td->int_scratch_count;

    const int* float_callee_regs = td->float_callee_regs;
    const int float_callee_count = td->float_callee_count;
    const int* float_scratch_regs = td->float_scratch_regs;
    const int float_scratch_count = td->float_scratch_count;

    int* active = (int*)malloc(ir->vreg_count * sizeof(int));
    int active_count = 0;

    ir->total_spill_bytes = 0;

    for (int i = 0; i < interval_count; i++) {
        int v = intervals[i].vreg_id;
        int cur_start = intervals[i].start_ip;
        VRegInfo* vi = &ir->vregs[v];

        int new_active_count = 0;
        for (int a = 0; a < active_count; a++) {
            int act_v = active[a];
            if (ir->vregs[act_v].end_ip >= cur_start) {
                active[new_active_count++] = act_v;
            }
        }
        active_count = new_active_count;

        bool preg_busy[32] = { false };
        bool xmm_busy[32] = { false };
        for (int a = 0; a < active_count; a++) {
            int act_v = active[a];
            if (!ir->vregs[act_v].is_spilled) {
                int p = ir->vregs[act_v].preg;
                if (p >= 0 && p < 32) {
                    if (ir->vregs[act_v].is_float) {
                        xmm_busy[p] = true;
                    } else {
                        preg_busy[p] = true;
                    }
                }
            }
        }

        int allocated_preg = PREG_NONE;

        if (vi->is_float) {
            if (vi->crosses_call) {
                for (int r = 0; r < float_callee_count; r++) {
                    int p = float_callee_regs[r];
                    if (!xmm_busy[p]) { allocated_preg = p; break; }
                }
            } else {
                for (int r = 0; r < float_scratch_count; r++) {
                    int p = float_scratch_regs[r];
                    if (!xmm_busy[p]) { allocated_preg = p; break; }
                }
                if (allocated_preg == PREG_NONE) {
                    for (int r = 0; r < float_callee_count; r++) {
                        int p = float_callee_regs[r];
                        if (!xmm_busy[p]) { allocated_preg = p; break; }
                    }
                }
            }
        } else {
            if (vi->crosses_call) {
                for (int r = 0; r < int_callee_count; r++) {
                    int p = int_callee_regs[r];
                    if (!preg_busy[p]) { allocated_preg = p; break; }
                }
            } else {
                for (int r = 0; r < int_scratch_count; r++) {
                    int p = int_scratch_regs[r];
                    if (!preg_busy[p]) { allocated_preg = p; break; }
                }
                if (allocated_preg == PREG_NONE) {
                    for (int r = 0; r < int_callee_count; r++) {
                        int p = int_callee_regs[r];
                        if (!preg_busy[p]) { allocated_preg = p; break; }
                    }
                }
            }
        }

        if (allocated_preg != PREG_NONE) {
            vi->preg = allocated_preg;
            vi->is_spilled = false;
            active[active_count++] = v;
        } else if (weight) {
            /* Weight-based eviction: evict a cold outer interval to keep hot loop vars in registers */
            int best_idx = -1;
            int w_v = weight[v];
            for (int a = 0; a < active_count; a++) {
                int act_v = active[a];
                VRegInfo* ai = &ir->vregs[act_v];
                if (ai->is_spilled || ai->is_float != vi->is_float || ai->preg == PREG_NONE) continue;
                if (vi->crosses_call) {
                    bool ok_callee = vi->is_float
                        ? preg_in_list(ai->preg, float_callee_regs, float_callee_count)
                        : preg_in_list(ai->preg, int_callee_regs, int_callee_count);
                    if (!ok_callee) continue;
                }
                int w_a = weight[act_v];
                bool hotter = (w_a * 2 < w_v) || (w_a == w_v && ai->end_ip > vi->end_ip);
                if (!hotter) continue;
                if (best_idx == -1 ||
                    weight[act_v] < weight[active[best_idx]] ||
                    (weight[act_v] == weight[active[best_idx]] && ai->end_ip > ir->vregs[active[best_idx]].end_ip)) {
                    best_idx = a;
                }
            }
            if (best_idx != -1) {
                int ev_v = active[best_idx];
                VRegInfo* ei = &ir->vregs[ev_v];
                int freed_p = ei->preg;
                ir->total_spill_bytes += 8;
                ei->spill_offset = td->spill_base_offset + ir->total_spill_bytes;
                ei->is_spilled = true;
                ei->preg = PREG_NONE;
                vi->preg = freed_p;
                vi->is_spilled = false;
                active[best_idx] = v;
            } else {
                ir->total_spill_bytes += 8;
                vi->spill_offset = td->spill_base_offset + ir->total_spill_bytes;
                vi->is_spilled = true;
                vi->preg = PREG_NONE;
            }
        } else {
            ir->total_spill_bytes += 8;
            vi->spill_offset = td->spill_base_offset + ir->total_spill_bytes;
            vi->is_spilled = true;
            vi->preg = PREG_NONE;
        }
    }

    if (g_target_arch == VORTEX_ARCH_X86_64) {
        uint32_t mask = 0;
        for (int v = 0; v < ir->vreg_count; v++) {
            if (!ir->vregs[v].is_spilled && !ir->vregs[v].is_float && ir->vregs[v].preg != PREG_NONE) {
                if (preg_in_list(ir->vregs[v].preg, td->int_callee_regs, td->int_callee_count)) {
                    mask |= (1u << ir->vregs[v].preg);
                }
            }
        }
        uint32_t fmask = 0;
        if (g_target_abi == VORTEX_ABI_WIN64) {
            for (int v = 0; v < ir->vreg_count; v++) {
                if (!ir->vregs[v].is_spilled && ir->vregs[v].is_float && ir->vregs[v].preg != PREG_NONE) {
                    if (preg_in_list(ir->vregs[v].preg, td->float_callee_regs, td->float_callee_count)) {
                        fmask |= (1u << ir->vregs[v].preg);
                    }
                }
            }
        }
        ir->callee_save_mask = mask;
        ir->float_callee_save_mask = fmask;
        ir->optimize_frame = true;
        ir->omit_fp = (g_compile_mode == VORTEX_MODE_RELEASE &&
                       ir->total_spill_bytes == 0 &&
                       fmask == 0 &&
                       (ir->param_count == 0 ||
                        (ir->param_count == 1 && ir->vreg_count > 0 && !ir->vregs[0].is_float)));
    }

    free(weight);
    free(intervals);
    free(active);
}
