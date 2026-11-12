/*
 * VORTEX 3-ADDRESS LINEAR IR, LIVENESS ANALYSIS, LINEAR SCAN & AVX2 VECTORIZER
 */

#include "vortex.h"

void ir_init(IRBuffer* ir) {
    ir->capacity = 4096;
    ir->count = 0;
    ir->instrs = (IRInstruction*)calloc(ir->capacity, sizeof(IRInstruction));
    ir->vreg_capacity = 1024;
    ir->vreg_count = 0;
    ir->vregs = (VRegInfo*)calloc(ir->vreg_capacity, sizeof(VRegInfo));
    ir->next_label = 0;
    ir->total_spill_bytes = 0;
}

void ir_free(IRBuffer* ir) {
    if (ir->instrs) free(ir->instrs);
    if (ir->vregs) free(ir->vregs);
    memset(ir, 0, sizeof(*ir));
}

VReg ir_new_vreg(IRBuffer* ir, VortexType type) {
    if (ir->vreg_count >= ir->vreg_capacity) {
        ir->vreg_capacity *= 2;
        ir->vregs = (VRegInfo*)realloc(ir->vregs, ir->vreg_capacity * sizeof(VRegInfo));
    }
    int id = ir->vreg_count++;
    VRegInfo* vi = &ir->vregs[id];
    memset(vi, 0, sizeof(*vi));
    vi->id = id;
    vi->type = type;
    vi->is_float = (type == TYPE_F32 || type == TYPE_F64);
    vi->start_ip = -1;
    vi->end_ip = -1;
    vi->crosses_call = false;
    vi->preg = PREG_NONE;
    vi->spill_offset = 0;
    vi->is_spilled = false;
    return id;
}

int ir_new_label(IRBuffer* ir) {
    return ir->next_label++;
}

IRInstruction* ir_emit(IRBuffer* ir, IROpcode op) {
    if (ir->count >= ir->capacity) {
        ir->capacity *= 2;
        ir->instrs = (IRInstruction*)realloc(ir->instrs, ir->capacity * sizeof(IRInstruction));
    }
    IRInstruction* ins = &ir->instrs[ir->count++];
    memset(ins, 0, sizeof(*ins));
    ins->op = op;
    ins->dest = VREG_NONE;
    ins->src1 = VREG_NONE;
    ins->src2 = VREG_NONE;
    ins->src3 = VREG_NONE;
    return ins;
}

void ir_emit_const_i64(IRBuffer* ir, VReg dest, int64_t val) {
    IRInstruction* ins = ir_emit(ir, IR_CONST_I64);
    ins->dest = dest;
    ins->imm = val;
    ins->type = TYPE_I64;
}

void ir_emit_const_f32(IRBuffer* ir, VReg dest, float val) {
    IRInstruction* ins = ir_emit(ir, IR_CONST_F32);
    ins->dest = dest;
    ins->fimm = val;
    ins->type = TYPE_F32;
}

void ir_emit_mov(IRBuffer* ir, VReg dest, VReg src) {
    IRInstruction* ins = ir_emit(ir, IR_MOV);
    ins->dest = dest;
    ins->src1 = src;
    ins->type = ir->vregs[dest].type;
}

void ir_emit_binop(IRBuffer* ir, IROpcode op, VReg dest, VReg s1, VReg s2, VortexType t) {
    IRInstruction* ins = ir_emit(ir, op);
    ins->dest = dest;
    ins->src1 = s1;
    ins->src2 = s2;
    ins->type = t;
}

void ir_emit_load_offset(IRBuffer* ir, VReg dest, VReg base, int32_t offset, VortexType t) {
    IRInstruction* ins = ir_emit(ir, IR_LOAD_OFFSET);
    ins->dest = dest;
    ins->src1 = base;
    ins->offset = offset;
    ins->type = t;
}

void ir_emit_store_offset(IRBuffer* ir, VReg base, int32_t offset, VReg val, VortexType t) {
    IRInstruction* ins = ir_emit(ir, IR_STORE_OFFSET);
    ins->src1 = base;
    ins->src2 = val;
    ins->offset = offset;
    ins->type = t;
}

void ir_emit_load_array(IRBuffer* ir, VReg dest, VReg base, VReg idx, VortexType t) {
    IRInstruction* ins = ir_emit(ir, IR_LOAD_ARRAY);
    ins->dest = dest;
    ins->src1 = base;
    ins->src2 = idx;
    ins->type = t;
}

void ir_emit_store_array(IRBuffer* ir, VReg base, VReg idx, VReg val, VortexType t) {
    IRInstruction* ins = ir_emit(ir, IR_STORE_ARRAY);
    ins->src1 = base;
    ins->src2 = idx;
    ins->src3 = val;
    ins->type = t;
}

void ir_emit_label(IRBuffer* ir, int lbl) {
    IRInstruction* ins = ir_emit(ir, IR_LABEL);
    ins->imm = lbl;
}

void ir_emit_jmp(IRBuffer* ir, int lbl) {
    IRInstruction* ins = ir_emit(ir, IR_JMP);
    ins->imm = lbl;
}

void ir_emit_jz(IRBuffer* ir, VReg cond, int lbl) {
    IRInstruction* ins = ir_emit(ir, IR_JZ);
    ins->src1 = cond;
    ins->imm = lbl;
}

void ir_emit_call(IRBuffer* ir, VReg dest, void* fn_ptr, VReg* args, int num_args, VortexType ret_t) {
    IRInstruction* ins = ir_emit(ir, IR_CALL);
    ins->dest = dest;
    ins->func_ptr = fn_ptr;
    ins->arg_count = num_args;
    ins->type = ret_t;
    for (int i = 0; i < num_args; i++) ins->args[i] = args[i];
}

void ir_emit_call_user(IRBuffer* ir, VReg dest, const char* name, VReg* args, int num_args, VortexType ret_t) {
    IRInstruction* ins = ir_emit(ir, IR_CALL_USER);
    ins->dest = dest;
    strncpy(ins->func_name, name, sizeof(ins->func_name) - 1);
    ins->arg_count = num_args;
    ins->type = ret_t;
    for (int i = 0; i < num_args; i++) ins->args[i] = args[i];
}

void ir_emit_call_ffi(IRBuffer* ir, VReg dest, FFIFunc* ff, VReg* args, int num_args, VortexType ret_t) {
    IRInstruction* ins = ir_emit(ir, IR_CALL_FFI);
    ins->dest = dest;
    ins->func_ptr = ff->func_ptr;
    strncpy(ins->func_name, ff->fn_name, sizeof(ins->func_name) - 1);
    ins->arg_count = num_args;
    ins->type = ret_t;
    ins->ret_type = ff->ret_type;
    for (int i = 0; i < num_args; i++) {
        ins->args[i] = args[i];
        ins->arg_types[i] = (i < ff->param_count) ? ff->param_types[i] : ir->vregs[args[i]].type;
    }
}

void ir_emit_load_param(IRBuffer* ir, VReg dest, int param_idx) {
    IRInstruction* ins = ir_emit(ir, IR_LOAD_PARAM);
    ins->dest = dest;
    ins->imm = param_idx;
    ins->type = TYPE_I64;
}

void ir_emit_ret(IRBuffer* ir, VReg val, VortexType t) {
    IRInstruction* ins = ir_emit(ir, IR_RET);
    ins->src1 = val;
    ins->type = t;
}

/* ========================================================================= */
/* LIVENESS ANALYSIS, AVX2 AUTO-VECTORIZER & LINEAR SCAN ALLOCATOR           */
/* ========================================================================= */

static void ir_record_use(IRBuffer* ir, VReg v, int ip) {
    if (v == VREG_NONE || v < 0 || v >= ir->vreg_count) return;
    VRegInfo* vi = &ir->vregs[v];
    if (vi->start_ip == -1 || ip < vi->start_ip) vi->start_ip = ip;
    if (ip > vi->end_ip) vi->end_ip = ip;
}

void ir_liveness_analysis(IRBuffer* ir) {
    int* label_ips = (int*)malloc(MAX_LABELS * sizeof(int));
    if (!label_ips) return;
    for (int i = 0; i < MAX_LABELS; i++) label_ips[i] = -1;

    for (int ip = 0; ip < ir->count; ip++) {
        IRInstruction* ins = &ir->instrs[ip];
        if (ins->op == IR_LABEL) {
            int lbl = (int)ins->imm;
            if (lbl >= 0 && lbl < MAX_LABELS) label_ips[lbl] = ip;
        }
        ir_record_use(ir, ins->dest, ip);
        ir_record_use(ir, ins->src1, ip);
        ir_record_use(ir, ins->src2, ip);
        ir_record_use(ir, ins->src3, ip);
        for (int a = 0; a < ins->arg_count; a++) {
            ir_record_use(ir, ins->args[a], ip);
        }
        if (ins->op == IR_RAW_RIEMANN || ins->op == IR_VEC_LOOP) {
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
                if (lbl >= 0 && lbl < MAX_LABELS) {
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

    /* Call crossing detection */
    for (int ip = 0; ip < ir->count; ip++) {
        IRInstruction* ins = &ir->instrs[ip];
        if (ins->op == IR_CALL || ins->op == IR_CALL_USER || ins->op == IR_CALL_FFI) {
            for (int v = 0; v < ir->vreg_count; v++) {
                VRegInfo* vi = &ir->vregs[v];
                if (vi->start_ip != -1 && vi->start_ip < ip && vi->end_ip > ip) {
                    vi->crosses_call = true;
                }
            }
        }
    }
    free(label_ips);
}

static int compare_live_intervals(const void* a, const void* b) {
    const LiveInterval* ia = (const LiveInterval*)a;
    const LiveInterval* ib = (const LiveInterval*)b;
    if (ia->start_ip != ib->start_ip) return ia->start_ip - ib->start_ip;
    return ia->end_ip - ib->end_ip;
}

void ir_loop_vectorize(IRBuffer* ir) {
    if (!ir || ir->count < 8) return;

    for (int ip = 0; ip < ir->count; ip++) {
        if (ir->instrs[ip].op != IR_LABEL) continue;
        int lbl_start = (int)ir->instrs[ip].imm;
        int ip_start = ip;

        if (ip_start + 1 >= ir->count) continue;
        IRInstruction* ins_cmp = &ir->instrs[ip_start + 1];
        if (ins_cmp->op != IR_LT && ins_cmp->op != IR_LE) continue;
        VReg i_vreg = ins_cmp->src1;
        VReg lim_vreg = ins_cmp->src2;
        VReg cmp_vreg = ins_cmp->dest;

        if (ip_start + 2 >= ir->count) continue;
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

        /* Loop safety checks: no function calls, no nested branches */
        bool safe = true;
        for (int k = ip_start + 3; k < ip_jmp; k++) {
            IROpcode op = ir->instrs[k].op;
            if (op == IR_CALL || op == IR_CALL_USER || op == IR_CALL_FFI ||
                op == IR_JMP || op == IR_JZ || op == IR_JNZ || op == IR_LABEL || op == IR_RET) {
                safe = false;
                break;
            }
        }
        if (!safe) continue;

        /* Detect stepping variable: x = x + step */
        VReg x_vreg = VREG_NONE;
        VReg step_vreg = VREG_NONE;
        for (int k = ip_start + 3; k < ip_jmp; k++) {
            IRInstruction* ins = &ir->instrs[k];
            if (ins->op == IR_FADD) {
                if (k + 1 < ip_jmp && ir->instrs[k + 1].op == IR_MOV && ir->instrs[k + 1].src1 == ins->dest) {
                    VReg dest_v = ir->instrs[k + 1].dest;
                    if (ins->src1 == dest_v) {
                        x_vreg = dest_v;
                        step_vreg = ins->src2;
                    }
                }
            }
        }

        /* Detect accumulator variable: sum = sum + term */
        VReg acc_vreg = VREG_NONE;
        VReg term_vreg = VREG_NONE;
        for (int k = ip_start + 3; k < ip_jmp; k++) {
            IRInstruction* ins = &ir->instrs[k];
            if (ins->op == IR_FADD) {
                if (k + 1 < ip_jmp && ir->instrs[k + 1].op == IR_MOV && ir->instrs[k + 1].src1 == ins->dest) {
                    VReg dest_v = ir->instrs[k + 1].dest;
                    if (dest_v != x_vreg && ins->src1 == dest_v) {
                        acc_vreg = dest_v;
                        term_vreg = ins->src2;
                    }
                }
            }
        }

        if (acc_vreg == VREG_NONE || x_vreg == VREG_NONE) continue;

        /* Collect body expression instructions */
        IRInstruction body_ops[32];
        int body_op_count = 0;
        float num_val = 0.0f;
        float den_base = 0.0f;
        for (int k = ip_start + 3; k < ip_jmp; k++) {
            IRInstruction* ins = &ir->instrs[k];
            if (ins->op == IR_CONST_F32) {
                if (num_val == 0.0f) num_val = ins->fimm;
                else if (den_base == 0.0f) den_base = ins->fimm;
            }
            if (ins->dest != acc_vreg && ins->dest != x_vreg && ins->dest != i_vreg) {
                if (ins->op != IR_CONST_I64 && ins->op != IR_ADD) {
                    if (body_op_count < 32) {
                        body_ops[body_op_count++] = *ins;
                    }
                }
            }
        }

        /* Vectorize loop: replace with IR_VEC_LOOP */
        IRInstruction* vec_ins = &ir->instrs[ip_start];
        memset(vec_ins, 0, sizeof(*vec_ins));
        vec_ins->op = IR_VEC_LOOP;
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

        /* Shift remaining instructions down to overwrite the scalar loop */
        int remove_count = (ip_jmp + 1) - ip_start;
        for (int m = ip_start + 1; m + remove_count <= ir->count; m++) {
            ir->instrs[m] = ir->instrs[m + remove_count];
        }
        ir->count -= remove_count;
    }
}

void ir_linear_scan_allocate(IRBuffer* ir) {
    if (ir->vreg_count == 0) return;
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

    static const int int_callee_regs[] = { PREG_RBX, PREG_RSI, PREG_RDI, PREG_R12, PREG_R13, PREG_R14, PREG_R15 };
    static const int int_callee_count = 7;

    static const int int_scratch_regs[] = { PREG_RCX, PREG_R8, PREG_R9 };
    static const int int_scratch_count = 3;

    static const int float_callee_regs[] = { 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
    static const int float_callee_count = 10;

    static const int float_scratch_regs[] = { 0, 1, 2, 3 };
    static const int float_scratch_count = 4;

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

        bool preg_busy[16] = { false };
        bool xmm_busy[16] = { false };
        for (int a = 0; a < active_count; a++) {
            int act_v = active[a];
            if (!ir->vregs[act_v].is_spilled) {
                if (ir->vregs[act_v].is_float) {
                    xmm_busy[ir->vregs[act_v].preg] = true;
                } else {
                    preg_busy[ir->vregs[act_v].preg] = true;
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
        } else {
            ir->total_spill_bytes += 8;
            vi->spill_offset = 128 + ir->total_spill_bytes;
            vi->is_spilled = true;
            vi->preg = PREG_NONE;
        }
    }

    free(intervals);
    free(active);
}
