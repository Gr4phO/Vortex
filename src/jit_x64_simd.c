/*
 * VORTEX X86-64 AVX2 / FMA3 SIMD LOOP EMITTERS
 */

#include "jit_x64_internal.h"

/* --- AVX2 / FMA3 SIMD TABLES & EMITTERS --- */
static const float g_simd_lane_offsets_A[8] __attribute__((aligned(32))) = {
    0.0f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f
};
static const float g_simd_lane_offsets_B[8] __attribute__((aligned(32))) = {
    8.0f, 9.0f, 10.0f, 11.0f, 12.0f, 13.0f, 14.0f, 15.0f
};
static const float g_simd_lane_offsets_C[8] __attribute__((aligned(32))) = {
    16.0f, 17.0f, 18.0f, 19.0f, 20.0f, 21.0f, 22.0f, 23.0f
};
static const float g_simd_lane_offsets_D[8] __attribute__((aligned(32))) = {
    24.0f, 25.0f, 26.0f, 27.0f, 28.0f, 29.0f, 30.0f, 31.0f
};
static const float g_simd_sixteen[8] __attribute__((aligned(32))) = {
    16.0f, 16.0f, 16.0f, 16.0f, 16.0f, 16.0f, 16.0f, 16.0f
};
static const float g_simd_thirtytwo[8] __attribute__((aligned(32))) = {
    32.0f, 32.0f, 32.0f, 32.0f, 32.0f, 32.0f, 32.0f, 32.0f
};

static bool cpu_has_fma3(void) {
    static int cached = -1;
    if (cached >= 0) return cached != 0;
#if defined(__x86_64__) || defined(_M_X64)
#ifdef _MSC_VER
    int info[4];
    __cpuid(info, 1);
    cached = ((info[2] & (1 << 12)) && (info[2] & (1 << 28))) ? 1 : 0;
#else
    uint32_t eax, ebx, ecx, edx;
    __asm__ __volatile__("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1), "c"(0));
    cached = ((ecx & (1u << 12)) && (ecx & (1u << 28))) ? 1 : 0;
#endif
#else
    cached = 0;
#endif
    return cached != 0;
}

static void emit_vex_ymm(JITBuffer* j, uint8_t opcode, int dest, int src1, int src2) {
    int r_bit = (dest < 8) ? 1 : 0;
    int b_bit = (src2 < 8) ? 1 : 0;
    int v_bits = (~src1) & 0x0F;
    if (src2 < 8) {
        uint8_t b1 = (uint8_t)((r_bit << 7) | (v_bits << 3) | 0x04);
        uint8_t modrm = (uint8_t)(0xC0 | ((dest & 7) << 3) | (src2 & 7));
        emit_u8(j, 0xC5); emit_u8(j, b1); emit_u8(j, opcode); emit_u8(j, modrm);
    } else {
        uint8_t b1 = (uint8_t)((r_bit << 7) | 0x40 | (b_bit << 5) | 0x01);
        uint8_t b2 = (uint8_t)((v_bits << 3) | 0x04);
        uint8_t modrm = (uint8_t)(0xC0 | ((dest & 7) << 3) | (src2 & 7));
        emit_u8(j, 0xC4); emit_u8(j, b1); emit_u8(j, b2); emit_u8(j, opcode); emit_u8(j, modrm);
    }
}

static void emit_vxorps_ymm(JITBuffer* j, int reg) {
    emit_vex_ymm(j, 0x57, reg, reg, reg);
}

static void emit_vmovaps_ymm_yy(JITBuffer* j, int dest, int src) {
    if (dest == src) return;
    emit_vex_ymm(j, 0x28, dest, 0, src);
}

static void emit_vmovss_zero_upper(JITBuffer* j, int dest, int src_xmm) {
    /* VEX.NDS.128.F3.0F 10 /r : dest[31:0] = src_xmm[31:0], upper bits zeroed via zeroed scratch z */
    int z = (src_xmm == 1) ? 2 : 1;
    emit_vxorps_ymm(j, z);
    uint8_t b1 = (uint8_t)(((dest < 8) ? 0x80 : 0) | 0x40 | ((src_xmm < 8) ? 0x20 : 0) | 0x01);
    uint8_t b2 = (uint8_t)((((~z) & 0x0F) << 3) | 0x02);
    emit_u8(j, 0xC4); emit_u8(j, b1); emit_u8(j, b2);
    emit_u8(j, 0x10);
    emit_u8(j, (uint8_t)(0xC0 | ((dest & 7) << 3) | (src_xmm & 7)));
}

static void emit_vfmadd231ps_ymm(JITBuffer* j, int dst, int src1, int src2) {
    /* VEX.256.66.0F38.W0 B8 /r : dst = src1 * src2 + dst */
    uint8_t b1 = (uint8_t)(((dst < 8) ? 0x80 : 0) | 0x40 | ((src2 < 8) ? 0x20 : 0) | 0x02);
    uint8_t b2 = (uint8_t)((((~src1) & 0x0F) << 3) | 0x04 | 0x01);
    emit_u8(j, 0xC4); emit_u8(j, b1); emit_u8(j, b2);
    emit_u8(j, 0xB8);
    emit_u8(j, (uint8_t)(0xC0 | ((dst & 7) << 3) | (src2 & 7)));
}

static void emit_vbroadcastss_yx(JITBuffer* j, int dest, int src) {
    int r_bit = (dest < 8) ? 1 : 0;
    int b_bit = (src < 8) ? 1 : 0;
    uint8_t b1 = (uint8_t)((r_bit << 7) | 0x40 | (b_bit << 5) | 0x02);
    uint8_t modrm = (uint8_t)(0xC0 | ((dest & 7) << 3) | (src & 7));
    emit_u8(j, 0xC4); emit_u8(j, b1); emit_u8(j, 0x7D); emit_u8(j, 0x18); emit_u8(j, modrm);
}

static void emit_vmovups_ymm_mem(JITBuffer* j, int dest, int base_reg) {
    int r_bit = (dest < 8) ? 1 : 0;
    int b_bit = (base_reg < 8) ? 1 : 0;
    uint8_t b1 = (uint8_t)((r_bit << 7) | 0x40 | (b_bit << 5) | 0x01);
    uint8_t modrm = (uint8_t)(0x00 | ((dest & 7) << 3) | (base_reg & 7));
    emit_u8(j, 0xC4); emit_u8(j, b1); emit_u8(j, 0x7C); emit_u8(j, 0x10); emit_u8(j, modrm);
}

static void emit_avx2_reduce_ymm0_ymm1(JITBuffer* j) {
    emit_u8(j, 0xC5); emit_u8(j, 0xFC); emit_u8(j, 0x58); emit_u8(j, 0xC1);
    emit_u8(j, 0xC4); emit_u8(j, 0xE3); emit_u8(j, 0x7D); emit_u8(j, 0x19); emit_u8(j, 0xC1); emit_u8(j, 0x01);
    emit_u8(j, 0xC5); emit_u8(j, 0xF8); emit_u8(j, 0x58); emit_u8(j, 0xC1);
    emit_u8(j, 0xC5); emit_u8(j, 0xFB); emit_u8(j, 0x7C); emit_u8(j, 0xC0);
    emit_u8(j, 0xC5); emit_u8(j, 0xFB); emit_u8(j, 0x7C); emit_u8(j, 0xC0);
    emit_vzeroupper(j);
}


static void emit_save_xmm_range(JITBuffer* j, int first_xmm, int count) {
    if (g_target_abi != VORTEX_ABI_WIN64 || count <= 0) return;
    int stack_bytes = ((count * 16) + 15) & ~15;
    emit_u8(j, 0x48); emit_u8(j, 0x81); emit_u8(j, 0xEC); emit_u32(j, (uint32_t)stack_bytes);
    for (int k = 0; k < count; k++) {
        int xr = first_xmm + k;
        int32_t off = k * 16;
        if (xr >= 8) emit_u8(j, 0x44);
        emit_u8(j, 0x0F); emit_u8(j, 0x11);
        if (off == 0) {
            emit_u8(j, (uint8_t)(0x04 | ((xr & 7) << 3)));
            emit_u8(j, 0x24);
        } else if (off <= 127) {
            emit_u8(j, (uint8_t)(0x44 | ((xr & 7) << 3)));
            emit_u8(j, 0x24);
            emit_u8(j, (uint8_t)off);
        } else {
            emit_u8(j, (uint8_t)(0x84 | ((xr & 7) << 3)));
            emit_u8(j, 0x24);
            emit_u32(j, (uint32_t)off);
        }
    }
}

static void emit_restore_xmm_range(JITBuffer* j, int first_xmm, int count) {
    if (g_target_abi != VORTEX_ABI_WIN64 || count <= 0) return;
    int stack_bytes = ((count * 16) + 15) & ~15;
    for (int k = 0; k < count; k++) {
        int xr = first_xmm + k;
        int32_t off = k * 16;
        if (xr >= 8) emit_u8(j, 0x44);
        emit_u8(j, 0x0F); emit_u8(j, 0x10);
        if (off == 0) {
            emit_u8(j, (uint8_t)(0x04 | ((xr & 7) << 3)));
            emit_u8(j, 0x24);
        } else if (off <= 127) {
            emit_u8(j, (uint8_t)(0x44 | ((xr & 7) << 3)));
            emit_u8(j, 0x24);
            emit_u8(j, (uint8_t)off);
        } else {
            emit_u8(j, (uint8_t)(0x84 | ((xr & 7) << 3)));
            emit_u8(j, 0x24);
            emit_u32(j, (uint32_t)off);
        }
    }
    emit_u8(j, 0x48); emit_u8(j, 0x81); emit_u8(j, 0xC4); emit_u32(j, (uint32_t)stack_bytes);
}

void emit_avx2_vector_loop(JITBuffer* j, IRBuffer* ir, IRInstruction* ins) {
    int i_reg = get_vreg_int_reg(ir, ins->i_vreg, PREG_RAX, j);
    if (i_reg != PREG_RAX) emit_mov_rr(j, PREG_RAX, i_reg);

    int lim_reg = get_vreg_int_reg(ir, ins->lim_vreg, PREG_RCX, j);
    if (lim_reg != PREG_RCX) emit_mov_rr(j, PREG_RCX, lim_reg);

    /* Stage x_init in xmm5, step in xmm4, acc in xmm0 BEFORE saving Win64 xmm6..xmm15 */
    int x_reg = get_vreg_float_reg(ir, ins->x_vreg, 5, j);
    if (x_reg != 5) emit_movaps_xx(j, 5, x_reg);

    int step_reg = get_vreg_float_reg(ir, ins->step_vreg, 4, j);
    if (step_reg != 4) emit_movaps_xx(j, 4, step_reg);

    int acc_src = get_vreg_float_reg(ir, ins->acc_vreg, 0, j);
    emit_vmovss_zero_upper(j, 0, acc_src);
    emit_vxorps_ymm(j, 1);

    emit_save_xmm_range(j, 6, 10);
    emit_movaps_xx(j, 13, 4); /* keep scalar step in xmm13 after xmm6..xmm15 are saved */

    float const1_val = (ins->num_val != 0.0f) ? ins->num_val : 4.0f;
    float const2_val = (ins->den_base != 0.0f) ? ins->den_base : 1.0f;
    for (int b = 0; b < ins->body_op_count; b++) {
        if (ins->body_ops[b].op == IR_CONST_F32) {
            if (b == 0) const1_val = ins->body_ops[b].fimm;
            else const2_val = ins->body_ops[b].fimm;
        }
    }

    if (ins->body_op_count > 0 && ins->num_val == 0.0f && ins->den_base == 0.0f) {
        /* General 16-way body_ops AVX2 loop */
        emit_vbroadcastss_yx(j, 4, 13); /* step bcast */
        emit_vbroadcastss_yx(j, 6, 5);  /* x_init bcast */

        emit_mov_ri64(j, PREG_R10, (int64_t)g_simd_lane_offsets_A);
        emit_mov_ri64(j, PREG_R11, (int64_t)g_simd_lane_offsets_B);
        emit_vmovups_ymm_mem(j, 2, PREG_R10);
        emit_vmovups_ymm_mem(j, 3, PREG_R11);

        emit_vex_ymm(j, 0x59, 2, 4, 2);
        emit_vex_ymm(j, 0x59, 3, 4, 3);
        emit_vex_ymm(j, 0x58, 8, 6, 2);
        emit_vex_ymm(j, 0x58, 9, 6, 3);

        emit_mov_ri64(j, PREG_R10, (int64_t)g_simd_sixteen);
        emit_vmovups_ymm_mem(j, 7, PREG_R10);
        emit_vex_ymm(j, 0x59, 7, 4, 7);

        emit_load_f32_imm_to(j, const1_val, 6);
        emit_vbroadcastss_yx(j, 14, 6);
        emit_load_f32_imm_to(j, const2_val, 6);
        emit_vbroadcastss_yx(j, 15, 6);

        emit_mov_rr(j, PREG_R10, PREG_RCX);
        emit_sub_ri(j, PREG_R10, 15);

        size_t loop_start = j->size;
        emit_u8(j, 0x4C); emit_u8(j, 0x39); emit_u8(j, 0xD0); /* cmp rax, r10 */
        size_t exit_unroll = emit_jcc_placeholder(j, 0x8D);

        for (int b = 0; b < ins->body_op_count; b++) {
            IRInstruction* bo = &ins->body_ops[b];
            if (bo->op == IR_CONST_F32) continue;
            int yd = 10 + (b % 4);
            int ys1 = (bo->src1 == ins->x_vreg) ? 8 : (10 + ((b - 1 + 4) % 4));
            int ys2 = (bo->src2 == ins->x_vreg) ? 8 : (10 + ((b - 2 + 4) % 4));
            uint8_t opc = 0x58;
            if (bo->op == IR_FMUL) opc = 0x59;
            else if (bo->op == IR_FSUB) opc = 0x5C;
            else if (bo->op == IR_FDIV) opc = 0x5E;
            emit_vex_ymm(j, opc, yd, ys1, ys2);
            if (bo->dest == ins->term_vreg || b == ins->body_op_count - 1) {
                emit_vex_ymm(j, 0x58, 0, 0, yd);
            }
        }
        for (int b = 0; b < ins->body_op_count; b++) {
            IRInstruction* bo = &ins->body_ops[b];
            if (bo->op == IR_CONST_F32) continue;
            int yd = 10 + (b % 4);
            int ys1 = (bo->src1 == ins->x_vreg) ? 9 : (10 + ((b - 1 + 4) % 4));
            int ys2 = (bo->src2 == ins->x_vreg) ? 9 : (10 + ((b - 2 + 4) % 4));
            uint8_t opc = 0x58;
            if (bo->op == IR_FMUL) opc = 0x59;
            else if (bo->op == IR_FSUB) opc = 0x5C;
            else if (bo->op == IR_FDIV) opc = 0x5E;
            emit_vex_ymm(j, opc, yd, ys1, ys2);
            if (bo->dest == ins->term_vreg || b == ins->body_op_count - 1) {
                emit_vex_ymm(j, 0x58, 1, 1, yd);
            }
        }

        emit_vex_ymm(j, 0x58, 8, 8, 7);
        emit_vex_ymm(j, 0x58, 9, 9, 7);
        emit_add_ri(j, PREG_RAX, 16);

        size_t back_jmp = emit_jmp_placeholder(j);
        int32_t back_rel = (int32_t)(loop_start - (back_jmp + 4));
        memcpy(&j->code[back_jmp], &back_rel, 4);

        patch_jump(j, exit_unroll);
        emit_movaps_xx(j, 4, 8); /* current x in xmm8[0] */
        emit_avx2_reduce_ymm0_ymm1(j);
        emit_movaps_xx(j, 5, 13); /* scalar step */
    } else {
        /* 32-Way 4-Accumulator FMA3 Riemann Kernel (xmm0..3 acc, xmm4 step32, xmm5..8 x0..3) */
        emit_vxorps_ymm(j, 2);
        emit_vxorps_ymm(j, 3);

        emit_vbroadcastss_yx(j, 4, 13);  /* ymm4 = step */
        emit_vbroadcastss_yx(j, 10, 5);  /* ymm10 = x_init */

        emit_mov_ri64(j, PREG_R10, (int64_t)g_simd_lane_offsets_A);
        emit_mov_ri64(j, PREG_R11, (int64_t)g_simd_lane_offsets_B);
        emit_vmovups_ymm_mem(j, 5, PREG_R10);
        emit_vmovups_ymm_mem(j, 6, PREG_R11);
        emit_vex_ymm(j, 0x59, 5, 5, 4);
        emit_vex_ymm(j, 0x59, 6, 6, 4);
        emit_vex_ymm(j, 0x58, 5, 5, 10); /* ymm5 = x0 */
        emit_vex_ymm(j, 0x58, 6, 6, 10); /* ymm6 = x1 */

        emit_mov_ri64(j, PREG_R10, (int64_t)g_simd_lane_offsets_C);
        emit_mov_ri64(j, PREG_R11, (int64_t)g_simd_lane_offsets_D);
        emit_vmovups_ymm_mem(j, 7, PREG_R10);
        emit_vmovups_ymm_mem(j, 8, PREG_R11);
        emit_vex_ymm(j, 0x59, 7, 7, 4);
        emit_vex_ymm(j, 0x59, 8, 8, 4);
        emit_vex_ymm(j, 0x58, 7, 7, 10); /* ymm7 = x2 */
        emit_vex_ymm(j, 0x58, 8, 8, 10); /* ymm8 = x3 */

        emit_mov_ri64(j, PREG_R10, (int64_t)g_simd_thirtytwo);
        emit_vmovups_ymm_mem(j, 11, PREG_R10);
        emit_vex_ymm(j, 0x59, 4, 4, 11); /* ymm4 = 32.0f * step */

        emit_load_f32_imm_to(j, const1_val, 9);
        emit_vbroadcastss_yx(j, 14, 9);
        emit_load_f32_imm_to(j, const2_val, 9);
        emit_vbroadcastss_yx(j, 15, 9);

        emit_mov_rr(j, PREG_R10, PREG_RCX);
        emit_sub_ri(j, PREG_R10, 31);

        bool use_fma = cpu_has_fma3();
        size_t loop_start = j->size;
        emit_u8(j, 0x4C); emit_u8(j, 0x39); emit_u8(j, 0xD0); /* cmp rax, r10 */
        size_t exit_unroll = emit_jcc_placeholder(j, 0x8D);

        if (use_fma) {
            emit_vmovaps_ymm_yy(j, 9, 15);
            emit_vmovaps_ymm_yy(j, 10, 15);
            emit_vmovaps_ymm_yy(j, 11, 15);
            emit_vmovaps_ymm_yy(j, 12, 15);
            emit_vfmadd231ps_ymm(j, 9, 5, 5);
            emit_vfmadd231ps_ymm(j, 10, 6, 6);
            emit_vfmadd231ps_ymm(j, 11, 7, 7);
            emit_vfmadd231ps_ymm(j, 12, 8, 8);
        } else {
            emit_vex_ymm(j, 0x59, 9, 5, 5);
            emit_vex_ymm(j, 0x59, 10, 6, 6);
            emit_vex_ymm(j, 0x59, 11, 7, 7);
            emit_vex_ymm(j, 0x59, 12, 8, 8);
            emit_vex_ymm(j, 0x58, 9, 9, 15);
            emit_vex_ymm(j, 0x58, 10, 10, 15);
            emit_vex_ymm(j, 0x58, 11, 11, 15);
            emit_vex_ymm(j, 0x58, 12, 12, 15);
        }

        emit_vex_ymm(j, 0x5E, 9, 14, 9);
        emit_vex_ymm(j, 0x5E, 10, 14, 10);
        emit_vex_ymm(j, 0x5E, 11, 14, 11);
        emit_vex_ymm(j, 0x5E, 12, 14, 12);

        emit_vex_ymm(j, 0x58, 0, 0, 9);
        emit_vex_ymm(j, 0x58, 1, 1, 10);
        emit_vex_ymm(j, 0x58, 2, 2, 11);
        emit_vex_ymm(j, 0x58, 3, 3, 12);

        emit_vex_ymm(j, 0x58, 5, 5, 4);
        emit_vex_ymm(j, 0x58, 6, 6, 4);
        emit_vex_ymm(j, 0x58, 7, 7, 4);
        emit_vex_ymm(j, 0x58, 8, 8, 4);

        emit_add_ri(j, PREG_RAX, 32);

        size_t back_jmp = emit_jmp_placeholder(j);
        int32_t back_rel = (int32_t)(loop_start - (back_jmp + 4));
        memcpy(&j->code[back_jmp], &back_rel, 4);

        patch_jump(j, exit_unroll);

        emit_vex_ymm(j, 0x58, 2, 2, 3);
        emit_vex_ymm(j, 0x58, 0, 0, 2);
        emit_avx2_reduce_ymm0_ymm1(j);
        emit_movaps_xx(j, 4, 5);   /* xmm4 = current x from xmm5[0] */
        emit_movaps_xx(j, 5, 13);  /* xmm5 = scalar step */
    }

    emit_load_f32_imm_to(j, const1_val, 7);
    emit_load_f32_imm_to(j, const2_val, 6);

    size_t tail_start = j->size;
    emit_u8(j, 0x48); emit_u8(j, 0x39); emit_u8(j, 0xC8);
    size_t tail_exit = emit_jcc_placeholder(j, 0x8D);

    emit_u8(j, 0x44); emit_u8(j, 0x0F); emit_u8(j, 0x28); emit_u8(j, 0xC4);
    emit_u8(j, 0x45); emit_u8(j, 0x0F); emit_u8(j, 0x59); emit_u8(j, 0xC0);
    emit_u8(j, 0x44); emit_u8(j, 0x0F); emit_u8(j, 0x58); emit_u8(j, 0xC6);
    emit_u8(j, 0x44); emit_u8(j, 0x0F); emit_u8(j, 0x28); emit_u8(j, 0xCF);
    emit_u8(j, 0x45); emit_u8(j, 0x0F); emit_u8(j, 0x5E); emit_u8(j, 0xC8);
    emit_u8(j, 0x41); emit_u8(j, 0x0F); emit_u8(j, 0x58); emit_u8(j, 0xC1);
    emit_u8(j, 0x0F); emit_u8(j, 0x58); emit_u8(j, 0xE5);
    emit_u8(j, 0x48); emit_u8(j, 0xFF); emit_u8(j, 0xC0);

    size_t tail_back = emit_jmp_placeholder(j);
    int32_t tail_rel = (int32_t)(tail_start - (tail_back + 4));
    memcpy(&j->code[tail_back], &tail_rel, 4);

    patch_jump(j, tail_exit);

    emit_vzeroupper(j);
    emit_restore_xmm_range(j, 6, 10);
    commit_vreg_float_reg(ir, ins->acc_vreg, 0, j);
    commit_vreg_float_reg(ir, ins->x_vreg, 4, j);
    commit_vreg_float_reg(ir, ins->step_vreg, 5, j);
    commit_vreg_int_reg(ir, ins->i_vreg, PREG_RAX, j);
}

static const int64_t g_simd_i64_03[4] __attribute__((aligned(32))) = { 0, 1, 2, 3 };
static const int64_t g_simd_i64_47[4] __attribute__((aligned(32))) = { 4, 5, 6, 7 };
static const int64_t g_simd_i64_14[4] __attribute__((aligned(32))) = { 1, 2, 3, 4 };
static const int64_t g_simd_i64_58[4] __attribute__((aligned(32))) = { 5, 6, 7, 8 };
static const int32_t g_simd_i32_07[8] __attribute__((aligned(32))) = { 0, 1, 2, 3, 4, 5, 6, 7 };
static const int32_t g_simd_i32_8[8]  __attribute__((aligned(32))) = { 8, 8, 8, 8, 8, 8, 8, 8 };

static void emit_vex3_ymm(JITBuffer* j, uint8_t mmmmm, uint8_t pp, int reg, int vvvv, int rm, uint8_t opcode) {
    uint8_t b1 = (uint8_t)(mmmmm & 0x1F);
    if ((reg & 8) == 0) b1 |= 0x80;
    b1 |= 0x40;
    if ((rm & 8) == 0) b1 |= 0x20;
    uint8_t b2 = (uint8_t)((((~vvvv) & 0x0F) << 3) | 0x04 | (pp & 0x03));
    emit_u8(j, 0xC4);
    emit_u8(j, b1);
    emit_u8(j, b2);
    emit_u8(j, opcode);
    emit_u8(j, (uint8_t)(0xC0 | ((reg & 7) << 3) | (rm & 7)));
}

static void emit_vmovdqa_ymm_r10(JITBuffer* j, int dst_ymm) {
    uint8_t b1 = (uint8_t)(((dst_ymm < 8) ? 0x80 : 0) | 0x40 | 0x01); /* X=1, B=0 (R10) */
    emit_u8(j, 0xC4); emit_u8(j, b1); emit_u8(j, 0x7D); emit_u8(j, 0x6F);
    emit_u8(j, (uint8_t)(0x00 | ((dst_ymm & 7) << 3) | 2));
}

static void emit_vmovdqu_store_ymm_r11(JITBuffer* j, int src_ymm, int32_t offset) {
    uint8_t b1 = (uint8_t)(((src_ymm < 8) ? 0x80 : 0) | 0x40 | 0x01); /* X=1, B=0 (R11) */
    emit_u8(j, 0xC4); emit_u8(j, b1); emit_u8(j, 0x7E); emit_u8(j, 0x7F);
    if (offset == 0) {
        emit_u8(j, (uint8_t)(0x00 | ((src_ymm & 7) << 3) | 3));
    } else if (offset >= -128 && offset <= 127) {
        emit_u8(j, (uint8_t)(0x40 | ((src_ymm & 7) << 3) | 3));
        emit_u8(j, (uint8_t)(int8_t)offset);
    } else {
        emit_u8(j, (uint8_t)(0x80 | ((src_ymm & 7) << 3) | 3));
        emit_u32(j, (uint32_t)offset);
    }
}

void emit_avx2_i64_array_loop(JITBuffer* j, IRBuffer* ir, IRInstruction* ins) {
    /* Kernel uses ONLY scratch GPRs (RAX, R10, R11) and YMM0..YMM7 (16x i64 = 128 bytes/iter) */
    emit_save_xmm_range(j, 6, 2);
    int r_i = get_vreg_int_reg(ir, ins->i_vreg, PREG_RAX, j);
    if (r_i != PREG_RAX) emit_mov_rr(j, PREG_RAX, r_i);

    int r_arr = get_vreg_int_reg(ir, ins->src1, PREG_R10, j);
    emit_mov_rr(j, PREG_R11, PREG_RAX);
    emit_shl_ri(j, PREG_R11, 3);
    emit_add_rr(j, PREG_R11, r_arr);

    /* Broadcast 64-bit add_c into ymm2 */
    emit_mov_ri64(j, PREG_R10, ins->known_len);
    emit_u8(j, 0x66); emit_u8(j, 0x49); emit_u8(j, 0x0F); emit_u8(j, 0x6E); emit_u8(j, 0xD2); /* movq xmm2, r10 */
    emit_u8(j, 0xC4); emit_u8(j, 0xE2); emit_u8(j, 0x7D); emit_u8(j, 0x59); emit_u8(j, 0xD2); /* vpbroadcastq ymm2, xmm2 */

    /* Zero 4x accumulators: ymm0, ymm1, ymm6, ymm7 */
    emit_u8(j, 0xC5); emit_u8(j, 0xFD); emit_u8(j, 0xEF); emit_u8(j, 0xC0); /* vpxor ymm0, ymm0, ymm0 */
    emit_u8(j, 0xC5); emit_u8(j, 0xF5); emit_u8(j, 0xEF); emit_u8(j, 0xC9); /* vpxor ymm1, ymm1, ymm1 */
    emit_u8(j, 0xC5); emit_u8(j, 0xCD); emit_u8(j, 0xEF); emit_u8(j, 0xF6); /* vpxor ymm6, ymm6, ymm6 */
    emit_u8(j, 0xC5); emit_u8(j, 0xC5); emit_u8(j, 0xEF); emit_u8(j, 0xFF); /* vpxor ymm7, ymm7, ymm7 */

    /* R10 = lim - 15 */
    int r_lim = get_vreg_int_reg(ir, ins->lim_vreg, PREG_R10, j);
    if (r_lim != PREG_R10) emit_mov_rr(j, PREG_R10, r_lim);
    emit_sub_ri(j, PREG_R10, 15);

    size_t simd_loop = j->size;
    emit_u8(j, 0x4C); emit_u8(j, 0x39); emit_u8(j, 0xD0); /* cmp rax, r10 */
    size_t simd_exit = emit_jcc_placeholder(j, 0x8D);       /* jge simd_exit */

    /* Software prefetch 512 bytes ahead */
    emit_u8(j, 0x41); emit_u8(j, 0x0F); emit_u8(j, 0x18); emit_u8(j, 0x8B);
    emit_u32(j, 512); /* prefetcht0 [r11 + 512] */

    /* Chunk 0 (+0): ymm3 -> ymm0 */
    emit_u8(j, 0xC4); emit_u8(j, 0xC1); emit_u8(j, 0x7E); emit_u8(j, 0x6F); emit_u8(j, 0x1B);             /* vmovdqu ymm3, [r11] */
    emit_u8(j, 0xC5); emit_u8(j, 0xD5); emit_u8(j, 0x73); emit_u8(j, 0xF3); emit_u8(j, 0x01);             /* vpsllq ymm5, ymm3, 1 */
    emit_u8(j, 0xC5); emit_u8(j, 0xE5); emit_u8(j, 0xD4); emit_u8(j, 0xDD);                               /* vpaddq ymm3, ymm3, ymm5 */
    emit_u8(j, 0xC5); emit_u8(j, 0xE5); emit_u8(j, 0xD4); emit_u8(j, 0xDA);                               /* vpaddq ymm3, ymm3, ymm2 */
    emit_u8(j, 0xC4); emit_u8(j, 0xC1); emit_u8(j, 0x7E); emit_u8(j, 0x7F); emit_u8(j, 0x1B);             /* vmovdqu [r11], ymm3 */
    emit_u8(j, 0xC5); emit_u8(j, 0xFD); emit_u8(j, 0xD4); emit_u8(j, 0xC3);                               /* vpaddq ymm0, ymm0, ymm3 */

    /* Chunk 1 (+32): ymm4 -> ymm1 */
    emit_u8(j, 0xC4); emit_u8(j, 0xC1); emit_u8(j, 0x7E); emit_u8(j, 0x6F); emit_u8(j, 0x63); emit_u8(j, 0x20); /* vmovdqu ymm4, [r11+32] */
    emit_u8(j, 0xC5); emit_u8(j, 0xD5); emit_u8(j, 0x73); emit_u8(j, 0xF4); emit_u8(j, 0x01);             /* vpsllq ymm5, ymm4, 1 */
    emit_u8(j, 0xC5); emit_u8(j, 0xDD); emit_u8(j, 0xD4); emit_u8(j, 0xE5);                               /* vpaddq ymm4, ymm4, ymm5 */
    emit_u8(j, 0xC5); emit_u8(j, 0xDD); emit_u8(j, 0xD4); emit_u8(j, 0xE2);                               /* vpaddq ymm4, ymm4, ymm2 */
    emit_u8(j, 0xC4); emit_u8(j, 0xC1); emit_u8(j, 0x7E); emit_u8(j, 0x7F); emit_u8(j, 0x63); emit_u8(j, 0x20); /* vmovdqu [r11+32], ymm4 */
    emit_u8(j, 0xC5); emit_u8(j, 0xF5); emit_u8(j, 0xD4); emit_u8(j, 0xCC);                               /* vpaddq ymm1, ymm1, ymm4 */

    /* Chunk 2 (+64): ymm3 -> ymm6 */
    emit_u8(j, 0xC4); emit_u8(j, 0xC1); emit_u8(j, 0x7E); emit_u8(j, 0x6F); emit_u8(j, 0x5B); emit_u8(j, 0x40); /* vmovdqu ymm3, [r11+64] */
    emit_u8(j, 0xC5); emit_u8(j, 0xD5); emit_u8(j, 0x73); emit_u8(j, 0xF3); emit_u8(j, 0x01);             /* vpsllq ymm5, ymm3, 1 */
    emit_u8(j, 0xC5); emit_u8(j, 0xE5); emit_u8(j, 0xD4); emit_u8(j, 0xDD);                               /* vpaddq ymm3, ymm3, ymm5 */
    emit_u8(j, 0xC5); emit_u8(j, 0xE5); emit_u8(j, 0xD4); emit_u8(j, 0xDA);                               /* vpaddq ymm3, ymm3, ymm2 */
    emit_u8(j, 0xC4); emit_u8(j, 0xC1); emit_u8(j, 0x7E); emit_u8(j, 0x7F); emit_u8(j, 0x5B); emit_u8(j, 0x40); /* vmovdqu [r11+64], ymm3 */
    emit_u8(j, 0xC5); emit_u8(j, 0xCD); emit_u8(j, 0xD4); emit_u8(j, 0xF3);                               /* vpaddq ymm6, ymm6, ymm3 */

    /* Chunk 3 (+96): ymm4 -> ymm7 */
    emit_u8(j, 0xC4); emit_u8(j, 0xC1); emit_u8(j, 0x7E); emit_u8(j, 0x6F); emit_u8(j, 0x63); emit_u8(j, 0x60); /* vmovdqu ymm4, [r11+96] */
    emit_u8(j, 0xC5); emit_u8(j, 0xD5); emit_u8(j, 0x73); emit_u8(j, 0xF4); emit_u8(j, 0x01);             /* vpsllq ymm5, ymm4, 1 */
    emit_u8(j, 0xC5); emit_u8(j, 0xDD); emit_u8(j, 0xD4); emit_u8(j, 0xE5);                               /* vpaddq ymm4, ymm4, ymm5 */
    emit_u8(j, 0xC5); emit_u8(j, 0xDD); emit_u8(j, 0xD4); emit_u8(j, 0xE2);                               /* vpaddq ymm4, ymm4, ymm2 */
    emit_u8(j, 0xC4); emit_u8(j, 0xC1); emit_u8(j, 0x7E); emit_u8(j, 0x7F); emit_u8(j, 0x63); emit_u8(j, 0x60); /* vmovdqu [r11+96], ymm4 */
    emit_u8(j, 0xC5); emit_u8(j, 0xC5); emit_u8(j, 0xD4); emit_u8(j, 0xFC);                               /* vpaddq ymm7, ymm7, ymm4 */

    /* r11 += 128; rax += 16 */
    emit_u8(j, 0x49); emit_u8(j, 0x81); emit_u8(j, 0xC3); emit_u32(j, 128);
    emit_u8(j, 0x48); emit_u8(j, 0x83); emit_u8(j, 0xC0); emit_u8(j, 0x10);

    size_t simd_back = emit_jmp_placeholder(j);
    int32_t simd_rel = (int32_t)(simd_loop - (simd_back + 4));
    memcpy(&j->code[simd_back], &simd_rel, 4);

    patch_jump(j, simd_exit);

    /* Horizontal sum of ymm0 + ymm1 + ymm6 + ymm7 -> r10, then add into acc_vreg */
    emit_u8(j, 0xC5); emit_u8(j, 0xFD); emit_u8(j, 0xD4); emit_u8(j, 0xC1);                               /* vpaddq ymm0, ymm0, ymm1 */
    emit_u8(j, 0xC5); emit_u8(j, 0xCD); emit_u8(j, 0xD4); emit_u8(j, 0xF7);                               /* vpaddq ymm6, ymm6, ymm7 */
    emit_u8(j, 0xC5); emit_u8(j, 0xFD); emit_u8(j, 0xD4); emit_u8(j, 0xC6);                               /* vpaddq ymm0, ymm0, ymm6 */
    emit_u8(j, 0xC4); emit_u8(j, 0xE3); emit_u8(j, 0x7D); emit_u8(j, 0x39); emit_u8(j, 0xC1); emit_u8(j, 0x01); /* vextracti128 xmm1, ymm0, 1 */
    emit_u8(j, 0xC5); emit_u8(j, 0xF9); emit_u8(j, 0xD4); emit_u8(j, 0xC1);                               /* vpaddq xmm0, xmm0, xmm1 */
    emit_u8(j, 0xC5); emit_u8(j, 0xF9); emit_u8(j, 0x70); emit_u8(j, 0xC8); emit_u8(j, 0xEE);             /* vpshufd xmm1, xmm0, 0xEE */
    emit_u8(j, 0xC5); emit_u8(j, 0xF9); emit_u8(j, 0xD4); emit_u8(j, 0xC1);                               /* vpaddq xmm0, xmm0, xmm1 */
    emit_vzeroupper(j);
    emit_u8(j, 0x66); emit_u8(j, 0x49); emit_u8(j, 0x0F); emit_u8(j, 0x7E); emit_u8(j, 0xC2);             /* movq r10, xmm0 */

    if (ir->vregs[ins->acc_vreg].is_spilled) {
        emit_add_mem_rbp(j, -ir->vregs[ins->acc_vreg].spill_offset, PREG_R10);
    } else {
        emit_add_rr(j, ir->vregs[ins->acc_vreg].preg, PREG_R10);
    }

    /* Scalar tail for remaining < 16 elements */
    size_t tail_loop = j->size;
    if (ir->vregs[ins->lim_vreg].is_spilled) {
        emit_cmp_reg_rbp(j, PREG_RAX, -ir->vregs[ins->lim_vreg].spill_offset);
    } else {
        emit_cmp_rr(j, PREG_RAX, ir->vregs[ins->lim_vreg].preg);
    }
    size_t tail_exit = emit_jcc_placeholder(j, 0x8D); /* jge tail_exit */

    emit_load_mem_offset(j, PREG_R10, PREG_R11, 0);
    emit_lea_sib(j, PREG_R10, PREG_R10, PREG_R10, 2);
    if (ins->known_len != 0) emit_add_ri(j, PREG_R10, (int32_t)ins->known_len);
    emit_store_mem_offset(j, PREG_R11, 0, PREG_R10);
    if (ir->vregs[ins->acc_vreg].is_spilled) {
        emit_add_mem_rbp(j, -ir->vregs[ins->acc_vreg].spill_offset, PREG_R10);
    } else {
        emit_add_rr(j, ir->vregs[ins->acc_vreg].preg, PREG_R10);
    }
    emit_add_ri(j, PREG_RAX, 1);
    emit_add_ri(j, PREG_R11, 8);

    size_t tail_back = emit_jmp_placeholder(j);
    int32_t tail_rel = (int32_t)(tail_loop - (tail_back + 4));
    memcpy(&j->code[tail_back], &tail_rel, 4);

    patch_jump(j, tail_exit);
    emit_restore_xmm_range(j, 6, 2);
    commit_vreg_int_reg(ir, ins->i_vreg, PREG_RAX, j);
}

void emit_avx2_grid_mod_loop(JITBuffer* j, IRBuffer* ir, IRInstruction* ins) {
    /* Vectorizes 32 rows/iter in 32-bit AVX2 (when bounded) + 8-row cleanup + 64-bit fallback */
    emit_save_xmm_range(j, 6, 7);
    int r_i = get_vreg_int_reg(ir, ins->i_vreg, PREG_RAX, j);
    if (r_i != PREG_RAX) emit_mov_rr(j, PREG_RAX, r_i);

    int r_arr = get_vreg_int_reg(ir, ins->src1, PREG_R10, j);
    emit_mov_rr(j, PREG_R11, PREG_RAX);
    emit_shl_ri(j, PREG_R11, 3);
    emit_add_rr(j, PREG_R11, r_arr);

    int64_t step_c = ins->imm;
    int64_t mod_m = ins->known_len;

    if (step_c > 0 && mod_m > 0 && (mod_m + step_c) <= 0x7FFFFFFFLL) {
        int r_lim_chk = get_vreg_int_reg(ir, ins->lim_vreg, PREG_R10, j);
        if (r_lim_chk != PREG_R10) emit_mov_rr(j, PREG_R10, r_lim_chk);
        emit_u8(j, 0x49); emit_u8(j, 0x81); emit_u8(j, 0xFA); emit_u32(j, (uint32_t)mod_m); /* cmp r10, mod_m */
        size_t fb_hi = emit_jcc_placeholder(j, 0x8F); /* jg i64_fallback */
        emit_test_rr(j, PREG_RAX, PREG_RAX);
        size_t fb_neg = emit_jcc_placeholder(j, 0x8C); /* jl i64_fallback */

        /* 32-bit constants: ymm4=step_c, ymm5=mod_m, ymm6=mod_m-1, ymm11=[0..7], ymm12=[8..8] */
        emit_mov_ri64(j, PREG_R10, step_c);
        emit_u8(j, 0x66); emit_u8(j, 0x49); emit_u8(j, 0x0F); emit_u8(j, 0x6E); emit_u8(j, 0xE2); /* movq xmm4, r10 */
        emit_vex3_ymm(j, 2, 1, 4, 0, 4, 0x58); /* vpbroadcastd ymm4, xmm4 */

        emit_mov_ri64(j, PREG_R10, mod_m);
        emit_u8(j, 0x66); emit_u8(j, 0x49); emit_u8(j, 0x0F); emit_u8(j, 0x6E); emit_u8(j, 0xEA); /* movq xmm5, r10 */
        emit_vex3_ymm(j, 2, 1, 5, 0, 5, 0x58); /* vpbroadcastd ymm5, xmm5 */

        emit_mov_ri64(j, PREG_R10, mod_m - 1);
        emit_u8(j, 0x66); emit_u8(j, 0x49); emit_u8(j, 0x0F); emit_u8(j, 0x6E); emit_u8(j, 0xF2); /* movq xmm6, r10 */
        emit_vex3_ymm(j, 2, 1, 6, 0, 6, 0x58); /* vpbroadcastd ymm6, xmm6 */

        emit_mov_ri64(j, PREG_R10, (int64_t)(uintptr_t)g_simd_i32_07);
        emit_vmovdqa_ymm_r10(j, 11);
        emit_mov_ri64(j, PREG_R10, (int64_t)(uintptr_t)g_simd_i32_8);
        emit_vmovdqa_ymm_r10(j, 12);

        /* Phase 1: 32 rows per iteration */
        size_t p1_loop = j->size;
        int r_lim32 = get_vreg_int_reg(ir, ins->lim_vreg, PREG_R10, j);
        if (r_lim32 != PREG_R10) emit_mov_rr(j, PREG_R10, r_lim32);
        emit_sub_ri(j, PREG_R10, 31);
        emit_u8(j, 0x4C); emit_u8(j, 0x39); emit_u8(j, 0xD0); /* cmp rax, r10 */
        size_t p1_exit = emit_jcc_placeholder(j, 0x8D);       /* jge p2_loop */

        emit_u8(j, 0x66); emit_u8(j, 0x0F); emit_u8(j, 0x6E); emit_u8(j, 0xC0); /* movd xmm0, eax */
        emit_vex3_ymm(j, 2, 1, 0, 0, 0, 0x58); /* vpbroadcastd ymm0, xmm0 */
        emit_vex3_ymm(j, 1, 1, 0, 0, 11, 0xFE); /* vpaddd ymm0, ymm0, ymm11 */
        emit_vex3_ymm(j, 1, 1, 1, 0, 12, 0xFE); /* vpaddd ymm1, ymm0, ymm12 */
        emit_vex3_ymm(j, 1, 1, 2, 1, 12, 0xFE); /* vpaddd ymm2, ymm1, ymm12 */
        emit_vex3_ymm(j, 1, 1, 3, 2, 12, 0xFE); /* vpaddd ymm3, ymm2, ymm12 */

        int r_c32 = get_vreg_int_reg(ir, ins->src2, PREG_R10, j);
        if (r_c32 != PREG_R10) emit_mov_rr(j, PREG_R10, r_c32);
        emit_test_rr(j, PREG_R10, PREG_R10);
        size_t p1_in_done = emit_jcc_placeholder(j, 0x8E); /* jle p1_in_done */

        size_t p1_in = j->size;
        emit_vex3_ymm(j, 1, 1, 0, 0, 4, 0xFE); /* vpaddd ymm0..3, ymm4 */
        emit_vex3_ymm(j, 1, 1, 1, 1, 4, 0xFE);
        emit_vex3_ymm(j, 1, 1, 2, 2, 4, 0xFE);
        emit_vex3_ymm(j, 1, 1, 3, 3, 4, 0xFE);
        emit_vex3_ymm(j, 1, 1, 7,  0, 6, 0x66); /* vpcmpgtd ymm7..10, ymm0..3, ymm6 */
        emit_vex3_ymm(j, 1, 1, 8,  1, 6, 0x66);
        emit_vex3_ymm(j, 1, 1, 9,  2, 6, 0x66);
        emit_vex3_ymm(j, 1, 1, 10, 3, 6, 0x66);
        emit_vex3_ymm(j, 1, 1, 7,  7,  5, 0xDB); /* vpand ymm7..10, ymm5 */
        emit_vex3_ymm(j, 1, 1, 8,  8,  5, 0xDB);
        emit_vex3_ymm(j, 1, 1, 9,  9,  5, 0xDB);
        emit_vex3_ymm(j, 1, 1, 10, 10, 5, 0xDB);
        emit_vex3_ymm(j, 1, 1, 0, 0, 7,  0xFA); /* vpsubd ymm0..3, ymm7..10 */
        emit_vex3_ymm(j, 1, 1, 1, 1, 8,  0xFA);
        emit_vex3_ymm(j, 1, 1, 2, 2, 9,  0xFA);
        emit_vex3_ymm(j, 1, 1, 3, 3, 10, 0xFA);
        emit_u8(j, 0x49); emit_u8(j, 0xFF); emit_u8(j, 0xCA); /* dec r10 */
        size_t p1_in_back = emit_jcc_placeholder(j, 0x85);
        int32_t p1_in_rel = (int32_t)(p1_in - (p1_in_back + 4));
        memcpy(&j->code[p1_in_back], &p1_in_rel, 4);

        patch_jump(j, p1_in_done);
        for (int k = 0; k < 4; k++) {
            emit_vex3_ymm(j, 2, 1, 7, 0, k, 0x25); /* vpmovsxdq ymm7, xmm{k} */
            emit_vmovdqu_store_ymm_r11(j, 7, k * 64);
            emit_u8(j, 0xC4); emit_u8(j, 0xE3); emit_u8(j, 0x7D); emit_u8(j, 0x39);
            emit_u8(j, (uint8_t)(0xC0 | ((k & 7) << 3) | 7)); emit_u8(j, 0x01); /* vextracti128 xmm7, ymm{k}, 1 */
            emit_vex3_ymm(j, 2, 1, 7, 0, 7, 0x25); /* vpmovsxdq ymm7, xmm7 */
            emit_vmovdqu_store_ymm_r11(j, 7, k * 64 + 32);
        }
        emit_u8(j, 0x49); emit_u8(j, 0x81); emit_u8(j, 0xC3); emit_u32(j, 256); /* add r11, 256 */
        emit_u8(j, 0x48); emit_u8(j, 0x83); emit_u8(j, 0xC0); emit_u8(j, 32);  /* add rax, 32 */
        size_t p1_back = emit_jmp_placeholder(j);
        int32_t p1_rel = (int32_t)(p1_loop - (p1_back + 4));
        memcpy(&j->code[p1_back], &p1_rel, 4);

        patch_jump(j, p1_exit);

        /* Phase 2: 8 rows per iteration in 32-bit AVX2 */
        size_t p2_loop = j->size;
        int r_lim8 = get_vreg_int_reg(ir, ins->lim_vreg, PREG_R10, j);
        if (r_lim8 != PREG_R10) emit_mov_rr(j, PREG_R10, r_lim8);
        emit_sub_ri(j, PREG_R10, 7);
        emit_u8(j, 0x4C); emit_u8(j, 0x39); emit_u8(j, 0xD0); /* cmp rax, r10 */
        size_t p2_exit = emit_jcc_placeholder(j, 0x8D);       /* jge fast32_done */

        emit_u8(j, 0x66); emit_u8(j, 0x0F); emit_u8(j, 0x6E); emit_u8(j, 0xC0);
        emit_vex3_ymm(j, 2, 1, 0, 0, 0, 0x58);
        emit_vex3_ymm(j, 1, 1, 0, 0, 11, 0xFE); /* ymm0 = rax + [0..7] */

        int r_c8 = get_vreg_int_reg(ir, ins->src2, PREG_R10, j);
        if (r_c8 != PREG_R10) emit_mov_rr(j, PREG_R10, r_c8);
        emit_test_rr(j, PREG_R10, PREG_R10);
        size_t p2_in_done = emit_jcc_placeholder(j, 0x8E);

        size_t p2_in = j->size;
        emit_vex3_ymm(j, 1, 1, 0, 0, 4, 0xFE);
        emit_vex3_ymm(j, 1, 1, 7, 0, 6, 0x66);
        emit_vex3_ymm(j, 1, 1, 7, 7, 5, 0xDB);
        emit_vex3_ymm(j, 1, 1, 0, 0, 7, 0xFA);
        emit_u8(j, 0x49); emit_u8(j, 0xFF); emit_u8(j, 0xCA);
        size_t p2_in_back = emit_jcc_placeholder(j, 0x85);
        int32_t p2_in_rel = (int32_t)(p2_in - (p2_in_back + 4));
        memcpy(&j->code[p2_in_back], &p2_in_rel, 4);

        patch_jump(j, p2_in_done);
        emit_vex3_ymm(j, 2, 1, 7, 0, 0, 0x25);
        emit_vmovdqu_store_ymm_r11(j, 7, 0);
        emit_u8(j, 0xC4); emit_u8(j, 0xE3); emit_u8(j, 0x7D); emit_u8(j, 0x39);
        emit_u8(j, 0xC7); emit_u8(j, 0x01); /* vextracti128 xmm7, ymm0, 1 */
        emit_vex3_ymm(j, 2, 1, 7, 0, 7, 0x25);
        emit_vmovdqu_store_ymm_r11(j, 7, 32);

        emit_u8(j, 0x49); emit_u8(j, 0x83); emit_u8(j, 0xC3); emit_u8(j, 64); /* add r11, 64 */
        emit_u8(j, 0x48); emit_u8(j, 0x83); emit_u8(j, 0xC0); emit_u8(j, 8);  /* add rax, 8 */
        size_t p2_back = emit_jmp_placeholder(j);
        int32_t p2_rel = (int32_t)(p2_loop - (p2_back + 4));
        memcpy(&j->code[p2_back], &p2_rel, 4);

        patch_jump(j, p2_exit);
        size_t fast32_to_exit = emit_jmp_placeholder(j);

        patch_jump(j, fb_hi);
        patch_jump(j, fb_neg);
        /* Fall through to 64-bit AVX2 loop */
        emit_mov_ri64(j, PREG_R10, step_c);
        emit_u8(j, 0x66); emit_u8(j, 0x49); emit_u8(j, 0x0F); emit_u8(j, 0x6E); emit_u8(j, 0xD2);
        emit_vex3_ymm(j, 2, 1, 2, 0, 2, 0x59);
        emit_mov_ri64(j, PREG_R10, mod_m);
        emit_u8(j, 0x66); emit_u8(j, 0x49); emit_u8(j, 0x0F); emit_u8(j, 0x6E); emit_u8(j, 0xDA);
        emit_vex3_ymm(j, 2, 1, 3, 0, 3, 0x59);
        emit_mov_ri64(j, PREG_R10, mod_m - 1);
        emit_u8(j, 0x66); emit_u8(j, 0x49); emit_u8(j, 0x0F); emit_u8(j, 0x6E); emit_u8(j, 0xE2);
        emit_vex3_ymm(j, 2, 1, 4, 0, 4, 0x59);
        emit_mov_ri64(j, PREG_R10, (int64_t)(uintptr_t)g_simd_i64_03);
        emit_u8(j, 0xC4); emit_u8(j, 0x41); emit_u8(j, 0x7D); emit_u8(j, 0x6F); emit_u8(j, 0x02);
        emit_mov_ri64(j, PREG_R10, (int64_t)(uintptr_t)g_simd_i64_47);
        emit_u8(j, 0xC4); emit_u8(j, 0x41); emit_u8(j, 0x7D); emit_u8(j, 0x6F); emit_u8(j, 0x0A);

        size_t simd_loop = j->size;
        int r_lim = get_vreg_int_reg(ir, ins->lim_vreg, PREG_R10, j);
        if (r_lim != PREG_R10) emit_mov_rr(j, PREG_R10, r_lim);
        emit_sub_ri(j, PREG_R10, 7);
        emit_u8(j, 0x4C); emit_u8(j, 0x39); emit_u8(j, 0xD0);
        size_t simd_exit = emit_jcc_placeholder(j, 0x8D);
        emit_u8(j, 0x66); emit_u8(j, 0x48); emit_u8(j, 0x0F); emit_u8(j, 0x6E); emit_u8(j, 0xC0);
        emit_vex3_ymm(j, 2, 1, 0, 0, 0, 0x59);
        emit_vex3_ymm(j, 1, 1, 1, 0, 9, 0xD4);
        emit_vex3_ymm(j, 1, 1, 0, 0, 8, 0xD4);
        int r_cols = get_vreg_int_reg(ir, ins->src2, PREG_R10, j);
        if (r_cols != PREG_R10) emit_mov_rr(j, PREG_R10, r_cols);
        emit_test_rr(j, PREG_R10, PREG_R10);
        size_t inner_done = emit_jcc_placeholder(j, 0x8E);
        size_t inner_loop = j->size;
        emit_vex3_ymm(j, 1, 1, 0, 0, 2, 0xD4);
        emit_vex3_ymm(j, 1, 1, 1, 1, 2, 0xD4);
        emit_vex3_ymm(j, 2, 1, 5, 0, 4, 0x37);
        emit_vex3_ymm(j, 2, 1, 6, 1, 4, 0x37);
        emit_vex3_ymm(j, 1, 1, 5, 5, 3, 0xDB);
        emit_vex3_ymm(j, 1, 1, 6, 6, 3, 0xDB);
        emit_vex3_ymm(j, 1, 1, 0, 0, 5, 0xFB);
        emit_vex3_ymm(j, 1, 1, 1, 1, 6, 0xFB);
        emit_u8(j, 0x49); emit_u8(j, 0xFF); emit_u8(j, 0xCA);
        size_t inner_back = emit_jcc_placeholder(j, 0x85);
        int32_t inner_rel = (int32_t)(inner_loop - (inner_back + 4));
        memcpy(&j->code[inner_back], &inner_rel, 4);
        patch_jump(j, inner_done);
        emit_u8(j, 0xC4); emit_u8(j, 0xC1); emit_u8(j, 0x7E); emit_u8(j, 0x7F); emit_u8(j, 0x03);
        emit_u8(j, 0xC4); emit_u8(j, 0xC1); emit_u8(j, 0x7E); emit_u8(j, 0x7F); emit_u8(j, 0x4B); emit_u8(j, 0x20);
        emit_u8(j, 0x49); emit_u8(j, 0x83); emit_u8(j, 0xC3); emit_u8(j, 0x40);
        emit_u8(j, 0x48); emit_u8(j, 0x83); emit_u8(j, 0xC0); emit_u8(j, 0x08);
        size_t simd_back = emit_jmp_placeholder(j);
        int32_t simd_rel = (int32_t)(simd_loop - (simd_back + 4));
        memcpy(&j->code[simd_back], &simd_rel, 4);

        patch_jump(j, simd_exit);
        patch_jump(j, fast32_to_exit);
        goto tail_section;
    }

    /* 64-bit AVX2 loop when bounds exceed 32-bit range */
    emit_mov_ri64(j, PREG_R10, step_c);
    emit_u8(j, 0x66); emit_u8(j, 0x49); emit_u8(j, 0x0F); emit_u8(j, 0x6E); emit_u8(j, 0xD2);
    emit_vex3_ymm(j, 2, 1, 2, 0, 2, 0x59);

    emit_mov_ri64(j, PREG_R10, mod_m);
    emit_u8(j, 0x66); emit_u8(j, 0x49); emit_u8(j, 0x0F); emit_u8(j, 0x6E); emit_u8(j, 0xDA);
    emit_vex3_ymm(j, 2, 1, 3, 0, 3, 0x59);

    emit_mov_ri64(j, PREG_R10, mod_m - 1);
    emit_u8(j, 0x66); emit_u8(j, 0x49); emit_u8(j, 0x0F); emit_u8(j, 0x6E); emit_u8(j, 0xE2);
    emit_vex3_ymm(j, 2, 1, 4, 0, 4, 0x59);

    emit_mov_ri64(j, PREG_R10, (int64_t)(uintptr_t)g_simd_i64_03);
    emit_u8(j, 0xC4); emit_u8(j, 0x41); emit_u8(j, 0x7D); emit_u8(j, 0x6F); emit_u8(j, 0x02);
    emit_mov_ri64(j, PREG_R10, (int64_t)(uintptr_t)g_simd_i64_47);
    emit_u8(j, 0xC4); emit_u8(j, 0x41); emit_u8(j, 0x7D); emit_u8(j, 0x6F); emit_u8(j, 0x0A);

    size_t simd_loop2 = j->size;
    int r_lim2 = get_vreg_int_reg(ir, ins->lim_vreg, PREG_R10, j);
    if (r_lim2 != PREG_R10) emit_mov_rr(j, PREG_R10, r_lim2);
    emit_sub_ri(j, PREG_R10, 7);
    emit_u8(j, 0x4C); emit_u8(j, 0x39); emit_u8(j, 0xD0);
    size_t simd_exit2 = emit_jcc_placeholder(j, 0x8D);

    emit_u8(j, 0x66); emit_u8(j, 0x48); emit_u8(j, 0x0F); emit_u8(j, 0x6E); emit_u8(j, 0xC0);
    emit_vex3_ymm(j, 2, 1, 0, 0, 0, 0x59);
    emit_vex3_ymm(j, 1, 1, 1, 0, 9, 0xD4);
    emit_vex3_ymm(j, 1, 1, 0, 0, 8, 0xD4);

    int r_cols2 = get_vreg_int_reg(ir, ins->src2, PREG_R10, j);
    if (r_cols2 != PREG_R10) emit_mov_rr(j, PREG_R10, r_cols2);
    emit_test_rr(j, PREG_R10, PREG_R10);
    size_t inner_done2 = emit_jcc_placeholder(j, 0x8E);

    size_t inner_loop2 = j->size;
    emit_vex3_ymm(j, 1, 1, 0, 0, 2, 0xD4);
    emit_vex3_ymm(j, 1, 1, 1, 1, 2, 0xD4);
    emit_vex3_ymm(j, 2, 1, 5, 0, 4, 0x37);
    emit_vex3_ymm(j, 2, 1, 6, 1, 4, 0x37);
    emit_vex3_ymm(j, 1, 1, 5, 5, 3, 0xDB);
    emit_vex3_ymm(j, 1, 1, 6, 6, 3, 0xDB);
    emit_vex3_ymm(j, 1, 1, 0, 0, 5, 0xFB);
    emit_vex3_ymm(j, 1, 1, 1, 1, 6, 0xFB);

    emit_u8(j, 0x49); emit_u8(j, 0xFF); emit_u8(j, 0xCA);
    size_t inner_back2 = emit_jcc_placeholder(j, 0x85);
    int32_t inner_rel2 = (int32_t)(inner_loop2 - (inner_back2 + 4));
    memcpy(&j->code[inner_back2], &inner_rel2, 4);

    patch_jump(j, inner_done2);
    emit_u8(j, 0xC4); emit_u8(j, 0xC1); emit_u8(j, 0x7E); emit_u8(j, 0x7F); emit_u8(j, 0x03);
    emit_u8(j, 0xC4); emit_u8(j, 0xC1); emit_u8(j, 0x7E); emit_u8(j, 0x7F); emit_u8(j, 0x4B); emit_u8(j, 0x20);
    emit_u8(j, 0x49); emit_u8(j, 0x83); emit_u8(j, 0xC3); emit_u8(j, 0x40);
    emit_u8(j, 0x48); emit_u8(j, 0x83); emit_u8(j, 0xC0); emit_u8(j, 0x08);
    size_t simd_back2 = emit_jmp_placeholder(j);
    int32_t simd_rel2 = (int32_t)(simd_loop2 - (simd_back2 + 4));
    memcpy(&j->code[simd_back2], &simd_rel2, 4);
    patch_jump(j, simd_exit2);

tail_section:
    emit_vzeroupper(j);
    emit_restore_xmm_range(j, 6, 7);

    /* Scalar tail for remaining < 8 rows (save cols, lim, rcx, rdx on stack) */
    int r_cols_t = get_vreg_int_reg(ir, ins->src2, PREG_R10, j);
    emit_push_r(j, r_cols_t); /* [rsp+24] */
    int r_lim_t = get_vreg_int_reg(ir, ins->lim_vreg, PREG_R10, j);
    emit_push_r(j, r_lim_t);  /* [rsp+16] */
    emit_push_r(j, PREG_RCX); /* [rsp+8] */
    emit_push_r(j, PREG_RDX); /* [rsp] */

    size_t tail_loop = j->size;
    emit_u8(j, 0x48); emit_u8(j, 0x3B); emit_u8(j, 0x44); emit_u8(j, 0x24); emit_u8(j, 0x10); /* cmp rax, [rsp+16] */
    size_t tail_exit = emit_jcc_placeholder(j, 0x8D); /* jge tail_exit */

    emit_mov_rr(j, PREG_R10, PREG_RAX);
    emit_u8(j, 0x48); emit_u8(j, 0x8B); emit_u8(j, 0x4C); emit_u8(j, 0x24); emit_u8(j, 0x18); /* mov rcx, [rsp+24] */
    emit_test_rr(j, PREG_RCX, PREG_RCX);
    size_t tail_store = emit_jcc_placeholder(j, 0x8E); /* jle tail_store */

    size_t tail_inner = j->size;
    emit_add_ri(j, PREG_R10, (int32_t)step_c);
    emit_u8(j, 0x49); emit_u8(j, 0x8D); emit_u8(j, 0x92); emit_u32(j, (uint32_t)(-(int32_t)mod_m)); /* lea rdx, [r10 - mod_m] */
    emit_u8(j, 0x49); emit_u8(j, 0x81); emit_u8(j, 0xFA); emit_u32(j, (uint32_t)mod_m);              /* cmp r10, mod_m */
    emit_u8(j, 0x49); emit_u8(j, 0x0F); emit_u8(j, 0x4D); emit_u8(j, 0xD2);                          /* cmovge r10, rdx */
    emit_u8(j, 0x48); emit_u8(j, 0xFF); emit_u8(j, 0xC9);                                            /* dec rcx */
    size_t tail_inner_back = emit_jcc_placeholder(j, 0x85); /* jnz tail_inner */
    int32_t ti_rel = (int32_t)(tail_inner - (tail_inner_back + 4));
    memcpy(&j->code[tail_inner_back], &ti_rel, 4);

    patch_jump(j, tail_store);
    emit_store_mem_offset(j, PREG_R11, 0, PREG_R10);
    emit_add_ri(j, PREG_RAX, 1);
    emit_add_ri(j, PREG_R11, 8);

    size_t tail_back = emit_jmp_placeholder(j);
    int32_t tail_rel = (int32_t)(tail_loop - (tail_back + 4));
    memcpy(&j->code[tail_back], &tail_rel, 4);

    patch_jump(j, tail_exit);
    emit_u8(j, 0x5A); /* pop rdx */
    emit_u8(j, 0x59); /* pop rcx */
    emit_u8(j, 0x48); emit_u8(j, 0x83); emit_u8(j, 0xC4); emit_u8(j, 0x10); /* add rsp, 16 */
    commit_vreg_int_reg(ir, ins->i_vreg, PREG_RAX, j);
}

void emit_avx2_i64_mod_fill_loop(JITBuffer* j, IRBuffer* ir, IRInstruction* ins) {
    int64_t step_c = ins->imm;
    int64_t mod_m = ins->known_len;

    emit_push_r(j, PREG_RCX);                                           /* [rsp+24] */
    emit_push_r(j, get_vreg_int_reg(ir, ins->lim_vreg, PREG_R10, j));   /* [rsp+16] = lim */
    emit_push_r(j, get_vreg_int_reg(ir, ins->acc_vreg, PREG_R10, j));   /* [rsp+8]  = cur */
    emit_push_r(j, get_vreg_int_reg(ir, ins->src1, PREG_R10, j));       /* [rsp+0]  = arr */

    int r_i = get_vreg_int_reg(ir, ins->i_vreg, PREG_RAX, j);
    if (r_i != PREG_RAX) emit_mov_rr(j, PREG_RAX, r_i);

    emit_mov_rr(j, PREG_R11, PREG_RAX);
    emit_shl_ri(j, PREG_R11, 3);
    emit_u8(j, 0x4C); emit_u8(j, 0x03); emit_u8(j, 0x1C); emit_u8(j, 0x24); /* add r11, [rsp] */

    emit_u8(j, 0x4C); emit_u8(j, 0x8B); emit_u8(j, 0x54); emit_u8(j, 0x24); emit_u8(j, 0x08); /* mov r10, [rsp+8] */

    emit_test_rr(j, PREG_R10, PREG_R10);
    size_t skip_neg = emit_jcc_placeholder(j, 0x8C);
    emit_u8(j, 0x49); emit_u8(j, 0x81); emit_u8(j, 0xFA); emit_u32(j, (uint32_t)mod_m);
    size_t skip_hi = emit_jcc_placeholder(j, 0x8D);
    emit_u8(j, 0x48); emit_u8(j, 0x8B); emit_u8(j, 0x4C); emit_u8(j, 0x24); emit_u8(j, 0x10); /* mov rcx, [rsp+16] */
    emit_sub_ri(j, PREG_RCX, 7);
    emit_cmp_rr(j, PREG_RAX, PREG_RCX);
    size_t skip_len = emit_jcc_placeholder(j, 0x8D);

    emit_save_xmm_range(j, 6, 1);

    /* ymm0 = broadcast(cur) */
    emit_u8(j, 0x66); emit_u8(j, 0x49); emit_u8(j, 0x0F); emit_u8(j, 0x6E); emit_u8(j, 0xC2); /* movq xmm0, r10 */
    emit_vex3_ymm(j, 2, 1, 0, 0, 0, 0x59); /* vpbroadcastq ymm0, xmm0 */

    emit_mov_ri64(j, PREG_R10, mod_m - 1);
    emit_u8(j, 0x66); emit_u8(j, 0x49); emit_u8(j, 0x0F); emit_u8(j, 0x6E); emit_u8(j, 0xE2); /* movq xmm4, r10 */
    emit_vex3_ymm(j, 2, 1, 4, 0, 4, 0x59);

    emit_mov_ri64(j, PREG_R10, mod_m);
    emit_u8(j, 0x66); emit_u8(j, 0x49); emit_u8(j, 0x0F); emit_u8(j, 0x6E); emit_u8(j, 0xEA); /* movq xmm5, r10 */
    emit_vex3_ymm(j, 2, 1, 5, 0, 5, 0x59);

    emit_mov_ri64(j, PREG_R10, step_c * 8);
    emit_u8(j, 0x66); emit_u8(j, 0x49); emit_u8(j, 0x0F); emit_u8(j, 0x6E); emit_u8(j, 0xF2); /* movq xmm6, r10 */
    emit_vex3_ymm(j, 2, 1, 6, 0, 6, 0x59);

    emit_mov_ri64(j, PREG_R10, step_c);
    emit_u8(j, 0x66); emit_u8(j, 0x49); emit_u8(j, 0x0F); emit_u8(j, 0x6E); emit_u8(j, 0xD2); /* movq xmm2, r10 */
    emit_vex3_ymm(j, 2, 1, 2, 0, 2, 0x59);

    emit_mov_ri64(j, PREG_R10, (int64_t)(uintptr_t)g_simd_i64_14);
    emit_vmovdqa_ymm_r10(j, 1);
    emit_vex3_ymm(j, 1, 1, 3, 2, 1, 0xF4); /* vpmuludq ymm3, ymm2, [1..4] */

    emit_mov_ri64(j, PREG_R10, (int64_t)(uintptr_t)g_simd_i64_58);
    emit_vmovdqa_ymm_r10(j, 1);
    emit_vex3_ymm(j, 1, 1, 1, 2, 1, 0xF4); /* vpmuludq ymm1, ymm2, [5..8] */

    emit_vex3_ymm(j, 1, 1, 1, 0, 1, 0xD4); /* ymm1 += cur */
    emit_vex3_ymm(j, 1, 1, 0, 0, 3, 0xD4); /* ymm0 += [1..4]*step */

    /* Initial single wrap reduction */
    emit_vex3_ymm(j, 2, 1, 2, 0, 4, 0x37);
    emit_vex3_ymm(j, 2, 1, 3, 1, 4, 0x37);
    emit_vex3_ymm(j, 1, 1, 2, 2, 5, 0xDB);
    emit_vex3_ymm(j, 1, 1, 3, 3, 5, 0xDB);
    emit_vex3_ymm(j, 1, 1, 0, 0, 2, 0xFB);
    emit_vex3_ymm(j, 1, 1, 1, 1, 3, 0xFB);

    size_t simd_loop = j->size;
    emit_vmovdqu_store_ymm_r11(j, 0, 0);
    emit_vmovdqu_store_ymm_r11(j, 1, 32);

    emit_vex3_ymm(j, 1, 1, 0, 0, 6, 0xD4); /* vpaddq ymm0, ymm0, ymm6 */
    emit_vex3_ymm(j, 1, 1, 1, 1, 6, 0xD4); /* vpaddq ymm1, ymm1, ymm6 */
    emit_vex3_ymm(j, 2, 1, 2, 0, 4, 0x37); /* vpcmpgtq ymm2, ymm0, ymm4 */
    emit_vex3_ymm(j, 2, 1, 3, 1, 4, 0x37); /* vpcmpgtq ymm3, ymm1, ymm4 */
    emit_vex3_ymm(j, 1, 1, 2, 2, 5, 0xDB); /* vpand ymm2, ymm2, ymm5 */
    emit_vex3_ymm(j, 1, 1, 3, 3, 5, 0xDB); /* vpand ymm3, ymm3, ymm5 */
    emit_vex3_ymm(j, 1, 1, 0, 0, 2, 0xFB); /* vpsubq ymm0, ymm0, ymm2 */
    emit_vex3_ymm(j, 1, 1, 1, 1, 3, 0xFB); /* vpsubq ymm1, ymm1, ymm3 */

    emit_u8(j, 0x49); emit_u8(j, 0x83); emit_u8(j, 0xC3); emit_u8(j, 64); /* add r11, 64 */
    emit_u8(j, 0x48); emit_u8(j, 0x83); emit_u8(j, 0xC0); emit_u8(j, 8);  /* add rax, 8 */
    emit_cmp_rr(j, PREG_RAX, PREG_RCX);
    size_t simd_back = emit_jcc_placeholder(j, 0x8C); /* jl simd_loop */
    int32_t simd_rel = (int32_t)(simd_loop - (simd_back + 4));
    memcpy(&j->code[simd_back], &simd_rel, 4);

    emit_vzeroupper(j);
    emit_restore_xmm_range(j, 6, 1);
    /* Recover cur = *(r11 - 8) for scalar tail */
    emit_u8(j, 0x4D); emit_u8(j, 0x8B); emit_u8(j, 0x53); emit_u8(j, 0xF8); /* mov r10, [r11 - 8] */

    patch_jump(j, skip_neg);
    patch_jump(j, skip_hi);
    patch_jump(j, skip_len);

    /* Scalar tail */
    emit_u8(j, 0x48); emit_u8(j, 0x8B); emit_u8(j, 0x4C); emit_u8(j, 0x24); emit_u8(j, 0x10); /* mov rcx, [rsp+16] */
    size_t tail_loop = j->size;
    emit_cmp_rr(j, PREG_RAX, PREG_RCX);
    size_t tail_exit = emit_jcc_placeholder(j, 0x8D); /* jge tail_exit */

    emit_add_ri(j, PREG_R10, (int32_t)step_c);
    emit_u8(j, 0x49); emit_u8(j, 0x81); emit_u8(j, 0xFA); emit_u32(j, (uint32_t)mod_m); /* cmp r10, mod_m */
    size_t no_sub = emit_jcc_placeholder(j, 0x8C); /* jl no_sub */
    emit_sub_ri(j, PREG_R10, (int32_t)mod_m);
    patch_jump(j, no_sub);

    emit_store_mem_offset(j, PREG_R11, 0, PREG_R10);
    emit_add_ri(j, PREG_RAX, 1);
    emit_add_ri(j, PREG_R11, 8);
    size_t tail_back = emit_jmp_placeholder(j);
    int32_t tail_rel = (int32_t)(tail_loop - (tail_back + 4));
    memcpy(&j->code[tail_back], &tail_rel, 4);

    patch_jump(j, tail_exit);
    emit_u8(j, 0x48); emit_u8(j, 0x83); emit_u8(j, 0xC4); emit_u8(j, 24); /* add rsp, 24 */
    emit_u8(j, 0x59); /* pop rcx */
    commit_vreg_int_reg(ir, ins->i_vreg, PREG_RAX, j);
    commit_vreg_int_reg(ir, ins->acc_vreg, PREG_R10, j);
}
