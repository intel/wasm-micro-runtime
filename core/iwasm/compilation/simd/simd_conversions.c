/*
 * Copyright (C) 2019 Intel Corporation. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include "simd_conversions.h"
#include "simd_common.h"
#include "../aot_emit_exception.h"
#include "../aot_emit_numberic.h"
#include "../../aot/aot_runtime.h"

static bool
simd_integer_narrow(AOTCompContext *comp_ctx,
                    AOTFuncContext *func_ctx,
                    bool is_signed,
                    LLVMTypeRef in_vector_type,
                    LLVMTypeRef out_vector_type,
                    const char *instrinsic)
{
    LLVMValueRef vector1, vector2, result;
    LLVMTypeRef param_types[2] = { in_vector_type, in_vector_type };

    if (!(vector2 = simd_pop_v128_and_bitcast(comp_ctx, func_ctx,
                                              in_vector_type, "vec2"))) {
        goto fail;
    }

    if (!(vector1 = simd_pop_v128_and_bitcast(comp_ctx, func_ctx,
                                              in_vector_type, "vec1"))) {
        goto fail;
    }

    if (!(result =
            aot_call_llvm_intrinsic(comp_ctx, instrinsic, out_vector_type,
                                    param_types, 2, vector1, vector2))) {
        HANDLE_FAILURE("LLVMBuildCall");
        goto fail;
    }

    if (!(result = LLVMBuildBitCast(comp_ctx->builder, result, V128_i64x2_TYPE,
                                    "ret"))) {
        HANDLE_FAILURE("LLVMBuildBitCast");
        goto fail;
    }

    PUSH_V128(result);
    return true;
fail:
    return false;
}

bool
aot_compile_simd_i8x16_narrow_i16x8(AOTCompContext *comp_ctx,
                                    AOTFuncContext *func_ctx,
                                    bool is_signed)
{
    return simd_integer_narrow(
      comp_ctx, func_ctx, is_signed, V128_i16x8_TYPE, V128_i8x16_TYPE,
      is_signed ? "llvm.x86.sse2.packsswb.128" : "llvm.x86.sse2.packuswb.128");
}

bool
aot_compile_simd_i16x8_narrow_i32x4(AOTCompContext *comp_ctx,
                                    AOTFuncContext *func_ctx,
                                    bool is_signed)
{
    return simd_integer_narrow(
      comp_ctx, func_ctx, is_signed, V128_i32x4_TYPE, V128_i16x8_TYPE,
      is_signed ? "llvm.x86.sse2.packssdw.128" : "llvm.x86.sse41.packusdw");
}

bool
aot_compile_simd_i16x8_widen_i8x16(AOTCompContext *comp_ctx,
                                   AOTFuncContext *func_ctx,
                                   bool is_low_half,
                                   bool is_signed)
{
    LLVMValueRef vector, undef, mask_high[8], mask_low[8], mask, shuffled,
      result;
    uint8 mask_high_value[8] = { 0x8, 0x9, 0xa, 0xb, 0xc, 0xd, 0xe, 0xf },
          mask_low_value[8] = { 0x0, 0x1, 0x2, 0x3, 0x4, 0x5, 0x6, 0x7 }, i;

    if (!(vector = simd_pop_v128_and_bitcast(comp_ctx, func_ctx,
                                             V128_i8x16_TYPE, "vec"))) {
        goto fail;
    }

    if (!(undef = LLVMGetUndef(V128_i8x16_TYPE))) {
        HANDLE_FAILURE("LLVMGetUndef");
        goto fail;
    }

    /* create a mask */
    for (i = 0; i < 8; i++) {
        mask_high[i] = LLVMConstInt(I32_TYPE, mask_high_value[i], true);
        mask_low[i] = LLVMConstInt(I32_TYPE, mask_low_value[i], true);
    }

    mask = is_low_half ? LLVMConstVector(mask_low, 8)
                       : LLVMConstVector(mask_high, 8);
    if (!mask) {
        HANDLE_FAILURE("LLVMConstVector");
        goto fail;
    }

    /* retrive the low or high half */
    if (!(shuffled = LLVMBuildShuffleVector(comp_ctx->builder, vector, undef,
                                            mask, "shuffled"))) {
        HANDLE_FAILURE("LLVMBuildShuffleVector");
        goto fail;
    }

    if (is_signed) {
        if (!(result = LLVMBuildSExt(comp_ctx->builder, shuffled,
                                     V128_i16x8_TYPE, "ext"))) {
            HANDLE_FAILURE("LLVMBuildSExt");
            goto fail;
        }
    }
    else {
        if (!(result = LLVMBuildZExt(comp_ctx->builder, shuffled,
                                     V128_i16x8_TYPE, "ext"))) {
            HANDLE_FAILURE("LLVMBuildZExt");
            goto fail;
        }
    }

    if (!(result = LLVMBuildBitCast(comp_ctx->builder, result, V128_i64x2_TYPE,
                                    "ret"))) {
        HANDLE_FAILURE("LLVMBuildBitCast");
        goto fail;
    }

    PUSH_V128(result);
    return true;
fail:
    return false;
}

bool
aot_compile_simd_i32x4_widen_i16x8(AOTCompContext *comp_ctx,
                                   AOTFuncContext *func_ctx,
                                   bool is_low_half,
                                   bool is_signed)
{
    LLVMValueRef vector, undef, mask_high[4], mask_low[4], mask, shuffled,
      result;
    uint8 mask_high_value[4] = { 0x4, 0x5, 0x6, 0x7 },
          mask_low_value[4] = { 0x0, 0x1, 0x2, 0x3 }, i;

    if (!(vector = simd_pop_v128_and_bitcast(comp_ctx, func_ctx,
                                             V128_i16x8_TYPE, "vec"))) {
        goto fail;
    }

    if (!(undef = LLVMGetUndef(V128_i16x8_TYPE))) {
        HANDLE_FAILURE("LLVMGetUndef");
        goto fail;
    }

    /* create a mask */
    for (i = 0; i < 4; i++) {
        mask_high[i] = LLVMConstInt(I32_TYPE, mask_high_value[i], true);
        mask_low[i] = LLVMConstInt(I32_TYPE, mask_low_value[i], true);
    }

    mask = is_low_half ? LLVMConstVector(mask_low, 4)
                       : LLVMConstVector(mask_high, 4);
    if (!mask) {
        HANDLE_FAILURE("LLVMConstVector");
        goto fail;
    }

    /* retrive the low or high half */
    if (!(shuffled = LLVMBuildShuffleVector(comp_ctx->builder, vector, undef,
                                            mask, "shuffled"))) {
        HANDLE_FAILURE("LLVMBuildShuffleVector");
        goto fail;
    }

    if (is_signed) {
        if (!(result = LLVMBuildSExt(comp_ctx->builder, shuffled,
                                     V128_i32x4_TYPE, "ext"))) {
            HANDLE_FAILURE("LLVMBuildSExt");
            goto fail;
        }
    }
    else {
        if (!(result = LLVMBuildZExt(comp_ctx->builder, shuffled,
                                     V128_i32x4_TYPE, "ext"))) {
            HANDLE_FAILURE("LLVMBuildZExt");
            goto fail;
        }
    }

    if (!(result = LLVMBuildBitCast(comp_ctx->builder, result, V128_i64x2_TYPE,
                                    "ret"))) {
        HANDLE_FAILURE("LLVMBuildBitCast");
        goto fail;
    }

    PUSH_V128(result);
    return true;
fail:
    return false;
}

static LLVMValueRef
simd_build_const_f32x4(AOTCompContext *comp_ctx,
                       AOTFuncContext *func_ctx,
                       float f)
{
    LLVMValueRef elements[4], vector;

    if (!(elements[0] = LLVMConstReal(F32_TYPE, f))) {
        HANDLE_FAILURE("LLVMConstInt");
        goto fail;
    }

    elements[1] = elements[2] = elements[3] = elements[0];

    if (!(vector = LLVMConstVector(elements, 4))) {
        HANDLE_FAILURE("LLVMConstVector");
        goto fail;
    }

    return vector;
fail:
    return NULL;
}

static LLVMValueRef
simd_build_const_i32x4(AOTCompContext *comp_ctx,
                       AOTFuncContext *func_ctx,
                       uint64 integer,
                       bool is_signed)
{
    LLVMValueRef elements[4], vector;

    if (!(elements[0] = LLVMConstInt(I32_TYPE, integer, is_signed))) {
        HANDLE_FAILURE("LLVMConstInt");
        goto fail;
    }

    elements[1] = elements[2] = elements[3] = elements[0];

    if (!(vector = LLVMConstVector(elements, 4))) {
        HANDLE_FAILURE("LLVMConstVector");
        goto fail;
    }

    return vector;
fail:
    return NULL;
}

bool
aot_compile_simd_i32x4_trunc_sat_f32x4(AOTCompContext *comp_ctx,
                                       AOTFuncContext *func_ctx,
                                       bool is_signed)
{
    LLVMValueRef vector, zeros, is_nan, max_float_v, min_float_v, is_ge_max,
      is_le_min, result, max_int_v, min_int_v;
    uint32 max_ui = 0xFFffFFff, min_ui = 0x0;
    int32 max_si = 0x7FFFffff, min_si = 0x80000000;
    float max_f_ui = 4294967296.0f, min_f_ui = 0.0f, max_f_si = 2147483647.0f,
          min_f_si = -2147483648.0f;

    if (!(vector = simd_pop_v128_and_bitcast(comp_ctx, func_ctx,
                                             V128_f32x4_TYPE, "vec"))) {
        goto fail;
    }

    if (!(zeros = LLVMConstNull(V128_f32x4_TYPE))) {
        HANDLE_FAILURE("LLVMConstNull");
        goto fail;
    }

    if (is_signed) {
        if (!(max_float_v =
                simd_build_const_f32x4(comp_ctx, func_ctx, max_f_si))) {
            goto fail;
        }

        if (!(min_float_v =
                simd_build_const_f32x4(comp_ctx, func_ctx, min_f_si))) {
            goto fail;
        }

        if (!(max_int_v =
                simd_build_const_i32x4(comp_ctx, func_ctx, max_si, true))) {
            goto fail;
        }

        if (!(min_int_v =
                simd_build_const_i32x4(comp_ctx, func_ctx, min_si, true))) {
            goto fail;
        }
    }
    else {
        if (!(max_float_v =
                simd_build_const_f32x4(comp_ctx, func_ctx, max_f_ui))) {
            goto fail;
        }

        if (!(min_float_v =
                simd_build_const_f32x4(comp_ctx, func_ctx, min_f_ui))) {
            goto fail;
        }

        if (!(max_int_v =
                simd_build_const_i32x4(comp_ctx, func_ctx, max_ui, false))) {
            goto fail;
        }

        if (!(min_int_v =
                simd_build_const_i32x4(comp_ctx, func_ctx, min_ui, false))) {
            goto fail;
        }
    }

    if (!(is_nan = LLVMBuildFCmp(comp_ctx->builder, LLVMRealORD, vector, zeros,
                                 "is_nan"))) {
        HANDLE_FAILURE("LLVMBuildFCmp");
        goto fail;
    }

    if (!(is_le_min = LLVMBuildFCmp(comp_ctx->builder, LLVMRealOLE, vector,
                                    min_float_v, "le_min"))) {
        HANDLE_FAILURE("LLVMBuildFCmp");
        goto fail;
    }

    if (!(is_ge_max = LLVMBuildFCmp(comp_ctx->builder, LLVMRealOGE, vector,
                                    max_float_v, "ge_max"))) {
        HANDLE_FAILURE("LLVMBuildFCmp");
        goto fail;
    }

    if (is_signed) {
        if (!(result = LLVMBuildFPToSI(comp_ctx->builder, vector,
                                       V128_i32x4_TYPE, "truncated"))) {
            HANDLE_FAILURE("LLVMBuildSIToFP");
            goto fail;
        }
    }
    else {
        if (!(result = LLVMBuildFPToUI(comp_ctx->builder, vector,
                                       V128_i32x4_TYPE, "truncated"))) {
            HANDLE_FAILURE("LLVMBuildUIToFP");
            goto fail;
        }
    }

    if (!(result = LLVMBuildSelect(comp_ctx->builder, is_ge_max, max_int_v,
                                   result, "sat_w_max"))) {
        HANDLE_FAILURE("LLVMBuildSelect");
        goto fail;
    }

    if (!(result = LLVMBuildSelect(comp_ctx->builder, is_le_min, min_int_v,
                                   result, "sat_w_min"))) {
        HANDLE_FAILURE("LLVMBuildSelect");
        goto fail;
    }

    if (!(result = LLVMBuildSelect(comp_ctx->builder, is_nan, result,
                                   V128_i32x4_ZERO, "sat_w_nan"))) {
        HANDLE_FAILURE("LLVMBuildSelect");
        goto fail;
    }

    if (!(result = LLVMBuildBitCast(comp_ctx->builder, result, V128_i64x2_TYPE,
                                    "ret"))) {
        HANDLE_FAILURE("LLVMBuildBitCast");
        goto fail;
    }

    PUSH_V128(result);
    return true;
fail:
    return false;
}

bool
aot_compile_simd_f32x4_convert_i32x4(AOTCompContext *comp_ctx,
                                     AOTFuncContext *func_ctx,
                                     bool is_signed)
{
    LLVMValueRef vector, result;

    if (!(vector = simd_pop_v128_and_bitcast(comp_ctx, func_ctx,
                                             V128_i32x4_TYPE, "vec"))) {
        goto fail;
    }

    if (is_signed) {
        if (!(result = LLVMBuildSIToFP(comp_ctx->builder, vector,
                                       V128_f32x4_TYPE, "converted"))) {
            HANDLE_FAILURE("LLVMBuildSIToFP");
            goto fail;
        }
    }
    else {
        if (!(result = LLVMBuildUIToFP(comp_ctx->builder, vector,
                                       V128_f32x4_TYPE, "converted"))) {
            HANDLE_FAILURE("LLVMBuildSIToFP");
            goto fail;
        }
    }

    if (!(result = LLVMBuildBitCast(comp_ctx->builder, result, V128_i64x2_TYPE,
                                    "ret"))) {
        HANDLE_FAILURE("LLVMBuildBitCast");
        goto fail;
    }

    PUSH_V128(result);
    return true;
fail:
    return false;
}
