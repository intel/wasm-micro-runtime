/*
 * Copyright (C) 2019 Intel Corporation.  All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include "bh_common.h"
#include "bh_log.h"
#include "wasm_export.h"
#include "../interpreter/wasm.h"

#if defined(_WIN32) || defined(_WIN32_)
#define strncasecmp _strnicmp
#define strcasecmp _stricmp
#endif

void
wasm_runtime_set_exception(wasm_module_inst_t module, const char *exception);

uint32
wasm_runtime_get_temp_ret(wasm_module_inst_t module);

void
wasm_runtime_set_temp_ret(wasm_module_inst_t module, uint32 temp_ret);

uint32
wasm_runtime_get_llvm_stack(wasm_module_inst_t module);

void
wasm_runtime_set_llvm_stack(wasm_module_inst_t module, uint32 llvm_stack);

uint32
wasm_runtime_module_realloc(wasm_module_inst_t module, uint32 ptr,
                            uint32 size, void **p_native_addr);

#define get_module_inst(exec_env) \
    wasm_runtime_get_module_inst(exec_env)

#define validate_app_addr(offset, size) \
    wasm_runtime_validate_app_addr(module_inst, offset, size)

#define validate_app_str_addr(offset) \
    wasm_runtime_validate_app_str_addr(module_inst, offset)

#define validate_native_addr(addr, size) \
    wasm_runtime_validate_native_addr(module_inst, addr, size)

#define addr_app_to_native(offset) \
    wasm_runtime_addr_app_to_native(module_inst, offset)

#define addr_native_to_app(ptr) \
    wasm_runtime_addr_native_to_app(module_inst, ptr)

#define module_malloc(size, p_native_addr) \
    wasm_runtime_module_malloc(module_inst, size, p_native_addr)

#define module_free(offset) \
    wasm_runtime_module_free(module_inst, offset)

typedef int (*out_func_t)(int c, void *ctx);

enum pad_type {
    PAD_NONE,
    PAD_ZERO_BEFORE,
    PAD_SPACE_BEFORE,
    PAD_SPACE_AFTER,
};

typedef char *_va_list;
#define _INTSIZEOF(n)       \
    (((uint32)sizeof(n) +  3) & (uint32)~3)
#define _va_arg(ap, t)      \
    (*(t*)((ap += _INTSIZEOF(t)) - _INTSIZEOF(t)))

#define CHECK_VA_ARG(ap, t) do {                        \
    if ((uint8*)ap + _INTSIZEOF(t) > native_end_addr)   \
        goto fail;                                      \
} while (0)

/**
 * @brief Output an unsigned int in hex format
 *
 * Output an unsigned int on output installed by platform at init time. Should
 * be able to handle an unsigned int of any size, 32 or 64 bit.
 * @param num Number to output
 *
 * @return N/A
 */
static void
_printf_hex_uint(out_func_t out, void *ctx,
                 const uint64 num, bool is_u64,
                 enum pad_type padding,
                 int min_width)
{
    int shift = sizeof(num) * 8;
    int found_largest_digit = 0;
    int remaining = 16; /* 16 digits max */
    int digits = 0;
    char nibble;

     while (shift >= 4) {
         shift -= 4;
         nibble = (num >> shift) & 0xf;

        if (nibble || found_largest_digit || shift == 0) {
            found_largest_digit = 1;
            nibble = (char)(nibble + (nibble > 9 ? 87 : 48));
            out((int) nibble, ctx);
            digits++;
            continue;
        }

        if (remaining-- <= min_width) {
            if (padding == PAD_ZERO_BEFORE) {
                out('0', ctx);
            } else if (padding == PAD_SPACE_BEFORE) {
                out(' ', ctx);
            }
        }
    }

    if (padding == PAD_SPACE_AFTER) {
        remaining = min_width * 2 - digits;
        while (remaining-- > 0) {
            out(' ', ctx);
        }
    }
}

/**
 * @brief Output an unsigned int in decimal format
 *
 * Output an unsigned int on output installed by platform at init time. Only
 * works with 32-bit values.
 * @param num Number to output
 *
 * @return N/A
 */
static void
_printf_dec_uint(out_func_t out, void *ctx,
                 const uint32 num,
                 enum pad_type padding,
                 int min_width)
{
    uint32 pos = 999999999;
    uint32 remainder = num;
    int found_largest_digit = 0;
    int remaining = 10; /* 10 digits max */
    int digits = 1;

    /* make sure we don't skip if value is zero */
    if (min_width <= 0) {
        min_width = 1;
    }

    while (pos >= 9) {
        if (found_largest_digit || remainder > pos) {
            found_largest_digit = 1;
            out((int) ((remainder / (pos + 1)) + 48), ctx);
            digits++;
        } else if (remaining <= min_width && padding < PAD_SPACE_AFTER) {
            out((int) (padding == PAD_ZERO_BEFORE ? '0' : ' '), ctx);
            digits++;
        }
        remaining--;
        remainder %= (pos + 1);
        pos /= 10;
    }
    out((int) (remainder + 48), ctx);

    if (padding == PAD_SPACE_AFTER) {
        remaining = min_width - digits;
        while (remaining-- > 0) {
            out(' ', ctx);
        }
    }
}

static void
print_err(out_func_t out, void *ctx)
{
    out('E', ctx);
    out('R', ctx);
    out('R', ctx);
}

static bool
_vprintf_wa(out_func_t out, void *ctx, const char *fmt, _va_list ap,
            wasm_module_inst_t module_inst)
{
    int might_format = 0; /* 1 if encountered a '%' */
    enum pad_type padding = PAD_NONE;
    int min_width = -1;
    int long_ctr = 0;
    uint8 *native_end_addr;

    if (!wasm_runtime_get_native_addr_range(module_inst, (uint8*)ap,
                                            NULL, &native_end_addr))
        goto fail;

    /* fmt has already been adjusted if needed */

    while (*fmt) {
        if (!might_format) {
            if (*fmt != '%') {
                out((int) *fmt, ctx);
            }
            else {
                might_format = 1;
                min_width = -1;
                padding = PAD_NONE;
                long_ctr = 0;
            }
        }
        else {
            switch (*fmt) {
            case '-':
                padding = PAD_SPACE_AFTER;
                goto still_might_format;

            case '0':
                if (min_width < 0 && padding == PAD_NONE) {
                    padding = PAD_ZERO_BEFORE;
                    goto still_might_format;
                }
                goto handle_1_to_9;
            case '1':
            case '2':
            case '3':
            case '4':
            case '5':
            case '6':
            case '7':
            case '8':
            case '9':
handle_1_to_9:
                if (min_width < 0) {
                    min_width = *fmt - '0';
                } else {
                    min_width = 10 * min_width + *fmt - '0';
                }

                if (padding == PAD_NONE) {
                    padding = PAD_SPACE_BEFORE;
                }
                goto still_might_format;

            case 'l':
                long_ctr++;
                /* Fall through */
            case 'z':
            case 'h':
                /* FIXME: do nothing for these modifiers */
                goto still_might_format;

            case 'd':
            case 'i': {
                int32 d;

                if (long_ctr < 2) {
                    CHECK_VA_ARG(ap, int32);
                    d = _va_arg(ap, int32);
                }
                else {
                    int64 lld;
                    CHECK_VA_ARG(ap, int64);
                    lld = _va_arg(ap, int64);
                    if (lld > INT32_MAX || lld < INT32_MIN) {
                        print_err(out, ctx);
                        break;
                    }
                    d = (int32)lld;
                }

                if (d < 0) {
                    out((int)'-', ctx);
                    d = -d;
                    min_width--;
                }
                _printf_dec_uint(out, ctx, (uint32)d, padding, min_width);
                break;
            }
            case 'u': {
                uint32 u;

                if (long_ctr < 2) {
                    CHECK_VA_ARG(ap, uint32);
                    u = _va_arg(ap, uint32);
                }
                else {
                    uint64 llu;
                    CHECK_VA_ARG(ap, uint64);
                    llu = _va_arg(ap, uint64);
                    if (llu > INT32_MAX) {
                        print_err(out, ctx);
                        break;
                    }
                    u = (uint32)llu;
                }
                _printf_dec_uint(out, ctx, u, padding, min_width);
                break;
            }
            case 'p':
                out('0', ctx);
                out('x', ctx);
                /* left-pad pointers with zeros */
                padding = PAD_ZERO_BEFORE;
                min_width = 8;
                /* Fall through */
            case 'x':
            case 'X': {
                uint64 x;
                bool is_ptr = (*fmt == 'p') ? true : false;

                if (long_ctr < 2) {
                    CHECK_VA_ARG(ap, uint32);
                    x = _va_arg(ap, uint32);
                } else {
                    CHECK_VA_ARG(ap, uint64);
                    x = _va_arg(ap, uint64);
                }
                _printf_hex_uint(out, ctx, x, !is_ptr, padding, min_width);
                break;
            }

            case 's': {
                char *s;
                char *start;
                uint32 s_offset;

                CHECK_VA_ARG(ap, int32);
                s_offset = _va_arg(ap, uint32);

                if (!validate_app_str_addr(s_offset)) {
                    return false;
                }

                s = start = addr_app_to_native(s_offset);

                while (*s)
                    out((int) (*s++), ctx);

                if (padding == PAD_SPACE_AFTER) {
                    int remaining = min_width - (int32)(s - start);
                    while (remaining-- > 0) {
                        out(' ', ctx);
                    }
                }
                break;
            }

            case 'c': {
                int c;
                CHECK_VA_ARG(ap, int);
                c = _va_arg(ap, int);
                out(c, ctx);
                break;
            }

            case '%': {
                out((int) '%', ctx);
                break;
            }

            case 'f': {
                float64 f64;
                char buf[16], *s;

                /* Make 8-byte aligned */
                ap = (_va_list)(((uintptr_t)ap + 7) & ~(uintptr_t)7);
                CHECK_VA_ARG(ap, float64);
                f64 = _va_arg(ap, float64);
                snprintf(buf, sizeof(buf), "%f", f64);
                s = buf;
                while (*s)
                    out((int) (*s++), ctx);
                break;
            }

            default:
                out((int) '%', ctx);
                out((int) *fmt, ctx);
                break;
            }

            might_format = 0;
        }

still_might_format:
        ++fmt;
    }
    return true;

fail:
    wasm_runtime_set_exception(module_inst, "out of bounds memory access");
    return false;
}

struct str_context {
    char *str;
    uint32 max;
    uint32 count;
};

static int
sprintf_out(int c, struct str_context *ctx)
{
    if (!ctx->str || ctx->count >= ctx->max) {
        ctx->count++;
        return c;
    }

    if (ctx->count == ctx->max - 1) {
        ctx->str[ctx->count++] = '\0';
    } else {
        ctx->str[ctx->count++] = (char)c;
    }

    return c;
}

#ifdef BH_PLATFORM_OPENRTOS
PRIVILEGED_DATA static char print_buf[128] = { 0 };
PRIVILEGED_DATA static int print_buf_size = 0;

static int
printf_out(int c, struct str_context *ctx)
{
    if (c == '\n') {
        print_buf[print_buf_size] = '\0';
        os_printf("%s\n", print_buf);
        print_buf_size = 0;
    }
    else if (print_buf_size >= sizeof(print_buf) - 2) {
        print_buf[print_buf_size++] = (char)c;
        print_buf[print_buf_size] = '\0';
        os_printf("%s\n", print_buf);
        print_buf_size = 0;
    }
    else {
        print_buf[print_buf_size++] = (char)c;
    }
    ctx->count++;
    return c;
}
#else
static int
printf_out(int c, struct str_context *ctx)
{
    os_printf("%c", c);
    ctx->count++;
    return c;
}
#endif

static int
printf_wrapper(wasm_exec_env_t exec_env,
               const char * format, _va_list va_args)
{
    wasm_module_inst_t module_inst = get_module_inst(exec_env);
    struct str_context ctx = { NULL, 0, 0 };

    /* format has been checked by runtime */
    if (!validate_native_addr(va_args, sizeof(int32)))
        return 0;

    if (!_vprintf_wa((out_func_t)printf_out, &ctx, format, va_args, module_inst))
        return 0;

    return (int)ctx.count;
}

static int
sprintf_wrapper(wasm_exec_env_t exec_env,
                char *str, const char *format, _va_list va_args)
{
    wasm_module_inst_t module_inst = get_module_inst(exec_env);
    uint8 *native_end_offset;
    struct str_context ctx;

    /* str and format have been checked by runtime */
    if (!validate_native_addr(va_args, sizeof(uint32)))
        return 0;

    if (!wasm_runtime_get_native_addr_range(module_inst, (uint8*)str,
                                            NULL, &native_end_offset)) {
        wasm_runtime_set_exception(module_inst, "out of bounds memory access");
        return false;
    }

    ctx.str = str;
    ctx.max = (uint32)(native_end_offset - (uint8*)str);
    ctx.count = 0;

    if (!_vprintf_wa((out_func_t)sprintf_out, &ctx, format, va_args, module_inst))
        return 0;

    if (ctx.count < ctx.max) {
        str[ctx.count] = '\0';
    }

    return (int)ctx.count;
}

static int
snprintf_wrapper(wasm_exec_env_t exec_env, char *str, uint32 size,
                 const char *format, _va_list va_args)
{
    wasm_module_inst_t module_inst = get_module_inst(exec_env);
    struct str_context ctx;

    /* str and format have been checked by runtime */
    if (!validate_native_addr(va_args, sizeof(uint32)))
        return 0;

    ctx.str = str;
    ctx.max = size;
    ctx.count = 0;

    if (!_vprintf_wa((out_func_t)sprintf_out, &ctx, format, va_args, module_inst))
        return 0;

    if (ctx.count < ctx.max) {
        str[ctx.count] = '\0';
    }

    return (int)ctx.count;
}

static int
puts_wrapper(wasm_exec_env_t exec_env, const char *str)
{
    return os_printf("%s\n", str);
}

static int
putchar_wrapper(wasm_exec_env_t exec_env, int c)
{
    os_printf("%c", c);
    return 1;
}

static uint32
strdup_wrapper(wasm_exec_env_t exec_env, const char *str)
{
    wasm_module_inst_t module_inst = get_module_inst(exec_env);
    char *str_ret;
    uint32 len;
    uint32 str_ret_offset = 0;

    /* str has been checked by runtime */
    if (str) {
        len = (uint32)strlen(str) + 1;

        str_ret_offset = module_malloc(len, (void**)&str_ret);
        if (str_ret_offset) {
            bh_memcpy_s(str_ret, len, str, len);
        }
    }

    return str_ret_offset;
}

static uint32
_strdup_wrapper(wasm_exec_env_t exec_env, const char *str)
{
    return strdup_wrapper(exec_env, str);
}

static int32
memcmp_wrapper(wasm_exec_env_t exec_env,
               const void *s1, const void *s2, uint32 size)
{
    wasm_module_inst_t module_inst = get_module_inst(exec_env);

    /* s2 has been checked by runtime */
    if (!validate_native_addr((void*)s1, size))
        return 0;

    return memcmp(s1, s2, size);
}

static uint32
memcpy_wrapper(wasm_exec_env_t exec_env,
               void *dst, const void *src, uint32 size)
{
    wasm_module_inst_t module_inst = get_module_inst(exec_env);
    uint32 dst_offset = addr_native_to_app(dst);

    if (size == 0)
        return dst_offset;

    /* src has been checked by runtime */
    if (!validate_native_addr(dst, size))
        return dst_offset;

    bh_memcpy_s(dst, size, src, size);
    return dst_offset;
}

static uint32
memmove_wrapper(wasm_exec_env_t exec_env,
                void *dst, void *src, uint32 size)
{
    wasm_module_inst_t module_inst = get_module_inst(exec_env);
    uint32 dst_offset = addr_native_to_app(dst);

    if (size == 0)
        return dst_offset;

    /* src has been checked by runtime */
    if (!validate_native_addr(dst, size))
        return dst_offset;

    memmove(dst, src, size);
    return dst_offset;
}

static uint32
memset_wrapper(wasm_exec_env_t exec_env,
               void *s, int32 c, uint32 size)
{
    wasm_module_inst_t module_inst = get_module_inst(exec_env);
    uint32 s_offset = addr_native_to_app(s);

    if (!validate_native_addr(s, size))
        return s_offset;

    memset(s, c, size);
    return s_offset;
}

static uint32
strchr_wrapper(wasm_exec_env_t exec_env,
               const char *s, int32 c)
{
    wasm_module_inst_t module_inst = get_module_inst(exec_env);
    char *ret;

    /* s has been checked by runtime */
    ret = strchr(s, c);
    return ret ? addr_native_to_app(ret) : 0;
}

static int32
strcmp_wrapper(wasm_exec_env_t exec_env,
               const char *s1, const char *s2)
{
    /* s1 and s2 have been checked by runtime */
    return strcmp(s1, s2);
}

static int32
strncmp_wrapper(wasm_exec_env_t exec_env,
                const char *s1, const char *s2, uint32 size)
{
    wasm_module_inst_t module_inst = get_module_inst(exec_env);

    /* s2 has been checked by runtime */
    if (!validate_native_addr((void*)s1, size))
        return 0;

    return strncmp(s1, s2, size);
}

static uint32
strcpy_wrapper(wasm_exec_env_t exec_env, char *dst, const char *src)
{
    wasm_module_inst_t module_inst = get_module_inst(exec_env);
    uint32 len = (uint32)strlen(src) + 1;

    /* src has been checked by runtime */
    if (!validate_native_addr(dst, len))
        return 0;

#ifndef BH_PLATFORM_WINDOWS
    strncpy(dst, src, len);
#else
    strncpy_s(dst, len, src, len);
#endif
    return addr_native_to_app(dst);
}

static uint32
strncpy_wrapper(wasm_exec_env_t exec_env,
                char *dst, const char *src, uint32 size)
{
    wasm_module_inst_t module_inst = get_module_inst(exec_env);

    /* src has been checked by runtime */
    if (!validate_native_addr(dst, size))
        return 0;

#ifndef BH_PLATFORM_WINDOWS
    strncpy(dst, src, size);
#else
    strncpy_s(dst, size, src, size);
#endif
    return addr_native_to_app(dst);
}

static uint32
strlen_wrapper(wasm_exec_env_t exec_env, const char *s)
{
    /* s has been checked by runtime */
    return (uint32)strlen(s);
}

static uint32
malloc_wrapper(wasm_exec_env_t exec_env, uint32 size)
{
    wasm_module_inst_t module_inst = get_module_inst(exec_env);
    return module_malloc(size, NULL);
}

static uint32
calloc_wrapper(wasm_exec_env_t exec_env, uint32 nmemb, uint32 size)
{
    wasm_module_inst_t module_inst = get_module_inst(exec_env);
    uint64 total_size = (uint64) nmemb * (uint64) size;
    uint32 ret_offset = 0;
    uint8 *ret_ptr;

    if (total_size >= UINT32_MAX)
        return 0;

    ret_offset = module_malloc((uint32)total_size, (void**)&ret_ptr);
    if (ret_offset) {
        memset(ret_ptr, 0, (uint32)total_size);
    }

    return ret_offset;
}

static uint32
realloc_wrapper(wasm_exec_env_t exec_env, uint32 ptr, uint32 new_size)
{
    wasm_module_inst_t module_inst = get_module_inst(exec_env);

    return wasm_runtime_module_realloc(module_inst, ptr, new_size, NULL);
}

static void
free_wrapper(wasm_exec_env_t exec_env, void *ptr)
{
    wasm_module_inst_t module_inst = get_module_inst(exec_env);

    if (!validate_native_addr(ptr, sizeof(uint32)))
        return;

    module_free(addr_native_to_app(ptr));
}

static int32
atoi_wrapper(wasm_exec_env_t exec_env, const char *s)
{
    /* s has been checked by runtime */
    return atoi(s);
}

static void
exit_wrapper(wasm_exec_env_t exec_env, int32 status)
{
    wasm_module_inst_t module_inst = get_module_inst(exec_env);
    char buf[32];
    snprintf(buf, sizeof(buf), "env.exit(%"PRIi32")", status);
    wasm_runtime_set_exception(module_inst, buf);
}

static int32
strtol_wrapper(wasm_exec_env_t exec_env,
               const char *nptr, char **endptr, int32 base)
{
    wasm_module_inst_t module_inst = get_module_inst(exec_env);
    int32 num = 0;

    /* nptr has been checked by runtime */
    if (!validate_native_addr(endptr, sizeof(uint32)))
        return 0;

    num = (int32)strtol(nptr, endptr, base);
    *(uint32*)endptr = addr_native_to_app(*endptr);

    return num;
}

static uint32
strtoul_wrapper(wasm_exec_env_t exec_env,
                const char *nptr, char **endptr, int32 base)
{
    wasm_module_inst_t module_inst = get_module_inst(exec_env);
    uint32 num = 0;

    /* nptr has been checked by runtime */
    if (!validate_native_addr(endptr, sizeof(uint32)))
        return 0;

    num = (uint32)strtoul(nptr, endptr, base);
    *(uint32 *)endptr = addr_native_to_app(*endptr);

    return num;
}

static uint32
memchr_wrapper(wasm_exec_env_t exec_env,
               const void *s, int32 c, uint32 n)
{
    wasm_module_inst_t module_inst = get_module_inst(exec_env);
    void *res;

    if (!validate_native_addr((void*)s, n))
        return 0;

    res = memchr(s, c, n);
    return addr_native_to_app(res);
}

static int32
strncasecmp_wrapper(wasm_exec_env_t exec_env,
                    const char *s1, const char *s2, uint32 n)
{
    /* s1 and s2 have been checked by runtime */
    return strncasecmp(s1, s2, n);
}

static uint32
strspn_wrapper(wasm_exec_env_t exec_env,
               const char *s, const char *accept)
{
    /* s and accept have been checked by runtime */
    return (uint32)strspn(s, accept);
}

static uint32
strcspn_wrapper(wasm_exec_env_t exec_env,
                const char *s, const char *reject)
{
    /* s and reject have been checked by runtime */
    return (uint32)strcspn(s, reject);
}

static uint32
strstr_wrapper(wasm_exec_env_t exec_env,
               const char *s, const char *find)
{
    wasm_module_inst_t module_inst = get_module_inst(exec_env);
    /* s and find have been checked by runtime */
    char *res = strstr(s, find);
    return addr_native_to_app(res);
}

static int32
isupper_wrapper(wasm_exec_env_t exec_env, int32 c)
{
    return isupper(c);
}

static int32
isalpha_wrapper(wasm_exec_env_t exec_env, int32 c)
{
    return isalpha(c);
}

static int32
isspace_wrapper(wasm_exec_env_t exec_env, int32 c)
{
    return isspace(c);
}

static int32
isgraph_wrapper(wasm_exec_env_t exec_env, int32 c)
{
    return isgraph(c);
}

static int32
isprint_wrapper(wasm_exec_env_t exec_env, int32 c)
{
    return isprint(c);
}

static int32
isdigit_wrapper(wasm_exec_env_t exec_env, int32 c)
{
    return isdigit(c);
}

static int32
isxdigit_wrapper(wasm_exec_env_t exec_env, int32 c)
{
    return isxdigit(c);
}

static int32
tolower_wrapper(wasm_exec_env_t exec_env, int32 c)
{
    return tolower(c);
}

static int32
toupper_wrapper(wasm_exec_env_t exec_env, int32 c)
{
    return toupper(c);
}

static int32
isalnum_wrapper(wasm_exec_env_t exec_env, int32 c)
{
    return isalnum(c);
}

static void
setTempRet0_wrapper(wasm_exec_env_t exec_env, uint32 temp_ret)
{
    wasm_module_inst_t module_inst = get_module_inst(exec_env);
    wasm_runtime_set_temp_ret(module_inst, temp_ret);
}

static uint32
getTempRet0_wrapper(wasm_exec_env_t exec_env)
{
    wasm_module_inst_t module_inst = get_module_inst(exec_env);
    return wasm_runtime_get_temp_ret(module_inst);
}

static uint32
llvm_bswap_i16_wrapper(wasm_exec_env_t exec_env, uint32 data)
{
    return (data & 0xFFFF0000)
           | ((data & 0xFF) << 8)
           | ((data & 0xFF00) >> 8);
}

static uint32
llvm_bswap_i32_wrapper(wasm_exec_env_t exec_env, uint32 data)
{
    return ((data & 0xFF) << 24)
           | ((data & 0xFF00) << 8)
           | ((data & 0xFF0000) >> 8)
           | ((data & 0xFF000000) >> 24);
}

static uint32
bitshift64Lshr_wrapper(wasm_exec_env_t exec_env,
                       uint32 uint64_part0, uint32 uint64_part1,
                       uint32 bits)
{
    wasm_module_inst_t module_inst = get_module_inst(exec_env);
    union {
        uint64 value;
        uint32 parts[2];
    } u;

    u.parts[0] = uint64_part0;
    u.parts[1] = uint64_part1;

    u.value >>= bits;
    /* return low 32bit and save high 32bit to temp ret */
    wasm_runtime_set_temp_ret(module_inst, (uint32) (u.value >> 32));
    return (uint32) u.value;
}

static uint32
bitshift64Shl_wrapper(wasm_exec_env_t exec_env,
                      uint32 int64_part0, uint32 int64_part1,
                      uint32 bits)
{
    wasm_module_inst_t module_inst = get_module_inst(exec_env);
    union {
        int64 value;
        uint32 parts[2];
    } u;

    u.parts[0] = int64_part0;
    u.parts[1] = int64_part1;

    u.value <<= bits;
    /* return low 32bit and save high 32bit to temp ret */
    wasm_runtime_set_temp_ret(module_inst, (uint32) (u.value >> 32));
    return (uint32) u.value;
}

static void
llvm_stackrestore_wrapper(wasm_exec_env_t exec_env, uint32 llvm_stack)
{
    wasm_module_inst_t module_inst = get_module_inst(exec_env);
    os_printf("_llvm_stackrestore called!\n");
    wasm_runtime_set_llvm_stack(module_inst, llvm_stack);
}

static uint32
llvm_stacksave_wrapper(wasm_exec_env_t exec_env)
{
    wasm_module_inst_t module_inst = get_module_inst(exec_env);
    os_printf("_llvm_stacksave called!\n");
    return wasm_runtime_get_llvm_stack(module_inst);
}

static uint32
emscripten_memcpy_big_wrapper(wasm_exec_env_t exec_env,
                              void *dst, const void *src, uint32 size)
{
    wasm_module_inst_t module_inst = get_module_inst(exec_env);
    uint32 dst_offset = addr_native_to_app(dst);

    /* src has been checked by runtime */
    if (!validate_native_addr(dst, size))
        return dst_offset;

    bh_memcpy_s(dst, size, src, size);
    return dst_offset;
}

static void
abort_wrapper(wasm_exec_env_t exec_env, int32 code)
{
    wasm_module_inst_t module_inst = get_module_inst(exec_env);
    char buf[32];
    snprintf(buf, sizeof(buf), "env.abort(%"PRIi32")", code);
    wasm_runtime_set_exception(module_inst, buf);
}

static void
abortStackOverflow_wrapper(wasm_exec_env_t exec_env, int32 code)
{
    wasm_module_inst_t module_inst = get_module_inst(exec_env);
    char buf[32];
    snprintf(buf, sizeof(buf), "env.abortStackOverflow(%"PRIi32")", code);
    wasm_runtime_set_exception(module_inst, buf);
}

static void
nullFunc_X_wrapper(wasm_exec_env_t exec_env, int32 code)
{
    wasm_module_inst_t module_inst = get_module_inst(exec_env);
    char buf[32];
    snprintf(buf, sizeof(buf), "env.nullFunc_X(%"PRIi32")", code);
    wasm_runtime_set_exception(module_inst, buf);
}

static uint32
__cxa_allocate_exception_wrapper(wasm_exec_env_t exec_env,
                                 uint32 thrown_size)
{
    wasm_module_inst_t module_inst = get_module_inst(exec_env);
    uint32 exception = module_malloc(thrown_size, NULL);
    if (!exception)
        return 0;

    return exception;
}

static void
__cxa_begin_catch_wrapper(wasm_exec_env_t exec_env,
                          void *exception_object)
{
}

static void
__cxa_throw_wrapper(wasm_exec_env_t exec_env,
                    void *thrown_exception,
                    void *tinfo,
                    uint32 table_elem_idx)
{
    wasm_module_inst_t module_inst = get_module_inst(exec_env);
    char buf[32];

    snprintf(buf, sizeof(buf), "%s", "exception thrown by stdc++");
    wasm_runtime_set_exception(module_inst, buf);
}

#if WASM_ENABLE_SPEC_TEST != 0
static void
print_wrapper(wasm_exec_env_t exec_env)
{
    os_printf("in specttest.print()\n");

}

static void
print_i32_wrapper(wasm_exec_env_t exec_env, int32 i32)
{
    os_printf("in specttest.print_i32(%d)\n", i32);
}

static void
print_i32_f32_wrapper(wasm_exec_env_t exec_env, int32 i32, float f32)
{
    os_printf("in specttest.print_i32_f32(%d, %f)\n", i32, f32);
}

static void
print_f64_f64_wrapper(wasm_exec_env_t exec_env, double f64_1, double f64_2)
{
    os_printf("in specttest.print_f64_f64(%f, %f)\n", f64_1, f64_2);
}

static void
print_f32_wrapper(wasm_exec_env_t exec_env, float f32)
{
    os_printf("in specttest.print_f32(%f)\n", f32);
}

static void
print_f64_wrapper(wasm_exec_env_t exec_env, double f64)
{
    os_printf("in specttest.print_f64(%f)\n", f64);
}
#endif /* WASM_ENABLE_SPEC_TEST */

#define REG_NATIVE_FUNC(func_name, signature)  \
    { #func_name, func_name##_wrapper, signature, NULL }

static NativeSymbol native_symbols_libc_builtin[] = {
    REG_NATIVE_FUNC(printf, "($*)i"),
    REG_NATIVE_FUNC(sprintf, "($$*)i"),
    REG_NATIVE_FUNC(snprintf, "(*~$*)i"),
    REG_NATIVE_FUNC(puts, "($)i"),
    REG_NATIVE_FUNC(putchar, "(i)i"),
    REG_NATIVE_FUNC(memcmp, "(**~)i"),
    REG_NATIVE_FUNC(memcpy, "(**~)i"),
    REG_NATIVE_FUNC(memmove, "(**~)i"),
    REG_NATIVE_FUNC(memset, "(*ii)i"),
    REG_NATIVE_FUNC(strchr, "($i)i"),
    REG_NATIVE_FUNC(strcmp, "($$)i"),
    REG_NATIVE_FUNC(strcpy, "(*$)i"),
    REG_NATIVE_FUNC(strlen, "($)i"),
    REG_NATIVE_FUNC(strncmp, "(**~)i"),
    REG_NATIVE_FUNC(strncpy, "(**~)i"),
    REG_NATIVE_FUNC(malloc, "(i)i"),
    REG_NATIVE_FUNC(realloc, "(ii)i"),
    REG_NATIVE_FUNC(calloc, "(ii)i"),
    REG_NATIVE_FUNC(strdup, "($)i"),
    /* clang may introduce __strdup */
    REG_NATIVE_FUNC(_strdup, "($)i"),
    REG_NATIVE_FUNC(free, "(*)"),
    REG_NATIVE_FUNC(atoi, "($)i"),
    REG_NATIVE_FUNC(exit, "(i)"),
    REG_NATIVE_FUNC(strtol, "($*i)i"),
    REG_NATIVE_FUNC(strtoul, "($*i)i"),
    REG_NATIVE_FUNC(memchr, "(*ii)i"),
    REG_NATIVE_FUNC(strncasecmp, "($$i)"),
    REG_NATIVE_FUNC(strspn, "($$)i"),
    REG_NATIVE_FUNC(strcspn, "($$)i"),
    REG_NATIVE_FUNC(strstr, "($$)i"),
    REG_NATIVE_FUNC(isupper, "(i)i"),
    REG_NATIVE_FUNC(isalpha, "(i)i"),
    REG_NATIVE_FUNC(isspace, "(i)i"),
    REG_NATIVE_FUNC(isgraph, "(i)i"),
    REG_NATIVE_FUNC(isprint, "(i)i"),
    REG_NATIVE_FUNC(isdigit, "(i)i"),
    REG_NATIVE_FUNC(isxdigit, "(i)i"),
    REG_NATIVE_FUNC(tolower, "(i)i"),
    REG_NATIVE_FUNC(toupper, "(i)i"),
    REG_NATIVE_FUNC(isalnum, "(i)i"),
    REG_NATIVE_FUNC(setTempRet0, "(i)"),
    REG_NATIVE_FUNC(getTempRet0, "()i"),
    REG_NATIVE_FUNC(llvm_bswap_i16, "(i)i"),
    REG_NATIVE_FUNC(llvm_bswap_i32, "(i)i"),
    REG_NATIVE_FUNC(bitshift64Lshr, "(iii)i"),
    REG_NATIVE_FUNC(bitshift64Shl, "(iii)i"),
    REG_NATIVE_FUNC(llvm_stackrestore, "(i)"),
    REG_NATIVE_FUNC(llvm_stacksave, "()i"),
    REG_NATIVE_FUNC(emscripten_memcpy_big, "(**~)i"),
    REG_NATIVE_FUNC(abort, "(i)"),
    REG_NATIVE_FUNC(abortStackOverflow, "(i)"),
    REG_NATIVE_FUNC(nullFunc_X, "(i)"),
    REG_NATIVE_FUNC(__cxa_allocate_exception, "(i)i"),
    REG_NATIVE_FUNC(__cxa_begin_catch, "(*)"),
    REG_NATIVE_FUNC(__cxa_throw, "(**i)"),
};

#if WASM_ENABLE_SPEC_TEST != 0
static NativeSymbol native_symbols_spectest[] = {
    REG_NATIVE_FUNC(print, "()"),
    REG_NATIVE_FUNC(print_i32, "(i)"),
    REG_NATIVE_FUNC(print_i32_f32, "(if)"),
    REG_NATIVE_FUNC(print_f64_f64, "(FF)"),
    REG_NATIVE_FUNC(print_f32, "(f)"),
    REG_NATIVE_FUNC(print_f64, "(F)")
};
#endif

uint32
get_libc_builtin_export_apis(NativeSymbol **p_libc_builtin_apis)
{
    *p_libc_builtin_apis = native_symbols_libc_builtin;
    return sizeof(native_symbols_libc_builtin) / sizeof(NativeSymbol);
}

#if WASM_ENABLE_SPEC_TEST != 0
uint32
get_spectest_export_apis(NativeSymbol **p_libc_builtin_apis)
{
    *p_libc_builtin_apis = native_symbols_spectest;
    return sizeof(native_symbols_spectest) / sizeof(NativeSymbol);
}
#endif

/*************************************
 * Global Variables                  *
 *************************************/

typedef struct WASMNativeGlobalDef {
    const char *module_name;
    const char *global_name;
    uint8 type;
    bool is_mutable;
    WASMValue value;
} WASMNativeGlobalDef;

static WASMNativeGlobalDef native_global_defs[] = {
#if WASM_ENABLE_SPEC_TEST != 0
    { "spectest", "global_i32", VALUE_TYPE_I32, false, .value.i32 = 666 },
    { "spectest", "global_i64", VALUE_TYPE_I64, false, .value.i64 = 666 },
    { "spectest", "global_f32", VALUE_TYPE_F32, false, .value.f32 = 666.6 },
    { "spectest", "global_f64", VALUE_TYPE_F64, false, .value.f64 = 666.6 },
    { "test", "global-i32", VALUE_TYPE_I32, false, .value.i32 = 0 },
    { "test", "global-f32", VALUE_TYPE_F32, false, .value.f32 = 0 },
    { "test", "global-mut-i32", VALUE_TYPE_I32, true, .value.i32 = 0 },
    { "test", "global-mut-i64", VALUE_TYPE_I64, true, .value.i64 = 0 },
#endif
    { "global", "NaN", VALUE_TYPE_F64, .value.u64 = 0x7FF8000000000000LL },
    { "global", "Infinity", VALUE_TYPE_F64, .value.u64 = 0x7FF0000000000000LL }
};

bool
wasm_native_lookup_libc_builtin_global(const char *module_name,
                                       const char *global_name,
                                       WASMGlobalImport *global)
{
    uint32 size = sizeof(native_global_defs) / sizeof(WASMNativeGlobalDef);
    WASMNativeGlobalDef *global_def = native_global_defs;
    WASMNativeGlobalDef *global_def_end = global_def + size;

    if (!module_name || !global_name || !global)
        return false;

    /* Lookup constant globals which can be defined by table */
    while (global_def < global_def_end) {
        if (!strcmp(global_def->module_name, module_name)
            && !strcmp(global_def->global_name, global_name)) {
            global->type = global_def->type;
            global->is_mutable = global_def->is_mutable;
            global->global_data_linked = global_def->value;
            return true;
        }
        global_def++;
    }

    return false;
}

