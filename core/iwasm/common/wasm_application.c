/*
 * Copyright (C) 2019 Intel Corporation.  All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include "bh_platform.h"
#if WASM_ENABLE_INTERP != 0
#include "../interpreter/wasm_runtime.h"
#endif
#if WASM_ENABLE_AOT != 0
#include "../aot/aot_runtime.h"
#endif

static void
set_error_buf(char *error_buf, uint32 error_buf_size, const char *string)
{
    if (error_buf != NULL)
        snprintf(error_buf, error_buf_size, "%s", string);
}

static void *
runtime_malloc(uint64 size, WASMModuleInstanceCommon *module_inst,
               char *error_buf, uint32 error_buf_size)
{
    void *mem;

    if (size >= UINT32_MAX
        || !(mem = wasm_runtime_malloc((uint32)size))) {
        if (module_inst != NULL) {
            wasm_runtime_set_exception(module_inst,
                                       "allocate memory failed");
        }
        else if (error_buf != NULL) {
            set_error_buf(error_buf, error_buf_size,
                          "allocate memory failed");
        }
        return NULL;
    }

    memset(mem, 0, (uint32)size);
    return mem;
}

static union {
    int a;
    char b;
} __ue = { .a = 1 };

#define is_little_endian() (__ue.b == 1)

/**
 * Implementation of wasm_application_execute_main()
 */

static WASMFunctionInstanceCommon*
resolve_function(const WASMModuleInstanceCommon *module_inst,
                 const char *name);

static bool
check_main_func_type(const WASMType *type)
{
    if (!(type->param_count == 0 || type->param_count == 2)
        ||type->result_count > 1) {
        LOG_ERROR("WASM execute application failed: invalid main function type.\n");
        return false;
    }

    if (type->param_count == 2
        && !(type->types[0] == VALUE_TYPE_I32
        && type->types[1] == VALUE_TYPE_I32)) {
        LOG_ERROR("WASM execute application failed: invalid main function type.\n");
        return false;
    }

    if (type->result_count
        && type->types[type->param_count] != VALUE_TYPE_I32) {
        LOG_ERROR("WASM execute application failed: invalid main function type.\n");
        return false;
    }

    return true;
}

bool
wasm_application_execute_main(WASMModuleInstanceCommon *module_inst,
                              int32 argc, char *argv[])
{
    WASMFunctionInstanceCommon *func;
    WASMType *func_type = NULL;
    uint32 argc1 = 0, argv1[2] = { 0 };
    uint32 total_argv_size = 0;
    uint64 total_size;
    uint32 argv_buf_offset = 0;
    int32 i;
    char *argv_buf, *p, *p_end;
    uint32 *argv_offsets, module_type;
    bool ret, is_import_func = true;

#if WASM_ENABLE_LIBC_WASI != 0
    if (wasm_runtime_is_wasi_mode(module_inst)) {
        /* In wasi mode, we should call function named "_start"
           which initializes the wasi envrionment and then calls
           the actual main function. Directly call main function
           may cause exception thrown. */
        if ((func = wasm_runtime_lookup_wasi_start_function(module_inst)))
            return wasm_runtime_create_exec_env_and_call_wasm(
                                            module_inst, func, 0, NULL);
        /* if no start function is found, we execute
           the main function as normal */
    }
#endif /* end of WASM_ENABLE_LIBC_WASI */

    if (!(func = resolve_function(module_inst, "main"))
        && !(func = resolve_function(module_inst, "__main_argc_argv"))
        && !(func = resolve_function(module_inst, "_main"))) {
        wasm_runtime_set_exception(module_inst,
                                   "lookup main function failed");
        return false;
    }

#if WASM_ENABLE_INTERP != 0
    if (module_inst->module_type == Wasm_Module_Bytecode) {
        is_import_func = ((WASMFunctionInstance*)func)->is_import_func;
    }
#endif
#if WASM_ENABLE_AOT != 0
    if (module_inst->module_type == Wasm_Module_AoT) {
        is_import_func = ((AOTFunctionInstance*)func)->is_import_func;
    }
#endif

    if (is_import_func) {
        wasm_runtime_set_exception(module_inst,
                                   "lookup main function failed");
        return false;
    }

    module_type = module_inst->module_type;
    func_type = wasm_runtime_get_function_type(func, module_type);

    if (!func_type) {
        LOG_ERROR("invalid module instance type");
        return false;
    }

    if (!check_main_func_type(func_type)) {
        wasm_runtime_set_exception(module_inst,
                                   "invalid function type of main function");
        return false;
    }

    if (func_type->param_count) {
        for (i = 0; i < argc; i++)
            total_argv_size += (uint32)(strlen(argv[i]) + 1);
        total_argv_size = align_uint(total_argv_size, 4);

        total_size = (uint64)total_argv_size + sizeof(int32) * (uint64)argc;

        if (total_size >= UINT32_MAX
            || !(argv_buf_offset =
                    wasm_runtime_module_malloc(module_inst, (uint32)total_size,
                                               (void**)&argv_buf))) {
            wasm_runtime_set_exception(module_inst,
                                       "allocate memory failed");
            return false;
        }

        p = argv_buf;
        argv_offsets = (uint32*)(p + total_argv_size);
        p_end = p + total_size;

        for (i = 0; i < argc; i++) {
            bh_memcpy_s(p, (uint32)(p_end - p), argv[i], (uint32)(strlen(argv[i]) + 1));
            argv_offsets[i] = argv_buf_offset + (uint32)(p - argv_buf);
            p += strlen(argv[i]) + 1;
        }

        argc1 = 2;
        argv1[0] = (uint32)argc;
        argv1[1] = (uint32)wasm_runtime_addr_native_to_app(module_inst, argv_offsets);
    }

    ret = wasm_runtime_create_exec_env_and_call_wasm(module_inst, func,
                                                     argc1, argv1);
    if (ret && func_type->result_count > 0 && argc > 0 && argv)
        /* copy the return value */
        *(int*)argv = (int)argv1[0];

    if (argv_buf_offset)
        wasm_runtime_module_free(module_inst, argv_buf_offset);
    return ret;
}

#if WASM_ENABLE_MULTI_MODULE != 0
static WASMModuleInstance *
get_sub_module_inst(const WASMModuleInstance *parent_module_inst,
                    const char *sub_module_name)
{
    WASMSubModInstNode *node =
      bh_list_first_elem(parent_module_inst->sub_module_inst_list);

    while (node && strcmp(node->module_name, sub_module_name)) {
        node = bh_list_elem_next(node);
    }
    return node ? node->module_inst : NULL;
}

static bool
parse_function_name(char *orig_function_name, char **p_module_name,
                    char **p_function_name)
{
    if (orig_function_name[0] != '$') {
        *p_module_name = NULL;
        *p_function_name = orig_function_name;
        return true;
    }

    /**
     * $module_name$function_name\0
     *  ===>
     * module_name\0function_name\0
     *  ===>
     * module_name
     * function_name
     */
    char *p1 = orig_function_name;
    char *p2 = strchr(p1 + 1, '$');
    if (!p2) {
        LOG_DEBUG("can not parse the incoming function name");
        return false;
    }

    *p_module_name = p1 + 1;
    *p2 = '\0';
    *p_function_name = p2 + 1;
    return strlen(*p_module_name) && strlen(*p_function_name);
}
#endif

/**
 * Implementation of wasm_application_execute_func()
 */

static WASMFunctionInstanceCommon*
resolve_function(const WASMModuleInstanceCommon *module_inst,
                 const char *name)
{
    uint32 i = 0;
    WASMFunctionInstanceCommon *ret = NULL;
#if WASM_ENABLE_MULTI_MODULE != 0
    WASMModuleInstance *sub_module_inst = NULL;
    char *orig_name = NULL;
    char *sub_module_name = NULL;
    char *function_name = NULL;
    uint32 length = (uint32)(strlen(name) + 1);

    orig_name = runtime_malloc(sizeof(char) * length, NULL, NULL, 0);
    if (!orig_name) {
        return NULL;
    }

    strncpy(orig_name, name, length);

    if (!parse_function_name(orig_name, &sub_module_name, &function_name)) {
        goto LEAVE;
    }

    LOG_DEBUG("%s -> %s and %s", name, sub_module_name, function_name);

    if (sub_module_name) {
        sub_module_inst = get_sub_module_inst(
          (WASMModuleInstance *)module_inst, sub_module_name);
        if (!sub_module_inst) {
            LOG_DEBUG("can not find a sub module named %s", sub_module_name);
            goto LEAVE;
        }
    }
#else
    const char *function_name = name;
#endif

#if WASM_ENABLE_INTERP != 0
    if (module_inst->module_type == Wasm_Module_Bytecode) {
        WASMModuleInstance *wasm_inst = (WASMModuleInstance*)module_inst;

#if WASM_ENABLE_MULTI_MODULE != 0
        wasm_inst = sub_module_inst ? sub_module_inst : wasm_inst;
#endif /* WASM_ENABLE_MULTI_MODULE */

        for (i = 0; i < wasm_inst->export_func_count; i++) {
           if (!strcmp(wasm_inst->export_functions[i].name, function_name)) {
                ret = wasm_inst->export_functions[i].function;
                break;
           }
        }
    }
#endif /* WASM_ENABLE_INTERP */

#if WASM_ENABLE_AOT != 0
    if (module_inst->module_type == Wasm_Module_AoT) {
        AOTModuleInstance *aot_inst = (AOTModuleInstance*)module_inst;
        AOTFunctionInstance *export_funcs = (AOTFunctionInstance *)
                                            aot_inst->export_funcs.ptr;
        for (i = 0; i < aot_inst->export_func_count; i++) {
            if (!strcmp(export_funcs[i].func_name, function_name)) {
                ret = &export_funcs[i];
                break;
            }
        }
    }
#endif

#if WASM_ENABLE_MULTI_MODULE != 0
LEAVE:
    wasm_runtime_free(orig_name);
#endif
    return ret;
}

union ieee754_float {
    float f;

    /* This is the IEEE 754 single-precision format.  */
    union {
        struct {
            unsigned int negative:1;
            unsigned int exponent:8;
            unsigned int mantissa:23;
        } ieee_big_endian;
        struct {
            unsigned int mantissa:23;
            unsigned int exponent:8;
            unsigned int negative:1;
        } ieee_little_endian;
    } ieee;
};

union ieee754_double {
    double d;

    /* This is the IEEE 754 double-precision format.  */
    union {
        struct {
            unsigned int negative:1;
            unsigned int exponent:11;
            /* Together these comprise the mantissa.  */
            unsigned int mantissa0:20;
            unsigned int mantissa1:32;
        } ieee_big_endian;

        struct {
            /* Together these comprise the mantissa.  */
            unsigned int mantissa1:32;
            unsigned int mantissa0:20;
            unsigned int exponent:11;
            unsigned int negative:1;
        } ieee_little_endian;
    } ieee;
};

bool
wasm_application_execute_func(WASMModuleInstanceCommon *module_inst,
                              const char *name, int32 argc, char *argv[])
{
    WASMFunctionInstanceCommon *func;
    WASMType *type = NULL;
    uint32 argc1, *argv1 = NULL, cell_num = 0, j, k = 0;
    int32 i, p, module_type;
    uint64 total_size;
    const char *exception;
    char buf[128];

    bh_assert(argc >= 0);
    LOG_DEBUG("call a function \"%s\" with %d arguments", name, argc);
    func = resolve_function(module_inst, name);

    if (!func) {
        snprintf(buf, sizeof(buf), "lookup function %s failed", name);
        wasm_runtime_set_exception(module_inst, buf);
        goto fail;
    }

#if WASM_ENABLE_INTERP != 0
    if (module_inst->module_type == Wasm_Module_Bytecode) {
        WASMFunctionInstance *wasm_func = (WASMFunctionInstance*)func;
        if (wasm_func->is_import_func
#if WASM_ENABLE_MULTI_MODULE != 0
            && !wasm_func->import_func_inst
#endif
        ) {
            snprintf(buf, sizeof(buf), "lookup function %s failed", name);
            wasm_runtime_set_exception(module_inst, buf);
            goto fail;
        }
    }
#endif

    module_type = module_inst->module_type;
    type = wasm_runtime_get_function_type(func, module_type);

    if (!type) {
        LOG_ERROR("invalid module instance type");
        return false;
    }

    if (type->param_count != (uint32)argc) {
        wasm_runtime_set_exception(module_inst,
                                   "invalid input argument count");
        goto fail;
    }

    argc1 = type->param_cell_num;
    cell_num = (argc1 > type->ret_cell_num) ? argc1 : type->ret_cell_num;

    total_size = sizeof(uint32) * (uint64)(cell_num > 2 ? cell_num : 2);
    if ((!(argv1 = runtime_malloc((uint32)total_size, module_inst,
                                  NULL, 0)))) {
        goto fail;
    }

    /* Parse arguments */
    for (i = 0, p = 0; i < argc; i++) {
        char *endptr = NULL;
        bh_assert(argv[i] != NULL);
        if (argv[i][0] == '\0') {
            snprintf(buf, sizeof(buf), "invalid input argument %"PRId32"", i);
            wasm_runtime_set_exception(module_inst, buf);
            goto fail;
        }
        switch (type->types[i]) {
            case VALUE_TYPE_I32:
                argv1[p++] = (uint32)strtoul(argv[i], &endptr, 0);
                break;
            case VALUE_TYPE_I64:
            {
                union { uint64 val; uint32 parts[2]; } u;
                u.val = strtoull(argv[i], &endptr, 0);
                argv1[p++] = u.parts[0];
                argv1[p++] = u.parts[1];
                break;
            }
            case VALUE_TYPE_F32:
            {
                float32 f32 = strtof(argv[i], &endptr);
                if (isnan(f32)) {
                    if (argv[i][0] == '-') {
                        union ieee754_float u;
                        u.f = f32;
                        if (is_little_endian())
                            u.ieee.ieee_little_endian.negative = 1;
                        else
                            u.ieee.ieee_big_endian.negative = 1;
                        memcpy(&f32, &u.f, sizeof(float));
                    }
                    if (endptr[0] == ':') {
                        uint32 sig;
                        union ieee754_float u;
                        sig = (uint32)strtoul(endptr + 1, &endptr, 0);
                        u.f = f32;
                        if (is_little_endian())
                            u.ieee.ieee_little_endian.mantissa = sig;
                        else
                            u.ieee.ieee_big_endian.mantissa = sig;
                        memcpy(&f32, &u.f, sizeof(float));
                    }
                }
                memcpy(&argv1[p++], &f32, sizeof(float));
                break;
            }
            case VALUE_TYPE_F64:
            {
                union { float64 val; uint32 parts[2]; } u;
                u.val = strtod(argv[i], &endptr);
                if (isnan(u.val)) {
                    if (argv[i][0] == '-') {
                        union ieee754_double ud;
                        ud.d = u.val;
                        if (is_little_endian())
                            ud.ieee.ieee_little_endian.negative = 1;
                        else
                            ud.ieee.ieee_big_endian.negative = 1;
                        memcpy(&u.val, &ud.d, sizeof(double));
                    }
                    if (endptr[0] == ':') {
                        uint64 sig;
                        union ieee754_double ud;
                        sig = strtoull(endptr + 1, &endptr, 0);
                        ud.d = u.val;
                        if (is_little_endian()) {
                            ud.ieee.ieee_little_endian.mantissa0 = sig >> 32;
                            ud.ieee.ieee_little_endian.mantissa1 = (uint32)sig;
                        }
                        else {
                            ud.ieee.ieee_big_endian.mantissa0 = sig >> 32;
                            ud.ieee.ieee_big_endian.mantissa1 = (uint32)sig;
                        }
                        memcpy(&u.val, &ud.d, sizeof(double));
                    }
                }
                argv1[p++] = u.parts[0];
                argv1[p++] = u.parts[1];
                break;
            }
#if WASM_ENABLE_SIMD != 0
            case VALUE_TYPE_V128:
            {
                /* it likes 0x123\0x234 or 123\234 */
                /* retrive first i64 */
                *(uint64*)(argv1 + p) = strtoull(argv[i], &endptr, 0);
                /* skip \ */
                endptr++;
                /* retrive second i64 */
                *(uint64*)(argv1 + p + 2) = strtoull(endptr, &endptr, 0);
                p += 4;
                break;
            }
#endif /* WASM_ENABLE_SIMD != 0 */
#if WASM_ENABLE_REF_TYPES != 0
            case VALUE_TYPE_FUNCREF:
            {
                if (strncmp(argv[i], "null", 4) == 0
                    || strncmp(argv[i], "NULL", 4) == 0) {
                    argv1[p++] = NULL_REF;
                }
                else {
                    argv1[p++] = (uint32)strtoul(argv[i], &endptr, 0);
                }
                break;
            }
            case VALUE_TYPE_EXTERNREF:
            {
                if (strncmp(argv[i], "null", 4) == 0
                    || strncmp(argv[i], "NULL", 4) == 0) {
                    argv1[p++] = NULL_REF;
                }
                else {
                    uint64 val = strtoull(argv[i], &endptr, 0);
                    void *extern_obj = (void *)(uintptr_t)val;
                    uint32 externref_idx;

                    if (!wasm_externref_obj2ref(module_inst, extern_obj,
                                                &externref_idx)) {
                        wasm_runtime_set_exception(
                          module_inst, "map extern object to ref failed");
                        goto fail;
                    }
                    argv1[p++] = externref_idx;
                }
                break;
            }
#endif /* WASM_ENABLE_REF_TYPES */
            default:
                bh_assert(0);
                break;
        }
        if (endptr && *endptr != '\0' && *endptr != '_') {
            snprintf(buf, sizeof(buf), "invalid input argument %"PRId32": %s",
                     i, argv[i]);
            wasm_runtime_set_exception(module_inst, buf);
            goto fail;
        }
    }
    bh_assert(p == (int32)argc1);

    wasm_runtime_set_exception(module_inst, NULL);
    if (!wasm_runtime_create_exec_env_and_call_wasm(module_inst, func,
                                                    argc1, argv1)) {
        goto fail;
    }

    /* print return value */
    for (j = 0; j < type->result_count; j++) {
        switch (type->types[type->param_count + j]) {
            case VALUE_TYPE_I32:
            {
                os_printf("0x%"PRIx32":i32", argv1[k]);
                k++;
                break;
            }
            case VALUE_TYPE_I64:
            {
                union { uint64 val; uint32 parts[2]; } u;
                u.parts[0] = argv1[k];
                u.parts[1] = argv1[k + 1];
                k += 2;
#ifdef PRIx64
                os_printf("0x%"PRIx64":i64", u.val);
#else
                char buf[16];
                if (sizeof(long) == 4)
                    snprintf(buf, sizeof(buf), "%s", "0x%llx:i64");
                else
                    snprintf(buf, sizeof(buf), "%s", "0x%lx:i64");
                os_printf(buf, u.val);
#endif
                break;
            }
            case VALUE_TYPE_F32:
            {
                os_printf("%.7g:f32", *(float32*)(argv1 + k));
                k++;
                break;
            }
            case VALUE_TYPE_F64:
            {
                union { float64 val; uint32 parts[2]; } u;
                u.parts[0] = argv1[k];
                u.parts[1] = argv1[k + 1];
                k += 2;
                os_printf("%.7g:f64", u.val);
                break;
            }
#if WASM_ENABLE_REF_TYPES
            case VALUE_TYPE_FUNCREF:
            {
                if (argv1[k] != NULL_REF)
                    os_printf("%u:ref.func", argv1[k]);
                else
                    os_printf("func:ref.null");
                k++;
                break;
            }
            case VALUE_TYPE_EXTERNREF:
            {
                if (argv1[k] != NULL_REF) {
                    void *extern_obj = NULL;
                    bool ret = wasm_externref_ref2obj(argv1[k], &extern_obj);
                    bh_assert(ret);
                    (void)ret;
                    os_printf("%p:ref.extern", extern_obj);
                }
                else
                    os_printf("extern:ref.null");
                k++;
                break;
            }
#endif
#if WASM_ENABLE_SIMD != 0
            case VALUE_TYPE_V128:
            {
                uint64 *v = (uint64*)(argv1 + k);
#if defined(PRIx64)
                os_printf("<0x%016"PRIx64" 0x%016"PRIx64">:v128", *v, *(v + 1));
#else
                if (4 == sizeof(long)) {
                    os_printf("<0x%016llx 0x%016llx>:v128", *v, *(v + 1));
                }
                else {
                    os_printf("<0x%016lx 0x%016lx>:v128", *v, *(v + 1));
                }
#endif /* PRIx64 */
                k += 4;
                break;
            }
#endif /*  WASM_ENABLE_SIMD != 0 */
            default:
                bh_assert(0);
                break;
        }
        if (j < (uint32)(type->result_count - 1))
            os_printf(",");
    }
    os_printf("\n");

    wasm_runtime_free(argv1);
    return true;

fail:
    if (argv1)
        wasm_runtime_free(argv1);

    exception = wasm_runtime_get_exception(module_inst);
    bh_assert(exception);
    os_printf("%s\n", exception);
    return false;
}
