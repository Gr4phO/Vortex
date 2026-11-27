/*
 * VORTEX AARCH64 (ARM64) DUAL-ABI JIT CODE GENERATOR & NEON SIMD BACKEND
 * - Fixed 32-bit A64 machine code emitter in pure C99 (x0..x28, v0..v31)
 * - AAPCS64 (Linux ARM64 / Windows ARM64) & Darwin64 (Apple Silicon) ABIs
 * - Including >8 register stack arguments & Darwin variadic stack rules
 * - 128-bit ASIMD / NEON 8-Way Unrolled Float Vectorizer for IR_VEC_LOOP
 */

#include "vortex.h"

/* ========================================================================= */
/* LOW-LEVEL AARCH64 (A64) 32-BIT FIXED INSTRUCTION EMITTERS                 */
/* ========================================================================= */

static const float g_neon_lane_offsets_A[4] = { 0.0f, 1.0f, 2.0f, 3.0f };
static const float g_neon_lane_offsets_B[4] = { 4.0f, 5.0f, 6.0f, 7.0f };
static const float g_neon_eight[4]          = { 8.0f, 8.0f, 8.0f, 8.0f };

static void a64_emit32(JITBuffer* j, uint32_t insn) {
    if (j->size + 4 > j->capacity) {
        fprintf(stderr, "[VORTEX ERROR] ARM64 JIT Buffer overflow\n");
        vortex_error("");
    }
    memcpy(&j->code[j->size], &insn, 4);
    j->size += 4;
}

static uint32_t a64_read32(const JITBuffer* j, size_t offset) {
    uint32_t insn = 0;
    memcpy(&insn, &j->code[offset], 4);
    return insn;
}

static void a64_write32(JITBuffer* j, size_t offset, uint32_t insn) {
    memcpy(&j->code[offset], &insn, 4);
}

static void a64_movz64(JITBuffer* j, int rd, uint16_t imm16, int shift) {
    uint32_t hw = (uint32_t)(shift / 16) & 3U;
    a64_emit32(j, 0xD2800000U | (hw << 21) | ((uint32_t)imm16 << 5) | ((uint32_t)rd & 31U));
}

static void a64_movk64(JITBuffer* j, int rd, uint16_t imm16, int shift) {
    uint32_t hw = (uint32_t)(shift / 16) & 3U;
    a64_emit32(j, 0xF2800000U | (hw << 21) | ((uint32_t)imm16 << 5) | ((uint32_t)rd & 31U));
}

static void a64_mov_imm64(JITBuffer* j, int rd, uint64_t val) {
    a64_movz64(j, rd, (uint16_t)(val & 0xFFFFU), 0);
    if ((val >> 16) & 0xFFFFU) a64_movk64(j, rd, (uint16_t)((val >> 16) & 0xFFFFU), 16);
    if ((val >> 32) & 0xFFFFU) a64_movk64(j, rd, (uint16_t)((val >> 32) & 0xFFFFU), 32);
    if ((val >> 48) & 0xFFFFU) a64_movk64(j, rd, (uint16_t)((val >> 48) & 0xFFFFU), 48);
}

static void a64_mov_rr(JITBuffer* j, int rd, int rn) {
    if (rd == rn) return;
    /* ORR Xd, XZR, Xn */
    a64_emit32(j, 0xAA0003E0U | (((uint32_t)rn & 31U) << 16) | ((uint32_t)rd & 31U));
}

static void a64_mov_sp_from(JITBuffer* j, int rd_or_sp, int rn_or_sp) {
    /* ADD Xd/SP, Xn/SP, #0 */
    a64_emit32(j, 0x91000000U | (((uint32_t)rn_or_sp & 31U) << 5) | ((uint32_t)rd_or_sp & 31U));
}

static void a64_add_rr(JITBuffer* j, int rd, int rn, int rm) {
    a64_emit32(j, 0x8B000000U | (((uint32_t)rm & 31U) << 16) | (((uint32_t)rn & 31U) << 5) | ((uint32_t)rd & 31U));
}

static void a64_sub_rr(JITBuffer* j, int rd, int rn, int rm) {
    a64_emit32(j, 0xCB000000U | (((uint32_t)rm & 31U) << 16) | (((uint32_t)rn & 31U) << 5) | ((uint32_t)rd & 31U));
}

static void a64_add_sp_imm(JITBuffer* j, int rd, int rn, uint32_t imm) {
    if (imm == 0 && rd == rn) return;
    if (imm <= 4095U) {
        a64_emit32(j, 0x91000000U | ((imm & 0xFFFU) << 10) | (((uint32_t)rn & 31U) << 5) | ((uint32_t)rd & 31U));
    } else {
        a64_mov_imm64(j, A64_X16, imm);
        /* ADD (extended register) supports SP as Rn/Rd */
        a64_emit32(j, 0x8B206000U | ((uint32_t)A64_X16 << 16) | (((uint32_t)rn & 31U) << 5) | ((uint32_t)rd & 31U));
    }
}

static void a64_sub_sp_imm(JITBuffer* j, int rd, int rn, uint32_t imm) {
    if (imm == 0 && rd == rn) return;
    if (imm <= 4095U) {
        a64_emit32(j, 0xD1000000U | ((imm & 0xFFFU) << 10) | (((uint32_t)rn & 31U) << 5) | ((uint32_t)rd & 31U));
    } else {
        a64_mov_imm64(j, A64_X16, imm);
        a64_emit32(j, 0xCB206000U | ((uint32_t)A64_X16 << 16) | (((uint32_t)rn & 31U) << 5) | ((uint32_t)rd & 31U));
    }
}

static void a64_mul_rr(JITBuffer* j, int rd, int rn, int rm) {
    /* MADD Xd, Xn, Xm, XZR */
    a64_emit32(j, 0x9B007C00U | (((uint32_t)rm & 31U) << 16) | (((uint32_t)rn & 31U) << 5) | ((uint32_t)rd & 31U));
}

static void a64_sdiv_rr(JITBuffer* j, int rd, int rn, int rm) {
    a64_emit32(j, 0x9AC00C00U | (((uint32_t)rm & 31U) << 16) | (((uint32_t)rn & 31U) << 5) | ((uint32_t)rd & 31U));
}

static void a64_msub_rrr(JITBuffer* j, int rd, int rn, int rm, int ra) {
    a64_emit32(j, 0x9B008000U | (((uint32_t)rm & 31U) << 16) | (((uint32_t)ra & 31U) << 10) |
                  (((uint32_t)rn & 31U) << 5) | ((uint32_t)rd & 31U));
}

static void a64_cmp_rr(JITBuffer* j, int rn, int rm) {
    a64_emit32(j, 0xEB00001FU | (((uint32_t)rm & 31U) << 16) | (((uint32_t)rn & 31U) << 5));
}

static void a64_cset(JITBuffer* j, int rd, int cond) {
    /* CSINC Xd, XZR, XZR, invert(cond) */
    uint32_t inv = (uint32_t)(cond ^ 1) & 0xFU;
    a64_emit32(j, 0x9A9F07E0U | (inv << 12) | ((uint32_t)rd & 31U));
}

static void a64_and_rr(JITBuffer* j, int rd, int rn, int rm) {
    a64_emit32(j, 0x8A000000U | (((uint32_t)rm & 31U) << 16) | (((uint32_t)rn & 31U) << 5) | ((uint32_t)rd & 31U));
}

static void a64_orr_rr(JITBuffer* j, int rd, int rn, int rm) {
    a64_emit32(j, 0xAA000000U | (((uint32_t)rm & 31U) << 16) | (((uint32_t)rn & 31U) << 5) | ((uint32_t)rd & 31U));
}

static void a64_eor_rr(JITBuffer* j, int rd, int rn, int rm) {
    a64_emit32(j, 0xCA000000U | (((uint32_t)rm & 31U) << 16) | (((uint32_t)rn & 31U) << 5) | ((uint32_t)rd & 31U));
}

static void a64_mvn_rr(JITBuffer* j, int rd, int rm) {
    a64_emit32(j, 0xAA2003E0U | (((uint32_t)rm & 31U) << 16) | ((uint32_t)rd & 31U));
}

static void a64_lslv_rr(JITBuffer* j, int rd, int rn, int rm) {
    a64_emit32(j, 0x9AC02000U | (((uint32_t)rm & 31U) << 16) | (((uint32_t)rn & 31U) << 5) | ((uint32_t)rd & 31U));
}

static void a64_asrv_rr(JITBuffer* j, int rd, int rn, int rm) {
    a64_emit32(j, 0x9AC02800U | (((uint32_t)rm & 31U) << 16) | (((uint32_t)rn & 31U) << 5) | ((uint32_t)rd & 31U));
}

static void a64_sxtw(JITBuffer* j, int rd, int rn) {
    /* SBFM Xd, Xn, #0, #31 */
    a64_emit32(j, 0x93407C00U | (((uint32_t)rn & 31U) << 5) | ((uint32_t)rd & 31U));
}

static void a64_stp64_pre(JITBuffer* j, int rt1, int rt2, int rn, int32_t byte_off) {
    int32_t imm7 = byte_off / 8;
    a64_emit32(j, 0xA9800000U | (((uint32_t)imm7 & 0x7FU) << 15) |
                  (((uint32_t)rt2 & 31U) << 10) | (((uint32_t)rn & 31U) << 5) | ((uint32_t)rt1 & 31U));
}

static void a64_ldp64_post(JITBuffer* j, int rt1, int rt2, int rn, int32_t byte_off) {
    int32_t imm7 = byte_off / 8;
    a64_emit32(j, 0xA8C00000U | (((uint32_t)imm7 & 0x7FU) << 15) |
                  (((uint32_t)rt2 & 31U) << 10) | (((uint32_t)rn & 31U) << 5) | ((uint32_t)rt1 & 31U));
}

static void a64_stp64_off(JITBuffer* j, int rt1, int rt2, int rn, int32_t byte_off) {
    int32_t imm7 = byte_off / 8;
    a64_emit32(j, 0xA9000000U | (((uint32_t)imm7 & 0x7FU) << 15) |
                  (((uint32_t)rt2 & 31U) << 10) | (((uint32_t)rn & 31U) << 5) | ((uint32_t)rt1 & 31U));
}

static void a64_ldp64_off(JITBuffer* j, int rt1, int rt2, int rn, int32_t byte_off) {
    int32_t imm7 = byte_off / 8;
    a64_emit32(j, 0xA9400000U | (((uint32_t)imm7 & 0x7FU) << 15) |
                  (((uint32_t)rt2 & 31U) << 10) | (((uint32_t)rn & 31U) << 5) | ((uint32_t)rt1 & 31U));
}

static void a64_stpd_off(JITBuffer* j, int dt1, int dt2, int rn, int32_t byte_off) {
    int32_t imm7 = byte_off / 8;
    a64_emit32(j, 0x6D000000U | (((uint32_t)imm7 & 0x7FU) << 15) |
                  (((uint32_t)dt2 & 31U) << 10) | (((uint32_t)rn & 31U) << 5) | ((uint32_t)dt1 & 31U));
}

static void a64_ldpd_off(JITBuffer* j, int dt1, int dt2, int rn, int32_t byte_off) {
    int32_t imm7 = byte_off / 8;
    a64_emit32(j, 0x6D400000U | (((uint32_t)imm7 & 0x7FU) << 15) |
                  (((uint32_t)dt2 & 31U) << 10) | (((uint32_t)rn & 31U) << 5) | ((uint32_t)dt1 & 31U));
}

static int a64_effective_addr(JITBuffer* j, int base, int32_t off, int scratch) {
    if (off == 0) return base;
    if (off >= 0 && off <= 4095) {
        a64_emit32(j, 0x91000000U | (((uint32_t)off & 0xFFFU) << 10) |
                      (((uint32_t)base & 31U) << 5) | ((uint32_t)scratch & 31U));
    } else if (off < 0 && (-off) <= 4095) {
        a64_emit32(j, 0xD1000000U | (((uint32_t)(-off) & 0xFFFU) << 10) |
                      (((uint32_t)base & 31U) << 5) | ((uint32_t)scratch & 31U));
    } else {
        a64_mov_imm64(j, scratch, (uint64_t)(int64_t)off);
        a64_emit32(j, 0x8B206000U | (((uint32_t)scratch & 31U) << 16) |
                      (((uint32_t)base & 31U) << 5) | ((uint32_t)scratch & 31U));
    }
    return scratch;
}

static void a64_ldr64_off(JITBuffer* j, int rt, int base, int32_t off) {
    if (off >= 0 && (off % 8) == 0 && (off / 8) <= 4095) {
        uint32_t imm12 = (uint32_t)(off / 8);
        a64_emit32(j, 0xF9400000U | (imm12 << 10) | (((uint32_t)base & 31U) << 5) | ((uint32_t)rt & 31U));
    } else if (off >= -256 && off <= 255) {
        a64_emit32(j, 0xF8400000U | (((uint32_t)off & 0x1FFU) << 12) | (((uint32_t)base & 31U) << 5) | ((uint32_t)rt & 31U));
    } else {
        int ea = a64_effective_addr(j, base, off, A64_X16);
        a64_emit32(j, 0xF9400000U | (((uint32_t)ea & 31U) << 5) | ((uint32_t)rt & 31U));
    }
}

static void a64_str64_off(JITBuffer* j, int rt, int base, int32_t off) {
    if (off >= 0 && (off % 8) == 0 && (off / 8) <= 4095) {
        uint32_t imm12 = (uint32_t)(off / 8);
        a64_emit32(j, 0xF9000000U | (imm12 << 10) | (((uint32_t)base & 31U) << 5) | ((uint32_t)rt & 31U));
    } else if (off >= -256 && off <= 255) {
        a64_emit32(j, 0xF8000000U | (((uint32_t)off & 0x1FFU) << 12) | (((uint32_t)base & 31U) << 5) | ((uint32_t)rt & 31U));
    } else {
        int ea = a64_effective_addr(j, base, off, A64_X16);
        a64_emit32(j, 0xF9000000U | (((uint32_t)ea & 31U) << 5) | ((uint32_t)rt & 31U));
    }
}

static void a64_ldrsw_off(JITBuffer* j, int rt, int base, int32_t off) {
    if (off >= 0 && (off % 4) == 0 && (off / 4) <= 4095) {
        uint32_t imm12 = (uint32_t)(off / 4);
        a64_emit32(j, 0xB9800000U | (imm12 << 10) | (((uint32_t)base & 31U) << 5) | ((uint32_t)rt & 31U));
    } else if (off >= -256 && off <= 255) {
        a64_emit32(j, 0xB8800000U | (((uint32_t)off & 0x1FFU) << 12) | (((uint32_t)base & 31U) << 5) | ((uint32_t)rt & 31U));
    } else {
        int ea = a64_effective_addr(j, base, off, A64_X16);
        a64_emit32(j, 0xB9800000U | (((uint32_t)ea & 31U) << 5) | ((uint32_t)rt & 31U));
    }
}

static void a64_str32_off(JITBuffer* j, int wt, int base, int32_t off) {
    if (off >= 0 && (off % 4) == 0 && (off / 4) <= 4095) {
        uint32_t imm12 = (uint32_t)(off / 4);
        a64_emit32(j, 0xB9000000U | (imm12 << 10) | (((uint32_t)base & 31U) << 5) | ((uint32_t)wt & 31U));
    } else if (off >= -256 && off <= 255) {
        a64_emit32(j, 0xB8000000U | (((uint32_t)off & 0x1FFU) << 12) | (((uint32_t)base & 31U) << 5) | ((uint32_t)wt & 31U));
    } else {
        int ea = a64_effective_addr(j, base, off, A64_X16);
        a64_emit32(j, 0xB9000000U | (((uint32_t)ea & 31U) << 5) | ((uint32_t)wt & 31U));
    }
}

static void a64_ldrs_off(JITBuffer* j, int st, int base, int32_t off) {
    if (off >= 0 && (off % 4) == 0 && (off / 4) <= 4095) {
        uint32_t imm12 = (uint32_t)(off / 4);
        a64_emit32(j, 0xBD400000U | (imm12 << 10) | (((uint32_t)base & 31U) << 5) | ((uint32_t)st & 31U));
    } else if (off >= -256 && off <= 255) {
        a64_emit32(j, 0xBC400000U | (((uint32_t)off & 0x1FFU) << 12) | (((uint32_t)base & 31U) << 5) | ((uint32_t)st & 31U));
    } else {
        int ea = a64_effective_addr(j, base, off, A64_X16);
        a64_emit32(j, 0xBD400000U | (((uint32_t)ea & 31U) << 5) | ((uint32_t)st & 31U));
    }
}

static void a64_strs_off(JITBuffer* j, int st, int base, int32_t off) {
    if (off >= 0 && (off % 4) == 0 && (off / 4) <= 4095) {
        uint32_t imm12 = (uint32_t)(off / 4);
        a64_emit32(j, 0xBD000000U | (imm12 << 10) | (((uint32_t)base & 31U) << 5) | ((uint32_t)st & 31U));
    } else if (off >= -256 && off <= 255) {
        a64_emit32(j, 0xBC000000U | (((uint32_t)off & 0x1FFU) << 12) | (((uint32_t)base & 31U) << 5) | ((uint32_t)st & 31U));
    } else {
        int ea = a64_effective_addr(j, base, off, A64_X16);
        a64_emit32(j, 0xBD000000U | (((uint32_t)ea & 31U) << 5) | ((uint32_t)st & 31U));
    }
}

static void a64_ldrd_off(JITBuffer* j, int dt, int base, int32_t off) {
    if (off >= 0 && (off % 8) == 0 && (off / 8) <= 4095) {
        uint32_t imm12 = (uint32_t)(off / 8);
        a64_emit32(j, 0xFD400000U | (imm12 << 10) | (((uint32_t)base & 31U) << 5) | ((uint32_t)dt & 31U));
    } else if (off >= -256 && off <= 255) {
        a64_emit32(j, 0xFC400000U | (((uint32_t)off & 0x1FFU) << 12) | (((uint32_t)base & 31U) << 5) | ((uint32_t)dt & 31U));
    } else {
        int ea = a64_effective_addr(j, base, off, A64_X16);
        a64_emit32(j, 0xFD400000U | (((uint32_t)ea & 31U) << 5) | ((uint32_t)dt & 31U));
    }
}

static void a64_strd_off(JITBuffer* j, int dt, int base, int32_t off) {
    if (off >= 0 && (off % 8) == 0 && (off / 8) <= 4095) {
        uint32_t imm12 = (uint32_t)(off / 8);
        a64_emit32(j, 0xFD000000U | (imm12 << 10) | (((uint32_t)base & 31U) << 5) | ((uint32_t)dt & 31U));
    } else if (off >= -256 && off <= 255) {
        a64_emit32(j, 0xFC000000U | (((uint32_t)off & 0x1FFU) << 12) | (((uint32_t)base & 31U) << 5) | ((uint32_t)dt & 31U));
    } else {
        int ea = a64_effective_addr(j, base, off, A64_X16);
        a64_emit32(j, 0xFD000000U | (((uint32_t)ea & 31U) << 5) | ((uint32_t)dt & 31U));
    }
}

static void a64_ldr64_idx(JITBuffer* j, int rt, int rn, int rm) {
    a64_emit32(j, 0xF8607800U | (((uint32_t)rm & 31U) << 16) | (((uint32_t)rn & 31U) << 5) | ((uint32_t)rt & 31U));
}

static void a64_str64_idx(JITBuffer* j, int rt, int rn, int rm) {
    a64_emit32(j, 0xF8207800U | (((uint32_t)rm & 31U) << 16) | (((uint32_t)rn & 31U) << 5) | ((uint32_t)rt & 31U));
}

static void a64_ldrsw_idx(JITBuffer* j, int rt, int rn, int rm) {
    a64_emit32(j, 0xB8A07800U | (((uint32_t)rm & 31U) << 16) | (((uint32_t)rn & 31U) << 5) | ((uint32_t)rt & 31U));
}

static void a64_str32_idx(JITBuffer* j, int wt, int rn, int rm) {
    a64_emit32(j, 0xB8207800U | (((uint32_t)rm & 31U) << 16) | (((uint32_t)rn & 31U) << 5) | ((uint32_t)wt & 31U));
}

static void a64_ldrs_idx(JITBuffer* j, int st, int rn, int rm) {
    a64_emit32(j, 0xBC607800U | (((uint32_t)rm & 31U) << 16) | (((uint32_t)rn & 31U) << 5) | ((uint32_t)st & 31U));
}

static void a64_strs_idx(JITBuffer* j, int st, int rn, int rm) {
    a64_emit32(j, 0xBC207800U | (((uint32_t)rm & 31U) << 16) | (((uint32_t)rn & 31U) << 5) | ((uint32_t)st & 31U));
}

static void a64_ldrd_idx(JITBuffer* j, int dt, int rn, int rm) {
    a64_emit32(j, 0xFC607800U | (((uint32_t)rm & 31U) << 16) | (((uint32_t)rn & 31U) << 5) | ((uint32_t)dt & 31U));
}

static void a64_strd_idx(JITBuffer* j, int dt, int rn, int rm) {
    a64_emit32(j, 0xFC207800U | (((uint32_t)rm & 31U) << 16) | (((uint32_t)rn & 31U) << 5) | ((uint32_t)dt & 31U));
}

/* --- Scalar FP & NEON Emitters --- */
static void a64_fmov_s_from_w(JITBuffer* j, int sd, int wn) {
    a64_emit32(j, 0x1E270000U | (((uint32_t)wn & 31U) << 5) | ((uint32_t)sd & 31U));
}

static void a64_fmov_w_from_s(JITBuffer* j, int wd, int sn) {
    a64_emit32(j, 0x1E260000U | (((uint32_t)sn & 31U) << 5) | ((uint32_t)wd & 31U));
}

static void a64_fmov_ss(JITBuffer* j, int sd, int sn) {
    if (sd == sn) return;
    a64_emit32(j, 0x1E204000U | (((uint32_t)sn & 31U) << 5) | ((uint32_t)sd & 31U));
}

static void a64_fadds(JITBuffer* j, int sd, int sn, int sm) {
    a64_emit32(j, 0x1E202800U | (((uint32_t)sm & 31U) << 16) | (((uint32_t)sn & 31U) << 5) | ((uint32_t)sd & 31U));
}

static void a64_fsubs(JITBuffer* j, int sd, int sn, int sm) {
    a64_emit32(j, 0x1E203800U | (((uint32_t)sm & 31U) << 16) | (((uint32_t)sn & 31U) << 5) | ((uint32_t)sd & 31U));
}

static void a64_fmuls(JITBuffer* j, int sd, int sn, int sm) {
    a64_emit32(j, 0x1E200800U | (((uint32_t)sm & 31U) << 16) | (((uint32_t)sn & 31U) << 5) | ((uint32_t)sd & 31U));
}

static void a64_fdivs(JITBuffer* j, int sd, int sn, int sm) {
    a64_emit32(j, 0x1E201800U | (((uint32_t)sm & 31U) << 16) | (((uint32_t)sn & 31U) << 5) | ((uint32_t)sd & 31U));
}

static void a64_fcmps(JITBuffer* j, int sn, int sm) {
    a64_emit32(j, 0x1E202000U | (((uint32_t)sm & 31U) << 16) | (((uint32_t)sn & 31U) << 5));
}

static void a64_fcvt_d_from_s(JITBuffer* j, int dd, int sn) {
    a64_emit32(j, 0x1E22C000U | (((uint32_t)sn & 31U) << 5) | ((uint32_t)dd & 31U));
}

static void a64_fcvt_s_from_d(JITBuffer* j, int sd, int dn) {
    a64_emit32(j, 0x1E624000U | (((uint32_t)dn & 31U) << 5) | ((uint32_t)sd & 31U));
}

static void a64_scvtf_s_from_x(JITBuffer* j, int sd, int xn) {
    a64_emit32(j, 0x9E220000U | (((uint32_t)xn & 31U) << 5) | ((uint32_t)sd & 31U));
}

static void a64_fcvtzs_x_from_s(JITBuffer* j, int xd, int sn) {
    a64_emit32(j, 0x9E380000U | (((uint32_t)sn & 31U) << 5) | ((uint32_t)xd & 31U));
}

static void a64_load_f32_imm(JITBuffer* j, float val, int sd) {
    uint32_t bits = 0;
    memcpy(&bits, &val, 4);
    a64_movz64(j, A64_X15, (uint16_t)(bits & 0xFFFFU), 0);
    if ((bits >> 16) != 0) a64_movk64(j, A64_X15, (uint16_t)(bits >> 16), 16);
    a64_fmov_s_from_w(j, sd, A64_X15);
}

static void a64_fmov_d_from_x(JITBuffer* j, int dd, int xn) {
    a64_emit32(j, 0x9E670000U | (((uint32_t)xn & 31U) << 5) | ((uint32_t)dd & 31U));
}

static void a64_fmov_x_from_d(JITBuffer* j, int xd, int dn) {
    a64_emit32(j, 0x9E660000U | (((uint32_t)dn & 31U) << 5) | ((uint32_t)xd & 31U));
}

static void a64_fmov_dd(JITBuffer* j, int dd, int dn) {
    if (dd == dn) return;
    a64_emit32(j, 0x1E604000U | (((uint32_t)dn & 31U) << 5) | ((uint32_t)dd & 31U));
}

static void a64_faddd(JITBuffer* j, int dd, int dn, int dm) {
    a64_emit32(j, 0x1E602800U | (((uint32_t)dm & 31U) << 16) | (((uint32_t)dn & 31U) << 5) | ((uint32_t)dd & 31U));
}

static void a64_fsubd(JITBuffer* j, int dd, int dn, int dm) {
    a64_emit32(j, 0x1E603800U | (((uint32_t)dm & 31U) << 16) | (((uint32_t)dn & 31U) << 5) | ((uint32_t)dd & 31U));
}

static void a64_fmuld(JITBuffer* j, int dd, int dn, int dm) {
    a64_emit32(j, 0x1E600800U | (((uint32_t)dm & 31U) << 16) | (((uint32_t)dn & 31U) << 5) | ((uint32_t)dd & 31U));
}

static void a64_fdivd(JITBuffer* j, int dd, int dn, int dm) {
    a64_emit32(j, 0x1E601800U | (((uint32_t)dm & 31U) << 16) | (((uint32_t)dn & 31U) << 5) | ((uint32_t)dd & 31U));
}

static void a64_fcmpd(JITBuffer* j, int dn, int dm) {
    a64_emit32(j, 0x1E602000U | (((uint32_t)dm & 31U) << 16) | (((uint32_t)dn & 31U) << 5));
}

static void a64_scvtf_d_from_x(JITBuffer* j, int dd, int xn) {
    a64_emit32(j, 0x9E620000U | (((uint32_t)xn & 31U) << 5) | ((uint32_t)dd & 31U));
}

static void a64_fcvtzs_x_from_d(JITBuffer* j, int xd, int dn) {
    a64_emit32(j, 0x9E780000U | (((uint32_t)dn & 31U) << 5) | ((uint32_t)xd & 31U));
}

static void a64_load_f64_imm(JITBuffer* j, double val, int dd) {
    uint64_t bits = 0;
    memcpy(&bits, &val, 8);
    a64_mov_imm64(j, A64_X15, bits);
    a64_fmov_d_from_x(j, dd, A64_X15);
}

/* NEON 128-bit vector instructions (.4S / .16B) */
static void a64_ldr_q(JITBuffer* j, int qd, int xn) {
    a64_emit32(j, 0x3DC00000U | (((uint32_t)xn & 31U) << 5) | ((uint32_t)qd & 31U));
}

static void a64_eor_16b(JITBuffer* j, int vd, int vn, int vm) {
    a64_emit32(j, 0x6E201C00U | (((uint32_t)vm & 31U) << 16) | (((uint32_t)vn & 31U) << 5) | ((uint32_t)vd & 31U));
}

static void a64_dup_4s_elem0(JITBuffer* j, int vd, int vn) {
    a64_emit32(j, 0x4E040400U | (((uint32_t)vn & 31U) << 5) | ((uint32_t)vd & 31U));
}

static void a64_fadd_4s(JITBuffer* j, int vd, int vn, int vm) {
    a64_emit32(j, 0x4E20D400U | (((uint32_t)vm & 31U) << 16) | (((uint32_t)vn & 31U) << 5) | ((uint32_t)vd & 31U));
}

static void a64_fsub_4s(JITBuffer* j, int vd, int vn, int vm) {
    a64_emit32(j, 0x4EA0D400U | (((uint32_t)vm & 31U) << 16) | (((uint32_t)vn & 31U) << 5) | ((uint32_t)vd & 31U));
}

static void a64_fmul_4s(JITBuffer* j, int vd, int vn, int vm) {
    a64_emit32(j, 0x6E20DC00U | (((uint32_t)vm & 31U) << 16) | (((uint32_t)vn & 31U) << 5) | ((uint32_t)vd & 31U));
}

static void a64_fdiv_4s(JITBuffer* j, int vd, int vn, int vm) {
    a64_emit32(j, 0x6E20FC00U | (((uint32_t)vm & 31U) << 16) | (((uint32_t)vn & 31U) << 5) | ((uint32_t)vd & 31U));
}

static void a64_faddp_4s(JITBuffer* j, int vd, int vn, int vm) {
    a64_emit32(j, 0x6E20D400U | (((uint32_t)vm & 31U) << 16) | (((uint32_t)vn & 31U) << 5) | ((uint32_t)vd & 31U));
}

static void a64_blr(JITBuffer* j, int xn) {
    a64_emit32(j, 0xD63F0000U | (((uint32_t)xn & 31U) << 5));
}

static void a64_ret(JITBuffer* j) {
    a64_emit32(j, 0xD65F03C0U);
}

/* ========================================================================= */
/* FRAME, PROLOGUE, EPILOGUE & JUMP PATCHING                                 */
/* ========================================================================= */

static int a64_compute_frame_size(int total_spill_bytes) {
    int size = 224 + total_spill_bytes + 32;
    return (size + 15) & ~15;
}

static size_t a64_emit_jmp_placeholder(JITBuffer* j) {
    size_t pos = j->size;
    a64_emit32(j, 0x14000000U); /* B #0 */
    return pos;
}

static void a64_patch_jump(JITBuffer* j, size_t patch_pos) {
    int32_t word_off = (int32_t)((int64_t)(j->size - patch_pos) / 4);
    uint32_t insn = a64_read32(j, patch_pos);
    insn = (insn & 0xFC000000U) | ((uint32_t)word_off & 0x03FFFFFFU);
    a64_write32(j, patch_pos, insn);
}

static void a64_patch_branch(JITBuffer* j, size_t patch_pos, size_t target_offset, bool is_cond19) {
    int32_t word_off = (int32_t)((int64_t)(target_offset - patch_pos) / 4);
    uint32_t insn = a64_read32(j, patch_pos);
    if (is_cond19) {
        insn = (insn & 0xFF00001FU) | (((uint32_t)word_off & 0x7FFFFU) << 5);
    } else {
        insn = (insn & 0xFC000000U) | ((uint32_t)word_off & 0x03FFFFFFU);
    }
    a64_write32(j, patch_pos, insn);
}

static void a64_patch_call_rel(JITBuffer* j, size_t patch_pos, size_t target_offset) {
    int32_t word_off = (int32_t)((int64_t)(target_offset - patch_pos) / 4);
    uint32_t bl = 0x94000000U | ((uint32_t)word_off & 0x03FFFFFFU);
    a64_write32(j, patch_pos, bl);
}

static void a64_emit_save_callee(JITBuffer* j, int frame_size) {
    /* STP X29, X30, [SP, #-16]! ; MOV X29, SP ; SUB SP, SP, #frame_size */
    a64_stp64_pre(j, A64_FP, A64_LR, 31, -16);
    a64_mov_sp_from(j, A64_FP, 31);
    a64_sub_sp_imm(j, 31, 31, (uint32_t)frame_size);

    /* Save callee-saved GPRs x19..x28 at [X29, #-16 .. #-80] */
    a64_stp64_off(j, A64_X19, A64_X20, A64_FP, -16);
    a64_stp64_off(j, A64_X21, A64_X22, A64_FP, -32);
    a64_stp64_off(j, A64_X23, A64_X24, A64_FP, -48);
    a64_stp64_off(j, A64_X25, A64_X26, A64_FP, -64);
    a64_stp64_off(j, A64_X27, A64_X28, A64_FP, -80);

    /* Save callee-saved FP/NEON d8..d15 at [X29, #-96 .. #-144] */
    a64_stpd_off(j, 8,  9,  A64_FP, -96);
    a64_stpd_off(j, 10, 11, A64_FP, -112);
    a64_stpd_off(j, 12, 13, A64_FP, -128);
    a64_stpd_off(j, 14, 15, A64_FP, -144);
}

static void a64_emit_prologue(JITBuffer* j, int frame_size) {
    a64_emit_save_callee(j, frame_size);
}

static void a64_emit_epilogue(JITBuffer* j, int frame_size) {
    (void)frame_size;
    a64_ldpd_off(j, 14, 15, A64_FP, -144);
    a64_ldpd_off(j, 12, 13, A64_FP, -128);
    a64_ldpd_off(j, 10, 11, A64_FP, -112);
    a64_ldpd_off(j, 8,  9,  A64_FP, -96);

    a64_ldp64_off(j, A64_X27, A64_X28, A64_FP, -80);
    a64_ldp64_off(j, A64_X25, A64_X26, A64_FP, -64);
    a64_ldp64_off(j, A64_X23, A64_X24, A64_FP, -48);
    a64_ldp64_off(j, A64_X21, A64_X22, A64_FP, -32);
    a64_ldp64_off(j, A64_X19, A64_X20, A64_FP, -16);

    a64_mov_sp_from(j, 31, A64_FP);
    a64_ldp64_post(j, A64_FP, A64_LR, 31, 16);
    a64_ret(j);
}

static void a64_emit_func_prologue(JITBuffer* j, int frame_size) {
    a64_emit_save_callee(j, frame_size);
    /* Home incoming AAPCS64/Darwin64 register arguments x0..x7 at [X29, #-208 + p*8] */
    a64_stp64_off(j, A64_X0, A64_X1, A64_FP, -208);
    a64_stp64_off(j, A64_X2, A64_X3, A64_FP, -192);
    a64_stp64_off(j, A64_X4, A64_X5, A64_FP, -176);
    a64_stp64_off(j, A64_X6, A64_X7, A64_FP, -160);
}

static void a64_emit_func_epilogue(JITBuffer* j, int frame_size) {
    a64_emit_epilogue(j, frame_size);
}

/* ========================================================================= */
/* REGISTER RESOLUTION & CALL MARSHALING HELPERS                             */
/* ========================================================================= */

static int a64_get_int_reg(IRBuffer* ir, VReg v, int scratch_reg, JITBuffer* j) {
    if (v == VREG_NONE || v < 0 || v >= ir->vreg_count) return PREG_NONE;
    if (ir->vregs[v].is_spilled) {
        a64_ldr64_off(j, scratch_reg, A64_FP, -ir->vregs[v].spill_offset);
        return scratch_reg;
    }
    return ir->vregs[v].preg;
}

static void a64_commit_int_reg(IRBuffer* ir, VReg v, int source_reg, JITBuffer* j) {
    if (v == VREG_NONE || v < 0 || v >= ir->vreg_count) return;
    if (ir->vregs[v].is_spilled) {
        a64_str64_off(j, source_reg, A64_FP, -ir->vregs[v].spill_offset);
    } else {
        int target_reg = ir->vregs[v].preg;
        if (target_reg != source_reg && target_reg != PREG_NONE) {
            a64_mov_rr(j, target_reg, source_reg);
        }
    }
}

static int a64_get_float_reg(IRBuffer* ir, VReg v, int scratch_s, JITBuffer* j) {
    if (v == VREG_NONE || v < 0 || v >= ir->vreg_count) return PREG_NONE;
    if (ir->vregs[v].is_spilled) {
        if (ir->vregs[v].type == TYPE_F64) a64_ldrd_off(j, scratch_s, A64_FP, -ir->vregs[v].spill_offset);
        else a64_ldrs_off(j, scratch_s, A64_FP, -ir->vregs[v].spill_offset);
        return scratch_s;
    }
    return ir->vregs[v].preg;
}

static void a64_commit_float_reg(IRBuffer* ir, VReg v, int source_s, JITBuffer* j) {
    if (v == VREG_NONE || v < 0 || v >= ir->vreg_count) return;
    if (ir->vregs[v].is_spilled) {
        if (ir->vregs[v].type == TYPE_F64) a64_strd_off(j, source_s, A64_FP, -ir->vregs[v].spill_offset);
        else a64_strs_off(j, source_s, A64_FP, -ir->vregs[v].spill_offset);
    } else {
        int target_s = ir->vregs[v].preg;
        if (target_s != source_s && target_s != PREG_NONE) {
            if (ir->vregs[v].type == TYPE_F64) a64_fmov_dd(j, target_s, source_s);
            else a64_fmov_ss(j, target_s, source_s);
        }
    }
}

typedef enum {
    A64_ARG_REG = 0,
    A64_ARG_IMM = 1
} A64ArgKind;

typedef struct {
    A64ArgKind kind;
    VReg vreg;
    int64_t imm;
    VortexType ftype;
} A64CallArg;

static void a64_emit_stage_and_call(JITBuffer* j, IRBuffer* ir, void* fn_ptr,
                                    const A64CallArg* args, int n, bool is_ffi,
                                    bool is_variadic, int fixed_params) {
    const VortexTargetDesc* td = vortex_get_target_desc();
    bool is_darwin = (td->abi == VORTEX_ABI_DARWIN64);

    int int_cnt = 0;
    int flt_cnt = 0;
    int stk_cnt = 0;
    bool arg_on_stack[MAX_CALL_ARGS];
    int arg_slot[MAX_CALL_ARGS];

    for (int k = 0; k < n; k++) {
        bool is_flt = is_ffi && (args[k].ftype == TYPE_F32 || args[k].ftype == TYPE_F64);
        bool force_stack = (is_ffi && is_darwin && is_variadic && k >= fixed_params);
        if (!force_stack && is_flt && flt_cnt < 8) {
            arg_on_stack[k] = false;
            arg_slot[k] = flt_cnt++;
        } else if (!force_stack && !is_flt && int_cnt < 8) {
            arg_on_stack[k] = false;
            arg_slot[k] = int_cnt++;
        } else {
            arg_on_stack[k] = true;
            arg_slot[k] = stk_cnt++;
        }
    }

    uint32_t eval_bytes = (uint32_t)((n * 8 + 15) & ~15);
    uint32_t stk_bytes = (uint32_t)((stk_cnt * 8 + 15) & ~15);
    uint32_t total_bytes = eval_bytes + stk_bytes;
    if (total_bytes > 0) {
        a64_sub_sp_imm(j, 31, 31, total_bytes);
    }

    /* Evaluate all argument values into bottom eval staging [SP + k*8] */
    for (int k = 0; k < n; k++) {
        int32_t off = (int32_t)(k * 8);
        if (args[k].kind == A64_ARG_IMM) {
            a64_mov_imm64(j, A64_X13, (uint64_t)args[k].imm);
            a64_str64_off(j, A64_X13, 31, off);
        } else {
            VReg av = args[k].vreg;
            if (ir->vregs[av].is_float) {
                int s = a64_get_float_reg(ir, av, 24, j);
                if (ir->vregs[av].type == TYPE_F64) {
                    if (is_ffi && args[k].ftype == TYPE_F32) {
                        a64_fcvt_s_from_d(j, 25, s);
                        a64_strs_off(j, 25, 31, off);
                    } else if (is_ffi && (args[k].ftype == TYPE_F64 || (is_variadic && k >= fixed_params))) {
                        a64_strd_off(j, s, 31, off);
                    } else {
                        a64_fmov_x_from_d(j, A64_X13, s);
                        a64_str64_off(j, A64_X13, 31, off);
                    }
                } else if (is_ffi && (args[k].ftype == TYPE_F64 || (is_variadic && k >= fixed_params))) {
                    a64_fcvt_d_from_s(j, 25, s);
                    a64_strd_off(j, 25, 31, off);
                } else if (is_ffi && args[k].ftype == TYPE_F32) {
                    a64_strs_off(j, s, 31, off);
                } else {
                    a64_fmov_w_from_s(j, A64_X13, s);
                    a64_str64_off(j, A64_X13, 31, off);
                }
            } else {
                int r = a64_get_int_reg(ir, av, A64_X13, j);
                a64_str64_off(j, r, 31, off);
            }
        }
    }

    /* Copy stack arguments to outgoing stack slots [SP + eval_bytes + slot*8] */
    for (int k = 0; k < n; k++) {
        if (!arg_on_stack[k]) continue;
        a64_ldr64_off(j, A64_X13, 31, (int32_t)(k * 8));
        a64_str64_off(j, A64_X13, 31, (int32_t)(eval_bytes + (uint32_t)(arg_slot[k] * 8)));
    }

    /* Load register arguments into x0..x7 and v0..v7 */
    for (int k = 0; k < n; k++) {
        if (arg_on_stack[k]) continue;
        int32_t off = (int32_t)(k * 8);
        bool is_flt = is_ffi && (args[k].ftype == TYPE_F32 || args[k].ftype == TYPE_F64);
        if (is_flt) {
            if (args[k].ftype == TYPE_F64 || (is_variadic && k >= fixed_params)) {
                a64_ldrd_off(j, arg_slot[k], 31, off);
            } else {
                a64_ldrs_off(j, arg_slot[k], 31, off);
            }
        } else {
            a64_ldr64_off(j, arg_slot[k], 31, off);
        }
    }

    if (eval_bytes > 0) {
        a64_add_sp_imm(j, 31, 31, eval_bytes);
    }

    a64_mov_imm64(j, A64_X16, (uint64_t)(uintptr_t)fn_ptr);
    a64_blr(j, A64_X16);

    if (stk_bytes > 0) {
        a64_add_sp_imm(j, 31, 31, stk_bytes);
    }
}

/* ========================================================================= */
/* 128-BIT NEON (ASIMD) 8-WAY UNROLLED VECTOR LOOP EMITTER                   */
/* ========================================================================= */

static void emit_neon_vector_loop(JITBuffer* j, IRBuffer* ir, IRInstruction* ins) {
    /* Register partition:
     *   v0, v1   = 4S accumulators
     *   v2       = saved initial scalar acc (s2)
     *   v4, v5   = scalar x (s4), scalar step (s5)
     *   v6, v7   = scalar c2 (s6), scalar c1 (s7)
     *   v24, v25 = xA.4s, xB.4s stepping vectors
     *   v26      = step8.4s
     *   v27, v28 = c1.4s, c2.4s
     *   v29..v31 = body expression temporaries
     * Notice v8..v23 (allocated float registers) are never clobbered mid-loop! */
    int i_reg = a64_get_int_reg(ir, ins->i_vreg, A64_X13, j);
    a64_mov_rr(j, A64_X14, i_reg); /* X14 = i */

    int lim_reg = a64_get_int_reg(ir, ins->lim_vreg, A64_X13, j);
    a64_mov_rr(j, A64_X15, lim_reg); /* X15 = lim */

    int acc_reg = a64_get_float_reg(ir, ins->acc_vreg, 2, j);
    a64_fmov_ss(j, 2, acc_reg); /* s2 = pre-loop acc */

    int x_reg = a64_get_float_reg(ir, ins->x_vreg, 4, j);
    a64_fmov_ss(j, 4, x_reg); /* s4 = x */

    int step_reg = a64_get_float_reg(ir, ins->step_vreg, 5, j);
    a64_fmov_ss(j, 5, step_reg); /* s5 = step */

    a64_eor_16b(j, 0, 0, 0); /* v0.4s = 0 */
    a64_eor_16b(j, 1, 1, 1); /* v1.4s = 0 */

    a64_dup_4s_elem0(j, 29, 5); /* v29.4s = step */
    a64_dup_4s_elem0(j, 30, 4); /* v30.4s = x */

    a64_mov_imm64(j, A64_X13, (uint64_t)(uintptr_t)g_neon_lane_offsets_A);
    a64_ldr_q(j, 24, A64_X13);
    a64_mov_imm64(j, A64_X13, (uint64_t)(uintptr_t)g_neon_lane_offsets_B);
    a64_ldr_q(j, 25, A64_X13);

    a64_fmul_4s(j, 24, 24, 29); /* xA = {0,1,2,3}*step */
    a64_fmul_4s(j, 25, 25, 29); /* xB = {4,5,6,7}*step */
    a64_fadd_4s(j, 24, 24, 30); /* xA += x0 */
    a64_fadd_4s(j, 25, 25, 30); /* xB += x0 */

    a64_mov_imm64(j, A64_X13, (uint64_t)(uintptr_t)g_neon_eight);
    a64_ldr_q(j, 26, A64_X13);
    a64_fmul_4s(j, 26, 26, 29); /* v26.4s = 8.0 * step */

    float const1_val = (ins->num_val != 0.0f) ? ins->num_val : 4.0f;
    float const2_val = (ins->den_base != 0.0f) ? ins->den_base : 1.0f;
    for (int b = 0; b < ins->body_op_count; b++) {
        if (ins->body_ops[b].op == IR_CONST_F32) {
            if (b == 0) const1_val = ins->body_ops[b].fimm;
            else const2_val = ins->body_ops[b].fimm;
        }
    }
    a64_load_f32_imm(j, const1_val, 7);
    a64_dup_4s_elem0(j, 27, 7); /* v27.4s = const1 */
    a64_load_f32_imm(j, const2_val, 6);
    a64_dup_4s_elem0(j, 28, 6); /* v28.4s = const2 */

    /* X16 = start i, X17 = unroll_limit = lim & ~7ULL */
    a64_mov_rr(j, A64_X16, A64_X14);
    a64_mov_imm64(j, A64_X13, ~7ULL);
    /* AND X17, X15, X13 */
    a64_emit32(j, 0x8A000000U | ((uint32_t)A64_X13 << 16) | ((uint32_t)A64_X15 << 5) | (uint32_t)A64_X17);

    size_t loop_start = j->size;
    a64_cmp_rr(j, A64_X14, A64_X17);
    size_t exit_unroll = j->size;
    a64_emit32(j, 0x54000000U | 10U); /* B.GE exit_unroll */

    if (ins->body_op_count > 0 && ins->num_val == 0.0f && ins->den_base == 0.0f) {
        for (int b = 0; b < ins->body_op_count; b++) {
            IRInstruction* bo = &ins->body_ops[b];
            if (bo->op == IR_CONST_F32) continue;
            int vd = 29 + (b % 3);
            int vs1 = (bo->src1 == ins->x_vreg) ? 24 : (29 + ((b - 1 + 3) % 3));
            int vs2 = (bo->src2 == ins->x_vreg) ? 24 : (29 + ((b - 2 + 3) % 3));
            if (bo->op == IR_FADD) a64_fadd_4s(j, vd, vs1, vs2);
            else if (bo->op == IR_FSUB) a64_fsub_4s(j, vd, vs1, vs2);
            else if (bo->op == IR_FMUL) a64_fmul_4s(j, vd, vs1, vs2);
            else a64_fdiv_4s(j, vd, vs1, vs2);
        }
        int res_v = 29 + ((ins->body_op_count - 1) % 3);
        a64_fadd_4s(j, 0, 0, res_v);
    } else {
        /* Pipeline 8 lanes: v29/v30 = xA^2 + c2, v31/v3 = xB^2 + c2 */
        a64_fmul_4s(j, 29, 24, 24);
        a64_fmul_4s(j, 30, 25, 25);
        a64_fadd_4s(j, 29, 29, 28);
        a64_fadd_4s(j, 30, 30, 28);
        a64_fdiv_4s(j, 29, 27, 29);
        a64_fdiv_4s(j, 30, 27, 30);
        a64_fadd_4s(j, 0, 0, 29);
        a64_fadd_4s(j, 1, 1, 30);
    }

    a64_fadd_4s(j, 24, 24, 26); /* xA += 8*step */
    a64_fadd_4s(j, 25, 25, 26); /* xB += 8*step */
    a64_add_sp_imm(j, A64_X14, A64_X14, 8); /* i += 8 */

    int32_t back_word = (int32_t)((int64_t)(loop_start - j->size) / 4);
    a64_emit32(j, 0x14000000U | ((uint32_t)back_word & 0x03FFFFFFU));

    a64_patch_branch(j, exit_unroll, j->size, true);

    /* Reduce v0.4s + v1.4s -> s0, then add initial scalar acc (s2) */
    a64_fadd_4s(j, 0, 0, 1);
    a64_faddp_4s(j, 0, 0, 0);
    a64_faddp_4s(j, 0, 0, 0);
    a64_fadds(j, 0, 0, 2);

    /* Advance scalar x: s4 += (i - i_start) * s5 */
    a64_sub_rr(j, A64_X13, A64_X14, A64_X16);
    a64_scvtf_s_from_x(j, 3, A64_X13);
    a64_fmuls(j, 3, 3, 5);
    a64_fadds(j, 4, 4, 3);

    /* Tail loop: while i (X14) < lim (X15) */
    size_t tail_start = j->size;
    a64_cmp_rr(j, A64_X14, A64_X15);
    size_t tail_exit = j->size;
    a64_emit32(j, 0x54000000U | 10U); /* B.GE tail_exit */

    a64_fmuls(j, 3, 4, 4);    /* s3 = x*x */
    a64_fadds(j, 3, 3, 6);    /* s3 = x*x + c2 */
    a64_fdivs(j, 3, 7, 3);    /* s3 = c1 / (x*x + c2) */
    a64_fadds(j, 0, 0, 3);    /* acc += s3 */
    a64_fadds(j, 4, 4, 5);    /* x += step */
    a64_add_sp_imm(j, A64_X14, A64_X14, 1);

    int32_t tail_back_word = (int32_t)((int64_t)(tail_start - j->size) / 4);
    a64_emit32(j, 0x14000000U | ((uint32_t)tail_back_word & 0x03FFFFFFU));

    a64_patch_branch(j, tail_exit, j->size, true);

    a64_commit_float_reg(ir, ins->acc_vreg, 0, j);
    a64_commit_float_reg(ir, ins->x_vreg, 4, j);
    a64_commit_float_reg(ir, ins->step_vreg, 5, j);
    a64_commit_int_reg(ir, ins->i_vreg, A64_X14, j);
}

/* ========================================================================= */
/* MAIN AARCH64 IR CODE GENERATOR                                            */
/* ========================================================================= */

typedef struct {
    int label_id;
    size_t patch_pos;
    bool is_cond19;
} A64JumpPatch;

void ir_codegen_arm64(IRBuffer* ir, JITBuffer* j) {
    int* label_offsets = (int*)malloc(MAX_LABELS * sizeof(int));
    A64JumpPatch* patches = (A64JumpPatch*)malloc(MAX_IR_INSTRS * sizeof(A64JumpPatch));
    if (!label_offsets || !patches) {
        if (label_offsets) free(label_offsets);
        if (patches) free(patches);
        return;
    }
    for (int i = 0; i < MAX_LABELS; i++) label_offsets[i] = -1;
    int patch_count = 0;

    for (int ip = 0; ip < ir->count; ip++) {
        IRInstruction* ins = &ir->instrs[ip];
        size_t ins_start_off = j->size;
        switch (ins->op) {
            case IR_NOP: break;

            case IR_CONST_I64: {
                int rd = ir->vregs[ins->dest].is_spilled ? A64_X13 : ir->vregs[ins->dest].preg;
                a64_mov_imm64(j, rd, (uint64_t)ins->imm);
                a64_commit_int_reg(ir, ins->dest, rd, j);
                break;
            }


            case IR_CONST_F32: {
                int sd = ir->vregs[ins->dest].is_spilled ? 24 : ir->vregs[ins->dest].preg;
                a64_load_f32_imm(j, ins->fimm, sd);
                a64_commit_float_reg(ir, ins->dest, sd, j);
                break;
            }

            case IR_CONST_F64: {
                int dd = ir->vregs[ins->dest].is_spilled ? 24 : ir->vregs[ins->dest].preg;
                a64_load_f64_imm(j, ins->f64_imm, dd);
                a64_commit_float_reg(ir, ins->dest, dd, j);
                break;
            }

            case IR_MOV: {
                if (ir->vregs[ins->dest].is_float) {
                    int ss = a64_get_float_reg(ir, ins->src1, 24, j);
                    a64_commit_float_reg(ir, ins->dest, ss, j);
                } else {
                    int rs = a64_get_int_reg(ir, ins->src1, A64_X13, j);
                    a64_commit_int_reg(ir, ins->dest, rs, j);
                }
                break;
            }

            case IR_ADD:
            case IR_SUB:
            case IR_MUL: {
                int r1 = a64_get_int_reg(ir, ins->src1, A64_X13, j);
                int r2 = a64_get_int_reg(ir, ins->src2, A64_X14, j);
                int rd = ir->vregs[ins->dest].is_spilled ? A64_X13 : ir->vregs[ins->dest].preg;
                if (ins->op == IR_ADD) a64_add_rr(j, rd, r1, r2);
                else if (ins->op == IR_SUB) a64_sub_rr(j, rd, r1, r2);
                else a64_mul_rr(j, rd, r1, r2);
                a64_commit_int_reg(ir, ins->dest, rd, j);
                break;
            }


            case IR_DIV:
            case IR_MOD: {
                int r1 = a64_get_int_reg(ir, ins->src1, A64_X13, j);
                int r2 = a64_get_int_reg(ir, ins->src2, A64_X14, j);
                int rd = ir->vregs[ins->dest].is_spilled ? A64_X15 : ir->vregs[ins->dest].preg;
                if (ins->op == IR_DIV) {
                    a64_sdiv_rr(j, rd, r1, r2);
                } else {
                    a64_sdiv_rr(j, A64_X15, r1, r2);
                    a64_msub_rrr(j, rd, A64_X15, r2, r1);
                }
                a64_commit_int_reg(ir, ins->dest, rd, j);
                break;
            }


            case IR_FADD:
            case IR_FSUB:
            case IR_FMUL:
            case IR_FDIV: {
                int s1 = a64_get_float_reg(ir, ins->src1, 24, j);
                int s2 = a64_get_float_reg(ir, ins->src2, 25, j);
                int sd = ir->vregs[ins->dest].is_spilled ? 24 : ir->vregs[ins->dest].preg;
                if (ins->type == TYPE_F64) {
                    if (ins->op == IR_FADD) a64_faddd(j, sd, s1, s2);
                    else if (ins->op == IR_FSUB) a64_fsubd(j, sd, s1, s2);
                    else if (ins->op == IR_FMUL) a64_fmuld(j, sd, s1, s2);
                    else a64_fdivd(j, sd, s1, s2);
                } else {
                    if (ins->op == IR_FADD) a64_fadds(j, sd, s1, s2);
                    else if (ins->op == IR_FSUB) a64_fsubs(j, sd, s1, s2);
                    else if (ins->op == IR_FMUL) a64_fmuls(j, sd, s1, s2);
                    else a64_fdivs(j, sd, s1, s2);
                }
                a64_commit_float_reg(ir, ins->dest, sd, j);
                break;
            }

            case IR_EQ:
            case IR_NE:
            case IR_LT:
            case IR_LE:
            case IR_GT:
            case IR_GE: {
                int r1 = a64_get_int_reg(ir, ins->src1, A64_X13, j);
                int r2 = a64_get_int_reg(ir, ins->src2, A64_X14, j);
                a64_cmp_rr(j, r1, r2);
                int cond = 0;
                switch (ins->op) {
                    case IR_EQ: cond = 0;  break; /* EQ */
                    case IR_NE: cond = 1;  break; /* NE */
                    case IR_LT: cond = 11; break; /* LT */
                    case IR_LE: cond = 13; break; /* LE */
                    case IR_GT: cond = 12; break; /* GT */
                    case IR_GE: cond = 10; break; /* GE */
                    default: break;
                }
                int rd = ir->vregs[ins->dest].is_spilled ? A64_X13 : ir->vregs[ins->dest].preg;
                a64_cset(j, rd, cond);
                a64_commit_int_reg(ir, ins->dest, rd, j);
                break;
            }


            case IR_FEQ:
            case IR_FNE:
            case IR_FLT:
            case IR_FLE:
            case IR_FGT:
            case IR_FGE: {
                int s1 = a64_get_float_reg(ir, ins->src1, 24, j);
                int s2 = a64_get_float_reg(ir, ins->src2, 25, j);
                if (ins->type == TYPE_F64) a64_fcmpd(j, s1, s2);
                else a64_fcmps(j, s1, s2);
                int cond = 0;
                switch (ins->op) {
                    case IR_FEQ: cond = 0;  break; /* EQ */
                    case IR_FNE: cond = 1;  break; /* NE */
                    case IR_FLT: cond = 4;  break; /* MI (ordered <, false on NaN) */
                    case IR_FLE: cond = 9;  break; /* LS (ordered <=, false on NaN) */
                    case IR_FGT: cond = 12; break; /* GT (ordered >, false on NaN) */
                    case IR_FGE: cond = 10; break; /* GE (ordered >=, false on NaN) */
                    default: break;
                }
                int rd = ir->vregs[ins->dest].is_spilled ? A64_X13 : ir->vregs[ins->dest].preg;
                a64_cset(j, rd, cond);
                a64_commit_int_reg(ir, ins->dest, rd, j);
                break;
            }


            case IR_CAST_I64_TO_F32: {
                int rs = a64_get_int_reg(ir, ins->src1, A64_X13, j);
                int sd = ir->vregs[ins->dest].is_spilled ? 24 : ir->vregs[ins->dest].preg;
                a64_scvtf_s_from_x(j, sd, rs);
                a64_commit_float_reg(ir, ins->dest, sd, j);
                break;
            }

            case IR_CAST_F32_TO_I64: {
                int ss = a64_get_float_reg(ir, ins->src1, 24, j);
                int rd = ir->vregs[ins->dest].is_spilled ? A64_X13 : ir->vregs[ins->dest].preg;
                a64_fcvtzs_x_from_s(j, rd, ss);
                a64_commit_int_reg(ir, ins->dest, rd, j);
                break;
            }

            case IR_CAST_I64_TO_F64: {
                int rs = a64_get_int_reg(ir, ins->src1, A64_X13, j);
                int dd = ir->vregs[ins->dest].is_spilled ? 24 : ir->vregs[ins->dest].preg;
                a64_scvtf_d_from_x(j, dd, rs);
                a64_commit_float_reg(ir, ins->dest, dd, j);
                break;
            }

            case IR_CAST_F64_TO_I64: {
                int ds = a64_get_float_reg(ir, ins->src1, 24, j);
                int rd = ir->vregs[ins->dest].is_spilled ? A64_X13 : ir->vregs[ins->dest].preg;
                a64_fcvtzs_x_from_d(j, rd, ds);
                a64_commit_int_reg(ir, ins->dest, rd, j);
                break;
            }

            case IR_CAST_F32_TO_F64: {
                int ss = a64_get_float_reg(ir, ins->src1, 24, j);
                int dd = ir->vregs[ins->dest].is_spilled ? 24 : ir->vregs[ins->dest].preg;
                a64_fcvt_d_from_s(j, dd, ss);
                a64_commit_float_reg(ir, ins->dest, dd, j);
                break;
            }

            case IR_CAST_F64_TO_F32: {
                int ds = a64_get_float_reg(ir, ins->src1, 24, j);
                int sd = ir->vregs[ins->dest].is_spilled ? 24 : ir->vregs[ins->dest].preg;
                a64_fcvt_s_from_d(j, sd, ds);
                a64_commit_float_reg(ir, ins->dest, sd, j);
                break;
            }

            case IR_BAND:
            case IR_BOR:
            case IR_BXOR:
            case IR_SHL:
            case IR_SHR: {
                int r1 = a64_get_int_reg(ir, ins->src1, A64_X13, j);
                int r2;
                if (ins->src2 == VREG_NONE) {
                    a64_mov_imm64(j, A64_X14, (uint64_t)ins->imm);
                    r2 = A64_X14;
                } else {
                    r2 = a64_get_int_reg(ir, ins->src2, A64_X14, j);
                }
                int rd = ir->vregs[ins->dest].is_spilled ? A64_X15 : ir->vregs[ins->dest].preg;
                if (ins->op == IR_BAND) a64_and_rr(j, rd, r1, r2);
                else if (ins->op == IR_BOR) a64_orr_rr(j, rd, r1, r2);
                else if (ins->op == IR_BXOR) a64_eor_rr(j, rd, r1, r2);
                else if (ins->op == IR_SHL) a64_lslv_rr(j, rd, r1, r2);
                else a64_asrv_rr(j, rd, r1, r2);
                a64_commit_int_reg(ir, ins->dest, rd, j);
                break;
            }

            case IR_BNOT: {
                int r1 = a64_get_int_reg(ir, ins->src1, A64_X13, j);
                int rd = ir->vregs[ins->dest].is_spilled ? A64_X13 : ir->vregs[ins->dest].preg;
                a64_mvn_rr(j, rd, r1);
                a64_commit_int_reg(ir, ins->dest, rd, j);
                break;
            }

            case IR_LNOT: {
                int r1 = a64_get_int_reg(ir, ins->src1, A64_X13, j);
                a64_cmp_rr(j, r1, A64_XZR);
                int rd = ir->vregs[ins->dest].is_spilled ? A64_X13 : ir->vregs[ins->dest].preg;
                a64_cset(j, rd, 0); /* EQ */
                a64_commit_int_reg(ir, ins->dest, rd, j);
                break;
            }

            case IR_DIV_CHECK: {
                A64CallArg cargs[2];
                int r_div = a64_get_int_reg(ir, ins->src1, A64_X13, j);
                size_t cbnz_pos = j->size;
                a64_emit32(j, 0xB5000000U | ((uint32_t)r_div & 31U)); /* CBNZ Xr, +ok */
                cargs[0] = (A64CallArg){ A64_ARG_IMM, VREG_NONE, (int64_t)(uintptr_t)vortex_intern_cstr(ins->file ? ins->file : "<input>"), TYPE_I64 };
                cargs[1] = (A64CallArg){ A64_ARG_IMM, VREG_NONE, (int64_t)ins->line, TYPE_I64 };
                a64_emit_stage_and_call(j, ir, (void*)vortex_ultrasafe_div_zero, cargs, 2, false, false, 2);
                a64_patch_branch(j, cbnz_pos, j->size, true);
                break;
            }


            case IR_LOAD_OFFSET: {
                int r_base = a64_get_int_reg(ir, ins->src1, A64_X13, j);
                if (ins->type == TYPE_F64) {
                    int dd = ir->vregs[ins->dest].is_spilled ? 24 : ir->vregs[ins->dest].preg;
                    a64_ldrd_off(j, dd, r_base, ins->offset);
                    a64_commit_float_reg(ir, ins->dest, dd, j);
                } else if (ins->type == TYPE_F32) {
                    int sd = ir->vregs[ins->dest].is_spilled ? 24 : ir->vregs[ins->dest].preg;
                    a64_ldrs_off(j, sd, r_base, ins->offset);
                    a64_commit_float_reg(ir, ins->dest, sd, j);
                } else if (ins->type == TYPE_I32) {
                    int rd = ir->vregs[ins->dest].is_spilled ? A64_X13 : ir->vregs[ins->dest].preg;
                    a64_ldrsw_off(j, rd, r_base, ins->offset);
                    a64_commit_int_reg(ir, ins->dest, rd, j);
                } else {
                    int rd = ir->vregs[ins->dest].is_spilled ? A64_X13 : ir->vregs[ins->dest].preg;
                    a64_ldr64_off(j, rd, r_base, ins->offset);
                    a64_commit_int_reg(ir, ins->dest, rd, j);
                }
                break;
            }

            case IR_STORE_OFFSET: {
                int r_base = a64_get_int_reg(ir, ins->src1, A64_X13, j);
                if (ins->type == TYPE_F64) {
                    int ds = a64_get_float_reg(ir, ins->src2, 24, j);
                    a64_strd_off(j, ds, r_base, ins->offset);
                } else if (ins->type == TYPE_F32) {
                    int ss = a64_get_float_reg(ir, ins->src2, 24, j);
                    a64_strs_off(j, ss, r_base, ins->offset);
                } else if (ins->type == TYPE_I32) {
                    int rs = a64_get_int_reg(ir, ins->src2, A64_X14, j);
                    a64_str32_off(j, rs, r_base, ins->offset);
                } else {
                    int rs = a64_get_int_reg(ir, ins->src2, A64_X14, j);
                    a64_str64_off(j, rs, r_base, ins->offset);
                }
                break;
            }

            case IR_LOAD_ARRAY: {
                int r_base = a64_get_int_reg(ir, ins->src1, A64_X13, j);
                int r_idx = a64_get_int_reg(ir, ins->src2, A64_X14, j);
                if (ins->type == TYPE_F64) {
                    int dd = ir->vregs[ins->dest].is_spilled ? 24 : ir->vregs[ins->dest].preg;
                    a64_ldrd_idx(j, dd, r_base, r_idx);
                    a64_commit_float_reg(ir, ins->dest, dd, j);
                } else if (ins->type == TYPE_F32) {
                    int sd = ir->vregs[ins->dest].is_spilled ? 24 : ir->vregs[ins->dest].preg;
                    a64_ldrs_idx(j, sd, r_base, r_idx);
                    a64_commit_float_reg(ir, ins->dest, sd, j);
                } else if (ins->type == TYPE_I32) {
                    int rd = ir->vregs[ins->dest].is_spilled ? A64_X13 : ir->vregs[ins->dest].preg;
                    a64_ldrsw_idx(j, rd, r_base, r_idx);
                    a64_commit_int_reg(ir, ins->dest, rd, j);
                } else {
                    int rd = ir->vregs[ins->dest].is_spilled ? A64_X13 : ir->vregs[ins->dest].preg;
                    a64_ldr64_idx(j, rd, r_base, r_idx);
                    a64_commit_int_reg(ir, ins->dest, rd, j);
                }
                break;
            }

            case IR_STORE_ARRAY: {
                int r_base = a64_get_int_reg(ir, ins->src1, A64_X13, j);
                int r_idx = a64_get_int_reg(ir, ins->src2, A64_X14, j);
                if (ins->type == TYPE_F64) {
                    int ds = a64_get_float_reg(ir, ins->src3, 24, j);
                    a64_strd_idx(j, ds, r_base, r_idx);
                } else if (ins->type == TYPE_F32) {
                    int ss = a64_get_float_reg(ir, ins->src3, 24, j);
                    a64_strs_idx(j, ss, r_base, r_idx);
                } else if (ins->type == TYPE_I32) {
                    int rs = a64_get_int_reg(ir, ins->src3, A64_X15, j);
                    a64_str32_idx(j, rs, r_base, r_idx);
                } else {
                    int rs = a64_get_int_reg(ir, ins->src3, A64_X15, j);
                    a64_str64_idx(j, rs, r_base, r_idx);
                }
                break;
            }

            case IR_LOAD_DEREF: {
                int r_ptr = a64_get_int_reg(ir, ins->src1, A64_X13, j);
                if (ins->type == TYPE_F64) {
                    int dd = ir->vregs[ins->dest].is_spilled ? 24 : ir->vregs[ins->dest].preg;
                    a64_ldrd_off(j, dd, r_ptr, 0);
                    a64_commit_float_reg(ir, ins->dest, dd, j);
                } else if (ins->type == TYPE_F32) {
                    int sd = ir->vregs[ins->dest].is_spilled ? 24 : ir->vregs[ins->dest].preg;
                    a64_ldrs_off(j, sd, r_ptr, 0);
                    a64_commit_float_reg(ir, ins->dest, sd, j);
                } else if (ins->type == TYPE_I32) {
                    int rd = ir->vregs[ins->dest].is_spilled ? A64_X13 : ir->vregs[ins->dest].preg;
                    a64_ldrsw_off(j, rd, r_ptr, 0);
                    a64_commit_int_reg(ir, ins->dest, rd, j);
                } else {
                    int rd = ir->vregs[ins->dest].is_spilled ? A64_X13 : ir->vregs[ins->dest].preg;
                    a64_ldr64_off(j, rd, r_ptr, 0);
                    a64_commit_int_reg(ir, ins->dest, rd, j);
                }
                break;
            }

            case IR_STORE_DEREF: {
                int r_ptr = a64_get_int_reg(ir, ins->src1, A64_X13, j);
                if (ins->type == TYPE_F64) {
                    int ds = a64_get_float_reg(ir, ins->src2, 24, j);
                    a64_strd_off(j, ds, r_ptr, 0);
                } else if (ins->type == TYPE_F32) {
                    int ss = a64_get_float_reg(ir, ins->src2, 24, j);
                    a64_strs_off(j, ss, r_ptr, 0);
                } else if (ins->type == TYPE_I32) {
                    int rs = a64_get_int_reg(ir, ins->src2, A64_X14, j);
                    a64_str32_off(j, rs, r_ptr, 0);
                } else {
                    int rs = a64_get_int_reg(ir, ins->src2, A64_X14, j);
                    a64_str64_off(j, rs, r_ptr, 0);
                }
                break;
            }

            case IR_PTR_CHECK: {
                A64CallArg cargs[3];
                cargs[0] = (A64CallArg){ A64_ARG_REG, ins->src1, 0, TYPE_I64 };
                cargs[1] = (A64CallArg){ A64_ARG_IMM, VREG_NONE, (int64_t)(uintptr_t)vortex_intern_cstr(ins->file), TYPE_I64 };
                cargs[2] = (A64CallArg){ A64_ARG_IMM, VREG_NONE, (int64_t)ins->line, TYPE_I64 };
                a64_emit_stage_and_call(j, ir, (void*)vortex_safe_check_ptr, cargs, 3, false, false, 3);
                break;
            }

            case IR_BOUNDS_CHECK: {
                A64CallArg cargs[6];
                cargs[0] = (A64CallArg){ A64_ARG_REG, ins->src1, 0, TYPE_I64 };
                cargs[1] = (A64CallArg){ A64_ARG_REG, ins->src2, 0, TYPE_I64 };
                if (ins->src3 != VREG_NONE) cargs[2] = (A64CallArg){ A64_ARG_REG, ins->src3, 0, TYPE_I64 };
                else                        cargs[2] = (A64CallArg){ A64_ARG_IMM, VREG_NONE, ins->known_len, TYPE_I64 };
                cargs[3] = (A64CallArg){ A64_ARG_IMM, VREG_NONE, (int64_t)ins->offset, TYPE_I64 };
                cargs[4] = (A64CallArg){ A64_ARG_IMM, VREG_NONE, (int64_t)(uintptr_t)vortex_intern_cstr(ins->file), TYPE_I64 };
                cargs[5] = (A64CallArg){ A64_ARG_IMM, VREG_NONE, (int64_t)ins->line, TYPE_I64 };
                a64_emit_stage_and_call(j, ir, (void*)vortex_safe_check_bounds, cargs, 6, false, false, 6);
                break;
            }

            case IR_RANGE_CHECK: {
                A64CallArg cargs[7];
                cargs[0] = (A64CallArg){ A64_ARG_REG, ins->src1, 0, TYPE_I64 };
                cargs[1] = (A64CallArg){ A64_ARG_REG, ins->src2, 0, TYPE_I64 };
                cargs[2] = (A64CallArg){ A64_ARG_REG, ins->src3, 0, TYPE_I64 };
                if (ins->src4 != VREG_NONE) cargs[3] = (A64CallArg){ A64_ARG_REG, ins->src4, 0, TYPE_I64 };
                else                        cargs[3] = (A64CallArg){ A64_ARG_IMM, VREG_NONE, ins->known_len, TYPE_I64 };
                cargs[4] = (A64CallArg){ A64_ARG_IMM, VREG_NONE, (int64_t)ins->offset, TYPE_I64 };
                cargs[5] = (A64CallArg){ A64_ARG_IMM, VREG_NONE, (int64_t)(uintptr_t)vortex_intern_cstr(ins->file), TYPE_I64 };
                cargs[6] = (A64CallArg){ A64_ARG_IMM, VREG_NONE, (int64_t)ins->line, TYPE_I64 };
                a64_emit_stage_and_call(j, ir, (void*)vortex_safe_check_range, cargs, 7, false, false, 7);
                break;
            }

            case IR_FFI_CHECK: {
                A64CallArg cargs[7];
                cargs[0] = (A64CallArg){ A64_ARG_REG, ins->src1, 0, TYPE_I64 };
                cargs[1] = (A64CallArg){ A64_ARG_IMM, VREG_NONE, (int64_t)ins->type, TYPE_I64 };
                cargs[2] = (A64CallArg){ A64_ARG_IMM, VREG_NONE, ins->imm, TYPE_I64 };
                cargs[3] = (A64CallArg){ A64_ARG_IMM, VREG_NONE, (int64_t)(uintptr_t)vortex_intern_cstr(ins->func_name[0] ? ins->func_name : "<ffi>"), TYPE_I64 };
                cargs[4] = (A64CallArg){ A64_ARG_IMM, VREG_NONE, (int64_t)ins->offset, TYPE_I64 };
                cargs[5] = (A64CallArg){ A64_ARG_IMM, VREG_NONE, (int64_t)(uintptr_t)vortex_intern_cstr(ins->file), TYPE_I64 };
                cargs[6] = (A64CallArg){ A64_ARG_IMM, VREG_NONE, (int64_t)ins->line, TYPE_I64 };
                a64_emit_stage_and_call(j, ir, (void*)vortex_safe_check_ffi_arg, cargs, 7, false, false, 7);
                break;
            }

            case IR_LABEL: {
                int lbl = (int)ins->imm;
                if (lbl >= 0 && lbl < MAX_LABELS) {
                    label_offsets[lbl] = (int)j->size;
                    for (int p = 0; p < patch_count; p++) {
                        if (patches[p].label_id == lbl) {
                            a64_patch_branch(j, patches[p].patch_pos, j->size, patches[p].is_cond19);
                        }
                    }
                }
                break;
            }

            case IR_JMP: {
                int lbl = (int)ins->imm;
                if (lbl >= 0 && lbl < MAX_LABELS && label_offsets[lbl] >= 0) {
                    int32_t word_off = (int32_t)((int64_t)(label_offsets[lbl] - (int)j->size) / 4);
                    a64_emit32(j, 0x14000000U | ((uint32_t)word_off & 0x03FFFFFFU));
                } else {
                    patches[patch_count++] = (A64JumpPatch){ lbl, j->size, false };
                    a64_emit32(j, 0x14000000U);
                }
                break;
            }

            case IR_JZ:
            case IR_JNZ: {
                int r = a64_get_int_reg(ir, ins->src1, A64_X13, j);
                int lbl = (int)ins->imm;
                uint32_t base_op = (ins->op == IR_JZ) ? 0xB4000000U : 0xB5000000U; /* CBZ / CBNZ Xr */
                if (lbl >= 0 && lbl < MAX_LABELS && label_offsets[lbl] >= 0) {
                    int32_t word_off = (int32_t)((int64_t)(label_offsets[lbl] - (int)j->size) / 4);
                    a64_emit32(j, base_op | (((uint32_t)word_off & 0x7FFFFU) << 5) | ((uint32_t)r & 31U));
                } else {
                    patches[patch_count++] = (A64JumpPatch){ lbl, j->size, true };
                    a64_emit32(j, base_op | ((uint32_t)r & 31U));
                }
                break;
            }

            case IR_CALL: {
                if (ins->func_ptr == (void*)helper_print_f32 ||
                    ins->func_ptr == (void*)vortex_math_sinf ||
                    ins->func_ptr == (void*)vortex_math_cosf ||
                    ins->func_ptr == (void*)vortex_math_sqrtf ||
                    ins->func_ptr == (void*)vortex_math_tanf ||
                    ins->func_ptr == (void*)vortex_math_floorf ||
                    ins->func_ptr == (void*)vortex_math_ceilf ||
                    ins->func_ptr == (void*)vortex_math_fabsf ||
                    ins->func_ptr == (void*)vortex_math_expf ||
                    ins->func_ptr == (void*)vortex_math_logf) {
                    int s = a64_get_float_reg(ir, ins->args[0], 0, j);
                    a64_fmov_ss(j, 0, s);
                    a64_mov_imm64(j, A64_X16, (uint64_t)(uintptr_t)ins->func_ptr);
                    a64_blr(j, A64_X16);
                } else if (ins->func_ptr == (void*)helper_print_f64 ||
                           ins->func_ptr == (void*)vortex_math_sin_f64 ||
                           ins->func_ptr == (void*)vortex_math_cos_f64 ||
                           ins->func_ptr == (void*)vortex_math_sqrt_f64 ||
                           ins->func_ptr == (void*)vortex_math_tan_f64 ||
                           ins->func_ptr == (void*)vortex_math_floor_f64 ||
                           ins->func_ptr == (void*)vortex_math_ceil_f64 ||
                           ins->func_ptr == (void*)vortex_math_fabs_f64 ||
                           ins->func_ptr == (void*)vortex_math_exp_f64 ||
                           ins->func_ptr == (void*)vortex_math_log_f64) {
                    int d = a64_get_float_reg(ir, ins->args[0], 0, j);
                    a64_fmov_dd(j, 0, d);
                    a64_mov_imm64(j, A64_X16, (uint64_t)(uintptr_t)ins->func_ptr);
                    a64_blr(j, A64_X16);
                } else if (ins->func_ptr == (void*)vortex_math_powf ||
                           ins->func_ptr == (void*)vortex_math_atan2f) {
                    int s0 = a64_get_float_reg(ir, ins->args[0], 0, j);
                    int s1 = a64_get_float_reg(ir, ins->args[1], 1, j);
                    if (s0 == 1 && s1 == 0) {
                        a64_fmov_ss(j, 2, 1);
                        a64_fmov_ss(j, 1, 0);
                        a64_fmov_ss(j, 0, 2);
                    } else if (s1 == 0) {
                        a64_fmov_ss(j, 1, 0);
                        if (s0 != 0) a64_fmov_ss(j, 0, s0);
                    } else {
                        if (s0 != 0) a64_fmov_ss(j, 0, s0);
                        if (s1 != 1) a64_fmov_ss(j, 1, s1);
                    }
                    a64_mov_imm64(j, A64_X16, (uint64_t)(uintptr_t)ins->func_ptr);
                    a64_blr(j, A64_X16);
                } else if (ins->func_ptr == (void*)vortex_math_pow_f64 ||
                           ins->func_ptr == (void*)vortex_math_atan2_f64) {
                    int d0 = a64_get_float_reg(ir, ins->args[0], 0, j);
                    int d1 = a64_get_float_reg(ir, ins->args[1], 1, j);
                    if (d0 == 1 && d1 == 0) {
                        a64_fmov_dd(j, 2, 1);
                        a64_fmov_dd(j, 1, 0);
                        a64_fmov_dd(j, 0, 2);
                    } else if (d1 == 0) {
                        a64_fmov_dd(j, 1, 0);
                        if (d0 != 0) a64_fmov_dd(j, 0, d0);
                    } else {
                        if (d0 != 0) a64_fmov_dd(j, 0, d0);
                        if (d1 != 1) a64_fmov_dd(j, 1, d1);
                    }
                    a64_mov_imm64(j, A64_X16, (uint64_t)(uintptr_t)ins->func_ptr);
                    a64_blr(j, A64_X16);
                } else {
                    A64CallArg cargs[MAX_CALL_ARGS];
                    for (int a = 0; a < ins->arg_count; a++) {
                        cargs[a] = (A64CallArg){ A64_ARG_REG, ins->args[a], 0, TYPE_I64 };
                    }
                    a64_emit_stage_and_call(j, ir, ins->func_ptr, cargs, ins->arg_count, false, false, ins->arg_count);
                }
                if (ins->dest != VREG_NONE) {
                    if (ins->type == TYPE_F32 || ins->type == TYPE_F64) a64_commit_float_reg(ir, ins->dest, 0, j);
                    else a64_commit_int_reg(ir, ins->dest, A64_X0, j);
                }
                break;
            }

            case IR_CALL_USER: {
                int num_args = ins->arg_count;
                uint32_t eval_bytes = (num_args <= 8)
                    ? (uint32_t)((num_args * 8 + 15) & ~15)
                    : (uint32_t)(64 + (((num_args - 8) * 8 + 15) & ~15));
                if (eval_bytes > 0) a64_sub_sp_imm(j, 31, 31, eval_bytes);

                for (int a = 0; a < num_args; a++) {
                    if (ir->vregs[ins->args[a]].is_float) {
                        int s = a64_get_float_reg(ir, ins->args[a], 24, j);
                        if (ir->vregs[ins->args[a]].type == TYPE_F64) {
                            a64_fmov_x_from_d(j, A64_X13, s);
                        } else {
                            a64_fmov_w_from_s(j, A64_X13, s);
                        }
                        a64_str64_off(j, A64_X13, 31, (int32_t)(a * 8));
                    } else {
                        int r = a64_get_int_reg(ir, ins->args[a], A64_X13, j);
                        a64_str64_off(j, r, 31, (int32_t)(a * 8));
                    }
                }
                for (int k = 0; k < num_args && k < 8; k++) {
                    a64_ldr64_off(j, k, 31, (int32_t)(k * 8));
                }

                uint32_t pre_release = (num_args <= 8) ? eval_bytes : 64U;
                uint32_t post_release = (num_args <= 8) ? 0U : (eval_bytes - 64U);
                if (pre_release > 0) a64_add_sp_imm(j, 31, 31, pre_release);

                size_t call_pos = j->size;
                a64_emit32(j, 0x94000000U); /* BL #0 placeholder */

                if (post_release > 0) a64_add_sp_imm(j, 31, 31, post_release);

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
                    if (ir->vregs[ins->dest].is_float || ins->type == TYPE_F32 || ins->type == TYPE_F64) {
                        a64_commit_float_reg(ir, ins->dest, 0, j);
                    } else {
                        a64_commit_int_reg(ir, ins->dest, A64_X0, j);
                    }
                }
                break;
            }

            case IR_CALL_FFI: {
                A64CallArg cargs[MAX_CALL_ARGS];
                for (int a = 0; a < ins->arg_count; a++) {
                    cargs[a] = (A64CallArg){ A64_ARG_REG, ins->args[a], 0, ins->arg_types[a] };
                }
                a64_emit_stage_and_call(j, ir, ins->func_ptr, cargs, ins->arg_count,
                                        true, ins->is_variadic, ins->fixed_param_count);

                if (ins->dest != VREG_NONE) {
                    if (ins->ret_type == TYPE_F64) {
                        if (ir->vregs[ins->dest].type != TYPE_F64) {
                            a64_fcvt_s_from_d(j, 0, 0);
                        }
                        a64_commit_float_reg(ir, ins->dest, 0, j);
                    } else if (ins->ret_type == TYPE_F32) {
                        a64_commit_float_reg(ir, ins->dest, 0, j);
                    } else if (ins->ret_type == TYPE_I32) {
                        a64_sxtw(j, A64_X0, A64_X0);
                        a64_commit_int_reg(ir, ins->dest, A64_X0, j);
                    } else if (ins->ret_type != TYPE_VOID) {
                        a64_commit_int_reg(ir, ins->dest, A64_X0, j);
                    }
                }
                break;
            }

            case IR_LOAD_PARAM: {
                int param_idx = (int)ins->imm;
                int32_t off = (param_idx < 8) ? (-208 + 8 * param_idx) : (16 + 8 * (param_idx - 8));
                if (ir->vregs[ins->dest].is_float) {
                    int sd = (ir->vregs[ins->dest].is_spilled || ir->vregs[ins->dest].preg == PREG_NONE)
                           ? 24 : ir->vregs[ins->dest].preg;
                    if (ir->vregs[ins->dest].type == TYPE_F64) a64_ldrd_off(j, sd, A64_FP, off);
                    else a64_ldrs_off(j, sd, A64_FP, off);
                    a64_commit_float_reg(ir, ins->dest, sd, j);
                } else {
                    int rd = (ir->vregs[ins->dest].is_spilled || ir->vregs[ins->dest].preg == PREG_NONE)
                           ? A64_X13 : ir->vregs[ins->dest].preg;
                    a64_ldr64_off(j, rd, A64_FP, off);
                    a64_commit_int_reg(ir, ins->dest, rd, j);
                }
                break;
            }

            case IR_RET: {
                if (ins->src1 != VREG_NONE) {
                    if (ins->type == TYPE_F64) {
                        int d = a64_get_float_reg(ir, ins->src1, 0, j);
                        a64_fmov_dd(j, 0, d);
                    } else if (ins->type == TYPE_F32) {
                        int s = a64_get_float_reg(ir, ins->src1, 0, j);
                        a64_fmov_ss(j, 0, s);
                    } else {
                        int r = a64_get_int_reg(ir, ins->src1, A64_X0, j);
                        a64_mov_rr(j, A64_X0, r);
                    }
                }
                a64_emit_func_epilogue(j, a64_compute_frame_size(ir->total_spill_bytes));
                break;
            }

            case IR_RAW_RIEMANN:
            case IR_VEC_LOOP: {
                emit_neon_vector_loop(j, ir, ins);
                break;
            }
            case IR_SUB_MOD_IMM:
            case IR_ADD_SUB_MOD_IMM:
            case IR_VEC_ARR_OP:
            case IR_VEC_GRID_MOD:
            case IR_VEC_MOD_FILL:
                break;
        }
        if (ins->line > 0 && j->size > ins_start_off) {
            debug_info_record_line(ins_start_off, j->size, ins->line, ins->file ? ins->file : g_current_filename);
        }
    }

    for (int p = 0; p < patch_count; p++) {
        int lbl = patches[p].label_id;
        if (lbl >= 0 && lbl < MAX_LABELS && label_offsets[lbl] >= 0) {
            a64_patch_branch(j, patches[p].patch_pos, (size_t)label_offsets[lbl], patches[p].is_cond19);
        }
    }
    free(label_offsets);
    free(patches);
}

const VortexJITBackend g_backend_arm64 = {
    VORTEX_ARCH_ARM64,
    "arm64-neon",
    a64_compute_frame_size,
    a64_emit_prologue,
    a64_emit_epilogue,
    a64_emit_func_prologue,
    a64_emit_func_epilogue,
    a64_emit_jmp_placeholder,
    a64_patch_jump,
    a64_patch_call_rel,
    ir_codegen_arm64
};
