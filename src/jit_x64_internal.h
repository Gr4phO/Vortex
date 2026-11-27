/*
 * VORTEX X86-64 JIT INTERNAL SHARED DECLARATIONS
 * Shared emitters between jit_x64.c and jit_x64_simd.c
 */

#ifndef VORTEX_JIT_X64_INTERNAL_H
#define VORTEX_JIT_X64_INTERNAL_H

#include "vortex.h"

void jit_x64_emit_u8(JITBuffer* j, uint8_t b);
void jit_x64_emit_u32(JITBuffer* j, uint32_t val);
size_t jit_x64_emit_jcc_placeholder(JITBuffer* j, uint8_t cond_byte);
void jit_x64_emit_push_r(JITBuffer* j, int r);
void jit_x64_emit_mov_rr(JITBuffer* j, int dst, int src);
void jit_x64_emit_add_rr(JITBuffer* j, int dst, int src);
void jit_x64_emit_sub_rr(JITBuffer* j, int dst, int src);
void jit_x64_emit_cmp_rr(JITBuffer* j, int r1, int r2);
void jit_x64_emit_cmp_ri(JITBuffer* j, int r1, int32_t imm);
void jit_x64_emit_test_rr(JITBuffer* j, int r1, int r2);
void jit_x64_emit_mov_ri64(JITBuffer* j, int reg, int64_t val);
void jit_x64_emit_add_ri(JITBuffer* j, int rd, int32_t imm);
void jit_x64_emit_sub_ri(JITBuffer* j, int rd, int32_t imm);
void jit_x64_emit_shl_ri(JITBuffer* j, int rd, int shift);
void jit_x64_emit_lea_sib(JITBuffer* j, int rd, int base, int idx, int scale);
void jit_x64_emit_add_mem_rbp(JITBuffer* j, int32_t disp, int src);
void jit_x64_emit_cmp_reg_rbp(JITBuffer* j, int reg, int32_t disp);
void jit_x64_emit_load_mem_offset(JITBuffer* j, int dst, int base, int32_t offset);
void jit_x64_emit_store_mem_offset(JITBuffer* j, int base, int32_t offset, int src);
void jit_x64_emit_movaps_xx(JITBuffer* j, int dst_xmm, int src_xmm);
void jit_x64_emit_load_f32_imm_to(JITBuffer* j, float val, int xmm_reg);
int jit_x64_get_vreg_int_reg(IRBuffer* ir, VReg v, int scratch_reg, JITBuffer* j);
void jit_x64_commit_vreg_int_reg(IRBuffer* ir, VReg v, int source_reg, JITBuffer* j);
int jit_x64_get_vreg_float_reg(IRBuffer* ir, VReg v, int scratch_xmm, JITBuffer* j);
void jit_x64_commit_vreg_float_reg(IRBuffer* ir, VReg v, int source_xmm, JITBuffer* j);

static inline void emit_vzeroupper(JITBuffer* j) {
    jit_x64_emit_u8(j, 0xC5);
    jit_x64_emit_u8(j, 0xF8);
    jit_x64_emit_u8(j, 0x77);
}

void emit_avx2_vector_loop(JITBuffer* j, IRBuffer* ir, IRInstruction* ins);
void emit_avx2_i64_array_loop(JITBuffer* j, IRBuffer* ir, IRInstruction* ins);
void emit_avx2_grid_mod_loop(JITBuffer* j, IRBuffer* ir, IRInstruction* ins);
void emit_avx2_i64_mod_fill_loop(JITBuffer* j, IRBuffer* ir, IRInstruction* ins);

#define emit_u8                  jit_x64_emit_u8
#define emit_u32                 jit_x64_emit_u32
#define emit_jcc_placeholder     jit_x64_emit_jcc_placeholder
#define emit_push_r              jit_x64_emit_push_r
#define emit_mov_rr              jit_x64_emit_mov_rr
#define emit_add_rr              jit_x64_emit_add_rr
#define emit_sub_rr              jit_x64_emit_sub_rr
#define emit_cmp_rr              jit_x64_emit_cmp_rr
#define emit_cmp_ri              jit_x64_emit_cmp_ri
#define emit_test_rr             jit_x64_emit_test_rr
#define emit_mov_ri64            jit_x64_emit_mov_ri64
#define emit_add_ri              jit_x64_emit_add_ri
#define emit_sub_ri              jit_x64_emit_sub_ri
#define emit_shl_ri              jit_x64_emit_shl_ri
#define emit_lea_sib             jit_x64_emit_lea_sib
#define emit_add_mem_rbp         jit_x64_emit_add_mem_rbp
#define emit_cmp_reg_rbp         jit_x64_emit_cmp_reg_rbp
#define emit_load_mem_offset     jit_x64_emit_load_mem_offset
#define emit_store_mem_offset    jit_x64_emit_store_mem_offset
#define emit_movaps_xx           jit_x64_emit_movaps_xx
#define emit_load_f32_imm_to     jit_x64_emit_load_f32_imm_to
#define get_vreg_int_reg         jit_x64_get_vreg_int_reg
#define commit_vreg_int_reg      jit_x64_commit_vreg_int_reg
#define get_vreg_float_reg       jit_x64_get_vreg_float_reg
#define commit_vreg_float_reg    jit_x64_commit_vreg_float_reg

#endif /* VORTEX_JIT_X64_INTERNAL_H */
