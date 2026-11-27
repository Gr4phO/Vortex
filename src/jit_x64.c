/*
 * VORTEX X86-64 DUAL-ABI JIT CODE GENERATOR
 * - Windows x64 & Linux System V AMD64 ABI compliant machine code emitter
 */

#include "jit_x64_internal.h"

/* ========================================================================= */
/* LOW-LEVEL X86-64 / AVX2 EMITTER                                           */
/* ========================================================================= */

void emit_u8(JITBuffer* j, uint8_t b) {
    if (j->size >= j->capacity) {
        fprintf(stderr, "[VORTEX ERROR] JIT Buffer overflow\n");
        vortex_error("");
    }
    j->code[j->size++] = b;
}

void emit_u32(JITBuffer* j, uint32_t val) {
    emit_u8(j, (uint8_t)(val & 0xFF));
    emit_u8(j, (uint8_t)((val >> 8) & 0xFF));
    emit_u8(j, (uint8_t)((val >> 16) & 0xFF));
    emit_u8(j, (uint8_t)((val >> 24) & 0xFF));
}

static void emit_u64(JITBuffer* j, uint64_t val) {
    emit_u32(j, (uint32_t)(val & 0xFFFFFFFF));
    emit_u32(j, (uint32_t)((val >> 32) & 0xFFFFFFFF));
}

size_t emit_jmp_placeholder(JITBuffer* j) {
    emit_u8(j, 0xE9);
    size_t patch = j->size;
    emit_u32(j, 0);
    return patch;
}

size_t emit_jcc_placeholder(JITBuffer* j, uint8_t cond_byte) {
    emit_u8(j, 0x0F);
    emit_u8(j, cond_byte);
    size_t patch = j->size;
    emit_u32(j, 0);
    return patch;
}

void patch_jump(JITBuffer* j, size_t patch_pos) {
    int32_t rel = (int32_t)(j->size - (patch_pos + 4));
    memcpy(&j->code[patch_pos], &rel, 4);
}

static const IRBuffer* g_x64_frame_ir = NULL;

void jit_set_active_ir_frame(const IRBuffer* ir) {
    g_x64_frame_ir = ir;
}

static const int k_x64_callee_gprs[7] = {
    PREG_RBX, PREG_RSI, PREG_RDI, PREG_R12, PREG_R13, PREG_R14, PREG_R15
};

static int x64_callee_push_count(uint32_t mask) {
    int k = 0;
    for (int i = 0; i < 7; i++) {
        if (mask & (1u << k_x64_callee_gprs[i])) k++;
    }
    return k;
}

static int x64_callee_xmm_count(uint32_t fmask) {
    int k = 0;
    for (int r = 6; r < 16; r++) {
        if (fmask & (1u << r)) k++;
    }
    return k;
}

static void emit_movups_rsp_disp(JITBuffer* j, bool is_store, int xmm_reg, int32_t disp) {
    if (xmm_reg >= 8) emit_u8(j, 0x44);
    emit_u8(j, 0x0F);
    emit_u8(j, is_store ? 0x11 : 0x10);
    if (disp == 0) {
        emit_u8(j, (uint8_t)(0x04 | ((xmm_reg & 7) << 3)));
        emit_u8(j, 0x24);
    } else if (disp >= -128 && disp <= 127) {
        emit_u8(j, (uint8_t)(0x44 | ((xmm_reg & 7) << 3)));
        emit_u8(j, 0x24);
        emit_u8(j, (uint8_t)(int8_t)disp);
    } else {
        emit_u8(j, (uint8_t)(0x84 | ((xmm_reg & 7) << 3)));
        emit_u8(j, 0x24);
        emit_u32(j, (uint32_t)disp);
    }
}

int compute_frame_size(int total_spill_bytes) {
    if (g_x64_frame_ir && g_x64_frame_ir->optimize_frame) {
        int k = x64_callee_push_count(g_x64_frame_ir->callee_save_mask);
        int push_bytes = k * 8;
        int fxk = x64_callee_xmm_count(g_x64_frame_ir->float_callee_save_mask);
        int xmm_save_bytes = fxk * 16;
        if (g_x64_frame_ir->omit_fp) {
            if (!g_x64_frame_ir->has_calls) return 0;
            return (k % 2 == 0) ? 8 : 0;
        }
        int min_below = push_bytes;
        if (total_spill_bytes > 0) {
            int spill_need = 128 + total_spill_bytes;
            if (spill_need > min_below) min_below = spill_need;
        }
        if (g_target_abi == VORTEX_ABI_SYSV64 && g_x64_frame_ir->param_count > 0) {
            int pc = g_x64_frame_ir->param_count;
            if (pc > 6) pc = 6;
            int sysv_need = 56 + 8 * pc;
            if (sysv_need > min_below) min_below = sysv_need;
        }
        min_below += xmm_save_bytes;
        if (g_x64_frame_ir->has_calls || min_below > push_bytes) {
            min_below = (min_below + 15) & ~15;
        }
        return min_below - push_bytes;
    }
    int size = 128 + total_spill_bytes + 64;
    if ((size % 16) != 8) {
        size = ((size + 15) & ~15) + 8;
    }
    return size;
}

void emit_prologue(JITBuffer* j, int frame_size) {
    emit_u8(j, 0x55);                                     /* push rbp */
    emit_u8(j, 0x48); emit_u8(j, 0x89); emit_u8(j, 0xE5); /* mov rbp, rsp */
    emit_u8(j, 0x53); emit_u8(j, 0x56); emit_u8(j, 0x57); /* push rbx, rsi, rdi */
    emit_u8(j, 0x41); emit_u8(j, 0x54); emit_u8(j, 0x41); emit_u8(j, 0x55); /* push r12, r13 */
    emit_u8(j, 0x41); emit_u8(j, 0x56); emit_u8(j, 0x41); emit_u8(j, 0x57); /* push r14, r15 */
    emit_u8(j, 0x48); emit_u8(j, 0x81); emit_u8(j, 0xEC); /* sub rsp, frame_size */
    emit_u32(j, (uint32_t)frame_size);
}

void emit_epilogue(JITBuffer* j, int frame_size) {
    emit_u8(j, 0x48); emit_u8(j, 0x81); emit_u8(j, 0xC4); /* add rsp, frame_size */
    emit_u32(j, (uint32_t)frame_size);
    emit_u8(j, 0x41); emit_u8(j, 0x5F); emit_u8(j, 0x41); emit_u8(j, 0x5E); /* pop r15, r14 */
    emit_u8(j, 0x41); emit_u8(j, 0x5D); emit_u8(j, 0x41); emit_u8(j, 0x5C); /* pop r13, r12 */
    emit_u8(j, 0x5F); emit_u8(j, 0x5E); emit_u8(j, 0x5B); emit_u8(j, 0x5D); /* pop rdi, rsi, rbx, rbp */
    emit_u8(j, 0xC3);                                     /* ret */
}

void emit_store_mem_offset(JITBuffer* j, int base, int32_t offset, int src);

void emit_func_prologue(JITBuffer* j, int frame_size) {
    bool opt = (g_x64_frame_ir && g_x64_frame_ir->optimize_frame);
    bool omit_fp = (opt && g_x64_frame_ir->omit_fp);
    if (!omit_fp) {
        emit_u8(j, 0x55);                                     /* push rbp */
        emit_u8(j, 0x48); emit_u8(j, 0x89); emit_u8(j, 0xE5); /* mov rbp, rsp */
    }
    int pc = opt ? g_x64_frame_ir->param_count : 6;
    bool direct_p0 = (opt && pc == 1 && g_x64_frame_ir->vreg_count > 0 && !g_x64_frame_ir->vregs[0].is_float);
    if (g_target_abi != VORTEX_ABI_SYSV64 && !omit_fp) {
        static const uint8_t win_modrm[4] = { 0x4D, 0x55, 0x45, 0x4D };
        static const uint8_t win_rex[4]   = { 0x48, 0x48, 0x4C, 0x4C };
        int max_win_reg = (pc < 4) ? pc : 4;
        for (int k = 0; k < max_win_reg; k++) {
            if (k == 0 && direct_p0) continue;
            bool is_flt = (g_x64_frame_ir && k < g_x64_frame_ir->vreg_count && g_x64_frame_ir->vregs[k].is_float);
            if (is_flt) {
                uint8_t pfx = (g_x64_frame_ir->vregs[k].type == TYPE_F64) ? 0xF2 : 0xF3;
                emit_u8(j, pfx); emit_u8(j, 0x0F); emit_u8(j, 0x11);
                emit_u8(j, (uint8_t)(0x45 | (k << 3)));
                emit_u8(j, (uint8_t)(0x10 + 8 * k)); /* movss/movsd [rbp + 16 + 8*k], xmm_k */
            } else {
                emit_u8(j, win_rex[k]); emit_u8(j, 0x89); emit_u8(j, win_modrm[k]); emit_u8(j, (uint8_t)(0x10 + 8 * k));
            }
        }
    }
    if (opt) {
        uint32_t mask = g_x64_frame_ir->callee_save_mask;
        for (int i = 0; i < 7; i++) {
            int r = k_x64_callee_gprs[i];
            if (mask & (1u << r)) {
                if (r < 8) emit_u8(j, (uint8_t)(0x50 + r));
                else { emit_u8(j, 0x41); emit_u8(j, (uint8_t)(0x50 + (r & 7))); }
            }
        }
        if (frame_size > 0) {
            if (frame_size <= 127) {
                emit_u8(j, 0x48); emit_u8(j, 0x83); emit_u8(j, 0xEC); emit_u8(j, (uint8_t)frame_size);
            } else {
                emit_u8(j, 0x48); emit_u8(j, 0x81); emit_u8(j, 0xEC); emit_u32(j, (uint32_t)frame_size);
            }
        }
        uint32_t fmask = g_x64_frame_ir->float_callee_save_mask;
        if (fmask != 0) {
            int32_t xoff = 0;
            for (int r = 6; r < 16; r++) {
                if (fmask & (1u << r)) {
                    emit_movups_rsp_disp(j, true, r, xoff);
                    xoff += 16;
                }
            }
        }
    } else {
        emit_u8(j, 0x53); emit_u8(j, 0x56); emit_u8(j, 0x57); /* push rbx, rsi, rdi */
        emit_u8(j, 0x41); emit_u8(j, 0x54); emit_u8(j, 0x41); emit_u8(j, 0x55); /* push r12, r13 */
        emit_u8(j, 0x41); emit_u8(j, 0x56); emit_u8(j, 0x41); emit_u8(j, 0x57); /* push r14, r15 */
        emit_u8(j, 0x48); emit_u8(j, 0x81); emit_u8(j, 0xEC); /* sub rsp, frame_size */
        emit_u32(j, (uint32_t)frame_size);
    }
    if (g_target_abi == VORTEX_ABI_SYSV64 && !omit_fp) {
        static const int sysv_p[6] = { PREG_RDI, PREG_RSI, PREG_RDX, PREG_RCX, PREG_R8, PREG_R9 };
        int lim = pc < 6 ? pc : 6;
        for (int i = direct_p0 ? 1 : 0; i < lim; i++) {
            emit_store_mem_offset(j, PREG_RBP, -(64 + 8 * i), sysv_p[i]);
        }
    }
}

void emit_func_epilogue(JITBuffer* j, int frame_size) {
    if (g_x64_frame_ir && g_x64_frame_ir->optimize_frame) {
        uint32_t fmask = g_x64_frame_ir->float_callee_save_mask;
        if (fmask != 0) {
            int32_t xoff = 0;
            for (int r = 6; r < 16; r++) {
                if (fmask & (1u << r)) {
                    emit_movups_rsp_disp(j, false, r, xoff);
                    xoff += 16;
                }
            }
        }
        if (frame_size > 0) {
            if (frame_size <= 127) {
                emit_u8(j, 0x48); emit_u8(j, 0x83); emit_u8(j, 0xC4); emit_u8(j, (uint8_t)frame_size);
            } else {
                emit_u8(j, 0x48); emit_u8(j, 0x81); emit_u8(j, 0xC4); emit_u32(j, (uint32_t)frame_size);
            }
        }
        uint32_t mask = g_x64_frame_ir->callee_save_mask;
        for (int i = 6; i >= 0; i--) {
            int r = k_x64_callee_gprs[i];
            if (mask & (1u << r)) {
                if (r < 8) emit_u8(j, (uint8_t)(0x58 + r));
                else { emit_u8(j, 0x41); emit_u8(j, (uint8_t)(0x58 + (r & 7))); }
            }
        }
        if (!g_x64_frame_ir->omit_fp) {
            emit_u8(j, 0x5D); /* pop rbp */
        }
        emit_u8(j, 0xC3); /* ret */
        return;
    }
    emit_epilogue(j, frame_size);
}

void emit_push_r(JITBuffer* j, int r) {
    if (r < 8) emit_u8(j, (uint8_t)(0x50 + r));
    else { emit_u8(j, 0x41); emit_u8(j, (uint8_t)(0x50 + (r & 7))); }
}

void emit_mov_rr(JITBuffer* j, int dst, int src) {
    if (dst == src) return;
    uint8_t rex = 0x48;
    if (src >= 8) rex |= 0x04;
    if (dst >= 8) rex |= 0x01;
    emit_u8(j, rex);
    emit_u8(j, 0x89);
    emit_u8(j, (uint8_t)(0xC0 | ((src & 7) << 3) | (dst & 7)));
}

void emit_add_rr(JITBuffer* j, int dst, int src) {
    uint8_t rex = 0x48;
    if (src >= 8) rex |= 0x04;
    if (dst >= 8) rex |= 0x01;
    emit_u8(j, rex);
    emit_u8(j, 0x01);
    emit_u8(j, (uint8_t)(0xC0 | ((src & 7) << 3) | (dst & 7)));
}

void emit_sub_rr(JITBuffer* j, int dst, int src) {
    uint8_t rex = 0x48;
    if (src >= 8) rex |= 0x04;
    if (dst >= 8) rex |= 0x01;
    emit_u8(j, rex);
    emit_u8(j, 0x29);
    emit_u8(j, (uint8_t)(0xC0 | ((src & 7) << 3) | (dst & 7)));
}

static void emit_imul_rr(JITBuffer* j, int dst, int src) {
    uint8_t rex = 0x48;
    if (dst >= 8) rex |= 0x04;
    if (src >= 8) rex |= 0x01;
    emit_u8(j, rex);
    emit_u8(j, 0x0F);
    emit_u8(j, 0xAF);
    emit_u8(j, (uint8_t)(0xC0 | ((dst & 7) << 3) | (src & 7)));
}

static void emit_idiv_r(JITBuffer* j, int divisor) {
    if (divisor >= 8) {
        emit_u8(j, 0x49); emit_u8(j, 0xF7); emit_u8(j, (uint8_t)(0xF8 | (divisor & 7)));
    } else {
        emit_u8(j, 0x48); emit_u8(j, 0xF7); emit_u8(j, (uint8_t)(0xF8 | (divisor & 7)));
    }
}

void emit_cmp_rr(JITBuffer* j, int r1, int r2) {
    uint8_t rex = 0x48;
    if (r2 >= 8) rex |= 0x04;
    if (r1 >= 8) rex |= 0x01;
    emit_u8(j, rex);
    emit_u8(j, 0x39);
    emit_u8(j, (uint8_t)(0xC0 | ((r2 & 7) << 3) | (r1 & 7)));
}

void emit_test_rr(JITBuffer* j, int r1, int r2) {
    uint8_t rex = 0x48;
    if (r2 >= 8) rex |= 0x04;
    if (r1 >= 8) rex |= 0x01;
    emit_u8(j, rex);
    emit_u8(j, 0x85);
    emit_u8(j, (uint8_t)(0xC0 | ((r2 & 7) << 3) | (r1 & 7)));
}

static void emit_and_rr(JITBuffer* j, int dst, int src) {
    uint8_t rex = 0x48 | (src >= 8 ? 4 : 0) | (dst >= 8 ? 1 : 0);
    emit_u8(j, rex); emit_u8(j, 0x21);
    emit_u8(j, (uint8_t)(0xC0 | ((src & 7) << 3) | (dst & 7)));
}

static void emit_or_rr(JITBuffer* j, int dst, int src) {
    uint8_t rex = 0x48 | (src >= 8 ? 4 : 0) | (dst >= 8 ? 1 : 0);
    emit_u8(j, rex); emit_u8(j, 0x09);
    emit_u8(j, (uint8_t)(0xC0 | ((src & 7) << 3) | (dst & 7)));
}

static void emit_xor_rr(JITBuffer* j, int dst, int src) {
    uint8_t rex = 0x48 | (src >= 8 ? 4 : 0) | (dst >= 8 ? 1 : 0);
    emit_u8(j, rex); emit_u8(j, 0x31);
    emit_u8(j, (uint8_t)(0xC0 | ((src & 7) << 3) | (dst & 7)));
}

static void emit_not_r(JITBuffer* j, int reg) {
    emit_u8(j, (uint8_t)(0x48 | (reg >= 8 ? 1 : 0)));
    emit_u8(j, 0xF7);
    emit_u8(j, (uint8_t)(0xD0 | (reg & 7)));
}

static void emit_shl_cl(JITBuffer* j, int reg) {
    emit_u8(j, (uint8_t)(0x48 | (reg >= 8 ? 1 : 0)));
    emit_u8(j, 0xD3);
    emit_u8(j, (uint8_t)(0xE0 | (reg & 7)));
}

static void emit_sar_cl(JITBuffer* j, int reg) {
    emit_u8(j, (uint8_t)(0x48 | (reg >= 8 ? 1 : 0)));
    emit_u8(j, 0xD3);
    emit_u8(j, (uint8_t)(0xF8 | (reg & 7)));
}

static void emit_or_ri32(JITBuffer* j, int reg, int32_t imm) {
    emit_u8(j, (uint8_t)(0x48 | (reg >= 8 ? 1 : 0)));
    if (imm >= -128 && imm <= 127) {
        emit_u8(j, 0x83);
        emit_u8(j, (uint8_t)(0xC8 | (reg & 7)));
        emit_u8(j, (uint8_t)(int8_t)imm);
    } else {
        emit_u8(j, 0x81);
        emit_u8(j, (uint8_t)(0xC8 | (reg & 7)));
        emit_u32(j, (uint32_t)imm);
    }
}

static void emit_xor_ri32(JITBuffer* j, int reg, int32_t imm) {
    emit_u8(j, (uint8_t)(0x48 | (reg >= 8 ? 1 : 0)));
    if (imm >= -128 && imm <= 127) {
        emit_u8(j, 0x83);
        emit_u8(j, (uint8_t)(0xF0 | (reg & 7)));
        emit_u8(j, (uint8_t)(int8_t)imm);
    } else {
        emit_u8(j, 0x81);
        emit_u8(j, (uint8_t)(0xF0 | (reg & 7)));
        emit_u32(j, (uint32_t)imm);
    }
}

static void emit_setcc(JITBuffer* j, uint8_t cond_byte, int dst_reg8) {
    (void)dst_reg8;
    emit_u8(j, 0x0F);
    emit_u8(j, cond_byte);
    emit_u8(j, 0xC0); /* AL */
}

static void emit_movzx_r64_r8(JITBuffer* j, int dst, int src8) {
    (void)src8;
    uint8_t rex = 0x48 | (dst >= 8 ? 4 : 0);
    emit_u8(j, rex);
    emit_u8(j, 0x0F);
    emit_u8(j, 0xB6);
    emit_u8(j, (uint8_t)(0xC0 | ((dst & 7) << 3) | 0));
}

void emit_mov_ri64(JITBuffer* j, int reg, int64_t val) {
    if (val == 0) {
        if (reg >= 8) emit_u8(j, 0x45);
        emit_u8(j, 0x31);
        emit_u8(j, (uint8_t)(0xC0 | ((reg & 7) << 3) | (reg & 7)));
    } else if (val > 0 && val <= 0xFFFFFFFFLL) {
        if (reg >= 8) emit_u8(j, 0x41);
        emit_u8(j, (uint8_t)(0xB8 | (reg & 7)));
        emit_u32(j, (uint32_t)val);
    } else if (val >= -2147483648LL && val < 0) {
        uint8_t rex = 0x48 | (reg >= 8 ? 1 : 0);
        emit_u8(j, rex);
        emit_u8(j, 0xC7);
        emit_u8(j, (uint8_t)(0xC0 | (reg & 7)));
        emit_u32(j, (uint32_t)(int32_t)val);
    } else {
        uint8_t rex = 0x48 | (reg >= 8 ? 1 : 0);
        emit_u8(j, rex);
        emit_u8(j, (uint8_t)(0xB8 | (reg & 7)));
        emit_u64(j, (uint64_t)val);
    }
}

void emit_add_ri(JITBuffer* j, int rd, int32_t imm) {
    uint8_t rex = 0x48 | (rd >= 8 ? 1 : 0);
    emit_u8(j, rex);
    if (imm >= -128 && imm <= 127) {
        emit_u8(j, 0x83);
        emit_u8(j, (uint8_t)(0xC0 | (rd & 7)));
        emit_u8(j, (uint8_t)(int8_t)imm);
    } else {
        emit_u8(j, 0x81);
        emit_u8(j, (uint8_t)(0xC0 | (rd & 7)));
        emit_u32(j, (uint32_t)imm);
    }
}

void emit_sub_ri(JITBuffer* j, int rd, int32_t imm) {
    uint8_t rex = 0x48 | (rd >= 8 ? 1 : 0);
    emit_u8(j, rex);
    if (imm >= -128 && imm <= 127) {
        emit_u8(j, 0x83);
        emit_u8(j, (uint8_t)(0xE8 | (rd & 7)));
        emit_u8(j, (uint8_t)(int8_t)imm);
    } else {
        emit_u8(j, 0x81);
        emit_u8(j, (uint8_t)(0xE8 | (rd & 7)));
        emit_u32(j, (uint32_t)imm);
    }
}

void emit_cmp_ri(JITBuffer* j, int r1, int32_t imm) {
    uint8_t rex = 0x48 | (r1 >= 8 ? 1 : 0);
    emit_u8(j, rex);
    if (imm >= -128 && imm <= 127) {
        emit_u8(j, 0x83);
        emit_u8(j, (uint8_t)(0xF8 | (r1 & 7)));
        emit_u8(j, (uint8_t)(int8_t)imm);
    } else {
        emit_u8(j, 0x81);
        emit_u8(j, (uint8_t)(0xF8 | (r1 & 7)));
        emit_u32(j, (uint32_t)imm);
    }
}

void emit_shl_ri(JITBuffer* j, int rd, int shift) {
    uint8_t rex = 0x48 | (rd >= 8 ? 1 : 0);
    emit_u8(j, rex);
    if (shift == 1) {
        emit_u8(j, 0xD1);
        emit_u8(j, (uint8_t)(0xE0 | (rd & 7)));
    } else {
        emit_u8(j, 0xC1);
        emit_u8(j, (uint8_t)(0xE0 | (rd & 7)));
        emit_u8(j, (uint8_t)shift);
    }
}

static void emit_imul_rri32(JITBuffer* j, int rd, int r1, int32_t imm) {
    uint8_t rex = 0x48 | ((rd >= 8 ? 4 : 0) | (r1 >= 8 ? 1 : 0));
    emit_u8(j, rex);
    if (imm >= -128 && imm <= 127) {
        emit_u8(j, 0x6B);
        emit_u8(j, (uint8_t)(0xC0 | ((rd & 7) << 3) | (r1 & 7)));
        emit_u8(j, (uint8_t)(int8_t)imm);
    } else {
        emit_u8(j, 0x69);
        emit_u8(j, (uint8_t)(0xC0 | ((rd & 7) << 3) | (r1 & 7)));
        emit_u32(j, (uint32_t)imm);
    }
}

static void emit_lea_disp(JITBuffer* j, int rd, int base, int32_t disp) {
    uint8_t rex = 0x48 | ((rd >= 8 ? 4 : 0) | (base >= 8 ? 1 : 0));
    emit_u8(j, rex);
    emit_u8(j, 0x8D);
    uint8_t mod = (disp >= -128 && disp <= 127) ? 0x40 : 0x80;
    if ((base & 7) == 4) {
        emit_u8(j, (uint8_t)(mod | ((rd & 7) << 3) | 4));
        emit_u8(j, 0x24);
    } else {
        emit_u8(j, (uint8_t)(mod | ((rd & 7) << 3) | (base & 7)));
    }
    if (mod == 0x40) emit_u8(j, (uint8_t)(int8_t)disp);
    else emit_u32(j, (uint32_t)disp);
}

void emit_lea_sib(JITBuffer* j, int rd, int base, int idx, int scale) {
    uint8_t rex = 0x48 | ((rd >= 8 ? 4 : 0) | (idx >= 8 ? 2 : 0) | (base >= 8 ? 1 : 0));
    uint8_t sb = (scale == 1) ? 0x00 : ((scale == 2) ? 0x40 : ((scale == 4) ? 0x80 : 0xC0));
    emit_u8(j, rex);
    emit_u8(j, 0x8D);
    if ((base & 7) == 5) {
        emit_u8(j, (uint8_t)(0x44 | ((rd & 7) << 3)));
        emit_u8(j, (uint8_t)(sb | ((idx & 7) << 3) | 5));
        emit_u8(j, 0x00);
    } else {
        emit_u8(j, (uint8_t)(0x04 | ((rd & 7) << 3)));
        emit_u8(j, (uint8_t)(sb | ((idx & 7) << 3) | (base & 7)));
    }
}

static void emit_cmovge_rr(JITBuffer* j, int dst, int src) {
    uint8_t rex = 0x48 | ((dst >= 8 ? 4 : 0) | (src >= 8 ? 1 : 0));
    emit_u8(j, rex);
    emit_u8(j, 0x0F);
    emit_u8(j, 0x4D);
    emit_u8(j, (uint8_t)(0xC0 | ((dst & 7) << 3) | (src & 7)));
}

void emit_add_mem_rbp(JITBuffer* j, int32_t disp, int src) {
    uint8_t rex = 0x48 | (src >= 8 ? 4 : 0);
    emit_u8(j, rex);
    emit_u8(j, 0x01);
    if (disp >= -128 && disp <= 127) {
        emit_u8(j, (uint8_t)(0x45 | ((src & 7) << 3)));
        emit_u8(j, (uint8_t)(int8_t)disp);
    } else {
        emit_u8(j, (uint8_t)(0x85 | ((src & 7) << 3)));
        emit_u32(j, (uint32_t)disp);
    }
}

void emit_cmp_reg_rbp(JITBuffer* j, int reg, int32_t disp) {
    uint8_t rex = 0x48 | (reg >= 8 ? 4 : 0);
    emit_u8(j, rex);
    emit_u8(j, 0x3B);
    if (disp >= -128 && disp <= 127) {
        emit_u8(j, (uint8_t)(0x45 | ((reg & 7) << 3)));
        emit_u8(j, (uint8_t)(int8_t)disp);
    } else {
        emit_u8(j, (uint8_t)(0x85 | ((reg & 7) << 3)));
        emit_u32(j, (uint32_t)disp);
    }
}

static void emit_load_spill(JITBuffer* j, int reg, int spill_offset) {
    uint8_t rex = 0x48 | (reg >= 8 ? 4 : 0);
    int32_t disp = -spill_offset;
    emit_u8(j, rex);
    emit_u8(j, 0x8B);
    if (disp >= -128 && disp <= 127) {
        emit_u8(j, (uint8_t)(0x40 | ((reg & 7) << 3) | 5));
        emit_u8(j, (uint8_t)(int8_t)disp);
    } else {
        emit_u8(j, (uint8_t)(0x80 | ((reg & 7) << 3) | 5));
        emit_u32(j, (uint32_t)disp);
    }
}

static void emit_store_spill(JITBuffer* j, int reg, int spill_offset) {
    uint8_t rex = 0x48 | (reg >= 8 ? 4 : 0);
    int32_t disp = -spill_offset;
    emit_u8(j, rex);
    emit_u8(j, 0x89);
    if (disp >= -128 && disp <= 127) {
        emit_u8(j, (uint8_t)(0x40 | ((reg & 7) << 3) | 5));
        emit_u8(j, (uint8_t)(int8_t)disp);
    } else {
        emit_u8(j, (uint8_t)(0x80 | ((reg & 7) << 3) | 5));
        emit_u32(j, (uint32_t)disp);
    }
}

static void emit_loadss_spill(JITBuffer* j, int xmm_reg, int spill_offset) {
    int32_t disp = -spill_offset;
    emit_u8(j, 0xF3);
    if (xmm_reg >= 8) emit_u8(j, 0x44);
    emit_u8(j, 0x0F);
    emit_u8(j, 0x10);
    if (disp >= -128 && disp <= 127) {
        emit_u8(j, (uint8_t)(0x40 | ((xmm_reg & 7) << 3) | 5));
        emit_u8(j, (uint8_t)(int8_t)disp);
    } else {
        emit_u8(j, (uint8_t)(0x80 | ((xmm_reg & 7) << 3) | 5));
        emit_u32(j, (uint32_t)disp);
    }
}

static void emit_storess_spill(JITBuffer* j, int xmm_reg, int spill_offset) {
    int32_t disp = -spill_offset;
    emit_u8(j, 0xF3);
    if (xmm_reg >= 8) emit_u8(j, 0x44);
    emit_u8(j, 0x0F);
    emit_u8(j, 0x11);
    if (disp >= -128 && disp <= 127) {
        emit_u8(j, (uint8_t)(0x40 | ((xmm_reg & 7) << 3) | 5));
        emit_u8(j, (uint8_t)(int8_t)disp);
    } else {
        emit_u8(j, (uint8_t)(0x80 | ((xmm_reg & 7) << 3) | 5));
        emit_u32(j, (uint32_t)disp);
    }
}

static void emit_loadsd_spill(JITBuffer* j, int xmm_reg, int spill_offset) {
    int32_t disp = -spill_offset;
    emit_u8(j, 0xF2);
    if (xmm_reg >= 8) emit_u8(j, 0x44);
    emit_u8(j, 0x0F);
    emit_u8(j, 0x10);
    if (disp >= -128 && disp <= 127) {
        emit_u8(j, (uint8_t)(0x40 | ((xmm_reg & 7) << 3) | 5));
        emit_u8(j, (uint8_t)(int8_t)disp);
    } else {
        emit_u8(j, (uint8_t)(0x80 | ((xmm_reg & 7) << 3) | 5));
        emit_u32(j, (uint32_t)disp);
    }
}

static void emit_storesd_spill(JITBuffer* j, int xmm_reg, int spill_offset) {
    int32_t disp = -spill_offset;
    emit_u8(j, 0xF2);
    if (xmm_reg >= 8) emit_u8(j, 0x44);
    emit_u8(j, 0x0F);
    emit_u8(j, 0x11);
    if (disp >= -128 && disp <= 127) {
        emit_u8(j, (uint8_t)(0x40 | ((xmm_reg & 7) << 3) | 5));
        emit_u8(j, (uint8_t)(int8_t)disp);
    } else {
        emit_u8(j, (uint8_t)(0x80 | ((xmm_reg & 7) << 3) | 5));
        emit_u32(j, (uint32_t)disp);
    }
}

void emit_load_mem_offset(JITBuffer* j, int dst, int base, int32_t offset) {
    uint8_t rex = 0x48 | ((dst >= 8 ? 4 : 0) | (base >= 8 ? 1 : 0));
    uint8_t mod;
    if (offset == 0 && (base & 7) != 5) mod = 0x00;
    else if (offset >= -128 && offset <= 127) mod = 0x40;
    else mod = 0x80;
    emit_u8(j, rex);
    emit_u8(j, 0x8B);
    if ((base & 7) == 4) {
        emit_u8(j, (uint8_t)(mod | ((dst & 7) << 3) | 4));
        emit_u8(j, 0x24);
    } else {
        emit_u8(j, (uint8_t)(mod | ((dst & 7) << 3) | (base & 7)));
    }
    if (mod == 0x40) emit_u8(j, (uint8_t)(int8_t)offset);
    else if (mod == 0x80) emit_u32(j, (uint32_t)offset);
}

void emit_store_mem_offset(JITBuffer* j, int base, int32_t offset, int src) {
    uint8_t rex = 0x48 | ((src >= 8 ? 4 : 0) | (base >= 8 ? 1 : 0));
    uint8_t mod;
    if (offset == 0 && (base & 7) != 5) mod = 0x00;
    else if (offset >= -128 && offset <= 127) mod = 0x40;
    else mod = 0x80;
    emit_u8(j, rex);
    emit_u8(j, 0x89);
    if ((base & 7) == 4) {
        emit_u8(j, (uint8_t)(mod | ((src & 7) << 3) | 4));
        emit_u8(j, 0x24);
    } else {
        emit_u8(j, (uint8_t)(mod | ((src & 7) << 3) | (base & 7)));
    }
    if (mod == 0x40) emit_u8(j, (uint8_t)(int8_t)offset);
    else if (mod == 0x80) emit_u32(j, (uint32_t)offset);
}

static void emit_loadss_offset(JITBuffer* j, int dst_xmm, int base, int32_t offset) {
    emit_u8(j, 0xF3);
    uint8_t rex = 0x40 | ((dst_xmm >= 8 ? 4 : 0) | (base >= 8 ? 1 : 0));
    if (rex != 0x40) emit_u8(j, rex);
    emit_u8(j, 0x0F);
    emit_u8(j, 0x10);
    if (base == PREG_RSP || base == PREG_R12) {
        emit_u8(j, (uint8_t)(0x84 | ((dst_xmm & 7) << 3)));
        emit_u8(j, 0x24);
    } else {
        emit_u8(j, (uint8_t)(0x80 | ((dst_xmm & 7) << 3) | (base & 7)));
    }
    emit_u32(j, (uint32_t)offset);
}

static void emit_loadsd_offset(JITBuffer* j, int dst_xmm, int base, int32_t offset) {
    emit_u8(j, 0xF2);
    uint8_t rex = 0x40 | ((dst_xmm >= 8 ? 4 : 0) | (base >= 8 ? 1 : 0));
    if (rex != 0x40) emit_u8(j, rex);
    emit_u8(j, 0x0F);
    emit_u8(j, 0x10);
    if (base == PREG_RSP || base == PREG_R12) {
        emit_u8(j, (uint8_t)(0x84 | ((dst_xmm & 7) << 3)));
        emit_u8(j, 0x24);
    } else {
        emit_u8(j, (uint8_t)(0x80 | ((dst_xmm & 7) << 3) | (base & 7)));
    }
    emit_u32(j, (uint32_t)offset);
}

static void emit_storess_offset(JITBuffer* j, int base, int32_t offset, int src_xmm) {
    emit_u8(j, 0xF3);
    uint8_t rex = 0x40 | ((src_xmm >= 8 ? 4 : 0) | (base >= 8 ? 1 : 0));
    if (rex != 0x40) emit_u8(j, rex);
    emit_u8(j, 0x0F);
    emit_u8(j, 0x11);
    if (base == PREG_RSP || base == PREG_R12) {
        emit_u8(j, (uint8_t)(0x84 | ((src_xmm & 7) << 3)));
        emit_u8(j, 0x24);
    } else {
        emit_u8(j, (uint8_t)(0x80 | ((src_xmm & 7) << 3) | (base & 7)));
    }
    emit_u32(j, (uint32_t)offset);
}

static void emit_storesd_offset(JITBuffer* j, int base, int32_t offset, int src_xmm) {
    emit_u8(j, 0xF2);
    uint8_t rex = 0x40 | ((src_xmm >= 8 ? 4 : 0) | (base >= 8 ? 1 : 0));
    if (rex != 0x40) emit_u8(j, rex);
    emit_u8(j, 0x0F);
    emit_u8(j, 0x11);
    if (base == PREG_RSP || base == PREG_R12) {
        emit_u8(j, (uint8_t)(0x84 | ((src_xmm & 7) << 3)));
        emit_u8(j, 0x24);
    } else {
        emit_u8(j, (uint8_t)(0x80 | ((src_xmm & 7) << 3) | (base & 7)));
    }
    emit_u32(j, (uint32_t)offset);
}

static void emit_load_mem32s_offset(JITBuffer* j, int dst, int base, int32_t offset) {
    uint8_t rex = 0x48 | ((dst >= 8 ? 4 : 0) | (base >= 8 ? 1 : 0));
    emit_u8(j, rex);
    emit_u8(j, 0x63); /* movsxd r64, dword [base + offset] */
    if (base == PREG_RSP || base == PREG_R12) {
        emit_u8(j, (uint8_t)(0x84 | ((dst & 7) << 3)));
        emit_u8(j, 0x24);
    } else {
        emit_u8(j, (uint8_t)(0x80 | ((dst & 7) << 3) | (base & 7)));
    }
    emit_u32(j, (uint32_t)offset);
}

static void emit_store_mem32_offset(JITBuffer* j, int base, int32_t offset, int src) {
    uint8_t rex = 0x40 | ((src >= 8 ? 4 : 0) | (base >= 8 ? 1 : 0));
    if (rex != 0x40) emit_u8(j, rex);
    emit_u8(j, 0x89);
    if (base == PREG_RSP || base == PREG_R12) {
        emit_u8(j, (uint8_t)(0x84 | ((src & 7) << 3)));
        emit_u8(j, 0x24);
    } else {
        emit_u8(j, (uint8_t)(0x80 | ((src & 7) << 3) | (base & 7)));
    }
    emit_u32(j, (uint32_t)offset);
}

static void emit_load_mem32s_array(JITBuffer* j, int dst, int base, int idx, int scale) {
    uint8_t rex = 0x48 | ((dst >= 8 ? 4 : 0) | (idx >= 8 ? 2 : 0) | (base >= 8 ? 1 : 0));
    emit_u8(j, rex);
    emit_u8(j, 0x63);
    uint8_t scale_bits = (scale == 4) ? 0x80 : 0xC0;
    emit_u8(j, (uint8_t)(0x44 | ((dst & 7) << 3)));
    emit_u8(j, (uint8_t)(scale_bits | ((idx & 7) << 3) | (base & 7)));
    emit_u8(j, 0x00);
}

static void emit_store_mem32_array(JITBuffer* j, int base, int idx, int src, int scale) {
    uint8_t rex = 0x40 | ((src >= 8 ? 4 : 0) | (idx >= 8 ? 2 : 0) | (base >= 8 ? 1 : 0));
    if (rex != 0x40) emit_u8(j, rex);
    emit_u8(j, 0x89);
    uint8_t scale_bits = (scale == 4) ? 0x80 : 0xC0;
    emit_u8(j, (uint8_t)(0x44 | ((src & 7) << 3)));
    emit_u8(j, (uint8_t)(scale_bits | ((idx & 7) << 3) | (base & 7)));
    emit_u8(j, 0x00);
}

static void emit_cvtsi2ss_xr(JITBuffer* j, int dst_xmm, int src_gpr) {
    emit_u8(j, 0xF3);
    uint8_t rex = 0x48 | ((dst_xmm >= 8 ? 4 : 0) | (src_gpr >= 8 ? 1 : 0));
    emit_u8(j, rex);
    emit_u8(j, 0x0F);
    emit_u8(j, 0x2A);
    emit_u8(j, (uint8_t)(0xC0 | ((dst_xmm & 7) << 3) | (src_gpr & 7)));
}

static void emit_cvttss2si_rx(JITBuffer* j, int dst_gpr, int src_xmm) {
    emit_u8(j, 0xF3);
    uint8_t rex = 0x48 | ((dst_gpr >= 8 ? 4 : 0) | (src_xmm >= 8 ? 1 : 0));
    emit_u8(j, rex);
    emit_u8(j, 0x0F);
    emit_u8(j, 0x2C);
    emit_u8(j, (uint8_t)(0xC0 | ((dst_gpr & 7) << 3) | (src_xmm & 7)));
}

static void emit_cvtsi2sd_xr(JITBuffer* j, int dst_xmm, int src_gpr) {
    emit_u8(j, 0xF2);
    uint8_t rex = 0x48 | ((dst_xmm >= 8 ? 4 : 0) | (src_gpr >= 8 ? 1 : 0));
    emit_u8(j, rex);
    emit_u8(j, 0x0F);
    emit_u8(j, 0x2A);
    emit_u8(j, (uint8_t)(0xC0 | ((dst_xmm & 7) << 3) | (src_gpr & 7)));
}

static void emit_cvttsd2si_rx(JITBuffer* j, int dst_gpr, int src_xmm) {
    emit_u8(j, 0xF2);
    uint8_t rex = 0x48 | ((dst_gpr >= 8 ? 4 : 0) | (src_xmm >= 8 ? 1 : 0));
    emit_u8(j, rex);
    emit_u8(j, 0x0F);
    emit_u8(j, 0x2C);
    emit_u8(j, (uint8_t)(0xC0 | ((dst_gpr & 7) << 3) | (src_xmm & 7)));
}

static void emit_cvtss2sd_xx(JITBuffer* j, int dst_xmm, int src_xmm) {
    emit_u8(j, 0xF3);
    if (dst_xmm >= 8 || src_xmm >= 8) {
        emit_u8(j, (uint8_t)(0x40 | (dst_xmm >= 8 ? 4 : 0) | (src_xmm >= 8 ? 1 : 0)));
    }
    emit_u8(j, 0x0F);
    emit_u8(j, 0x5A);
    emit_u8(j, (uint8_t)(0xC0 | ((dst_xmm & 7) << 3) | (src_xmm & 7)));
}

static void emit_cvtsd2ss_xx(JITBuffer* j, int dst_xmm, int src_xmm) {
    emit_u8(j, 0xF2);
    if (dst_xmm >= 8 || src_xmm >= 8) {
        emit_u8(j, (uint8_t)(0x40 | (dst_xmm >= 8 ? 4 : 0) | (src_xmm >= 8 ? 1 : 0)));
    }
    emit_u8(j, 0x0F);
    emit_u8(j, 0x5A);
    emit_u8(j, (uint8_t)(0xC0 | ((dst_xmm & 7) << 3) | (src_xmm & 7)));
}

static void emit_sub_rsp(JITBuffer* j, int32_t bytes) {
    if (bytes <= 0) return;
    if (bytes <= 127) {
        emit_u8(j, 0x48); emit_u8(j, 0x83); emit_u8(j, 0xEC); emit_u8(j, (uint8_t)bytes);
    } else {
        emit_u8(j, 0x48); emit_u8(j, 0x81); emit_u8(j, 0xEC); emit_u32(j, (uint32_t)bytes);
    }
}

static void emit_load_mem_array(JITBuffer* j, int dst, int base, int idx, int scale) {
    uint8_t rex = 0x48 | ((dst >= 8 ? 4 : 0) | (idx >= 8 ? 2 : 0) | (base >= 8 ? 1 : 0));
    emit_u8(j, rex);
    emit_u8(j, 0x8B);
    uint8_t scale_bits = (scale == 4) ? 0x80 : 0xC0;
    emit_u8(j, (uint8_t)(0x44 | ((dst & 7) << 3)));
    emit_u8(j, (uint8_t)(scale_bits | ((idx & 7) << 3) | (base & 7)));
    emit_u8(j, 0x00);
}

static void emit_store_mem_array(JITBuffer* j, int base, int idx, int src, int scale) {
    uint8_t rex = 0x48 | ((src >= 8 ? 4 : 0) | (idx >= 8 ? 2 : 0) | (base >= 8 ? 1 : 0));
    emit_u8(j, rex);
    emit_u8(j, 0x89);
    uint8_t scale_bits = (scale == 4) ? 0x80 : 0xC0;
    emit_u8(j, (uint8_t)(0x44 | ((src & 7) << 3)));
    emit_u8(j, (uint8_t)(scale_bits | ((idx & 7) << 3) | (base & 7)));
    emit_u8(j, 0x00);
}

static void emit_loadss_array(JITBuffer* j, int dst_xmm, int base, int idx, int scale) {
    emit_u8(j, 0xF3);
    uint8_t rex = 0x40 | ((dst_xmm >= 8 ? 4 : 0) | (idx >= 8 ? 2 : 0) | (base >= 8 ? 1 : 0));
    if (rex != 0x40) emit_u8(j, rex);
    emit_u8(j, 0x0F);
    emit_u8(j, 0x10);
    uint8_t scale_bits = (scale == 4) ? 0x80 : 0xC0;
    emit_u8(j, (uint8_t)(0x44 | ((dst_xmm & 7) << 3)));
    emit_u8(j, (uint8_t)(scale_bits | ((idx & 7) << 3) | (base & 7)));
    emit_u8(j, 0x00);
}

static void emit_storess_array(JITBuffer* j, int base, int idx, int src_xmm, int scale) {
    emit_u8(j, 0xF3);
    uint8_t rex = 0x40 | ((src_xmm >= 8 ? 4 : 0) | (idx >= 8 ? 2 : 0) | (base >= 8 ? 1 : 0));
    if (rex != 0x40) emit_u8(j, rex);
    emit_u8(j, 0x0F);
    emit_u8(j, 0x11);
    uint8_t scale_bits = (scale == 4) ? 0x80 : 0xC0;
    emit_u8(j, (uint8_t)(0x44 | ((src_xmm & 7) << 3)));
    emit_u8(j, (uint8_t)(scale_bits | ((idx & 7) << 3) | (base & 7)));
    emit_u8(j, 0x00);
}

static void emit_loadsd_array(JITBuffer* j, int dst_xmm, int base, int idx, int scale) {
    emit_u8(j, 0xF2);
    uint8_t rex = 0x40 | ((dst_xmm >= 8 ? 4 : 0) | (idx >= 8 ? 2 : 0) | (base >= 8 ? 1 : 0));
    if (rex != 0x40) emit_u8(j, rex);
    emit_u8(j, 0x0F);
    emit_u8(j, 0x10);
    uint8_t scale_bits = (scale == 4) ? 0x80 : 0xC0;
    emit_u8(j, (uint8_t)(0x44 | ((dst_xmm & 7) << 3)));
    emit_u8(j, (uint8_t)(scale_bits | ((idx & 7) << 3) | (base & 7)));
    emit_u8(j, 0x00);
}

static void emit_storesd_array(JITBuffer* j, int base, int idx, int src_xmm, int scale) {
    emit_u8(j, 0xF2);
    uint8_t rex = 0x40 | ((src_xmm >= 8 ? 4 : 0) | (idx >= 8 ? 2 : 0) | (base >= 8 ? 1 : 0));
    if (rex != 0x40) emit_u8(j, rex);
    emit_u8(j, 0x0F);
    emit_u8(j, 0x11);
    uint8_t scale_bits = (scale == 4) ? 0x80 : 0xC0;
    emit_u8(j, (uint8_t)(0x44 | ((src_xmm & 7) << 3)));
    emit_u8(j, (uint8_t)(scale_bits | ((idx & 7) << 3) | (base & 7)));
    emit_u8(j, 0x00);
}

void emit_movaps_xx(JITBuffer* j, int dst_xmm, int src_xmm) {
    if (dst_xmm == src_xmm) return;
    if (dst_xmm >= 8 || src_xmm >= 8) {
        emit_u8(j, (uint8_t)(0x40 | (dst_xmm >= 8 ? 4 : 0) | (src_xmm >= 8 ? 1 : 0)));
    }
    emit_u8(j, 0x0F);
    emit_u8(j, 0x28);
    emit_u8(j, (uint8_t)(0xC0 | ((dst_xmm & 7) << 3) | (src_xmm & 7)));
}

static void emit_addss_xx(JITBuffer* j, int dst_xmm, int src_xmm) {
    emit_u8(j, 0xF3);
    if (dst_xmm >= 8 || src_xmm >= 8) {
        emit_u8(j, (uint8_t)(0x40 | (dst_xmm >= 8 ? 4 : 0) | (src_xmm >= 8 ? 1 : 0)));
    }
    emit_u8(j, 0x0F);
    emit_u8(j, 0x58);
    emit_u8(j, (uint8_t)(0xC0 | ((dst_xmm & 7) << 3) | (src_xmm & 7)));
}

static void emit_subss_xx(JITBuffer* j, int dst_xmm, int src_xmm) {
    emit_u8(j, 0xF3);
    if (dst_xmm >= 8 || src_xmm >= 8) {
        emit_u8(j, (uint8_t)(0x40 | (dst_xmm >= 8 ? 4 : 0) | (src_xmm >= 8 ? 1 : 0)));
    }
    emit_u8(j, 0x0F);
    emit_u8(j, 0x5C);
    emit_u8(j, (uint8_t)(0xC0 | ((dst_xmm & 7) << 3) | (src_xmm & 7)));
}

static void emit_mulss_xx(JITBuffer* j, int dst_xmm, int src_xmm) {
    emit_u8(j, 0xF3);
    if (dst_xmm >= 8 || src_xmm >= 8) {
        emit_u8(j, (uint8_t)(0x40 | (dst_xmm >= 8 ? 4 : 0) | (src_xmm >= 8 ? 1 : 0)));
    }
    emit_u8(j, 0x0F);
    emit_u8(j, 0x59);
    emit_u8(j, (uint8_t)(0xC0 | ((dst_xmm & 7) << 3) | (src_xmm & 7)));
}

static void emit_divss_xx(JITBuffer* j, int dst_xmm, int src_xmm) {
    emit_u8(j, 0xF3);
    if (dst_xmm >= 8 || src_xmm >= 8) {
        emit_u8(j, (uint8_t)(0x40 | (dst_xmm >= 8 ? 4 : 0) | (src_xmm >= 8 ? 1 : 0)));
    }
    emit_u8(j, 0x0F);
    emit_u8(j, 0x5E);
    emit_u8(j, (uint8_t)(0xC0 | ((dst_xmm & 7) << 3) | (src_xmm & 7)));
}

static void emit_ucomiss_xx(JITBuffer* j, int x1, int x2) {
    if (x1 >= 8 || x2 >= 8) {
        emit_u8(j, (uint8_t)(0x40 | (x1 >= 8 ? 4 : 0) | (x2 >= 8 ? 1 : 0)));
    }
    emit_u8(j, 0x0F);
    emit_u8(j, 0x2E);
    emit_u8(j, (uint8_t)(0xC0 | ((x1 & 7) << 3) | (x2 & 7)));
}

static void emit_addsd_xx(JITBuffer* j, int dst_xmm, int src_xmm) {
    emit_u8(j, 0xF2);
    if (dst_xmm >= 8 || src_xmm >= 8) {
        emit_u8(j, (uint8_t)(0x40 | (dst_xmm >= 8 ? 4 : 0) | (src_xmm >= 8 ? 1 : 0)));
    }
    emit_u8(j, 0x0F);
    emit_u8(j, 0x58);
    emit_u8(j, (uint8_t)(0xC0 | ((dst_xmm & 7) << 3) | (src_xmm & 7)));
}

static void emit_subsd_xx(JITBuffer* j, int dst_xmm, int src_xmm) {
    emit_u8(j, 0xF2);
    if (dst_xmm >= 8 || src_xmm >= 8) {
        emit_u8(j, (uint8_t)(0x40 | (dst_xmm >= 8 ? 4 : 0) | (src_xmm >= 8 ? 1 : 0)));
    }
    emit_u8(j, 0x0F);
    emit_u8(j, 0x5C);
    emit_u8(j, (uint8_t)(0xC0 | ((dst_xmm & 7) << 3) | (src_xmm & 7)));
}

static void emit_mulsd_xx(JITBuffer* j, int dst_xmm, int src_xmm) {
    emit_u8(j, 0xF2);
    if (dst_xmm >= 8 || src_xmm >= 8) {
        emit_u8(j, (uint8_t)(0x40 | (dst_xmm >= 8 ? 4 : 0) | (src_xmm >= 8 ? 1 : 0)));
    }
    emit_u8(j, 0x0F);
    emit_u8(j, 0x59);
    emit_u8(j, (uint8_t)(0xC0 | ((dst_xmm & 7) << 3) | (src_xmm & 7)));
}

static void emit_divsd_xx(JITBuffer* j, int dst_xmm, int src_xmm) {
    emit_u8(j, 0xF2);
    if (dst_xmm >= 8 || src_xmm >= 8) {
        emit_u8(j, (uint8_t)(0x40 | (dst_xmm >= 8 ? 4 : 0) | (src_xmm >= 8 ? 1 : 0)));
    }
    emit_u8(j, 0x0F);
    emit_u8(j, 0x5E);
    emit_u8(j, (uint8_t)(0xC0 | ((dst_xmm & 7) << 3) | (src_xmm & 7)));
}

static void emit_ucomisd_xx(JITBuffer* j, int x1, int x2) {
    emit_u8(j, 0x66);
    if (x1 >= 8 || x2 >= 8) {
        emit_u8(j, (uint8_t)(0x40 | (x1 >= 8 ? 4 : 0) | (x2 >= 8 ? 1 : 0)));
    }
    emit_u8(j, 0x0F);
    emit_u8(j, 0x2E);
    emit_u8(j, (uint8_t)(0xC0 | ((x1 & 7) << 3) | (x2 & 7)));
}

void emit_load_f32_imm_to(JITBuffer* j, float val, int xmm_reg) {
    uint32_t bits;
    memcpy(&bits, &val, 4);
    emit_u8(j, 0x41); emit_u8(j, 0xBA); /* mov r10d, bits */
    emit_u32(j, bits);
    emit_u8(j, 0x66);
    uint8_t rex = 0x40 | (xmm_reg >= 8 ? 4 : 0) | 1; /* 1 for R10 */
    emit_u8(j, rex);
    emit_u8(j, 0x0F);
    emit_u8(j, 0x6E);
    uint8_t modrm = (uint8_t)(0xC0 | ((xmm_reg & 7) << 3) | 2); /* 2 = R10 & 7 */
    emit_u8(j, modrm);
}

static void emit_load_f64_imm_to(JITBuffer* j, double val, int xmm_reg) {
    uint64_t bits;
    memcpy(&bits, &val, 8);
    emit_mov_ri64(j, PREG_R10, (int64_t)bits);
    emit_u8(j, 0x66);
    uint8_t rex = (uint8_t)(0x48 | (xmm_reg >= 8 ? 4 : 0) | 1); /* 1 for R10 */
    emit_u8(j, rex);
    emit_u8(j, 0x0F);
    emit_u8(j, 0x6E);
    emit_u8(j, (uint8_t)(0xC0 | ((xmm_reg & 7) << 3) | 2));
}

#ifdef _WIN32
static void emit_call_with_args(JITBuffer* j, void* func_ptr, int num_args) {
    if (num_args == 0) {
        emit_u8(j, 0x48); emit_u8(j, 0x83); emit_u8(j, 0xEC); emit_u8(j, 0x20);
        emit_u8(j, 0x48); emit_u8(j, 0xB8); emit_u64(j, (uint64_t)func_ptr);
        emit_u8(j, 0xFF); emit_u8(j, 0xD0);
        emit_u8(j, 0x48); emit_u8(j, 0x83); emit_u8(j, 0xC4); emit_u8(j, 0x20);
        return;
    }
    if (num_args == 1) {
        emit_u8(j, 0x59);
        emit_u8(j, 0x48); emit_u8(j, 0x83); emit_u8(j, 0xEC); emit_u8(j, 0x20);
        emit_u8(j, 0x48); emit_u8(j, 0xB8); emit_u64(j, (uint64_t)func_ptr);
        emit_u8(j, 0xFF); emit_u8(j, 0xD0);
        emit_u8(j, 0x48); emit_u8(j, 0x83); emit_u8(j, 0xC4); emit_u8(j, 0x20);
        return;
    }
    if (num_args == 2) {
        emit_u8(j, 0x5A);
        emit_u8(j, 0x59);
        emit_u8(j, 0x48); emit_u8(j, 0x83); emit_u8(j, 0xEC); emit_u8(j, 0x20);
        emit_u8(j, 0x48); emit_u8(j, 0xB8); emit_u64(j, (uint64_t)func_ptr);
        emit_u8(j, 0xFF); emit_u8(j, 0xD0);
        emit_u8(j, 0x48); emit_u8(j, 0x83); emit_u8(j, 0xC4); emit_u8(j, 0x20);
        return;
    }
    if (num_args == 3) {
        emit_u8(j, 0x41); emit_u8(j, 0x58);
        emit_u8(j, 0x5A);
        emit_u8(j, 0x59);
        emit_u8(j, 0x48); emit_u8(j, 0x83); emit_u8(j, 0xEC); emit_u8(j, 0x20);
        emit_u8(j, 0x48); emit_u8(j, 0xB8); emit_u64(j, (uint64_t)func_ptr);
        emit_u8(j, 0xFF); emit_u8(j, 0xD0);
        emit_u8(j, 0x48); emit_u8(j, 0x83); emit_u8(j, 0xC4); emit_u8(j, 0x20);
        return;
    }
    if (num_args == 4) {
        emit_u8(j, 0x41); emit_u8(j, 0x59);
        emit_u8(j, 0x41); emit_u8(j, 0x58);
        emit_u8(j, 0x5A);
        emit_u8(j, 0x59);
        emit_u8(j, 0x48); emit_u8(j, 0x83); emit_u8(j, 0xEC); emit_u8(j, 0x20);
        emit_u8(j, 0x48); emit_u8(j, 0xB8); emit_u64(j, (uint64_t)func_ptr);
        emit_u8(j, 0xFF); emit_u8(j, 0xD0);
        emit_u8(j, 0x48); emit_u8(j, 0x83); emit_u8(j, 0xC4); emit_u8(j, 0x20);
        return;
    }
    if (num_args == 5) {
        emit_u8(j, 0x41); emit_u8(j, 0x5A);
        emit_u8(j, 0x41); emit_u8(j, 0x59);
        emit_u8(j, 0x41); emit_u8(j, 0x58);
        emit_u8(j, 0x5A);
        emit_u8(j, 0x59);
        emit_u8(j, 0x48); emit_u8(j, 0x83); emit_u8(j, 0xEC); emit_u8(j, 0x30);
        emit_u8(j, 0x4C); emit_u8(j, 0x89); emit_u8(j, 0x54); emit_u8(j, 0x24); emit_u8(j, 0x20);
        emit_u8(j, 0x48); emit_u8(j, 0xB8); emit_u64(j, (uint64_t)func_ptr);
        emit_u8(j, 0xFF); emit_u8(j, 0xD0);
        emit_u8(j, 0x48); emit_u8(j, 0x83); emit_u8(j, 0xC4); emit_u8(j, 0x30);
        return;
    }
    if (num_args == 6) {
        emit_u8(j, 0x41); emit_u8(j, 0x5B);
        emit_u8(j, 0x41); emit_u8(j, 0x5A);
        emit_u8(j, 0x41); emit_u8(j, 0x59);
        emit_u8(j, 0x41); emit_u8(j, 0x58);
        emit_u8(j, 0x5A);
        emit_u8(j, 0x59);
        emit_u8(j, 0x48); emit_u8(j, 0x83); emit_u8(j, 0xEC); emit_u8(j, 0x30);
        emit_u8(j, 0x4C); emit_u8(j, 0x89); emit_u8(j, 0x54); emit_u8(j, 0x24); emit_u8(j, 0x20);
        emit_u8(j, 0x4C); emit_u8(j, 0x89); emit_u8(j, 0x5C); emit_u8(j, 0x24); emit_u8(j, 0x28);
        emit_u8(j, 0x48); emit_u8(j, 0xB8); emit_u64(j, (uint64_t)func_ptr);
        emit_u8(j, 0xFF); emit_u8(j, 0xD0);
        emit_u8(j, 0x48); emit_u8(j, 0x83); emit_u8(j, 0xC4); emit_u8(j, 0x30);
        return;
    }
    if (num_args == 7) {
        emit_u8(j, 0x41); emit_u8(j, 0x5A);
        emit_u8(j, 0x41); emit_u8(j, 0x5B);
        emit_u8(j, 0x58);
        emit_u8(j, 0x41); emit_u8(j, 0x59);
        emit_u8(j, 0x41); emit_u8(j, 0x58);
        emit_u8(j, 0x5A);
        emit_u8(j, 0x59);
        emit_u8(j, 0x48); emit_u8(j, 0x83); emit_u8(j, 0xEC); emit_u8(j, 0x40);
        emit_u8(j, 0x48); emit_u8(j, 0x89); emit_u8(j, 0x44); emit_u8(j, 0x24); emit_u8(j, 0x20);
        emit_u8(j, 0x4C); emit_u8(j, 0x89); emit_u8(j, 0x5C); emit_u8(j, 0x24); emit_u8(j, 0x28);
        emit_u8(j, 0x4C); emit_u8(j, 0x89); emit_u8(j, 0x54); emit_u8(j, 0x24); emit_u8(j, 0x30);
        emit_u8(j, 0x48); emit_u8(j, 0xB8); emit_u64(j, (uint64_t)func_ptr);
        emit_u8(j, 0xFF); emit_u8(j, 0xD0);
        emit_u8(j, 0x48); emit_u8(j, 0x83); emit_u8(j, 0xC4); emit_u8(j, 0x40);
        return;
    }
}
#else
static void emit_call_with_args(JITBuffer* j, void* func_ptr, int num_args) {
    if (num_args == 0) {
        emit_u8(j, 0x48); emit_u8(j, 0xB8); emit_u64(j, (uint64_t)func_ptr);
        emit_u8(j, 0xFF); emit_u8(j, 0xD0);
        return;
    }
    if (num_args == 1) {
        emit_u8(j, 0x5F);
        emit_u8(j, 0x48); emit_u8(j, 0xB8); emit_u64(j, (uint64_t)func_ptr);
        emit_u8(j, 0xFF); emit_u8(j, 0xD0);
        return;
    }
    if (num_args == 2) {
        emit_u8(j, 0x5E);
        emit_u8(j, 0x5F);
        emit_u8(j, 0x48); emit_u8(j, 0xB8); emit_u64(j, (uint64_t)func_ptr);
        emit_u8(j, 0xFF); emit_u8(j, 0xD0);
        return;
    }
    if (num_args == 3) {
        emit_u8(j, 0x5A);
        emit_u8(j, 0x5E);
        emit_u8(j, 0x5F);
        emit_u8(j, 0x48); emit_u8(j, 0xB8); emit_u64(j, (uint64_t)func_ptr);
        emit_u8(j, 0xFF); emit_u8(j, 0xD0);
        return;
    }
    if (num_args == 4) {
        emit_u8(j, 0x59);
        emit_u8(j, 0x5A);
        emit_u8(j, 0x5E);
        emit_u8(j, 0x5F);
        emit_u8(j, 0x48); emit_u8(j, 0xB8); emit_u64(j, (uint64_t)func_ptr);
        emit_u8(j, 0xFF); emit_u8(j, 0xD0);
        return;
    }
    if (num_args == 5) {
        emit_u8(j, 0x41); emit_u8(j, 0x58);
        emit_u8(j, 0x59);
        emit_u8(j, 0x5A);
        emit_u8(j, 0x5E);
        emit_u8(j, 0x5F);
        emit_u8(j, 0x48); emit_u8(j, 0xB8); emit_u64(j, (uint64_t)func_ptr);
        emit_u8(j, 0xFF); emit_u8(j, 0xD0);
        return;
    }
    if (num_args == 6) {
        emit_u8(j, 0x41); emit_u8(j, 0x59);
        emit_u8(j, 0x41); emit_u8(j, 0x58);
        emit_u8(j, 0x59);
        emit_u8(j, 0x5A);
        emit_u8(j, 0x5E);
        emit_u8(j, 0x5F);
        emit_u8(j, 0x48); emit_u8(j, 0xB8); emit_u64(j, (uint64_t)func_ptr);
        emit_u8(j, 0xFF); emit_u8(j, 0xD0);
        return;
    }
    if (num_args == 7) {
        emit_u8(j, 0x41); emit_u8(j, 0x5A);
        emit_u8(j, 0x41); emit_u8(j, 0x59);
        emit_u8(j, 0x41); emit_u8(j, 0x58);
        emit_u8(j, 0x59);
        emit_u8(j, 0x5A);
        emit_u8(j, 0x5E);
        emit_u8(j, 0x5F);
        emit_u8(j, 0x6A); emit_u8(j, 0x00);
        emit_u8(j, 0x41); emit_u8(j, 0x52);
        emit_u8(j, 0x48); emit_u8(j, 0xB8); emit_u64(j, (uint64_t)func_ptr);
        emit_u8(j, 0xFF); emit_u8(j, 0xD0);
        emit_u8(j, 0x48); emit_u8(j, 0x83); emit_u8(j, 0xC4); emit_u8(j, 0x10);
        return;
    }
}
#endif

int get_vreg_int_reg(IRBuffer* ir, VReg v, int scratch_reg, JITBuffer* j) {
    if (v == VREG_NONE || v < 0 || v >= ir->vreg_count) return PREG_NONE;
    if (ir->vregs[v].is_spilled) {
        emit_load_spill(j, scratch_reg, ir->vregs[v].spill_offset);
        return scratch_reg;
    }
    return ir->vregs[v].preg;
}

void commit_vreg_int_reg(IRBuffer* ir, VReg v, int source_reg, JITBuffer* j) {
    if (v == VREG_NONE || v < 0 || v >= ir->vreg_count) return;
    if (ir->vregs[v].is_spilled) {
        emit_store_spill(j, source_reg, ir->vregs[v].spill_offset);
    } else {
        int target_reg = ir->vregs[v].preg;
        if (target_reg != source_reg && target_reg != PREG_NONE) {
            emit_mov_rr(j, target_reg, source_reg);
        }
    }
}

int get_vreg_float_reg(IRBuffer* ir, VReg v, int scratch_xmm, JITBuffer* j) {
    if (v == VREG_NONE || v < 0 || v >= ir->vreg_count) return 0;
    if (ir->vregs[v].is_spilled) {
        if (ir->vregs[v].type == TYPE_F64) emit_loadsd_spill(j, scratch_xmm, ir->vregs[v].spill_offset);
        else emit_loadss_spill(j, scratch_xmm, ir->vregs[v].spill_offset);
        return scratch_xmm;
    }
    return ir->vregs[v].preg;
}

void commit_vreg_float_reg(IRBuffer* ir, VReg v, int source_xmm, JITBuffer* j) {
    if (v == VREG_NONE || v < 0 || v >= ir->vreg_count) return;
    if (ir->vregs[v].is_spilled) {
        if (ir->vregs[v].type == TYPE_F64) emit_storesd_spill(j, source_xmm, ir->vregs[v].spill_offset);
        else emit_storess_spill(j, source_xmm, ir->vregs[v].spill_offset);
    } else {
        int target_xmm = ir->vregs[v].preg;
        if (target_xmm != source_xmm && target_xmm != PREG_NONE) {
            emit_movaps_xx(j, target_xmm, source_xmm);
        }
    }
}

typedef struct {
    int label_id;
    size_t patch_pos;
} JumpPatch;

static void emit_sar_ri(JITBuffer* j, int reg, int shift) {
    emit_u8(j, (uint8_t)(0x48 | (reg >= 8 ? 1 : 0)));
    emit_u8(j, 0xC1);
    emit_u8(j, (uint8_t)(0xF8 | (reg & 7)));
    emit_u8(j, (uint8_t)(shift & 63));
}

static void emit_shr_ri(JITBuffer* j, int reg, int shift) {
    emit_u8(j, (uint8_t)(0x48 | (reg >= 8 ? 1 : 0)));
    emit_u8(j, 0xC1);
    emit_u8(j, (uint8_t)(0xE8 | (reg & 7)));
    emit_u8(j, (uint8_t)(shift & 63));
}

static void emit_and_ri32(JITBuffer* j, int reg, int32_t imm) {
    emit_u8(j, (uint8_t)(0x48 | (reg >= 8 ? 1 : 0)));
    if (imm >= -128 && imm <= 127) {
        emit_u8(j, 0x83);
        emit_u8(j, (uint8_t)(0xE0 | (reg & 7)));
        emit_u8(j, (uint8_t)(int8_t)imm);
    } else {
        emit_u8(j, 0x81);
        emit_u8(j, (uint8_t)(0xE0 | (reg & 7)));
        emit_u32(j, (uint32_t)imm);
    }
}

static void emit_bounds_check_slow_call(JITBuffer* j, IRBuffer* ir, const IRInstruction* ins) {
    emit_vzeroupper(j);
    int rb = get_vreg_int_reg(ir, ins->src1, PREG_RAX, j);
    emit_push_r(j, rb);
    int ri = get_vreg_int_reg(ir, ins->src2, PREG_RAX, j);
    emit_push_r(j, ri);
    if (ins->src3 != VREG_NONE) {
        int rl = get_vreg_int_reg(ir, ins->src3, PREG_RAX, j);
        emit_push_r(j, rl);
    } else {
        emit_mov_ri64(j, PREG_RAX, ins->known_len);
        emit_push_r(j, PREG_RAX);
    }
    emit_mov_ri64(j, PREG_RAX, (int64_t)ins->offset);
    emit_push_r(j, PREG_RAX);
    emit_mov_ri64(j, PREG_RAX, (int64_t)(uintptr_t)vortex_intern_cstr(ins->file ? ins->file : "<unknown>"));
    emit_push_r(j, PREG_RAX);
    emit_mov_ri64(j, PREG_RAX, (int64_t)ins->line);
    emit_push_r(j, PREG_RAX);
    emit_call_with_args(j, (void*)vortex_safe_check_bounds, 6);
}

typedef struct {
    size_t jz_patch;
    size_t jae_patch;
    const IRInstruction* ins;
} ColdBoundsTrap;

void ir_codegen_x86_64(IRBuffer* ir, JITBuffer* j) {
    int label_cap = ir->next_label + 16;
    if (label_cap < 64) label_cap = 64;
    int small_labels[64];
    int* label_offsets = (label_cap <= 64) ? small_labels : (int*)malloc((size_t)label_cap * sizeof(int));

    int patch_cap = ir->count + 16;
    if (patch_cap < 64) patch_cap = 64;
    JumpPatch small_patches[64];
    JumpPatch* patches = (patch_cap <= 64) ? small_patches : (JumpPatch*)malloc((size_t)patch_cap * sizeof(JumpPatch));

    ColdBoundsTrap small_traps[64];
    ColdBoundsTrap* cold_traps = small_traps;
    int cold_count = 0;
    int cold_cap = 64;

    if (!label_offsets || !patches) {
        if (label_offsets && label_offsets != small_labels) free(label_offsets);
        if (patches && patches != small_patches) free(patches);
        return;
    }
    for (int i = 0; i < label_cap; i++) label_offsets[i] = -1;
    int patch_count = 0;

    for (int ip = 0; ip < ir->count; ip++) {
        IRInstruction* ins = &ir->instrs[ip];
        size_t ins_start_off = j->size;
        switch (ins->op) {
            case IR_NOP: break;

            case IR_CONST_I64: {
                int rd = ir->vregs[ins->dest].is_spilled ? PREG_R10 : ir->vregs[ins->dest].preg;
                emit_mov_ri64(j, rd, ins->imm);
                commit_vreg_int_reg(ir, ins->dest, rd, j);
                break;
            }

            case IR_CONST_F32: {
                int xd = ir->vregs[ins->dest].is_spilled ? 4 : ir->vregs[ins->dest].preg;
                emit_load_f32_imm_to(j, ins->fimm, xd);
                commit_vreg_float_reg(ir, ins->dest, xd, j);
                break;
            }

            case IR_CONST_F64: {
                int xd = ir->vregs[ins->dest].is_spilled ? 4 : ir->vregs[ins->dest].preg;
                emit_load_f64_imm_to(j, ins->f64_imm, xd);
                commit_vreg_float_reg(ir, ins->dest, xd, j);
                break;
            }

            case IR_MOV: {
                if (ir->vregs[ins->dest].is_float) {
                    int xs = get_vreg_float_reg(ir, ins->src1, 4, j);
                    commit_vreg_float_reg(ir, ins->dest, xs, j);
                } else {
                    int rs = get_vreg_int_reg(ir, ins->src1, PREG_R10, j);
                    commit_vreg_int_reg(ir, ins->dest, rs, j);
                }
                break;
            }

            case IR_ADD:
            case IR_SUB:
            case IR_MUL: {
                int r1 = get_vreg_int_reg(ir, ins->src1, PREG_R10, j);
                int rd = ir->vregs[ins->dest].is_spilled ? PREG_R10 : ir->vregs[ins->dest].preg;
                if (ins->src2 == VREG_NONE) {
                    int32_t imm = (int32_t)ins->imm;
                    if (ins->op == IR_ADD) {
                        if (imm == 0) {
                            if (rd != r1) emit_mov_rr(j, rd, r1);
                        } else if (rd != r1) {
                            emit_lea_disp(j, rd, r1, imm);
                        } else {
                            emit_add_ri(j, rd, imm);
                        }
                    } else if (ins->op == IR_SUB) {
                        if (imm == 0) {
                            if (rd != r1) emit_mov_rr(j, rd, r1);
                        } else if (rd != r1 && ins->imm != -2147483648LL) {
                            emit_lea_disp(j, rd, r1, -imm);
                        } else {
                            if (rd != r1) emit_mov_rr(j, rd, r1);
                            emit_sub_ri(j, rd, imm);
                        }
                    } else {
                        if (imm == 0) {
                            emit_mov_ri64(j, rd, 0);
                        } else if (imm == 1) {
                            if (rd != r1) emit_mov_rr(j, rd, r1);
                        } else if (imm == 2) {
                            if (rd != r1) emit_mov_rr(j, rd, r1);
                            emit_shl_ri(j, rd, 1);
                        } else if (imm == 4) {
                            if (rd != r1) emit_mov_rr(j, rd, r1);
                            emit_shl_ri(j, rd, 2);
                        } else if (imm == 8) {
                            if (rd != r1) emit_mov_rr(j, rd, r1);
                            emit_shl_ri(j, rd, 3);
                        } else if ((imm == 3 || imm == 5 || imm == 9) && (r1 & 7) != 4) {
                            emit_lea_sib(j, rd, r1, r1, (int)(imm - 1));
                        } else {
                            emit_imul_rri32(j, rd, r1, imm);
                        }
                    }
                    commit_vreg_int_reg(ir, ins->dest, rd, j);
                    break;
                }
                int r2 = get_vreg_int_reg(ir, ins->src2, PREG_R11, j);
                if (ins->op == IR_ADD) {
                    if (rd == r1) {
                        emit_add_rr(j, rd, r2);
                    } else if (rd == r2) {
                        emit_add_rr(j, rd, r1);
                    } else if ((r2 & 7) != 4) {
                        emit_lea_sib(j, rd, r1, r2, 1);
                    } else if ((r1 & 7) != 4) {
                        emit_lea_sib(j, rd, r2, r1, 1);
                    } else {
                        emit_mov_rr(j, rd, r1);
                        emit_add_rr(j, rd, r2);
                    }
                } else if (ins->op == IR_SUB) {
                    if (rd == r1) {
                        emit_sub_rr(j, rd, r2);
                    } else if (rd == r2) {
                        int work_rd = PREG_R10;
                        if (work_rd != r1) emit_mov_rr(j, work_rd, r1);
                        emit_sub_rr(j, work_rd, r2);
                        commit_vreg_int_reg(ir, ins->dest, work_rd, j);
                        break;
                    } else {
                        emit_mov_rr(j, rd, r1);
                        emit_sub_rr(j, rd, r2);
                    }
                } else {
                    if (rd == r1) {
                        emit_imul_rr(j, rd, r2);
                    } else if (rd == r2) {
                        emit_imul_rr(j, rd, r1);
                    } else {
                        emit_mov_rr(j, rd, r1);
                        emit_imul_rr(j, rd, r2);
                    }
                }
                commit_vreg_int_reg(ir, ins->dest, rd, j);
                break;
            }

            case IR_SUB_MOD_IMM: {
                int r1 = get_vreg_int_reg(ir, ins->src1, PREG_R10, j);
                int rd = ir->vregs[ins->dest].is_spilled ? PREG_R10 : ir->vregs[ins->dest].preg;
                int32_t m = (int32_t)ins->imm;
                emit_lea_disp(j, PREG_R11, r1, -m);
                if (rd != r1) emit_mov_rr(j, rd, r1);
                emit_cmp_ri(j, rd, m);
                emit_cmovge_rr(j, rd, PREG_R11);
                commit_vreg_int_reg(ir, ins->dest, rd, j);
                break;
            }

            case IR_ADD_SUB_MOD_IMM: {
                int r1 = get_vreg_int_reg(ir, ins->src1, PREG_R10, j);
                int rd = ir->vregs[ins->dest].is_spilled ? PREG_R10 : ir->vregs[ins->dest].preg;
                int32_t m = (int32_t)ins->imm;
                int32_t step = (int32_t)ins->offset;
                emit_lea_disp(j, PREG_R11, r1, step - m);
                emit_cmp_ri(j, r1, m - step);
                emit_lea_disp(j, rd, r1, step);
                emit_cmovge_rr(j, rd, PREG_R11);
                commit_vreg_int_reg(ir, ins->dest, rd, j);
                break;
            }

            case IR_DIV:
            case IR_MOD: {
                int r1 = get_vreg_int_reg(ir, ins->src1, PREG_R10, j);
                if (ins->src2 == VREG_NONE && ins->imm > 0 && (ins->imm & (ins->imm - 1)) == 0) {
                    int k = 0;
                    int64_t tmp = ins->imm;
                    while (tmp > 1) { tmp >>= 1; k++; }
                    int rd = ir->vregs[ins->dest].is_spilled ? PREG_R10 : ir->vregs[ins->dest].preg;
                    if (rd != r1) emit_mov_rr(j, rd, r1);
                    if (k > 0) {
                        emit_mov_rr(j, PREG_R11, rd);
                        emit_sar_ri(j, PREG_R11, 63);
                        emit_shr_ri(j, PREG_R11, 64 - k);
                        emit_add_rr(j, rd, PREG_R11);
                        if (ins->op == IR_DIV) {
                            emit_sar_ri(j, rd, k);
                        } else {
                            emit_and_ri32(j, rd, (int32_t)(ins->imm - 1));
                            emit_sub_rr(j, rd, PREG_R11);
                        }
                    } else if (ins->op == IR_MOD) {
                        emit_mov_ri64(j, rd, 0);
                    }
                    commit_vreg_int_reg(ir, ins->dest, rd, j);
                    break;
                }
                int r2 = get_vreg_int_reg(ir, ins->src2, PREG_R11, j);
                if (r2 == PREG_RAX || r2 == PREG_RDX) {
                    emit_mov_rr(j, PREG_R11, r2);
                    r2 = PREG_R11;
                }
                emit_mov_rr(j, PREG_RAX, r1);
                emit_u8(j, 0x48); emit_u8(j, 0x99);
                emit_idiv_r(j, r2);
                if (ins->op == IR_DIV) {
                    commit_vreg_int_reg(ir, ins->dest, PREG_RAX, j);
                } else {
                    commit_vreg_int_reg(ir, ins->dest, PREG_RDX, j);
                }
                break;
            }

            case IR_BAND:
            case IR_BOR:
            case IR_BXOR: {
                int r1 = get_vreg_int_reg(ir, ins->src1, PREG_R10, j);
                int rd = ir->vregs[ins->dest].is_spilled ? PREG_R10 : ir->vregs[ins->dest].preg;
                if (ins->src2 == VREG_NONE) {
                    if (rd != r1) emit_mov_rr(j, rd, r1);
                    if (ins->op == IR_BAND) emit_and_ri32(j, rd, (int32_t)ins->imm);
                    else if (ins->op == IR_BOR) emit_or_ri32(j, rd, (int32_t)ins->imm);
                    else emit_xor_ri32(j, rd, (int32_t)ins->imm);
                } else {
                    int r2 = get_vreg_int_reg(ir, ins->src2, PREG_R11, j);
                    if (rd == r1) {
                        if (ins->op == IR_BAND) emit_and_rr(j, rd, r2);
                        else if (ins->op == IR_BOR) emit_or_rr(j, rd, r2);
                        else emit_xor_rr(j, rd, r2);
                    } else if (rd == r2) {
                        if (ins->op == IR_BAND) emit_and_rr(j, rd, r1);
                        else if (ins->op == IR_BOR) emit_or_rr(j, rd, r1);
                        else emit_xor_rr(j, rd, r1);
                    } else {
                        emit_mov_rr(j, rd, r1);
                        if (ins->op == IR_BAND) emit_and_rr(j, rd, r2);
                        else if (ins->op == IR_BOR) emit_or_rr(j, rd, r2);
                        else emit_xor_rr(j, rd, r2);
                    }
                }
                commit_vreg_int_reg(ir, ins->dest, rd, j);
                break;
            }

            case IR_BNOT: {
                int r1 = get_vreg_int_reg(ir, ins->src1, PREG_R10, j);
                int rd = ir->vregs[ins->dest].is_spilled ? PREG_R10 : ir->vregs[ins->dest].preg;
                if (rd != r1) emit_mov_rr(j, rd, r1);
                emit_not_r(j, rd);
                commit_vreg_int_reg(ir, ins->dest, rd, j);
                break;
            }

            case IR_LNOT: {
                int r1 = get_vreg_int_reg(ir, ins->src1, PREG_R10, j);
                emit_test_rr(j, r1, r1);
                emit_setcc(j, 0x94, PREG_RAX);
                int rd = ir->vregs[ins->dest].is_spilled ? PREG_R10 : ir->vregs[ins->dest].preg;
                emit_movzx_r64_r8(j, rd, PREG_RAX);
                commit_vreg_int_reg(ir, ins->dest, rd, j);
                break;
            }

            case IR_SHL:
            case IR_SHR: {
                int r1 = get_vreg_int_reg(ir, ins->src1, PREG_R10, j);
                int rd = ir->vregs[ins->dest].is_spilled ? PREG_R10 : ir->vregs[ins->dest].preg;
                if (ins->src2 == VREG_NONE) {
                    if (rd != r1) emit_mov_rr(j, rd, r1);
                    int sh = (int)(ins->imm & 63);
                    if (sh > 0) {
                        if (ins->op == IR_SHL) emit_shl_ri(j, rd, sh);
                        else emit_sar_ri(j, rd, sh);
                    }
                } else {
                    int r2 = get_vreg_int_reg(ir, ins->src2, PREG_R11, j);
                    emit_mov_rr(j, PREG_RAX, r1);
                    bool saved_rcx = (r2 != PREG_RCX && rd != PREG_RCX);
                    if (saved_rcx) emit_push_r(j, PREG_RCX);
                    if (r2 != PREG_RCX) emit_mov_rr(j, PREG_RCX, r2);
                    if (ins->op == IR_SHL) emit_shl_cl(j, PREG_RAX);
                    else emit_sar_cl(j, PREG_RAX);
                    if (saved_rcx) emit_u8(j, 0x59); /* pop rcx */
                    if (rd != PREG_RAX) emit_mov_rr(j, rd, PREG_RAX);
                }
                commit_vreg_int_reg(ir, ins->dest, rd, j);
                break;
            }

            case IR_DIV_CHECK: {
                int r_div = get_vreg_int_reg(ir, ins->src1, PREG_R10, j);
                emit_test_rr(j, r_div, r_div);
                emit_u8(j, 0x75);
                size_t jnz_off = j->size;
                emit_u8(j, 0);
                if (ir->uses_avx) emit_vzeroupper(j);
                emit_mov_ri64(j, PREG_RAX, (int64_t)(uintptr_t)vortex_intern_cstr(ins->file ? ins->file : "<input>"));
                emit_push_r(j, PREG_RAX);
                emit_mov_ri64(j, PREG_RAX, (int64_t)ins->line);
                emit_push_r(j, PREG_RAX);
                emit_call_with_args(j, (void*)vortex_ultrasafe_div_zero, 2);
                j->code[jnz_off] = (uint8_t)(j->size - (jnz_off + 1));
                break;
            }

            case IR_FADD:
            case IR_FSUB:
            case IR_FMUL:
            case IR_FDIV: {
                int x1 = get_vreg_float_reg(ir, ins->src1, 4, j);
                int x2 = get_vreg_float_reg(ir, ins->src2, 5, j);
                int xd = ir->vregs[ins->dest].is_spilled ? 4 : ir->vregs[ins->dest].preg;
                int work_xd = (xd == x2 && xd != x1) ? 4 : xd;
                if (work_xd != x1) {
                    emit_movaps_xx(j, work_xd, x1);
                }
                if (ins->type == TYPE_F64) {
                    if (ins->op == IR_FADD) emit_addsd_xx(j, work_xd, x2);
                    else if (ins->op == IR_FSUB) emit_subsd_xx(j, work_xd, x2);
                    else if (ins->op == IR_FMUL) emit_mulsd_xx(j, work_xd, x2);
                    else if (ins->op == IR_FDIV) emit_divsd_xx(j, work_xd, x2);
                } else {
                    if (ins->op == IR_FADD) emit_addss_xx(j, work_xd, x2);
                    else if (ins->op == IR_FSUB) emit_subss_xx(j, work_xd, x2);
                    else if (ins->op == IR_FMUL) emit_mulss_xx(j, work_xd, x2);
                    else if (ins->op == IR_FDIV) emit_divss_xx(j, work_xd, x2);
                }
                commit_vreg_float_reg(ir, ins->dest, work_xd, j);
                break;
            }

            case IR_EQ:
            case IR_NE:
            case IR_LT:
            case IR_LE:
            case IR_GT:
            case IR_GE: {
                int r1 = get_vreg_int_reg(ir, ins->src1, PREG_R10, j);
                if (ins->src2 == VREG_NONE) {
                    if (ins->imm == 0) emit_test_rr(j, r1, r1);
                    else emit_cmp_ri(j, r1, (int32_t)ins->imm);
                } else {
                    int r2 = get_vreg_int_reg(ir, ins->src2, PREG_R11, j);
                    emit_cmp_rr(j, r1, r2);
                }
                uint8_t cc_low = 0x4;
                switch (ins->op) {
                    case IR_EQ: cc_low = 0x4; break;
                    case IR_NE: cc_low = 0x5; break;
                    case IR_LT: cc_low = 0xC; break;
                    case IR_LE: cc_low = 0xE; break;
                    case IR_GT: cc_low = 0xF; break;
                    case IR_GE: cc_low = 0xD; break;
                    default: break;
                }
                if (ip + 1 < ir->count &&
                    (ir->instrs[ip + 1].op == IR_JZ || ir->instrs[ip + 1].op == IR_JNZ) &&
                    ir->instrs[ip + 1].src1 == ins->dest &&
                    ins->dest >= 0 && ir->vregs[ins->dest].end_ip <= ip + 1) {
                    uint8_t jcc_op = (ir->instrs[ip + 1].op == IR_JZ)
                                     ? (uint8_t)(0x80 | (cc_low ^ 1))
                                     : (uint8_t)(0x80 | cc_low);
                    int lbl = (int)ir->instrs[ip + 1].imm;
                    emit_u8(j, 0x0F); emit_u8(j, jcc_op);
                    if (lbl >= 0 && lbl < label_cap && label_offsets[lbl] >= 0) {
                        int32_t rel = (int32_t)(label_offsets[lbl] - (j->size + 4));
                        emit_u32(j, (uint32_t)rel);
                    } else {
                        if (patch_count < patch_cap) patches[patch_count++] = (JumpPatch){lbl, j->size};
                        emit_u32(j, 0);
                    }
                    ip++;
                    break;
                }
                emit_setcc(j, (uint8_t)(0x90 | cc_low), PREG_RAX);
                int rd = ir->vregs[ins->dest].is_spilled ? PREG_R10 : ir->vregs[ins->dest].preg;
                emit_movzx_r64_r8(j, rd, PREG_RAX);
                commit_vreg_int_reg(ir, ins->dest, rd, j);
                break;
            }

            case IR_FEQ:
            case IR_FNE:
            case IR_FLT:
            case IR_FLE:
            case IR_FGT:
            case IR_FGE: {
                int x1 = get_vreg_float_reg(ir, ins->src1, 4, j);
                int x2 = get_vreg_float_reg(ir, ins->src2, 5, j);
                if (ins->type == TYPE_F64) emit_ucomisd_xx(j, x1, x2);
                else emit_ucomiss_xx(j, x1, x2);
                uint8_t cc_low = 0x4;
                switch (ins->op) {
                    case IR_FEQ: cc_low = 0x4; break;
                    case IR_FNE: cc_low = 0x5; break;
                    case IR_FLT: cc_low = 0x2; break;
                    case IR_FLE: cc_low = 0x6; break;
                    case IR_FGT: cc_low = 0x7; break;
                    case IR_FGE: cc_low = 0x3; break;
                    default: break;
                }
                if (ip + 1 < ir->count &&
                    (ir->instrs[ip + 1].op == IR_JZ || ir->instrs[ip + 1].op == IR_JNZ) &&
                    ir->instrs[ip + 1].src1 == ins->dest &&
                    ins->dest >= 0 && ir->vregs[ins->dest].end_ip <= ip + 1) {
                    uint8_t jcc_op = (ir->instrs[ip + 1].op == IR_JZ)
                                     ? (uint8_t)(0x80 | (cc_low ^ 1))
                                     : (uint8_t)(0x80 | cc_low);
                    int lbl = (int)ir->instrs[ip + 1].imm;
                    emit_u8(j, 0x0F); emit_u8(j, jcc_op);
                    if (lbl >= 0 && lbl < label_cap && label_offsets[lbl] >= 0) {
                        int32_t rel = (int32_t)(label_offsets[lbl] - (j->size + 4));
                        emit_u32(j, (uint32_t)rel);
                    } else {
                        if (patch_count < patch_cap) patches[patch_count++] = (JumpPatch){lbl, j->size};
                        emit_u32(j, 0);
                    }
                    ip++;
                    break;
                }
                emit_setcc(j, (uint8_t)(0x90 | cc_low), PREG_RAX);
                int rd = ir->vregs[ins->dest].is_spilled ? PREG_R10 : ir->vregs[ins->dest].preg;
                emit_movzx_r64_r8(j, rd, PREG_RAX);
                commit_vreg_int_reg(ir, ins->dest, rd, j);
                break;
            }

            case IR_CAST_I64_TO_F32: {
                int rs = get_vreg_int_reg(ir, ins->src1, PREG_R10, j);
                int xd = ir->vregs[ins->dest].is_spilled ? 4 : ir->vregs[ins->dest].preg;
                emit_cvtsi2ss_xr(j, xd, rs);
                commit_vreg_float_reg(ir, ins->dest, xd, j);
                break;
            }

            case IR_CAST_F32_TO_I64: {
                int xs = get_vreg_float_reg(ir, ins->src1, 4, j);
                int rd = ir->vregs[ins->dest].is_spilled ? PREG_R10 : ir->vregs[ins->dest].preg;
                emit_cvttss2si_rx(j, rd, xs);
                commit_vreg_int_reg(ir, ins->dest, rd, j);
                break;
            }

            case IR_CAST_I64_TO_F64: {
                int rs = get_vreg_int_reg(ir, ins->src1, PREG_R10, j);
                int xd = ir->vregs[ins->dest].is_spilled ? 4 : ir->vregs[ins->dest].preg;
                emit_cvtsi2sd_xr(j, xd, rs);
                commit_vreg_float_reg(ir, ins->dest, xd, j);
                break;
            }

            case IR_CAST_F64_TO_I64: {
                int xs = get_vreg_float_reg(ir, ins->src1, 4, j);
                int rd = ir->vregs[ins->dest].is_spilled ? PREG_R10 : ir->vregs[ins->dest].preg;
                emit_cvttsd2si_rx(j, rd, xs);
                commit_vreg_int_reg(ir, ins->dest, rd, j);
                break;
            }

            case IR_CAST_F32_TO_F64: {
                int xs = get_vreg_float_reg(ir, ins->src1, 4, j);
                int xd = ir->vregs[ins->dest].is_spilled ? 4 : ir->vregs[ins->dest].preg;
                emit_cvtss2sd_xx(j, xd, xs);
                commit_vreg_float_reg(ir, ins->dest, xd, j);
                break;
            }

            case IR_CAST_F64_TO_F32: {
                int xs = get_vreg_float_reg(ir, ins->src1, 4, j);
                int xd = ir->vregs[ins->dest].is_spilled ? 4 : ir->vregs[ins->dest].preg;
                emit_cvtsd2ss_xx(j, xd, xs);
                commit_vreg_float_reg(ir, ins->dest, xd, j);
                break;
            }

            case IR_LOAD_OFFSET: {
                int r_base = get_vreg_int_reg(ir, ins->src1, PREG_R10, j);
                if (ins->type == TYPE_F64) {
                    int xd = ir->vregs[ins->dest].is_spilled ? 4 : ir->vregs[ins->dest].preg;
                    emit_loadsd_offset(j, xd, r_base, ins->offset);
                    commit_vreg_float_reg(ir, ins->dest, xd, j);
                } else if (ins->type == TYPE_F32) {
                    int xd = ir->vregs[ins->dest].is_spilled ? 4 : ir->vregs[ins->dest].preg;
                    emit_loadss_offset(j, xd, r_base, ins->offset);
                    commit_vreg_float_reg(ir, ins->dest, xd, j);
                } else if (ins->type == TYPE_I32) {
                    int rd = ir->vregs[ins->dest].is_spilled ? PREG_R10 : ir->vregs[ins->dest].preg;
                    emit_load_mem32s_offset(j, rd, r_base, ins->offset);
                    commit_vreg_int_reg(ir, ins->dest, rd, j);
                } else {
                    int rd = ir->vregs[ins->dest].is_spilled ? PREG_R10 : ir->vregs[ins->dest].preg;
                    emit_load_mem_offset(j, rd, r_base, ins->offset);
                    commit_vreg_int_reg(ir, ins->dest, rd, j);
                }
                break;
            }

            case IR_STORE_OFFSET: {
                int r_base = get_vreg_int_reg(ir, ins->src1, PREG_R10, j);
                if (ins->type == TYPE_F64) {
                    int xs = get_vreg_float_reg(ir, ins->src2, 4, j);
                    emit_storesd_offset(j, r_base, ins->offset, xs);
                } else if (ins->type == TYPE_F32) {
                    int xs = get_vreg_float_reg(ir, ins->src2, 4, j);
                    emit_storess_offset(j, r_base, ins->offset, xs);
                } else if (ins->type == TYPE_I32) {
                    int rs = get_vreg_int_reg(ir, ins->src2, PREG_R11, j);
                    emit_store_mem32_offset(j, r_base, ins->offset, rs);
                } else {
                    int rs = get_vreg_int_reg(ir, ins->src2, PREG_R11, j);
                    emit_store_mem_offset(j, r_base, ins->offset, rs);
                }
                break;
            }

            case IR_LOAD_ARRAY: {
                int r_base = get_vreg_int_reg(ir, ins->src1, PREG_R10, j);
                int r_idx = get_vreg_int_reg(ir, ins->src2, PREG_R11, j);
                int scale = (ins->type == TYPE_F32 || ins->type == TYPE_I32) ? 4 : 8;
                if (ins->type == TYPE_F64) {
                    int xd = ir->vregs[ins->dest].is_spilled ? 4 : ir->vregs[ins->dest].preg;
                    emit_loadsd_array(j, xd, r_base, r_idx, scale);
                    commit_vreg_float_reg(ir, ins->dest, xd, j);
                } else if (ins->type == TYPE_F32) {
                    int xd = ir->vregs[ins->dest].is_spilled ? 4 : ir->vregs[ins->dest].preg;
                    emit_loadss_array(j, xd, r_base, r_idx, scale);
                    commit_vreg_float_reg(ir, ins->dest, xd, j);
                } else if (ins->type == TYPE_I32) {
                    int rd = ir->vregs[ins->dest].is_spilled ? PREG_R10 : ir->vregs[ins->dest].preg;
                    emit_load_mem32s_array(j, rd, r_base, r_idx, scale);
                    commit_vreg_int_reg(ir, ins->dest, rd, j);
                } else {
                    int rd = ir->vregs[ins->dest].is_spilled ? PREG_R10 : ir->vregs[ins->dest].preg;
                    emit_load_mem_array(j, rd, r_base, r_idx, scale);
                    commit_vreg_int_reg(ir, ins->dest, rd, j);
                }
                break;
            }

            case IR_STORE_ARRAY: {
                int r_base = get_vreg_int_reg(ir, ins->src1, PREG_R10, j);
                int r_idx = get_vreg_int_reg(ir, ins->src2, PREG_R11, j);
                int scale = (ins->type == TYPE_F32 || ins->type == TYPE_I32) ? 4 : 8;
                if (ins->type == TYPE_F64) {
                    int xs = get_vreg_float_reg(ir, ins->src3, 4, j);
                    emit_storesd_array(j, r_base, r_idx, xs, scale);
                } else if (ins->type == TYPE_F32) {
                    int xs = get_vreg_float_reg(ir, ins->src3, 4, j);
                    emit_storess_array(j, r_base, r_idx, xs, scale);
                } else if (ins->type == TYPE_I32) {
                    int rs = get_vreg_int_reg(ir, ins->src3, PREG_RAX, j);
                    emit_store_mem32_array(j, r_base, r_idx, rs, scale);
                } else {
                    int rs = get_vreg_int_reg(ir, ins->src3, PREG_RAX, j);
                    emit_store_mem_array(j, r_base, r_idx, rs, scale);
                }
                break;
            }

            case IR_LOAD_DEREF: {
                int r_ptr = get_vreg_int_reg(ir, ins->src1, PREG_R10, j);
                if (ins->type == TYPE_F64) {
                    int xd = ir->vregs[ins->dest].is_spilled ? 4 : ir->vregs[ins->dest].preg;
                    emit_loadsd_offset(j, xd, r_ptr, 0);
                    commit_vreg_float_reg(ir, ins->dest, xd, j);
                } else if (ins->type == TYPE_F32) {
                    int xd = ir->vregs[ins->dest].is_spilled ? 4 : ir->vregs[ins->dest].preg;
                    emit_loadss_offset(j, xd, r_ptr, 0);
                    commit_vreg_float_reg(ir, ins->dest, xd, j);
                } else if (ins->type == TYPE_I32) {
                    int rd = ir->vregs[ins->dest].is_spilled ? PREG_R10 : ir->vregs[ins->dest].preg;
                    emit_load_mem32s_offset(j, rd, r_ptr, 0);
                    commit_vreg_int_reg(ir, ins->dest, rd, j);
                } else {
                    int rd = ir->vregs[ins->dest].is_spilled ? PREG_R10 : ir->vregs[ins->dest].preg;
                    emit_load_mem_offset(j, rd, r_ptr, 0);
                    commit_vreg_int_reg(ir, ins->dest, rd, j);
                }
                break;
            }

            case IR_STORE_DEREF: {
                int r_ptr = get_vreg_int_reg(ir, ins->src1, PREG_R10, j);
                if (ins->type == TYPE_F64) {
                    int xs = get_vreg_float_reg(ir, ins->src2, 4, j);
                    emit_storesd_offset(j, r_ptr, 0, xs);
                } else if (ins->type == TYPE_F32) {
                    int xs = get_vreg_float_reg(ir, ins->src2, 4, j);
                    emit_storess_offset(j, r_ptr, 0, xs);
                } else if (ins->type == TYPE_I32) {
                    int rs = get_vreg_int_reg(ir, ins->src2, PREG_RAX, j);
                    emit_store_mem32_offset(j, r_ptr, 0, rs);
                } else {
                    int rs = get_vreg_int_reg(ir, ins->src2, PREG_RAX, j);
                    emit_store_mem_offset(j, r_ptr, 0, rs);
                }
                break;
            }

            case IR_PTR_CHECK: {
                emit_vzeroupper(j);
                int rp = get_vreg_int_reg(ir, ins->src1, PREG_RAX, j);
                emit_push_r(j, rp);
                emit_mov_ri64(j, PREG_RAX, (int64_t)(uintptr_t)vortex_intern_cstr(ins->file ? ins->file : "<unknown>"));
                emit_push_r(j, PREG_RAX);
                emit_mov_ri64(j, PREG_RAX, (int64_t)ins->line);
                emit_push_r(j, PREG_RAX);
                emit_call_with_args(j, (void*)vortex_safe_check_ptr, 3);
                break;
            }

            case IR_BOUNDS_CHECK: {
                if (g_compile_mode == VORTEX_MODE_RELEASE) {
                    int rb = get_vreg_int_reg(ir, ins->src1, PREG_RAX, j);
                    emit_test_rr(j, rb, rb);
                    emit_u8(j, 0x0F); emit_u8(j, 0x84); /* jz rel32 -> cold trap */
                    size_t jz_patch = j->size;
                    emit_u32(j, 0);

                    size_t jae_patch = (size_t)-1;
                    if (ins->src3 != VREG_NONE || ins->known_len >= 0) {
                        int ri = get_vreg_int_reg(ir, ins->src2, PREG_R11, j);
                        if (ins->src3 != VREG_NONE) {
                            int rl = get_vreg_int_reg(ir, ins->src3, PREG_R10, j);
                            emit_cmp_rr(j, ri, rl);
                        } else if (ins->known_len <= INT32_MAX) {
                            emit_cmp_ri(j, ri, (int32_t)ins->known_len);
                        } else {
                            emit_mov_ri64(j, PREG_R10, ins->known_len);
                            emit_cmp_rr(j, ri, PREG_R10);
                        }
                        emit_u8(j, 0x0F); emit_u8(j, 0x83); /* jae rel32 (unsigned >=) -> cold trap */
                        jae_patch = j->size;
                        emit_u32(j, 0);
                    }

                    if (cold_count >= cold_cap) {
                        int new_cap = cold_cap * 2;
                        ColdBoundsTrap* nb = (ColdBoundsTrap*)malloc((size_t)new_cap * sizeof(ColdBoundsTrap));
                        if (nb) {
                            memcpy(nb, cold_traps, (size_t)cold_count * sizeof(ColdBoundsTrap));
                            if (cold_traps != small_traps) free(cold_traps);
                            cold_traps = nb;
                            cold_cap = new_cap;
                        }
                    }
                    if (cold_count < cold_cap) {
                        cold_traps[cold_count++] = (ColdBoundsTrap){ jz_patch, jae_patch, ins };
                    } else {
                        int32_t rel = (int32_t)(j->size - (jz_patch + 4));
                        memcpy(&j->code[jz_patch], &rel, 4);
                        if (jae_patch != (size_t)-1) {
                            int32_t rel2 = (int32_t)(j->size - (jae_patch + 4));
                            memcpy(&j->code[jae_patch], &rel2, 4);
                        }
                        emit_bounds_check_slow_call(j, ir, ins);
                    }
                } else {
                    emit_bounds_check_slow_call(j, ir, ins);
                }
                break;
            }

            case IR_RANGE_CHECK: {
                emit_vzeroupper(j);
                int rb = get_vreg_int_reg(ir, ins->src1, PREG_RAX, j);
                emit_push_r(j, rb);
                int rs = get_vreg_int_reg(ir, ins->src2, PREG_RAX, j);
                emit_push_r(j, rs);
                int re = get_vreg_int_reg(ir, ins->src3, PREG_RAX, j);
                emit_push_r(j, re);
                if (ins->src4 != VREG_NONE) {
                    int rl = get_vreg_int_reg(ir, ins->src4, PREG_RAX, j);
                    emit_push_r(j, rl);
                } else {
                    emit_mov_ri64(j, PREG_RAX, ins->known_len);
                    emit_push_r(j, PREG_RAX);
                }
                emit_mov_ri64(j, PREG_RAX, (int64_t)ins->offset);
                emit_push_r(j, PREG_RAX);
                emit_mov_ri64(j, PREG_RAX, (int64_t)(uintptr_t)vortex_intern_cstr(ins->file ? ins->file : "<unknown>"));
                emit_push_r(j, PREG_RAX);
                emit_mov_ri64(j, PREG_RAX, (int64_t)ins->line);
                emit_push_r(j, PREG_RAX);
                emit_call_with_args(j, (void*)vortex_safe_check_range, 7);
                break;
            }

            case IR_FFI_CHECK: {
                emit_vzeroupper(j);
                int rv = get_vreg_int_reg(ir, ins->src1, PREG_RAX, j);
                emit_push_r(j, rv);
                emit_mov_ri64(j, PREG_RAX, (int64_t)ins->type);
                emit_push_r(j, PREG_RAX);
                emit_mov_ri64(j, PREG_RAX, ins->imm);
                emit_push_r(j, PREG_RAX);
                emit_mov_ri64(j, PREG_RAX, (int64_t)(uintptr_t)vortex_intern_cstr(ins->func_name[0] ? ins->func_name : "<ffi>"));
                emit_push_r(j, PREG_RAX);
                emit_mov_ri64(j, PREG_RAX, (int64_t)ins->offset);
                emit_push_r(j, PREG_RAX);
                emit_mov_ri64(j, PREG_RAX, (int64_t)(uintptr_t)vortex_intern_cstr(ins->file ? ins->file : "<unknown>"));
                emit_push_r(j, PREG_RAX);
                emit_mov_ri64(j, PREG_RAX, (int64_t)ins->line);
                emit_push_r(j, PREG_RAX);
                emit_call_with_args(j, (void*)vortex_safe_check_ffi_arg, 7);
                break;
            }

            case IR_LABEL: {
                int lbl = (int)ins->imm;
                if (lbl >= 0 && lbl < label_cap) {
                    label_offsets[lbl] = (int)j->size;
                    for (int p = 0; p < patch_count; p++) {
                        if (patches[p].label_id == lbl) {
                            int32_t rel = (int32_t)(j->size - (patches[p].patch_pos + 4));
                            memcpy(&j->code[patches[p].patch_pos], &rel, 4);
                        }
                    }
                }
                break;
            }

            case IR_JMP: {
                int lbl = (int)ins->imm;
                emit_u8(j, 0xE9);
                if (lbl >= 0 && lbl < label_cap && label_offsets[lbl] >= 0) {
                    int32_t rel = (int32_t)(label_offsets[lbl] - (j->size + 4));
                    emit_u32(j, (uint32_t)rel);
                } else {
                    if (patch_count < patch_cap) patches[patch_count++] = (JumpPatch){lbl, j->size};
                    emit_u32(j, 0);
                }
                break;
            }

            case IR_JZ:
            case IR_JNZ: {
                int r = get_vreg_int_reg(ir, ins->src1, PREG_R10, j);
                emit_test_rr(j, r, r);
                int lbl = (int)ins->imm;
                uint8_t cond = (ins->op == IR_JZ) ? 0x84 : 0x85;
                emit_u8(j, 0x0F); emit_u8(j, cond);
                if (lbl >= 0 && lbl < label_cap && label_offsets[lbl] >= 0) {
                    int32_t rel = (int32_t)(label_offsets[lbl] - (j->size + 4));
                    emit_u32(j, (uint32_t)rel);
                } else {
                    if (patch_count < patch_cap) patches[patch_count++] = (JumpPatch){lbl, j->size};
                    emit_u32(j, 0);
                }
                break;
            }

            case IR_CALL: {
                if (ir->uses_avx) emit_vzeroupper(j);
                if (ins->func_ptr == (void*)helper_print_f32 ||
                    ins->func_ptr == (void*)helper_print_f64 ||
                    ins->func_ptr == (void*)vortex_math_sinf ||
                    ins->func_ptr == (void*)vortex_math_cosf ||
                    ins->func_ptr == (void*)vortex_math_sqrtf ||
                    ins->func_ptr == (void*)vortex_math_tanf ||
                    ins->func_ptr == (void*)vortex_math_floorf ||
                    ins->func_ptr == (void*)vortex_math_ceilf ||
                    ins->func_ptr == (void*)vortex_math_fabsf ||
                    ins->func_ptr == (void*)vortex_math_expf ||
                    ins->func_ptr == (void*)vortex_math_logf ||
                    ins->func_ptr == (void*)vortex_math_sin_f64 ||
                    ins->func_ptr == (void*)vortex_math_cos_f64 ||
                    ins->func_ptr == (void*)vortex_math_sqrt_f64 ||
                    ins->func_ptr == (void*)vortex_math_tan_f64 ||
                    ins->func_ptr == (void*)vortex_math_floor_f64 ||
                    ins->func_ptr == (void*)vortex_math_ceil_f64 ||
                    ins->func_ptr == (void*)vortex_math_fabs_f64 ||
                    ins->func_ptr == (void*)vortex_math_exp_f64 ||
                    ins->func_ptr == (void*)vortex_math_log_f64) {
                    int x = get_vreg_float_reg(ir, ins->args[0], 0, j);
                    if (x != 0) emit_movaps_xx(j, 0, x);
                    emit_u8(j, 0x48); emit_u8(j, 0x83); emit_u8(j, 0xEC); emit_u8(j, 0x20);
                    emit_u8(j, 0x48); emit_u8(j, 0xB8); emit_u64(j, (uint64_t)ins->func_ptr);
                    emit_u8(j, 0xFF); emit_u8(j, 0xD0);
                    emit_u8(j, 0x48); emit_u8(j, 0x83); emit_u8(j, 0xC4); emit_u8(j, 0x20);
                } else if (ins->func_ptr == (void*)vortex_math_powf ||
                           ins->func_ptr == (void*)vortex_math_atan2f ||
                           ins->func_ptr == (void*)vortex_math_pow_f64 ||
                           ins->func_ptr == (void*)vortex_math_atan2_f64) {
                    int x0 = get_vreg_float_reg(ir, ins->args[0], 0, j);
                    int x1 = get_vreg_float_reg(ir, ins->args[1], 1, j);
                    if (x0 == 1 && x1 == 0) {
                        emit_movaps_xx(j, 2, 1);
                        emit_movaps_xx(j, 1, 0);
                        emit_movaps_xx(j, 0, 2);
                    } else if (x1 == 0) {
                        emit_movaps_xx(j, 1, 0);
                        if (x0 != 0) emit_movaps_xx(j, 0, x0);
                    } else {
                        if (x0 != 0) emit_movaps_xx(j, 0, x0);
                        if (x1 != 1) emit_movaps_xx(j, 1, x1);
                    }
                    emit_u8(j, 0x48); emit_u8(j, 0x83); emit_u8(j, 0xEC); emit_u8(j, 0x20);
                    emit_u8(j, 0x48); emit_u8(j, 0xB8); emit_u64(j, (uint64_t)ins->func_ptr);
                    emit_u8(j, 0xFF); emit_u8(j, 0xD0);
                    emit_u8(j, 0x48); emit_u8(j, 0x83); emit_u8(j, 0xC4); emit_u8(j, 0x20);
                } else {
                    for (int a = 0; a < ins->arg_count; a++) {
                        VReg av = ins->args[a];
                        if (ir->vregs[av].is_float) {
                            int x = get_vreg_float_reg(ir, av, 4, j);
                            uint8_t rex = (uint8_t)(0x48 | (x >= 8 ? 4 : 0));
                            emit_u8(j, 0x66); emit_u8(j, rex); emit_u8(j, 0x0F); emit_u8(j, 0x7E);
                            emit_u8(j, (uint8_t)(0xC0 | ((x & 7) << 3)));
                            emit_u8(j, 0x50);
                        } else {
                            int r = get_vreg_int_reg(ir, av, PREG_RAX, j);
                            emit_push_r(j, r);
                        }
                    }
                    emit_call_with_args(j, ins->func_ptr, ins->arg_count);
                }
                if (ins->dest != VREG_NONE) {
                    if (ins->type == TYPE_F32 || ins->type == TYPE_F64) {
                        commit_vreg_float_reg(ir, ins->dest, 0, j);
                    } else {
                        commit_vreg_int_reg(ir, ins->dest, PREG_RAX, j);
                    }
                }
                break;
            }

            case IR_CALL_USER: {
                int num_args = ins->arg_count;
                size_t call_pos = 0;
                if (num_args <= 2) {
                    int r_a0 = (g_target_abi == VORTEX_ABI_SYSV64) ? PREG_RDI : PREG_RCX;
                    int r_a1 = (g_target_abi == VORTEX_ABI_SYSV64) ? PREG_RSI : PREG_RDX;
                    if (num_args == 2) {
                        VReg a1 = ins->args[1];
                        if (ir->vregs[a1].is_float) {
                            int xr = get_vreg_float_reg(ir, a1, 4, j);
                            emit_u8(j, 0x66);
                            uint8_t rex = (uint8_t)(0x40 | (ir->vregs[a1].type == TYPE_F64 ? 0x08 : 0) | (xr >= 8 ? 0x04 : 0) | 0x01);
                            emit_u8(j, rex);
                            emit_u8(j, 0x0F); emit_u8(j, 0x7E);
                            emit_u8(j, (uint8_t)(0xC0 | ((xr & 7) << 3) | (PREG_R11 & 7)));
                        } else {
                            int r1 = get_vreg_int_reg(ir, a1, PREG_R11, j);
                            if (r1 != PREG_R11) emit_mov_rr(j, PREG_R11, r1);
                        }
                    }
                    if (num_args >= 1) {
                        VReg a0 = ins->args[0];
                        if (ir->vregs[a0].is_float) {
                            int xr = get_vreg_float_reg(ir, a0, 4, j);
                            emit_u8(j, 0x66);
                            uint8_t rex = (uint8_t)(0x40 | (ir->vregs[a0].type == TYPE_F64 ? 0x08 : 0) | (xr >= 8 ? 0x04 : 0) | (r_a0 >= 8 ? 0x01 : 0));
                            if (rex != 0x40) emit_u8(j, rex);
                            emit_u8(j, 0x0F); emit_u8(j, 0x7E);
                            emit_u8(j, (uint8_t)(0xC0 | ((xr & 7) << 3) | (r_a0 & 7)));
                        } else {
                            int r0 = get_vreg_int_reg(ir, a0, r_a0, j);
                            if (r0 != r_a0) emit_mov_rr(j, r_a0, r0);
                        }
                    }
                    if (num_args == 2) {
                        emit_mov_rr(j, r_a1, PREG_R11);
                    }
                    if (g_target_abi != VORTEX_ABI_SYSV64) {
                        if (num_args >= 1 && ir->vregs[ins->args[0]].is_float) {
                            emit_u8(j, 0x66); emit_u8(j, 0x48); emit_u8(j, 0x0F); emit_u8(j, 0x6E); emit_u8(j, 0xC1); /* movq xmm0, rcx */
                        }
                        if (num_args == 2 && ir->vregs[ins->args[1]].is_float) {
                            emit_u8(j, 0x66); emit_u8(j, 0x48); emit_u8(j, 0x0F); emit_u8(j, 0x6E); emit_u8(j, 0xCA); /* movq xmm1, rdx */
                        }
                    }
                    bool need_shadow = (g_target_abi != VORTEX_ABI_SYSV64) &&
                                       (!ir->optimize_frame || num_args > 1 ||
                                        (num_args == 1 && ir->vregs[ins->args[0]].is_float));
                    if (need_shadow) {
                        emit_u8(j, 0x48); emit_u8(j, 0x83); emit_u8(j, 0xEC); emit_u8(j, 0x20);
                    }
                    emit_u8(j, 0xE8);
                    call_pos = j->size;
                    emit_u32(j, 0);
                    if (need_shadow) {
                        emit_u8(j, 0x48); emit_u8(j, 0x83); emit_u8(j, 0xC4); emit_u8(j, 0x20);
                    }
                } else {
                    for (int a = 0; a < num_args; a++) {
                        if (ir->vregs[ins->args[a]].is_float) {
                            int xr = get_vreg_float_reg(ir, ins->args[a], 4, j);
                            emit_u8(j, 0x66);
                            if (ir->vregs[ins->args[a]].type == TYPE_F64) {
                                emit_u8(j, (xr >= 8) ? 0x4C : 0x48);
                            } else if (xr >= 8) {
                                emit_u8(j, 0x44);
                            }
                            emit_u8(j, 0x0F); emit_u8(j, 0x7E);
                            emit_u8(j, (uint8_t)(0xC0 | ((xr & 7) << 3)));
                            emit_push_r(j, PREG_RAX);
                        } else {
                            int r = get_vreg_int_reg(ir, ins->args[a], PREG_RAX, j);
                            emit_push_r(j, r);
                        }
                    }

                    int alloc_space = 0;
                    if (g_target_abi == VORTEX_ABI_SYSV64) {
                        alloc_space = (num_args > 6) ? (num_args - 6) * 8 : 0;
                        if ((num_args * 8 + alloc_space) % 16 != 0) alloc_space += 8;
                        emit_sub_rsp(j, alloc_space);

                        for (int k = 6; k < num_args; k++) {
                            int src_off = alloc_space + (num_args - 1 - k) * 8;
                            int dst_off = (k - 6) * 8;
                            emit_load_mem_offset(j, PREG_R10, PREG_RSP, src_off);
                            emit_store_mem_offset(j, PREG_RSP, dst_off, PREG_R10);
                        }

                        static const int sysv_uargs[6] = { PREG_RDI, PREG_RSI, PREG_RDX, PREG_RCX, PREG_R8, PREG_R9 };
                        for (int k = 0; k < num_args && k < 6; k++) {
                            int off = alloc_space + (num_args - 1 - k) * 8;
                            emit_load_mem_offset(j, sysv_uargs[k], PREG_RSP, off);
                        }
                    } else {
                        int min_space = (num_args <= 4) ? 32 : (32 + (num_args - 4) * 8);
                        alloc_space = min_space;
                        if ((num_args * 8 + alloc_space) % 16 != 0) alloc_space += 8;
                        emit_sub_rsp(j, alloc_space);

                        for (int k = 4; k < num_args; k++) {
                            int src_off = alloc_space + (num_args - 1 - k) * 8;
                            int dst_off = 32 + (k - 4) * 8;
                            emit_load_mem_offset(j, PREG_R10, PREG_RSP, src_off);
                            emit_store_mem_offset(j, PREG_RSP, dst_off, PREG_R10);
                        }

                        static const int win_uargs[4] = { PREG_RCX, PREG_RDX, PREG_R8, PREG_R9 };
                        for (int k = 0; k < 4 && k < num_args; k++) {
                            int offk = alloc_space + (num_args - 1 - k) * 8;
                            if (ir->vregs[ins->args[k]].is_float) {
                                if (ir->vregs[ins->args[k]].type == TYPE_F64) emit_loadsd_offset(j, k, PREG_RSP, offk);
                                else emit_loadss_offset(j, k, PREG_RSP, offk);
                            } else {
                                emit_load_mem_offset(j, win_uargs[k], PREG_RSP, offk);
                            }
                        }
                    }

                    emit_u8(j, 0xE8);
                    call_pos = j->size;
                    emit_u32(j, 0);

                    int total_cleanup = alloc_space + num_args * 8;
                    if (total_cleanup > 0) {
                        if (total_cleanup <= 127) {
                            emit_u8(j, 0x48); emit_u8(j, 0x83); emit_u8(j, 0xC4); emit_u8(j, (uint8_t)total_cleanup);
                        } else {
                            emit_u8(j, 0x48); emit_u8(j, 0x81); emit_u8(j, 0xC4); emit_u32(j, (uint32_t)total_cleanup);
                        }
                    }
                }

                if (g_user_func_reloc_count < MAX_USER_FUNC_RELOCS) {
                    UserFuncReloc* ufr = &g_user_func_relocs[g_user_func_reloc_count++];
                    ufr->patch_pos = call_pos;
                    strncpy(ufr->name, ins->func_name, sizeof(ufr->name) - 1);
                    ufr->name[sizeof(ufr->name) - 1] = '\0';
                    ufr->called_in_unsafe = (ins->imm != 0);
                    ufr->call_line = ins->line;
                    ufr->call_file = vortex_intern_cstr(ins->file ? ins->file : "<unknown>");
                }

                if (ins->dest != VREG_NONE) {
                    if (ir->vregs[ins->dest].is_float) {
                        commit_vreg_float_reg(ir, ins->dest, 0, j);
                    } else {
                        commit_vreg_int_reg(ir, ins->dest, PREG_RAX, j);
                    }
                }
                break;
            }

            case IR_CALL_FFI: {
                int num_args = ins->arg_count;

                for (int a = 0; a < num_args; a++) {
                    VReg av = ins->args[a];
                    if (ir->vregs[av].is_float) {
                        int x = get_vreg_float_reg(ir, av, 4, j);
                        if (ir->vregs[av].type == TYPE_F64) {
                            if (ins->arg_types[a] == TYPE_F32) {
                                emit_cvtsd2ss_xx(j, 4, x);
                                emit_u8(j, 0x48); emit_u8(j, 0x83); emit_u8(j, 0xEC); emit_u8(j, 0x08);
                                emit_u8(j, 0xF3); emit_u8(j, 0x0F); emit_u8(j, 0x11); emit_u8(j, 0x24); emit_u8(j, 0x24);
                            } else {
                                emit_u8(j, 0x48); emit_u8(j, 0x83); emit_u8(j, 0xEC); emit_u8(j, 0x08);
                                emit_u8(j, 0xF2);
                                if (x >= 8) emit_u8(j, 0x44);
                                emit_u8(j, 0x0F); emit_u8(j, 0x11);
                                emit_u8(j, (uint8_t)(0x04 | ((x & 7) << 3)));
                                emit_u8(j, 0x24);
                            }
                        } else if (ins->arg_types[a] == TYPE_F64) {
                            emit_u8(j, 0xF3);
                            if (x >= 8) emit_u8(j, 0x41);
                            emit_u8(j, 0x0F); emit_u8(j, 0x5A);
                            emit_u8(j, (uint8_t)(0xC0 | (4 << 3) | (x & 7)));
                            emit_u8(j, 0x48); emit_u8(j, 0x83); emit_u8(j, 0xEC); emit_u8(j, 0x08);
                            emit_u8(j, 0xF2); emit_u8(j, 0x0F); emit_u8(j, 0x11); emit_u8(j, 0x24); emit_u8(j, 0x24);
                        } else {
                            emit_u8(j, 0x48); emit_u8(j, 0x83); emit_u8(j, 0xEC); emit_u8(j, 0x08);
                            emit_u8(j, 0xF3);
                            if (x >= 8) emit_u8(j, 0x44);
                            emit_u8(j, 0x0F); emit_u8(j, 0x11);
                            emit_u8(j, (uint8_t)(0x04 | ((x & 7) << 3)));
                            emit_u8(j, 0x24);
                        }
                    } else {
                        int r = get_vreg_int_reg(ir, av, PREG_RAX, j);
                        emit_push_r(j, r);
                    }
                }

#ifdef _WIN32
                int min_space = (num_args <= 4) ? 32 : (32 + (num_args - 4) * 8);
                int alloc_space = min_space;
                if ((num_args * 8 + alloc_space) % 16 != 0) alloc_space += 8;
                emit_sub_rsp(j, alloc_space);

                for (int k = 4; k < num_args; k++) {
                    int src_off = alloc_space + (num_args - 1 - k) * 8;
                    int dst_off = 32 + (k - 4) * 8;
                    emit_load_mem_offset(j, PREG_R10, PREG_RSP, src_off);
                    emit_store_mem_offset(j, PREG_RSP, dst_off, PREG_R10);
                }

                static const int win_arg_gprs[4] = { PREG_RCX, PREG_RDX, PREG_R8, PREG_R9 };
                for (int k = 0; k < 4 && k < num_args; k++) {
                    int offk = alloc_space + (num_args - 1 - k) * 8;
                    if (ins->arg_types[k] == TYPE_F64) {
                        emit_loadsd_offset(j, k, PREG_RSP, offk);
                    } else if (ins->arg_types[k] == TYPE_F32) {
                        emit_loadss_offset(j, k, PREG_RSP, offk);
                    } else {
                        emit_load_mem_offset(j, win_arg_gprs[k], PREG_RSP, offk);
                    }
                }

                emit_vzeroupper(j);
                emit_u8(j, 0x48); emit_u8(j, 0xB8); emit_u64(j, (uint64_t)ins->func_ptr);
                emit_u8(j, 0xFF); emit_u8(j, 0xD0);

                int total_cleanup = alloc_space + num_args * 8;
                emit_u8(j, 0x48); emit_u8(j, 0x81); emit_u8(j, 0xC4); emit_u32(j, (uint32_t)total_cleanup);
#else
                int int_count = 0;
                int flt_count = 0;
                int stack_arg_count = 0;
                for (int k = 0; k < num_args; k++) {
                    bool is_flt = (ins->arg_types[k] == TYPE_F32 || ins->arg_types[k] == TYPE_F64);
                    if (is_flt) {
                        if (flt_count < 8) flt_count++;
                        else stack_arg_count++;
                    } else {
                        if (int_count < 6) int_count++;
                        else stack_arg_count++;
                    }
                }

                int alloc_space = stack_arg_count * 8;
                if ((num_args * 8 + alloc_space) % 16 != 0) alloc_space += 8;
                emit_sub_rsp(j, alloc_space);

                int cur_int = 0;
                int cur_flt = 0;
                int cur_stack = 0;
                static const int sysv_gprs[6] = { PREG_RDI, PREG_RSI, PREG_RDX, PREG_RCX, PREG_R8, PREG_R9 };
                for (int k = 0; k < num_args; k++) {
                    int src_off = alloc_space + (num_args - 1 - k) * 8;
                    bool is_flt = (ins->arg_types[k] == TYPE_F32 || ins->arg_types[k] == TYPE_F64);
                    if (is_flt) {
                        if (cur_flt < 8) {
                            if (ins->arg_types[k] == TYPE_F64) emit_loadsd_offset(j, cur_flt, PREG_RSP, src_off);
                            else emit_loadss_offset(j, cur_flt, PREG_RSP, src_off);
                            cur_flt++;
                        } else {
                            int dst_off = cur_stack * 8;
                            cur_stack++;
                            emit_load_mem_offset(j, PREG_R10, PREG_RSP, src_off);
                            emit_store_mem_offset(j, PREG_RSP, dst_off, PREG_R10);
                        }
                    } else {
                        if (cur_int < 6) {
                            emit_load_mem_offset(j, sysv_gprs[cur_int], PREG_RSP, src_off);
                            cur_int++;
                        } else {
                            int dst_off = cur_stack * 8;
                            cur_stack++;
                            emit_load_mem_offset(j, PREG_R10, PREG_RSP, src_off);
                            emit_store_mem_offset(j, PREG_RSP, dst_off, PREG_R10);
                        }
                    }
                }

                emit_u8(j, 0xB0); emit_u8(j, (uint8_t)cur_flt);
                emit_vzeroupper(j);
                emit_u8(j, 0x48); emit_u8(j, 0xB8); emit_u64(j, (uint64_t)ins->func_ptr);
                emit_u8(j, 0xFF); emit_u8(j, 0xD0);

                int total_cleanup = alloc_space + num_args * 8;
                emit_u8(j, 0x48); emit_u8(j, 0x81); emit_u8(j, 0xC4); emit_u32(j, (uint32_t)total_cleanup);
#endif

                if (ins->dest != VREG_NONE) {
                    if (ins->ret_type == TYPE_F64) {
                        if (ir->vregs[ins->dest].type != TYPE_F64) {
                            emit_u8(j, 0xF2); emit_u8(j, 0x0F); emit_u8(j, 0x5A); emit_u8(j, 0xC0);
                        }
                        commit_vreg_float_reg(ir, ins->dest, 0, j);
                    } else if (ins->ret_type == TYPE_F32) {
                        commit_vreg_float_reg(ir, ins->dest, 0, j);
                    } else if (ins->ret_type == TYPE_I32) {
                        emit_u8(j, 0x48); emit_u8(j, 0x63); emit_u8(j, 0xC0);
                        commit_vreg_int_reg(ir, ins->dest, PREG_RAX, j);
                    } else if (ins->ret_type != TYPE_VOID) {
                        commit_vreg_int_reg(ir, ins->dest, PREG_RAX, j);
                    }
                }
                break;
            }

            case IR_LOAD_PARAM: {
                int param_idx = (int)ins->imm;
                int disp = (g_target_abi == VORTEX_ABI_SYSV64)
                         ? ((param_idx < 6) ? -(64 + 8 * param_idx) : (16 + 8 * (param_idx - 6)))
                         : (16 + 8 * param_idx);
                if (ir->vregs[ins->dest].is_float) {
                    int xrd = (ir->vregs[ins->dest].is_spilled || ir->vregs[ins->dest].preg == PREG_NONE)
                              ? 0 : ir->vregs[ins->dest].preg;
                    emit_u8(j, (ir->vregs[ins->dest].type == TYPE_F64) ? 0xF2 : 0xF3);
                    if (xrd >= 8) emit_u8(j, 0x44);
                    emit_u8(j, 0x0F); emit_u8(j, 0x10);
                    if (disp >= -128 && disp <= 127) {
                        emit_u8(j, (uint8_t)(0x45 | ((xrd & 7) << 3)));
                        emit_u8(j, (uint8_t)disp);
                    } else {
                        emit_u8(j, (uint8_t)(0x85 | ((xrd & 7) << 3)));
                        emit_u32(j, (uint32_t)disp);
                    }
                    commit_vreg_float_reg(ir, ins->dest, xrd, j);
                } else {
                    if (ir->optimize_frame && ir->param_count == 1 && param_idx == 0) {
                        int src_reg = (g_target_abi == VORTEX_ABI_SYSV64) ? PREG_RDI : PREG_RCX;
                        int rd = ir->vregs[ins->dest].is_spilled ? PREG_R10 : ir->vregs[ins->dest].preg;
                        if (rd != src_reg) emit_mov_rr(j, rd, src_reg);
                        commit_vreg_int_reg(ir, ins->dest, rd, j);
                    } else {
                        int rd = ir->vregs[ins->dest].is_spilled ? PREG_R10 : ir->vregs[ins->dest].preg;
                        emit_load_mem_offset(j, rd, PREG_RBP, disp);
                        commit_vreg_int_reg(ir, ins->dest, rd, j);
                    }
                }
                break;
            }

            case IR_RET: {
                if (ir->uses_avx) emit_vzeroupper(j);
                if (ins->src1 != VREG_NONE) {
                    if (ins->type == TYPE_F32 || ins->type == TYPE_F64) {
                        int x = get_vreg_float_reg(ir, ins->src1, 0, j);
                        if (x != 0) emit_movaps_xx(j, 0, x);
                    } else {
                        int r = get_vreg_int_reg(ir, ins->src1, PREG_RAX, j);
                        if (r != PREG_RAX) emit_mov_rr(j, PREG_RAX, r);
                    }
                }
                emit_func_epilogue(j, compute_frame_size(ir->total_spill_bytes));
                break;
            }

            case IR_RAW_RIEMANN:
            case IR_VEC_LOOP: {
                emit_avx2_vector_loop(j, ir, ins);
                break;
            }

            case IR_VEC_ARR_OP: {
                emit_avx2_i64_array_loop(j, ir, ins);
                break;
            }

            case IR_VEC_GRID_MOD: {
                emit_avx2_grid_mod_loop(j, ir, ins);
                break;
            }

            case IR_VEC_MOD_FILL: {
                emit_avx2_i64_mod_fill_loop(j, ir, ins);
                break;
            }
        }
        if (ins->line > 0 && j->size > ins_start_off) {
            debug_info_record_line(ins_start_off, j->size, ins->line, ins->file ? ins->file : g_current_filename);
        }
    }

    for (int p = 0; p < patch_count; p++) {
        int lbl = patches[p].label_id;
        if (lbl >= 0 && lbl < label_cap && label_offsets[lbl] >= 0) {
            int32_t rel = (int32_t)(label_offsets[lbl] - (patches[p].patch_pos + 4));
            memcpy(&j->code[patches[p].patch_pos], &rel, 4);
        }
    }
    if (cold_count > 0) {
        size_t skip_traps = emit_jmp_placeholder(j);
        for (int c = 0; c < cold_count; c++) {
            int32_t rel = (int32_t)(j->size - (cold_traps[c].jz_patch + 4));
            memcpy(&j->code[cold_traps[c].jz_patch], &rel, 4);
            if (cold_traps[c].jae_patch != (size_t)-1) {
                int32_t rel2 = (int32_t)(j->size - (cold_traps[c].jae_patch + 4));
                memcpy(&j->code[cold_traps[c].jae_patch], &rel2, 4);
            }
            emit_bounds_check_slow_call(j, ir, cold_traps[c].ins);
            emit_u8(j, 0x0F); emit_u8(j, 0x0B); /* ud2: never returns on error */
        }
        patch_jump(j, skip_traps);
    }
    if (cold_traps != small_traps) free(cold_traps);
    if (label_offsets != small_labels) free(label_offsets);
    if (patches != small_patches) free(patches);
}

static void patch_call_rel_x64(JITBuffer* j, size_t patch_pos, size_t target_offset) {
    int32_t rel = (int32_t)(target_offset - (patch_pos + 4));
    memcpy(&j->code[patch_pos], &rel, 4);
}

const VortexJITBackend g_backend_x64 = {
    VORTEX_ARCH_X86_64,
    "x86_64-avx2",
    compute_frame_size,
    emit_prologue,
    emit_epilogue,
    emit_func_prologue,
    emit_func_epilogue,
    emit_jmp_placeholder,
    patch_jump,
    patch_call_rel_x64,
    ir_codegen_x86_64
};
