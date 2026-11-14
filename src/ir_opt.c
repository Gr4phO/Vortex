/*
 * VORTEX 3-ADDRESS LINEAR IR, REGION VERIFIER, RCE/LICM, LIVENESS, LINEAR SCAN & SIMD VECTORIZER
 */

#include "vortex.h"
#ifdef fprintf
#undef fprintf
#endif
#define fprintf vortex_diag_emit

/* ========================================================================= */
/* PORTABLE TARGET ARCHITECTURE DESCRIPTORS & BACKEND SELECTION              */
/* ========================================================================= */

static const VortexTargetDesc g_desc_win64 = {
    VORTEX_ARCH_X86_64, VORTEX_ABI_WIN64, "x86_64-windows",
    VORTEX_SIMD_AVX2, 256, 8, 128,
    { PREG_RBX, PREG_RSI, PREG_RDI, PREG_R12, PREG_R13, PREG_R14, PREG_R15 }, 7,
    { PREG_RCX, PREG_R8, PREG_R9 }, 3,
    { 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 }, 10,
    { 0, 1, 2, 3 }, 4
};

static const VortexTargetDesc g_desc_sysv64 = {
    VORTEX_ARCH_X86_64, VORTEX_ABI_SYSV64, "x86_64-linux",
    VORTEX_SIMD_AVX2, 256, 8, 128,
    { PREG_RBX, PREG_R12, PREG_R13, PREG_R14, PREG_R15 }, 5,
    { PREG_RCX, PREG_RDX, PREG_RSI, PREG_RDI, PREG_R8, PREG_R9 }, 6,
    { 0 }, 0, /* SysV AMD64 has no callee-saved XMM registers */
    { 0, 1, 2, 3, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 }, 14
};

#ifdef __ARM_FEATURE_SVE
#define ARM64_DEFAULT_SIMD (VORTEX_SIMD_NEON | VORTEX_SIMD_SVE)
#else
#define ARM64_DEFAULT_SIMD (VORTEX_SIMD_NEON)
#endif

static const VortexTargetDesc g_desc_aapcs64 = {
    VORTEX_ARCH_ARM64, VORTEX_ABI_AAPCS64, "arm64-linux",
    ARM64_DEFAULT_SIMD, 128, 4, 224,
    { A64_X19, A64_X20, A64_X21, A64_X22, A64_X23, A64_X24, A64_X25, A64_X26, A64_X27, A64_X28 }, 10,
    { A64_X9, A64_X10, A64_X11, A64_X12 }, 4,
    { 8, 9, 10, 11, 12, 13, 14, 15 }, 8,
    { 16, 17, 18, 19, 20, 21, 22, 23 }, 8
};

static const VortexTargetDesc g_desc_darwin64 = {
    VORTEX_ARCH_ARM64, VORTEX_ABI_DARWIN64, "arm64-darwin",
    VORTEX_SIMD_NEON, 128, 4, 224,
    { A64_X19, A64_X20, A64_X21, A64_X22, A64_X23, A64_X24, A64_X25, A64_X26, A64_X27, A64_X28 }, 10,
    { A64_X9, A64_X10, A64_X11, A64_X12 }, 4,
    { 8, 9, 10, 11, 12, 13, 14, 15 }, 8,
    { 16, 17, 18, 19, 20, 21, 22, 23 }, 8
};

const VortexTargetDesc* vortex_get_target_desc_for(VortexTargetArch arch, VortexTargetABI abi) {
    if (arch == VORTEX_ARCH_ARM64) {
        return (abi == VORTEX_ABI_DARWIN64) ? &g_desc_darwin64 : &g_desc_aapcs64;
    }
    return (abi == VORTEX_ABI_WIN64) ? &g_desc_win64 : &g_desc_sysv64;
}

const VortexTargetDesc* vortex_get_target_desc(void) {
    return vortex_get_target_desc_for(g_target_arch, g_target_abi);
}

const VortexJITBackend* vortex_get_backend(void) {
    return (g_target_arch == VORTEX_ARCH_ARM64) ? &g_backend_arm64 : &g_backend_x64;
}

bool vortex_set_target_name(const char* name) {
    if (!name || !name[0]) return false;
    if (strcmp(name, "x86_64-windows") == 0 || strcmp(name, "x64-windows") == 0 ||
        strcmp(name, "x64-win") == 0 || strcmp(name, "win64") == 0) {
        g_target_arch = VORTEX_ARCH_X86_64;
        g_target_abi = VORTEX_ABI_WIN64;
        return true;
    }
    if (strcmp(name, "x86_64-linux") == 0 || strcmp(name, "x64-linux") == 0 ||
        strcmp(name, "linux-x64") == 0 || strcmp(name, "sysv64") == 0) {
        g_target_arch = VORTEX_ARCH_X86_64;
        g_target_abi = VORTEX_ABI_SYSV64;
        return true;
    }
    if (strcmp(name, "arm64-linux") == 0 || strcmp(name, "aarch64-linux") == 0 ||
        strcmp(name, "linux-arm64") == 0 || strcmp(name, "arm64") == 0 ||
        strcmp(name, "aarch64") == 0) {
        g_target_arch = VORTEX_ARCH_ARM64;
        g_target_abi = VORTEX_ABI_AAPCS64;
        return true;
    }
    if (strcmp(name, "arm64-darwin") == 0 || strcmp(name, "arm64-macos") == 0 ||
        strcmp(name, "darwin-arm64") == 0 || strcmp(name, "macos-arm64") == 0 ||
        strcmp(name, "apple-silicon") == 0) {
        g_target_arch = VORTEX_ARCH_ARM64;
        g_target_abi = VORTEX_ABI_DARWIN64;
        return true;
    }
    return false;
}

static IRBuffer* g_active_ir_bufs[16];
static int g_active_ir_count = 0;

void ir_init(IRBuffer* ir) {
    ir->capacity = 4096;
    ir->count = 0;
    ir->instrs = (IRInstruction*)calloc(ir->capacity, sizeof(IRInstruction));
    ir->vreg_capacity = 1024;
    ir->vreg_count = 0;
    ir->vregs = (VRegInfo*)calloc(ir->vreg_capacity, sizeof(VRegInfo));
    ir->next_label = 0;
    ir->total_spill_bytes = 0;
    if (g_active_ir_count < 16) {
        g_active_ir_bufs[g_active_ir_count++] = ir;
    }
}

void ir_free(IRBuffer* ir) {
    for (int i = 0; i < g_active_ir_count; i++) {
        if (g_active_ir_bufs[i] == ir) {
            g_active_ir_bufs[i] = g_active_ir_bufs[g_active_ir_count - 1];
            g_active_ir_count--;
            break;
        }
    }
    if (ir->instrs) free(ir->instrs);
    if (ir->vregs) free(ir->vregs);
    memset(ir, 0, sizeof(*ir));
}

void ir_free_all_active(void) {
    while (g_active_ir_count > 0) {
        IRBuffer* ir = g_active_ir_bufs[--g_active_ir_count];
        if (ir) {
            if (ir->instrs) free(ir->instrs);
            if (ir->vregs) free(ir->vregs);
            memset(ir, 0, sizeof(*ir));
        }
    }
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
    vi->len_vreg = VREG_NONE;
    vi->known_len = -1;
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
    ins->src4 = VREG_NONE;
    ins->known_len = -1;
    ins->file = g_current_filename;
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

void ir_emit_cast(IRBuffer* ir, IROpcode op, VReg dest, VReg src, VortexType t) {
    IRInstruction* ins = ir_emit(ir, op);
    ins->dest = dest;
    ins->src1 = src;
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

void ir_emit_load_deref(IRBuffer* ir, VReg dest, VReg ptr_vreg, VortexType t) {
    IRInstruction* ins = ir_emit(ir, IR_LOAD_DEREF);
    ins->dest = dest;
    ins->src1 = ptr_vreg;
    ins->type = t;
}

void ir_emit_store_deref(IRBuffer* ir, VReg ptr_vreg, VReg val, VortexType t) {
    IRInstruction* ins = ir_emit(ir, IR_STORE_DEREF);
    ins->src1 = ptr_vreg;
    ins->src2 = val;
    ins->type = t;
}

void ir_emit_bounds_check(IRBuffer* ir, VReg base, VReg idx, VReg len_vreg,
                          int64_t known_len, int32_t elem_size, const char* file, int line) {
    IRInstruction* ins = ir_emit(ir, IR_BOUNDS_CHECK);
    ins->src1 = base;
    ins->src2 = idx;
    ins->src3 = len_vreg;
    ins->known_len = known_len;
    ins->offset = elem_size;
    ins->file = file ? file : "<unknown>";
    ins->line = line;
}

void ir_emit_ptr_check(IRBuffer* ir, VReg ptr_vreg, const char* file, int line) {
    IRInstruction* ins = ir_emit(ir, IR_PTR_CHECK);
    ins->src1 = ptr_vreg;
    ins->file = file ? file : "<unknown>";
    ins->line = line;
}

void ir_emit_ffi_check(IRBuffer* ir, VReg val, VortexType expected_t, int64_t align_and_flags,
                       const char* fn_name, int arg_idx, const char* file, int line) {
    IRInstruction* ins = ir_emit(ir, IR_FFI_CHECK);
    ins->src1 = val;
    ins->type = expected_t;
    ins->imm = align_and_flags;
    if (fn_name) {
        strncpy(ins->func_name, fn_name, sizeof(ins->func_name) - 1);
    }
    ins->offset = arg_idx;
    ins->file = file ? file : "<unknown>";
    ins->line = line;
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
    ins->is_variadic = ff->is_variadic;
    ins->fixed_param_count = ff->param_count;
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
/* COMPILE-TIME STATIC REGION VERIFIER (ZERO RUNTIME COST)                   */
/* ========================================================================= */

static bool ir_is_allocator_fn(void* fn_ptr) {
    return fn_ptr == (void*)vortex_alloc_struct ||
           fn_ptr == (void*)arena_alloc ||
           fn_ptr == (void*)arena_alloc_fast ||
           fn_ptr == (void*)vortex_alloc_slice_i64 ||
           fn_ptr == (void*)vortex_alloc_slice_f32 ||
           fn_ptr == (void*)vortex_alloc_slice_i32 ||
           fn_ptr == (void*)vortex_vec_new ||
           fn_ptr == (void*)vortex_map_new ||
           fn_ptr == (void*)vortex_str_concat ||
           fn_ptr == (void*)vortex_str_sub ||
           fn_ptr == (void*)vortex_str_from_i64 ||
           fn_ptr == (void*)vortex_str_from_f32;
}

void ir_region_verify(IRBuffer* ir) {
    if (!ir || ir->vreg_count == 0 || ir->count == 0) return;

    int n_vregs = ir->vreg_count;
    bool* is_ref = (bool*)calloc((size_t)n_vregs, sizeof(bool));
    int* reg_id = (int*)calloc((size_t)n_vregs, sizeof(int));
    int* reg_depth = (int*)calloc((size_t)n_vregs, sizeof(int));
    bool* is_mark = (bool*)calloc((size_t)n_vregs, sizeof(bool));
    int* mark_rid = (int*)calloc((size_t)n_vregs, sizeof(int));
    if (!is_ref || !reg_id || !reg_depth || !is_mark || !mark_rid) {
        free(is_ref); free(reg_id); free(reg_depth); free(is_mark); free(mark_rid);
        return;
    }

    for (int v = 0; v < n_vregs; v++) {
        is_ref[v] = ir->vregs[v].is_ref;
        reg_id[v] = ir->vregs[v].region_id;
        reg_depth[v] = ir->vregs[v].region_depth;
    }

    bool popped[512] = { false };
    int stack_rid[VORTEX_MAX_SCOPE_DEPTH];
    int cur_depth = 0;
    int next_rid = 1;
    stack_rid[0] = 0;

    for (int ip = 0; ip < ir->count; ip++) {
        IRInstruction* ins = &ir->instrs[ip];
        int cur_rid = stack_rid[cur_depth];

        if (ins->op == IR_CALL && ins->func_ptr == (void*)vortex_scope_push) {
            if (cur_depth + 1 < VORTEX_MAX_SCOPE_DEPTH && next_rid < 512) {
                int new_rid = next_rid++;
                cur_depth++;
                stack_rid[cur_depth] = new_rid;
                if (ins->dest >= 0 && ins->dest < n_vregs) {
                    is_mark[ins->dest] = true;
                    mark_rid[ins->dest] = new_rid;
                }
            }
            continue;
        }

        if (ins->op == IR_CALL && ins->func_ptr == (void*)vortex_scope_pop) {
            int pop_rid = -1;
            if (ins->arg_count >= 1 && ins->args[0] >= 0 && ins->args[0] < n_vregs && is_mark[ins->args[0]]) {
                pop_rid = mark_rid[ins->args[0]];
            }
            if (pop_rid > 0 && pop_rid < 512) {
                for (int d = cur_depth; d >= 1; d--) {
                    int r = stack_rid[d];
                    if (r > 0 && r < 512) popped[r] = true;
                    cur_depth = d - 1;
                    if (r == pop_rid) break;
                }
            } else if (cur_depth > 0) {
                int r = stack_rid[cur_depth];
                if (r > 0 && r < 512) popped[r] = true;
                cur_depth--;
            }
            continue;
        }

        /* Check operand liveness against popped regions */
        VReg uses[4] = { ins->src1, ins->src2, ins->src3, ins->src4 };
        for (int u = 0; u < 4; u++) {
            VReg uv = uses[u];
            if (uv >= 0 && uv < n_vregs && is_ref[uv]) {
                int rid = reg_id[uv];
                if (rid > 0 && rid < 512 && popped[rid]) {
                    fprintf(stderr,
                            "[VORTEX REGION ERROR] Reference allocated in arena scope #%d used after scope_pop() at %s:%d\n",
                            rid, ins->file ? ins->file : "<unknown>", ins->line);
                    free(is_ref); free(reg_id); free(reg_depth); free(is_mark); free(mark_rid);
                    vortex_error("");
                }
            }
        }
        for (int a = 0; a < ins->arg_count; a++) {
            VReg av = ins->args[a];
            if (av >= 0 && av < n_vregs && is_ref[av]) {
                int rid = reg_id[av];
                if (rid > 0 && rid < 512 && popped[rid]) {
                    fprintf(stderr,
                            "[VORTEX REGION ERROR] Reference allocated in arena scope #%d passed to call after scope_pop() at %s:%d\n",
                            rid, ins->file ? ins->file : "<unknown>", ins->line);
                    free(is_ref); free(reg_id); free(reg_depth); free(is_mark); free(mark_rid);
                    vortex_error("");
                }
            }
        }

        /* Return escape check: cannot return reference from inner/popped scope_push region */
        if (ins->op == IR_RET && ins->src1 >= 0 && ins->src1 < n_vregs) {
            VReg rv = ins->src1;
            if (is_ref[rv] && reg_depth[rv] > 0) {
                fprintf(stderr,
                        "[VORTEX REGION ERROR] Cannot return reference allocated inside internal scope_push() region at %s:%d\n",
                        ins->file ? ins->file : "<unknown>", ins->line);
                free(is_ref); free(reg_id); free(reg_depth); free(is_mark); free(mark_rid);
                vortex_error("");
            }
        }

        /* Escape into outer variable / outer container */
        if (ins->op == IR_MOV && ins->dest >= 0 && ins->dest < n_vregs &&
            ins->src1 >= 0 && ins->src1 < n_vregs) {
            if (is_mark[ins->src1]) {
                is_mark[ins->dest] = true;
                mark_rid[ins->dest] = mark_rid[ins->src1];
            }
            if (is_ref[ins->src1]) {
                if (ir->vregs[ins->dest].region_depth >= 0 &&
                    reg_depth[ins->src1] > ir->vregs[ins->dest].region_depth &&
                    is_ref[ins->dest]) {
                    fprintf(stderr,
                            "[VORTEX REGION ERROR] Reference from inner arena scope (depth %d) cannot escape to outer binding (depth %d) at %s:%d\n",
                            reg_depth[ins->src1], ir->vregs[ins->dest].region_depth,
                            ins->file ? ins->file : "<unknown>", ins->line);
                    free(is_ref); free(reg_id); free(reg_depth); free(is_mark); free(mark_rid);
                    vortex_error("");
                }
                is_ref[ins->dest] = true;
                reg_id[ins->dest] = reg_id[ins->src1];
                reg_depth[ins->dest] = reg_depth[ins->src1];
            } else {
                is_ref[ins->dest] = false;
                reg_id[ins->dest] = 0;
                reg_depth[ins->dest] = 0;
            }
        } else if (ins->op == IR_STORE_OFFSET &&
                   ins->src1 >= 0 && ins->src1 < n_vregs &&
                   ins->src2 >= 0 && ins->src2 < n_vregs) {
            if (is_ref[ins->src2] && is_ref[ins->src1] &&
                reg_depth[ins->src2] > reg_depth[ins->src1]) {
                fprintf(stderr,
                        "[VORTEX REGION ERROR] Inner arena reference (depth %d) cannot be stored in outer struct (depth %d) at %s:%d\n",
                        reg_depth[ins->src2], reg_depth[ins->src1],
                        ins->file ? ins->file : "<unknown>", ins->line);
                free(is_ref); free(reg_id); free(reg_depth); free(is_mark); free(mark_rid);
                vortex_error("");
            }
        } else if (ins->op == IR_STORE_ARRAY &&
                   ins->src1 >= 0 && ins->src1 < n_vregs &&
                   ins->src3 >= 0 && ins->src3 < n_vregs) {
            if (is_ref[ins->src3] && is_ref[ins->src1] &&
                reg_depth[ins->src3] > reg_depth[ins->src1]) {
                fprintf(stderr,
                        "[VORTEX REGION ERROR] Inner arena reference (depth %d) cannot be stored in outer array (depth %d) at %s:%d\n",
                        reg_depth[ins->src3], reg_depth[ins->src1],
                        ins->file ? ins->file : "<unknown>", ins->line);
                free(is_ref); free(reg_id); free(reg_depth); free(is_mark); free(mark_rid);
                vortex_error("");
            }
        } else if (ins->op == IR_CALL && ir_is_allocator_fn(ins->func_ptr)) {
            if (ins->dest >= 0 && ins->dest < n_vregs) {
                is_ref[ins->dest] = true;
                reg_id[ins->dest] = cur_rid;
                reg_depth[ins->dest] = cur_depth;
            }
        }
    }

    free(is_ref);
    free(reg_id);
    free(reg_depth);
    free(is_mark);
    free(mark_rid);
}

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

void ir_optimize_bounds_and_licm(IRBuffer* ir) {
    if (!ir || ir->count == 0 || ir->vreg_count == 0) return;
    if (g_compile_mode != VORTEX_MODE_RELEASE) return;

    int n_vregs = ir->vreg_count;
    bool* has_const = (bool*)calloc((size_t)n_vregs, sizeof(bool));
    int64_t* const_val = (int64_t*)calloc((size_t)n_vregs, sizeof(int64_t));
    int* def_count = (int*)calloc((size_t)n_vregs, sizeof(int));
    int* def_ip = (int*)malloc((size_t)n_vregs * sizeof(int));
    VReg* equiv = (VReg*)malloc((size_t)n_vregs * sizeof(VReg));
    if (!has_const || !const_val || !def_count || !def_ip || !equiv) {
        free(has_const); free(const_val); free(def_count); free(def_ip); free(equiv);
        return;
    }
    for (int v = 0; v < n_vregs; v++) {
        def_ip[v] = -1;
        equiv[v] = v;
    }

    for (int ip = 0; ip < ir->count; ip++) {
        IRInstruction* ins = &ir->instrs[ip];
        if (ins->dest >= 0 && ins->dest < n_vregs) {
            def_count[ins->dest]++;
            def_ip[ins->dest] = ip;
        }
    }

    for (int ip = 0; ip < ir->count; ip++) {
        IRInstruction* ins = &ir->instrs[ip];
        if (ins->op == IR_CONST_I64 && ins->dest >= 0 && def_count[ins->dest] == 1) {
            has_const[ins->dest] = true;
            const_val[ins->dest] = ins->imm;
        } else if (ins->op == IR_MOV && ins->dest >= 0 && ins->src1 >= 0 &&
                   def_count[ins->dest] == 1 && def_count[ins->src1] == 1) {
            equiv[ins->dest] = equiv[ins->src1];
            if (has_const[ins->src1]) {
                has_const[ins->dest] = true;
                const_val[ins->dest] = const_val[ins->src1];
            }
        }
    }

    /* Pass 1: Static constant range check elimination */
    for (int ip = 0; ip < ir->count; ip++) {
        IRInstruction* ins = &ir->instrs[ip];
        if (ins->op == IR_BOUNDS_CHECK) {
            int64_t len_c = ins->known_len;
            if (len_c < 0 && ins->src3 >= 0 && has_const[ins->src3]) {
                len_c = const_val[ins->src3];
            }
            if (len_c >= 0 && ins->src2 >= 0 && has_const[ins->src2]) {
                int64_t idx_c = const_val[ins->src2];
                if (idx_c >= 0 && idx_c < len_c) {
                    ins->op = IR_NOP;
                }
            }
        }
    }

    /* Pass 2: Loop-Invariant Bounds Check Elimination & Hoisting (LICM) */
    bool* modified = (bool*)calloc((size_t)n_vregs, sizeof(bool));
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

            /* Verify i_vreg only increments by +1 at end of loop */
            bool valid_ind = false;
            int ind_writes = 0;
            for (int k = ip + 3; k < ip_jmp; k++) {
                if (ir->instrs[k].dest == i_vreg) {
                    ind_writes++;
                    if (ir->instrs[k].op == IR_ADD && ir->instrs[k].src1 == i_vreg &&
                        ir->instrs[k].src2 >= 0 && has_const[ir->instrs[k].src2] &&
                        const_val[ir->instrs[k].src2] == 1) {
                        valid_ind = true;
                    }
                }
            }
            if (!valid_ind || ind_writes != 1) continue;

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

            memset(modified, 0, (size_t)n_vregs * sizeof(bool));
            for (int k = ip + 1; k < ip_jmp; k++) {
                if (ir->instrs[k].dest >= 0 && ir->instrs[k].dest < n_vregs) {
                    modified[ir->instrs[k].dest] = true;
                }
            }

            /* Hoist or eliminate checks inside loop body */
            for (int k = ip + 3; k < ip_jmp; k++) {
                IRInstruction* chk = &ir->instrs[k];
                if (chk->op == IR_PTR_CHECK) {
                    if (chk->src1 >= 0 && !modified[chk->src1]) {
                        chk->op = IR_NOP; /* safe in release once verified at compile time */
                    }
                    continue;
                }
                if (chk->op != IR_BOUNDS_CHECK) continue;
                VReg base = chk->src1;
                VReg idx = chk->src2;
                VReg len_v = chk->src3;

                if (base < 0 || modified[base]) continue;
                if (len_v >= 0 && modified[len_v]) continue;

                bool idx_is_ind = (idx == i_vreg);
                if (!idx_is_ind) continue;

                bool start_nonneg = (has_const[start_vreg] && const_val[start_vreg] >= 0);
                bool lim_fits_len = false;
                if (len_v >= 0 && equiv[lim_vreg] == equiv[len_v]) {
                    lim_fits_len = true;
                } else if (has_const[lim_vreg]) {
                    int64_t lc = const_val[lim_vreg];
                    int64_t lenc = chk->known_len;
                    if (lenc < 0 && len_v >= 0 && has_const[len_v]) lenc = const_val[len_v];
                    if (lenc >= 0 && lc <= lenc) lim_fits_len = true;
                }

                if (start_nonneg && lim_fits_len) {
                    /* Completely proved safe: zero runtime check! */
                    chk->op = IR_NOP;
                } else if (len_v >= 0 || chk->known_len >= 0) {
                    /* Hoist single range check before loop label */
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
            }
        }
        free(modified);
    }

    /* Pass 3: Strip remaining IR_PTR_CHECK / IR_FFI_CHECK in --release mode */
    for (int ip = 0; ip < ir->count; ip++) {
        if (ir->instrs[ip].op == IR_PTR_CHECK || ir->instrs[ip].op == IR_FFI_CHECK) {
            ir->instrs[ip].op = IR_NOP;
        }
    }

    ir_compact_nops(ir);
    free(has_const);
    free(const_val);
    free(def_count);
    free(def_ip);
    free(equiv);
}

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
        ir_record_use(ir, ins->src4, ip);
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

    /* Call crossing detection (including runtime check traps) */
    for (int ip = 0; ip < ir->count; ip++) {
        IRInstruction* ins = &ir->instrs[ip];
        if (ins->op == IR_CALL || ins->op == IR_CALL_USER || ins->op == IR_CALL_FFI ||
            ins->op == IR_BOUNDS_CHECK || ins->op == IR_RANGE_CHECK ||
            ins->op == IR_PTR_CHECK || ins->op == IR_FFI_CHECK) {
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
                op == IR_JMP || op == IR_JZ || op == IR_JNZ || op == IR_LABEL || op == IR_RET ||
                op == IR_BOUNDS_CHECK || op == IR_PTR_CHECK || op == IR_FFI_CHECK) {
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
    const VortexTargetDesc* td = vortex_get_target_desc();
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
        } else {
            ir->total_spill_bytes += 8;
            vi->spill_offset = td->spill_base_offset + ir->total_spill_bytes;
            vi->is_spilled = true;
            vi->preg = PREG_NONE;
        }
    }

    free(intervals);
    free(active);
}
