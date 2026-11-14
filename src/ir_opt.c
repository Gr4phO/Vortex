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

static bool ir_has_side_effect(IROpcode op) {
    switch (op) {
        case IR_STORE_OFFSET:
        case IR_STORE_ARRAY:
        case IR_STORE_DEREF:
        case IR_BOUNDS_CHECK:
        case IR_RANGE_CHECK:
        case IR_PTR_CHECK:
        case IR_FFI_CHECK:
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
        case IR_SUB_MOD_IMM:
            return true;
        default:
            return false;
    }
}

static void ir_inline_small_functions(IRBuffer* ir) {
    if (!ir || ir->count == 0) return;
    int expansions = 0;
    for (int ip = 0; ip < ir->count && expansions < 64; ip++) {
        IRInstruction* ins = &ir->instrs[ip];
        if (ins->op != IR_CALL_USER) continue;
        UserFunc* uf = user_func_find(ins->func_name);
        if (!uf || !uf->can_inline || !uf->inline_instrs || !uf->inline_vregs) continue;
        if (ins->arg_count != uf->param_count) continue;
        if (uf->inline_vreg_count <= 0 || uf->inline_vreg_count > 256) continue;
        if (uf->inline_ir_count <= 0 || uf->inline_ir_count > 40) continue;
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
        if (ins->op == IR_RAW_RIEMANN || ins->op == IR_VEC_LOOP || ins->op == IR_VEC_ARR_OP) {
            if (ins->i_vreg >= 0 && ins->i_vreg < n) { use_count[ins->i_vreg]++; def_count[ins->i_vreg]++; }
            if (ins->lim_vreg >= 0 && ins->lim_vreg < n) use_count[ins->lim_vreg]++;
            if (ins->acc_vreg >= 0 && ins->acc_vreg < n) { use_count[ins->acc_vreg]++; def_count[ins->acc_vreg]++; }
            if (ins->x_vreg >= 0 && ins->x_vreg < n) { use_count[ins->x_vreg]++; def_count[ins->x_vreg]++; }
            if (ins->step_vreg >= 0 && ins->step_vreg < n) use_count[ins->step_vreg]++;
        }
    }
}

void ir_optimize_bounds_and_licm(IRBuffer* ir) {
    if (!ir || ir->count == 0 || ir->vreg_count == 0) return;
    if (g_compile_mode != VORTEX_MODE_RELEASE) return;

    /* Pillar #1: Inline short user functions in caller IR before RCE/LICM */
    ir_inline_small_functions(ir);

    int n_vregs = ir->vreg_count;
    bool* has_const = (bool*)calloc((size_t)n_vregs, sizeof(bool));
    int64_t* const_val = (int64_t*)calloc((size_t)n_vregs, sizeof(int64_t));
    int* def_count = (int*)calloc((size_t)n_vregs, sizeof(int));
    int* use_count = (int*)calloc((size_t)n_vregs, sizeof(int));
    int* def_ip = (int*)malloc((size_t)n_vregs * sizeof(int));
    VReg* equiv = (VReg*)malloc((size_t)n_vregs * sizeof(VReg));
    if (!has_const || !const_val || !def_count || !use_count || !def_ip || !equiv) {
        free(has_const); free(const_val); free(def_count); free(use_count); free(def_ip); free(equiv);
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
                        chk->op = IR_NOP;
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

    /* Pass 4 (Pillar #5): Constant folding on 2 constants & Copy propagation / Coalescing */
    for (int pass = 0; pass < 2; pass++) {
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
            if (ins->op == IR_NOP) continue;

            /* Fold constant arithmetic */
            if (ins->type == TYPE_I64 && ins->dest >= 0 &&
                ins->src1 >= 0 && ins->src2 >= 0 &&
                has_const[ins->src1] && has_const[ins->src2]) {
                uint64_t a = (uint64_t)const_val[ins->src1];
                uint64_t b = (uint64_t)const_val[ins->src2];
                bool folded = true;
                int64_t res = 0;
                if (ins->op == IR_ADD) res = (int64_t)(a + b);
                else if (ins->op == IR_SUB) res = (int64_t)(a - b);
                else if (ins->op == IR_MUL) res = (int64_t)(a * b);
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

            /* Propagate single-def immutable copies: d = MOV s (where s is single-def) */
            if (ins->op == IR_MOV && ins->dest >= 0 && ins->src1 >= 0 &&
                def_count[ins->dest] == 1 && def_count[ins->src1] == 1 &&
                ir->vregs[ins->dest].is_float == ir->vregs[ins->src1].is_float) {
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
                        if (qi->op == IR_RAW_RIEMANN || qi->op == IR_VEC_LOOP || qi->op == IR_VEC_ARR_OP) {
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
                ip + 1 < ir->count) {
                IRInstruction* nxt = &ir->instrs[ip + 1];
                VReg tmp = ins->dest;
                VReg orig = ins->src1;
                if (nxt->op != IR_NOP && nxt->op != IR_LABEL &&
                    (nxt->src1 == tmp || nxt->src2 == tmp || nxt->src3 == tmp || nxt->src4 == tmp ||
                     ((nxt->op == IR_RAW_RIEMANN || nxt->op == IR_VEC_LOOP || nxt->op == IR_VEC_ARR_OP) &&
                      (nxt->lim_vreg == tmp || nxt->step_vreg == tmp)))) {
                    if (nxt->src1 == tmp) nxt->src1 = orig;
                    if (nxt->src2 == tmp) nxt->src2 = orig;
                    if (nxt->src3 == tmp) nxt->src3 = orig;
                    if (nxt->src4 == tmp) nxt->src4 = orig;
                    if (nxt->op == IR_RAW_RIEMANN || nxt->op == IR_VEC_LOOP || nxt->op == IR_VEC_ARR_OP) {
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
                ins->op != IR_RAW_RIEMANN && ins->op != IR_VEC_LOOP && ins->op != IR_VEC_ARR_OP) {
                IRInstruction* mv = &ir->instrs[ip + 1];
                VReg tmp = ins->dest;
                VReg dst = mv->dest;
                if (mv->src1 == tmp && dst >= 0 &&
                    def_count[tmp] == 1 && use_count[tmp] == 1 &&
                    ir->vregs[tmp].is_float == ir->vregs[dst].is_float) {
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
            if (op != IR_CONST_I64 && op != IR_CONST_F32) continue;
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
            if ((ins->op == IR_ADD || ins->op == IR_SUB || ins->op == IR_MUL ||
                 (ins->op >= IR_EQ && ins->op <= IR_GE)) &&
                ins->type == TYPE_I64 && ins->src1 >= 0 && ins->src2 >= 0) {
                if (has_const[ins->src2]) {
                    int64_t cv = const_val[ins->src2];
                    if (cv >= -2147483648LL && cv <= 2147483647LL) {
                        ins->imm = cv;
                        ins->src2 = VREG_NONE;
                    }
                } else if ((ins->op == IR_ADD || ins->op == IR_MUL) && has_const[ins->src1]) {
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

    free(has_const);
    free(const_val);
    free(def_count);
    free(use_count);
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
        if (ins->op == IR_RAW_RIEMANN || ins->op == IR_VEC_LOOP || ins->op == IR_VEC_ARR_OP) {
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
        } else if (ins->op == IR_VEC_ARR_OP || ins->op == IR_VEC_LOOP || ins->op == IR_RAW_RIEMANN) {
            for (int v = 0; v < ir->vreg_count; v++) {
                VRegInfo* vi = &ir->vregs[v];
                if (vi->is_float && vi->start_ip != -1 && vi->start_ip < ip && vi->end_ip > ip) {
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

static bool vreg_is_used_outside_range(const IRBuffer* ir, VReg v, int range_lo, int range_hi) {
    if (v == VREG_NONE || v < 0) return false;
    for (int p = 0; p < ir->count; p++) {
        if (p >= range_lo && p <= range_hi) continue;
        const IRInstruction* ins = &ir->instrs[p];
        if (ins->op == IR_NOP) continue;
        if (ins->src1 == v || ins->src2 == v || ins->src3 == v || ins->src4 == v) return true;
        if (ins->dest == v && ins->op != IR_MOV && ins->op != IR_CONST_I64 && ins->op != IR_CONST_F32) return true;
        for (int a = 0; a < ins->arg_count; a++) {
            if (ins->args[a] == v) return true;
        }
        if (ins->op == IR_RAW_RIEMANN || ins->op == IR_VEC_LOOP || ins->op == IR_VEC_ARR_OP) {
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

        /* Loop safety checks: no function calls, no traps, no nested branches */
        bool safe = true;
        for (int k = ip_start + 3; k < ip_jmp; k++) {
            IROpcode op = ir->instrs[k].op;
            if (op == IR_CALL || op == IR_CALL_USER || op == IR_CALL_FFI ||
                op == IR_JMP || op == IR_JZ || op == IR_JNZ || op == IR_LABEL || op == IR_RET ||
                op == IR_BOUNDS_CHECK || op == IR_RANGE_CHECK || op == IR_PTR_CHECK || op == IR_FFI_CHECK) {
                safe = false;
                break;
            }
        }
        if (!safe) continue;

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

    /* Compute loop nesting weights per VReg for hot-loop eviction decisions */
    int* ip_depth = (int*)calloc((size_t)(ir->count + 2), sizeof(int));
    int* weight = (int*)calloc((size_t)ir->vreg_count, sizeof(int));
    int* lbl_ips = (int*)malloc(MAX_LABELS * sizeof(int));
    if (ip_depth && weight && lbl_ips) {
        for (int l = 0; l < MAX_LABELS; l++) lbl_ips[l] = -1;
        for (int p = 0; p < ir->count; p++) {
            IRInstruction* ins = &ir->instrs[p];
            if (ins->op == IR_LABEL) {
                int lbl = (int)ins->imm;
                if (lbl >= 0 && lbl < MAX_LABELS) lbl_ips[lbl] = p;
            }
            if (ins->op == IR_RAW_RIEMANN || ins->op == IR_VEC_LOOP || ins->op == IR_VEC_ARR_OP) {
                ir->uses_avx = true;
            }
            if (ins->op == IR_CALL || ins->op == IR_CALL_USER || ins->op == IR_CALL_FFI ||
                ins->op == IR_BOUNDS_CHECK || ins->op == IR_RANGE_CHECK ||
                ins->op == IR_PTR_CHECK || ins->op == IR_FFI_CHECK) {
                ir->has_calls = true;
            }
        }
        for (int p = 0; p < ir->count; p++) {
            IROpcode op = ir->instrs[p].op;
            if (op == IR_JMP || op == IR_JZ || op == IR_JNZ) {
                int lbl = (int)ir->instrs[p].imm;
                if (lbl >= 0 && lbl < MAX_LABELS && lbl_ips[lbl] >= 0 && lbl_ips[lbl] < p) {
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
            if (ins->op == IR_RAW_RIEMANN || ins->op == IR_VEC_LOOP || ins->op == IR_VEC_ARR_OP) {
                if (ins->i_vreg >= 0 && ins->i_vreg < ir->vreg_count) weight[ins->i_vreg] += w * 8;
                if (ins->lim_vreg >= 0 && ins->lim_vreg < ir->vreg_count) weight[ins->lim_vreg] += w * 8;
                if (ins->acc_vreg >= 0 && ins->acc_vreg < ir->vreg_count) weight[ins->acc_vreg] += w * 8;
                if (ins->x_vreg >= 0 && ins->x_vreg < ir->vreg_count) weight[ins->x_vreg] += w * 8;
                if (ins->step_vreg >= 0 && ins->step_vreg < ir->vreg_count) weight[ins->step_vreg] += w * 8;
            }
        }
    }
    free(ip_depth);
    free(lbl_ips);

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
        ir->callee_save_mask = mask;
        ir->optimize_frame = true;
    }

    free(weight);
    free(intervals);
    free(active);
}
