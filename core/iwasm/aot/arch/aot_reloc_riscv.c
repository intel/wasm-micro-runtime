/*
 * Copyright (C) 2021 XiaoMi Corporation. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include "aot_reloc.h"

#define R_RISCV_CALL     18
#define R_RISCV_CALL_PLT 19
#define R_RISCV_HI20     26
#define R_RISCV_LO12_I   27
#define R_RISCV_LO12_S   28

#define RV_OPCODE_SW 0x23

static SymbolMap target_sym_map[] = { REG_COMMON_SYMBOLS };

void
get_current_target(char *target_buf, uint32 target_buf_size)
{
    snprintf(target_buf, target_buf_size, "riscv");
}

uint32
get_plt_item_size()
{
    return 16;
}

SymbolMap *
get_target_symbol_map(uint32 *sym_num)
{
    *sym_num = sizeof(target_sym_map) / sizeof(SymbolMap);
    return target_sym_map;
}

/* Get a val from given address */
static uint32
rv_get_val(uint16_t *addr)
{
    uint32_t ret;
    ret = *addr | (*(addr + 1)) << 16;
    return ret;
}

/* Set a val to given address */
static void
rv_set_val(uint16 *addr, uint32_t val)
{
    *addr = (val & 0xffff);
    *(addr + 1) = (val >> 16);

    asm volatile("fence.i");
}

/* Add a val to given address */
static void
rv_add_val(uint16 *addr, uint32_t val)
{
    uint32_t cur = rv_get_val(addr);
    rv_set_val(addr, cur + val);
}

/**
 * Get imm_hi and imm_lo from given integer
 *
 * @param long given integer, signed 32bit
 * @param imm_hi signed 20bit
 * @param imm_lo signed 12bit
 *
 */
static void
rv_calc_imm(long offset, long *imm_hi, long *imm_lo)
{
    long lo;
    long hi = offset / 4096;
    long r = offset % 4096;

    if (2047 < r) {
        hi++;
    }
    else if (r < -2048) {
        hi--;
    }

    lo = offset - (hi * 4096);

    *imm_lo = lo;
    *imm_hi = hi;
}

uint32
get_plt_table_size()
{
    return get_plt_item_size() * (sizeof(target_sym_map) / sizeof(SymbolMap));
}

void
init_plt_table(uint8 *plt)
{}

bool
apply_relocation(AOTModule *module,
                 uint8 *target_section_addr,
                 uint32 target_section_size,
                 uint64 reloc_offset,
                 uint64 reloc_addend,
                 uint32 reloc_type,
                 void *symbol_addr,
                 int32 symbol_index,
                 char *error_buf,
                 uint32 error_buf_size)
{
    long imm_hi;
    long imm_lo;
    uint8 *addr = target_section_addr + reloc_offset;

    switch (reloc_type) {
        case R_RISCV_CALL:
        case R_RISCV_CALL_PLT:
        {
            long offset = (uint8 *)symbol_addr - addr;
            rv_calc_imm(offset, &imm_hi, &imm_lo);

            rv_add_val((uint16 *)addr, (imm_hi << 12));
            if ((rv_get_val((uint16 *)(addr + 4)) & 0x7f) == RV_OPCODE_SW) {
                /* Adjust imm for SW : S-type */

                uint32 val = (((int32)imm_lo >> 5) << 25)
                               + (((int32)imm_lo & 0x1f) << 7);

                rv_add_val((uint16 *)(addr + 4), val);
            }
            else {
                /* Adjust imm for MV(ADDI)/JALR : I-type */

                rv_add_val((uint16 *)(addr + 4), ((int32)imm_lo << 20));
            }
        } break;

        case R_RISCV_HI20:
        {
            long offset = (long)symbol_addr;
            uint8 *addr = target_section_addr + reloc_offset;
            uint32 insn = rv_get_val((uint16 *)addr);
            rv_calc_imm(offset, &imm_hi, &imm_lo);
            insn = (insn & 0x00000fff) | (imm_hi << 12);
            rv_set_val((uint16*)addr, insn);
        } break;

        case R_RISCV_LO12_I:
        {
            long offset = (long)symbol_addr;
            uint8 *addr = target_section_addr + reloc_offset;
            uint32 insn = rv_get_val((uint16 *)addr);
            rv_calc_imm(offset, &imm_hi, &imm_lo);
            insn = (insn & 0x000fffff) | (imm_lo << 20);
            rv_set_val((uint16*)addr, insn);
        } break;

        case R_RISCV_LO12_S:
        {
            long offset = (long)symbol_addr;
            uint8 *addr = target_section_addr + reloc_offset;
            rv_calc_imm(offset, &imm_hi, &imm_lo);
            uint32 val =
              (((int32)imm_lo >> 5) << 25) +
              (((int32)imm_lo & 0x1f) << 7);
            rv_add_val((uint16*)addr, val);
        } break;

        default:
            if (error_buf != NULL)
                snprintf(error_buf, error_buf_size,
                         "Load relocation section failed: "
                         "invalid relocation type %ld.",
                         reloc_type);
            return false;
    }
    return true;
}
