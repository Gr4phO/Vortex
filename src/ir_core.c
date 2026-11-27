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
    ir->uses_avx = false;
    ir->has_calls = false;
    ir->optimize_frame = false;
    ir->omit_fp = false;
    ir->param_count = 0;
    ir->max_call_args = 0;
    ir->callee_save_mask = 0;
    ir->float_callee_save_mask = 0;
    ir->func_name[0] = '\0';
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
           fn_ptr == (void*)vortex_str_from_f32 ||
           fn_ptr == (void*)vortex_str_from_byte ||
           fn_ptr == (void*)vortex_str_join ||
           fn_ptr == (void*)vortex_fs_read ||
           fn_ptr == (void*)vortex_env_get ||
           fn_ptr == (void*)vortex_args_get;
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
        bool reported_use = false;
        for (int u = 0; u < 4; u++) {
            VReg uv = uses[u];
            if (uv >= 0 && uv < n_vregs && is_ref[uv]) {
                int rid = reg_id[uv];
                if (rid > 0 && rid < 512 && popped[rid]) {
                    char msg[256];
                    snprintf(msg, sizeof(msg), "[VORTEX REGION ERROR] Reference allocated in arena scope #%d used after scope_pop()", rid);
                    if (g_ultrasafe_level >= 3 && g_recover_active) {
                        vortex_diag_add(VDIAG_ERROR, "E301", ins->file ? ins->file : g_current_filename,
                                        ins->line, 1, msg, "Keep arena references within their scope_push/scope_pop lifetime.");
                        reported_use = true;
                        break;
                    }
                    fprintf(stderr, "%s at %s:%d\n", msg, ins->file ? ins->file : "<unknown>", ins->line);
                    free(is_ref); free(reg_id); free(reg_depth); free(is_mark); free(mark_rid);
                    vortex_error("");
                }
            }
        }
        if (reported_use) continue;
        for (int a = 0; a < ins->arg_count; a++) {
            VReg av = ins->args[a];
            if (av >= 0 && av < n_vregs && is_ref[av]) {
                int rid = reg_id[av];
                if (rid > 0 && rid < 512 && popped[rid]) {
                    char msg[256];
                    snprintf(msg, sizeof(msg), "[VORTEX REGION ERROR] Reference allocated in arena scope #%d passed to call after scope_pop()", rid);
                    if (g_ultrasafe_level >= 3 && g_recover_active) {
                        vortex_diag_add(VDIAG_ERROR, "E301", ins->file ? ins->file : g_current_filename,
                                        ins->line, 1, msg, "Keep arena references within their scope_push/scope_pop lifetime.");
                        reported_use = true;
                        break;
                    }
                    fprintf(stderr, "%s at %s:%d\n", msg, ins->file ? ins->file : "<unknown>", ins->line);
                    free(is_ref); free(reg_id); free(reg_depth); free(is_mark); free(mark_rid);
                    vortex_error("");
                }
            }
        }
        if (reported_use) continue;

        /* Return escape check: cannot return reference from inner/popped scope_push region */
        if (ins->op == IR_RET && ins->src1 >= 0 && ins->src1 < n_vregs) {
            VReg rv = ins->src1;
            if (is_ref[rv] && reg_depth[rv] > 0) {
                const char* msg = "[VORTEX REGION ERROR] Cannot return reference allocated inside internal scope_push() region";
                if (g_ultrasafe_level >= 3 && g_recover_active) {
                    vortex_diag_add(VDIAG_ERROR, "E301", ins->file ? ins->file : g_current_filename,
                                    ins->line, 1, msg, "Allocate returned data in the caller's outer arena scope.");
                    continue;
                }
                fprintf(stderr, "%s at %s:%d\n", msg, ins->file ? ins->file : "<unknown>", ins->line);
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
                    char msg[256];
                    snprintf(msg, sizeof(msg), "[VORTEX REGION ERROR] Reference from inner arena scope (depth %d) cannot escape to outer binding (depth %d)",
                             reg_depth[ins->src1], ir->vregs[ins->dest].region_depth);
                    if (g_ultrasafe_level >= 3 && g_recover_active) {
                        vortex_diag_add(VDIAG_ERROR, "E301", ins->file ? ins->file : g_current_filename,
                                        ins->line, 1, msg, "Bind the value inside the same arena scope or copy it safely.");
                        continue;
                    }
                    fprintf(stderr, "%s at %s:%d\n", msg, ins->file ? ins->file : "<unknown>", ins->line);
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
                char msg[256];
                snprintf(msg, sizeof(msg), "[VORTEX REGION ERROR] Inner arena reference (depth %d) cannot be stored in outer struct (depth %d)",
                         reg_depth[ins->src2], reg_depth[ins->src1]);
                if (g_ultrasafe_level >= 3 && g_recover_active) {
                    vortex_diag_add(VDIAG_ERROR, "E301", ins->file ? ins->file : g_current_filename,
                                    ins->line, 1, msg, "Ensure struct and field share an outer arena scope.");
                    continue;
                }
                fprintf(stderr, "%s at %s:%d\n", msg, ins->file ? ins->file : "<unknown>", ins->line);
                free(is_ref); free(reg_id); free(reg_depth); free(is_mark); free(mark_rid);
                vortex_error("");
            }
        } else if (ins->op == IR_STORE_ARRAY &&
                   ins->src1 >= 0 && ins->src1 < n_vregs &&
                   ins->src3 >= 0 && ins->src3 < n_vregs) {
            if (is_ref[ins->src3] && is_ref[ins->src1] &&
                reg_depth[ins->src3] > reg_depth[ins->src1]) {
                char msg[256];
                snprintf(msg, sizeof(msg), "[VORTEX REGION ERROR] Inner arena reference (depth %d) cannot be stored in outer array (depth %d)",
                         reg_depth[ins->src3], reg_depth[ins->src1]);
                if (g_ultrasafe_level >= 3 && g_recover_active) {
                    vortex_diag_add(VDIAG_ERROR, "E301", ins->file ? ins->file : g_current_filename,
                                    ins->line, 1, msg, "Ensure array and element share an outer arena scope.");
                    continue;
                }
                fprintf(stderr, "%s at %s:%d\n", msg, ins->file ? ins->file : "<unknown>", ins->line);
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

void ir_emit_div_check(IRBuffer* ir, VReg divisor, const char* file, int line) {
    IRInstruction* ins = ir_emit(ir, IR_DIV_CHECK);
    ins->src1 = divisor;
    ins->file = file ? file : g_current_filename;
    ins->line = line;
}
