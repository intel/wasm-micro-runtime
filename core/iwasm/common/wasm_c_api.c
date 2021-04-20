/*
 * Copyright (C) 2019 Intel Corporation.  All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include "wasm_c_api_internal.h"
#include "wasm_memory.h"
#if WASM_ENABLE_INTERP != 0
#include "wasm_runtime.h"
#endif
#if WASM_ENABLE_AOT != 0
#include "aot_runtime.h"
#endif

#include <pthread.h>

#define ASSERT_NOT_IMPLEMENTED() bh_assert(!"not implemented")

typedef struct wasm_module_ex_t wasm_module_ex_t;

static void
wasm_module_delete_internal(wasm_module_t *);

static void
wasm_instance_delete_internal(wasm_instance_t *);

/* temporarily put stubs here */
static wasm_store_t *
wasm_store_copy(const wasm_store_t *src)
{
    (void)src;
    LOG_WARNING("in the stub of %s", __FUNCTION__);
    return NULL;
}

wasm_module_t *
wasm_module_copy(const wasm_module_t *src)
{
    (void)src;
    LOG_WARNING("in the stub of %s", __FUNCTION__);
    return NULL;
}

wasm_instance_t *
wasm_instance_copy(const wasm_instance_t *src)
{
    (void)src;
    LOG_WARNING("in the stub of %s", __FUNCTION__);
    return NULL;
}

static void *
malloc_internal(uint64 size)
{
    void *mem = NULL;

    if (size < UINT32_MAX && (mem = wasm_runtime_malloc((uint32)size))) {
        memset(mem, 0, size);
    }

    return mem;
}

#define FREEIF(p)                                                             \
    if (p) {                                                                  \
        wasm_runtime_free(p);                                                 \
    }

/* clang-format off */
#define RETURN_OBJ(obj, obj_del_func)                                         \
    return obj;                                                               \
failed:                                                                       \
    obj_del_func(obj);                                                        \
    return NULL;

#define RETURN_VOID(obj, obj_del_func)                                        \
    return;                                                                   \
failed:                                                                       \
    obj_del_func(obj);                                                        \
    return;
/* clang-format on */

/* Vectors */
#define INIT_VEC(vector_p, init_func, ...)                                    \
    do {                                                                      \
        if (!(vector_p = malloc_internal(sizeof(*(vector_p))))) {             \
            goto failed;                                                      \
        }                                                                     \
                                                                              \
        init_func(vector_p, ##__VA_ARGS__);                                   \
        if (vector_p->size && !(vector_p)->data) {                            \
            LOG_DEBUG("%s failed", #init_func);                               \
            goto failed;                                                      \
        }                                                                     \
    } while (false)

#define DEINIT_VEC(vector_p, deinit_func)                                     \
    if ((vector_p)) {                                                         \
        deinit_func(vector_p);                                                \
        wasm_runtime_free(vector_p);                                          \
        vector_p = NULL;                                                      \
    }

#define WASM_DEFINE_VEC(name)                                                 \
    void wasm_##name##_vec_new_empty(own wasm_##name##_vec_t *out)            \
    {                                                                         \
        wasm_##name##_vec_new_uninitialized(out, 0);                          \
    }                                                                         \
    void wasm_##name##_vec_new_uninitialized(own wasm_##name##_vec_t *out,    \
                                             size_t size)                     \
    {                                                                         \
        wasm_##name##_vec_new(out, size, NULL);                               \
    }

#define WASM_DEFINE_VEC_PLAIN(name)                                           \
    WASM_DEFINE_VEC(name)                                                     \
    void wasm_##name##_vec_new(own wasm_##name##_vec_t *out, size_t size,     \
                               own wasm_##name##_t const data[])              \
    {                                                                         \
        bh_assert(out);                                                       \
                                                                              \
        memset(out, 0, sizeof(wasm_##name##_vec_t));                          \
                                                                              \
        if (!size) {                                                          \
            return;                                                           \
        }                                                                     \
                                                                              \
        if (!bh_vector_init((Vector *)out, size, sizeof(wasm_##name##_t))) {  \
            LOG_DEBUG("bh_vector_init failed");                               \
            goto failed;                                                      \
        }                                                                     \
                                                                              \
        if (data) {                                                           \
            unsigned int size_in_bytes = 0;                                   \
            size_in_bytes = size * sizeof(wasm_##name##_t);                   \
            bh_memcpy_s(out->data, size_in_bytes, data, size_in_bytes);       \
            out->num_elems = size;                                            \
        }                                                                     \
                                                                              \
        RETURN_VOID(out, wasm_##name##_vec_delete)                            \
    }                                                                         \
    void wasm_##name##_vec_copy(wasm_##name##_vec_t *out,                     \
                                const wasm_##name##_vec_t *src)               \
    {                                                                         \
        wasm_##name##_vec_new(out, src->size, src->data);                     \
    }                                                                         \
    void wasm_##name##_vec_delete(wasm_##name##_vec_t *v)                     \
    {                                                                         \
        if (!v) {                                                             \
            return;                                                           \
        }                                                                     \
        bh_vector_destroy((Vector *)v);                                       \
    }

#define WASM_DEFINE_VEC_OWN(name, elem_destroy_func)                          \
    WASM_DEFINE_VEC(name)                                                     \
    void wasm_##name##_vec_new(own wasm_##name##_vec_t *out, size_t size,     \
                               own wasm_##name##_t *const data[])             \
    {                                                                         \
        bh_assert(out);                                                       \
                                                                              \
        memset(out, 0, sizeof(wasm_##name##_vec_t));                          \
                                                                              \
        if (!size) {                                                          \
            return;                                                           \
        }                                                                     \
                                                                              \
        if (!bh_vector_init((Vector *)out, size,                              \
                            sizeof(wasm_##name##_t *))) {                     \
            LOG_DEBUG("bh_vector_init failed");                               \
            goto failed;                                                      \
        }                                                                     \
                                                                              \
        if (data) {                                                           \
            unsigned int size_in_bytes = 0;                                   \
            size_in_bytes = size * sizeof(wasm_##name##_t *);                 \
            bh_memcpy_s(out->data, size_in_bytes, data, size_in_bytes);       \
            out->num_elems = size;                                            \
        }                                                                     \
                                                                              \
        RETURN_VOID(out, wasm_##name##_vec_delete)                            \
    }                                                                         \
    void wasm_##name##_vec_copy(own wasm_##name##_vec_t *out,                 \
                                const wasm_##name##_vec_t *src)               \
    {                                                                         \
        size_t i = 0;                                                         \
        memset(out, 0, sizeof(Vector));                                       \
                                                                              \
        if (!src->size) {                                                     \
            return;                                                           \
        }                                                                     \
                                                                              \
        if (!bh_vector_init((Vector *)out, src->size,                         \
                            sizeof(wasm_##name##_t *))) {                     \
            LOG_DEBUG("bh_vector_init failed");                               \
            goto failed;                                                      \
        }                                                                     \
                                                                              \
        for (i = 0; i != src->num_elems; ++i) {                               \
            if (!(out->data[i] = wasm_##name##_copy(src->data[i]))) {         \
                LOG_DEBUG("wasm_%s_copy failed", #name);                      \
                goto failed;                                                  \
            }                                                                 \
        }                                                                     \
        out->num_elems = src->num_elems;                                      \
                                                                              \
        RETURN_VOID(out, wasm_##name##_vec_delete)                            \
    }                                                                         \
    void wasm_##name##_vec_delete(wasm_##name##_vec_t *v)                     \
    {                                                                         \
        size_t i = 0;                                                         \
        if (!v) {                                                             \
            return;                                                           \
        }                                                                     \
        for (i = 0; i != v->num_elems; ++i) {                                 \
            elem_destroy_func(*(v->data + i));                                \
        }                                                                     \
        bh_vector_destroy((Vector *)v);                                       \
    }

WASM_DEFINE_VEC_PLAIN(byte)
WASM_DEFINE_VEC_PLAIN(val)

WASM_DEFINE_VEC_OWN(valtype, wasm_valtype_delete)
WASM_DEFINE_VEC_OWN(functype, wasm_functype_delete)
WASM_DEFINE_VEC_OWN(exporttype, wasm_exporttype_delete)
WASM_DEFINE_VEC_OWN(importtype, wasm_importtype_delete)
WASM_DEFINE_VEC_OWN(store, wasm_store_delete)
WASM_DEFINE_VEC_OWN(module, wasm_module_delete_internal)
WASM_DEFINE_VEC_OWN(instance, wasm_instance_delete_internal)
WASM_DEFINE_VEC_OWN(extern, wasm_extern_delete)

/* Runtime Environment */
static void
wasm_engine_delete_internal(wasm_engine_t *engine)
{
    if (engine) {
        DEINIT_VEC(engine->stores, wasm_store_vec_delete);
        wasm_runtime_free(engine);
    }

    wasm_runtime_destroy();
}

static wasm_engine_t *
wasm_engine_new_internal(mem_alloc_type_t type,
                         const MemAllocOption *opts,
                         runtime_mode_e mode)
{
    wasm_engine_t *engine = NULL;
    /* init runtime */
    RuntimeInitArgs init_args = { 0 };
    init_args.mem_alloc_type = type;

    if (type == Alloc_With_Pool) {
        init_args.mem_alloc_option.pool.heap_buf = opts->pool.heap_buf;
        init_args.mem_alloc_option.pool.heap_size = opts->pool.heap_size;
    }
    else if (type == Alloc_With_Allocator) {
        init_args.mem_alloc_option.allocator.malloc_func =
          opts->allocator.malloc_func;
        init_args.mem_alloc_option.allocator.free_func =
          opts->allocator.free_func;
        init_args.mem_alloc_option.allocator.realloc_func =
          opts->allocator.realloc_func;
    }
    else {
        init_args.mem_alloc_option.pool.heap_buf = NULL;
        init_args.mem_alloc_option.pool.heap_size = 0;
    }

    if (!wasm_runtime_full_init(&init_args)) {
        LOG_DEBUG("wasm_runtime_full_init failed");
        goto failed;
    }

#if BH_DEBUG != 0
    bh_log_set_verbose_level(5);
#else
    bh_log_set_verbose_level(3);
#endif

    /* create wasm_engine_t */
    engine = malloc_internal(sizeof(wasm_engine_t));
    if (!engine) {
        goto failed;
    }

    /* set running mode */
    LOG_WARNING("running under mode %d", mode);
    engine->mode = mode;

    /* create wasm_store_vec_t */
    INIT_VEC(engine->stores, wasm_store_vec_new_uninitialized, 1);

    RETURN_OBJ(engine, wasm_engine_delete_internal)
}

/* global engine instance */
static wasm_engine_t *singleton_engine = NULL;

static inline runtime_mode_e
current_runtime_mode()
{
    bh_assert(singleton_engine);
    return singleton_engine->mode;
}

wasm_engine_t *
wasm_engine_new()
{
    runtime_mode_e mode = INTERP_MODE;
#if WASM_ENABLE_INTERP == 0 && WASM_ENABLE_AOT != 0
    mode = AOT_MODE;
#endif

    if (INTERP_MODE == mode) {
#if WASM_ENABLE_INTERP == 0
        bh_assert(!"does not support INTERP_MODE. Please recompile");
#endif
    }
    else {
#if WASM_ENABLE_AOT == 0
        bh_assert(!"does not support AOT_MODE. Please recompile");
#endif
    }

    if (!singleton_engine) {
        singleton_engine =
          wasm_engine_new_internal(Alloc_With_System_Allocator, NULL, mode);
    }
    return singleton_engine;
}

wasm_engine_t *
wasm_engine_new_with_args(mem_alloc_type_t type,
                          const MemAllocOption *opts,
                          runtime_mode_e mode)
{
    if (!singleton_engine) {
        singleton_engine = wasm_engine_new_internal(type, opts, mode);
    }
    return singleton_engine;
}

/* BE AWARE: will RESET the singleton */
void
wasm_engine_delete(wasm_engine_t *engine)
{
    if (engine) {
        wasm_engine_delete_internal(engine);
        singleton_engine = NULL;
    }
}

wasm_store_t *
wasm_store_new(wasm_engine_t *engine)
{
    wasm_store_t *store = NULL;

    bh_assert(engine && singleton_engine == engine);

    store = malloc_internal(sizeof(wasm_store_t));
    if (!store) {
        goto failed;
    }

    /* new a vector, and new its data */
    INIT_VEC(store->modules, wasm_module_vec_new_uninitialized,
             DEFAULT_VECTOR_INIT_LENGTH);
    INIT_VEC(store->instances, wasm_instance_vec_new_uninitialized,
             DEFAULT_VECTOR_INIT_LENGTH);

    /* append to a store list of engine */
    if (!bh_vector_append((Vector *)singleton_engine->stores, &store)) {
        LOG_DEBUG("bh_vector_append failed");
        goto failed;
    }

    RETURN_OBJ(store, wasm_store_delete)
}

void
wasm_store_delete(wasm_store_t *store)
{
    size_t i, store_count;

    if (!store) {
        return;
    }

    /* TODO: if we release store here, we have no chance to verify
     * whether a wasm_func_call() is in a valid store or not
     */

    /* remove it from the list in the engine */
    store_count = bh_vector_size((Vector*)singleton_engine->stores);
    for (i = 0; i != store_count; ++i) {
        wasm_store_t *tmp;

        if (!bh_vector_get((Vector*)singleton_engine->stores, i, &tmp)) {
            break;
        }

        if (tmp == store) {
            bh_vector_remove((Vector*)singleton_engine->stores, i, NULL);
            break;
        }
    }

    DEINIT_VEC(store->modules, wasm_module_vec_delete);
    DEINIT_VEC(store->instances, wasm_instance_vec_delete);
    wasm_runtime_free(store);
}

static inline void
check_engine_and_store(wasm_engine_t *engine, wasm_store_t *store)
{
    /* remove it if we are supporting more than one store */
    bh_assert(engine && store);
}

/* Type Representations */
static wasm_valkind_t
val_type_rt_2_valkind(uint8 val_type_rt)
{
    switch (val_type_rt) {
        case VALUE_TYPE_I32:
            return WASM_I32;
        case VALUE_TYPE_I64:
            return WASM_I64;
        case VALUE_TYPE_F32:
            return WASM_F32;
        case VALUE_TYPE_F64:
            return WASM_F64;
        case VALUE_TYPE_ANY:
            return WASM_ANYREF;
        case VALUE_TYPE_FUNCREF:
            return WASM_FUNCREF;
        default:
            LOG_WARNING("%s meets unsupported type: %d", __FUNCTION__,
                        val_type_rt);
            return WASM_ANYREF;
    }
}

static wasm_valtype_t *
wasm_valtype_new_internal(uint8 val_type_rt)
{
    return wasm_valtype_new(val_type_rt_2_valkind(val_type_rt));
}

wasm_valtype_t *
wasm_valtype_new(wasm_valkind_t kind)
{
    wasm_valtype_t *val_type;

    if (!(val_type = malloc_internal(sizeof(wasm_valtype_t)))) {
        goto failed;
    }

    val_type->kind = kind;

    RETURN_OBJ(val_type, wasm_valtype_delete)
}

void
wasm_valtype_delete(wasm_valtype_t *val_type)
{
    FREEIF(val_type);
}

wasm_valtype_t *
wasm_valtype_copy(const wasm_valtype_t *src)
{
    bh_assert(src);
    return wasm_valtype_new(src->kind);
}

wasm_valkind_t
wasm_valtype_kind(const wasm_valtype_t *val_type)
{
    bh_assert(val_type);
    return val_type->kind;
}

bool
wasm_valtype_same(const wasm_valtype_t *vt1, const wasm_valtype_t *vt2)
{
    if (!vt1 && !vt2) {
        return true;
    }

    if (!vt1 || !vt2) {
        return false;
    }

    return vt1->kind == vt2->kind;
}

static wasm_functype_t *
wasm_functype_new_internal(WASMType *type_rt)
{
    wasm_functype_t *type = NULL;
    uint32 i = 0;

    bh_assert(type_rt);

    type = malloc_internal(sizeof(wasm_functype_t));
    if (!type) {
        goto failed;
    }

    type->extern_kind = WASM_EXTERN_FUNC;

    /* WASMType->types[0 : type_rt->param_count) -> type->params */
    INIT_VEC(type->params, wasm_valtype_vec_new_uninitialized,
             type_rt->param_count);
    for (i = 0; i < type_rt->param_count; ++i) {
        wasm_valtype_t *param_type;
        if (!(param_type = wasm_valtype_new_internal(*(type_rt->types + i)))) {
            goto failed;
        }

        if (!bh_vector_append((Vector *)type->params, &param_type)) {
            goto failed;
        }
    }

    /* WASMType->types[type_rt->param_count : type_rt->result_count) -> type->results */
    INIT_VEC(type->results, wasm_valtype_vec_new_uninitialized,
             type_rt->result_count);
    for (i = 0; i < type_rt->result_count; ++i) {
        wasm_valtype_t *result_type;
        if (!(result_type = wasm_valtype_new_internal(
                *(type_rt->types + type_rt->param_count + i)))) {
            goto failed;
        }

        if (!bh_vector_append((Vector *)type->results, &result_type)) {
            goto failed;
        }
    }

    return type;

failed:
    LOG_DEBUG("%s failed", __FUNCTION__);
    wasm_functype_delete(type);
    return NULL;
}

wasm_functype_t *
wasm_functype_new(own wasm_valtype_vec_t *params,
                  own wasm_valtype_vec_t *results)
{
    wasm_functype_t *type = NULL;

    bh_assert(params);
    bh_assert(results);

    type = malloc_internal(sizeof(wasm_functype_t));
    if (!type) {
        goto failed;
    }

    type->extern_kind = WASM_EXTERN_FUNC;

    /* different with INIT_VEC, reusing .data */
    if (!(type->params = malloc_internal(sizeof(wasm_valtype_vec_t)))) {
        goto failed;
    }
    bh_memcpy_s(type->params, sizeof(wasm_valtype_vec_t), params,
                sizeof(wasm_valtype_vec_t));

    if (!(type->results = malloc_internal(sizeof(wasm_valtype_vec_t)))) {
        goto failed;
    }
    bh_memcpy_s(type->results, sizeof(wasm_valtype_vec_t), results,
                sizeof(wasm_valtype_vec_t));

    return type;

failed:
    LOG_DEBUG("%s failed", __FUNCTION__);
    wasm_functype_delete(type);
    return NULL;
}

wasm_functype_t *
wasm_functype_copy(const wasm_functype_t *src)
{
    wasm_valtype_vec_t params = { 0 }, results = { 0 };
    bh_assert(src);

    wasm_valtype_vec_copy(&params, src->params);
    if (src->params->size && !params.data) {
        goto failed;
    }

    wasm_valtype_vec_copy(&results, src->results);
    if (src->results->size && !results.data) {
        goto failed;
    }

    return wasm_functype_new(&params, &results);

failed:
    wasm_valtype_vec_delete(&params);
    wasm_valtype_vec_delete(&results);
    return NULL;
}

void
wasm_functype_delete(wasm_functype_t *func_type)
{
    if (!func_type) {
        return;
    }

    DEINIT_VEC(func_type->params, wasm_valtype_vec_delete);
    DEINIT_VEC(func_type->results, wasm_valtype_vec_delete);

    wasm_runtime_free(func_type);
}

const wasm_valtype_vec_t *
wasm_functype_params(const wasm_functype_t *func_type)
{
    bh_assert(func_type);
    return func_type->params;
}

const wasm_valtype_vec_t *
wasm_functype_results(const wasm_functype_t *func_type)
{
    bh_assert(func_type);
    return func_type->results;
}

wasm_globaltype_t *
wasm_globaltype_new(own wasm_valtype_t *val_type, wasm_mutability_t mut)
{
    wasm_globaltype_t *global_type = NULL;

    bh_assert(val_type);

    if (!(global_type = malloc_internal(sizeof(wasm_globaltype_t)))) {
        goto failed;
    }

    global_type->extern_kind = WASM_EXTERN_GLOBAL;
    global_type->val_type = val_type;
    global_type->mutability = mut;

    RETURN_OBJ(global_type, wasm_globaltype_delete)
}

wasm_globaltype_t *
wasm_globaltype_new_internal(uint8 val_type_rt, bool is_mutable)
{
    wasm_valkind_t kind = val_type_rt_2_valkind(val_type_rt);
    wasm_valtype_t *val_type;

    if (!(val_type = wasm_valtype_new(kind))) {
        return NULL;
    }

    return wasm_globaltype_new(val_type, is_mutable ? WASM_VAR : WASM_CONST);
}

void
wasm_globaltype_delete(wasm_globaltype_t *global_type)
{
    if (!global_type) {
        return;
    }

    if (global_type->val_type) {
        wasm_valtype_delete(global_type->val_type);
        global_type->val_type = NULL;
    }

    wasm_runtime_free(global_type);
}

wasm_globaltype_t *
wasm_globaltype_copy(const wasm_globaltype_t *src)
{
    wasm_valtype_t *val_type;
    bh_assert(src);

    if (!(val_type = wasm_valtype_copy(src->val_type))) {
        return NULL;
    }

    return wasm_globaltype_new(val_type, src->mutability);
}

const wasm_valtype_t *
wasm_globaltype_content(const wasm_globaltype_t *global_type)
{
    bh_assert(global_type);
    return global_type->val_type;
}

wasm_mutability_t
wasm_globaltype_mutability(const wasm_globaltype_t *global_type)
{
    bh_assert(global_type);
    return global_type->mutability;
}

bool
wasm_globaltype_same(const wasm_globaltype_t *gt1,
                     const wasm_globaltype_t *gt2)
{
    if (!gt1 && !gt2) {
        return true;
    }

    if (!gt1 || !gt2) {
        return false;
    }

    return wasm_valtype_same(gt1->val_type, gt2->val_type)
           || gt1->mutability == gt2->mutability;
}

static wasm_tabletype_t *
wasm_tabletype_new_internal(uint8 val_type_rt,
                            uint32 init_size,
                            uint32 max_size)
{
    wasm_limits_t limits = { init_size, max_size };
    wasm_valtype_t *val_type;

    if (!(val_type = wasm_valtype_new_internal(val_type_rt))) {
        return NULL;
    }

    return wasm_tabletype_new(val_type, &limits);
}

wasm_tabletype_t *
wasm_tabletype_new(own wasm_valtype_t *val_type, const wasm_limits_t *limits)
{
    wasm_tabletype_t *table_type = NULL;

    bh_assert(val_type);

    if (!(table_type = malloc_internal(sizeof(wasm_tabletype_t)))) {
        goto failed;
    }

    table_type->extern_kind = WASM_EXTERN_TABLE;
    table_type->val_type = val_type;
    table_type->limits.min = limits->min;
    table_type->limits.max = limits->max;

    RETURN_OBJ(table_type, wasm_tabletype_delete)
}

wasm_tabletype_t *
wasm_tabletype_copy(const wasm_tabletype_t *src)
{
    wasm_valtype_t *val_type;
    if (!(val_type = wasm_valtype_copy(src->val_type))) {
        return NULL;
    }

    return wasm_tabletype_new(val_type, &src->limits);
}

void
wasm_tabletype_delete(wasm_tabletype_t *table_type)
{
    if (!table_type) {
        return;
    }

    if (table_type->val_type) {
        wasm_valtype_delete(table_type->val_type);
        table_type->val_type = NULL;
    }

    wasm_runtime_free(table_type);
}

const wasm_valtype_t *
wasm_tabletype_element(const wasm_tabletype_t *table_type)
{
    return table_type->val_type;
}

const wasm_limits_t *
wasm_tabletype_limits(const wasm_tabletype_t *table_type)
{
    return &(table_type->limits);
}

static wasm_memorytype_t *
wasm_memorytype_new_internal(uint32 min_pages, uint32 max_pages)
{
    wasm_limits_t limits = { min_pages, max_pages };
    return wasm_memorytype_new(&limits);
}

wasm_memorytype_t *
wasm_memorytype_new(const wasm_limits_t *limits)
{
    wasm_memorytype_t *memory_type = NULL;
    bh_assert(limits);

    if (!(memory_type = malloc_internal(sizeof(wasm_memorytype_t)))) {
        goto failed;
    }

    memory_type->extern_kind = WASM_EXTERN_MEMORY;
    memory_type->limits.min = limits->min;
    memory_type->limits.max = limits->max;

    RETURN_OBJ(memory_type, wasm_memorytype_delete)
}

wasm_memorytype_t *
wasm_memorytype_copy(const wasm_memorytype_t *src)
{
    bh_assert(src);
    return wasm_memorytype_new(&src->limits);
}

void
wasm_memorytype_delete(wasm_memorytype_t *memory_type)
{
    FREEIF(memory_type);
}

const wasm_limits_t *
wasm_memorytype_limits(const wasm_memorytype_t *memory_type)
{
    return &(memory_type->limits);
}

wasm_externkind_t
wasm_externtype_kind(const wasm_externtype_t *externtype)
{
    return externtype->extern_kind;
}

#define BASIC_FOUR_TYPE_LIST(V)                                               \
    V(functype)                                                               \
    V(globaltype)                                                             \
    V(memorytype)                                                             \
    V(tabletype)

#define WASM_EXTERNTYPE_AS_OTHERTYPE(name)                                    \
    wasm_##name##_t *wasm_externtype_as_##name(wasm_externtype_t *externtype) \
    {                                                                         \
        return (wasm_##name##_t *)externtype;                                 \
    }

BASIC_FOUR_TYPE_LIST(WASM_EXTERNTYPE_AS_OTHERTYPE)
#undef WASM_EXTERNTYPE_AS_OTHERTYPE

#define WASM_OTHERTYPE_AS_EXTERNTYPE(name)                                    \
    wasm_externtype_t *wasm_##name##_as_externtype(wasm_##name##_t *other)    \
    {                                                                         \
        return (wasm_externtype_t *)other;                                    \
    }

BASIC_FOUR_TYPE_LIST(WASM_OTHERTYPE_AS_EXTERNTYPE)
#undef WASM_OTHERTYPE_AS_EXTERNTYPE

#define WASM_EXTERNTYPE_AS_OTHERTYPE_CONST(name)                              \
    const wasm_##name##_t *wasm_externtype_as_##name##_const(                 \
      const wasm_externtype_t *externtype)                                    \
    {                                                                         \
        return (const wasm_##name##_t *)externtype;                           \
    }

BASIC_FOUR_TYPE_LIST(WASM_EXTERNTYPE_AS_OTHERTYPE_CONST)
#undef WASM_EXTERNTYPE_AS_OTHERTYPE_CONST

#define WASM_OTHERTYPE_AS_EXTERNTYPE_CONST(name)                              \
    const wasm_externtype_t *wasm_##name##_as_externtype_const(               \
      const wasm_##name##_t *other)                                           \
    {                                                                         \
        return (const wasm_externtype_t *)other;                              \
    }

BASIC_FOUR_TYPE_LIST(WASM_OTHERTYPE_AS_EXTERNTYPE_CONST)
#undef WASM_OTHERTYPE_AS_EXTERNTYPE_CONST

wasm_externtype_t *
wasm_externtype_copy(const wasm_externtype_t *src)
{
    wasm_externtype_t *externtype = NULL;

    bh_assert(src);

    switch (src->extern_kind) {
#define COPY_EXTERNTYPE(NAME, name)                                           \
    case WASM_EXTERN_##NAME:                                                  \
    {                                                                         \
        externtype = wasm_##name##_as_externtype(                             \
          wasm_##name##_copy(wasm_externtype_as_##name##_const(src)));        \
        break;                                                                \
    }
        COPY_EXTERNTYPE(FUNC, functype)
        COPY_EXTERNTYPE(GLOBAL, globaltype)
        COPY_EXTERNTYPE(MEMORY, memorytype)
        COPY_EXTERNTYPE(TABLE, tabletype)
#undef COPY_EXTERNTYPE
        default:
            LOG_WARNING("%s meets unsupported kind", __FUNCTION__,
                        src->extern_kind);
            break;
    }
    return externtype;
}

void
wasm_externtype_delete(wasm_externtype_t *externtype)
{
    if (!externtype) {
        return;
    }

    switch (wasm_externtype_kind(externtype)) {
        case WASM_EXTERN_FUNC:
            wasm_functype_delete(wasm_externtype_as_functype(externtype));
            break;
        case WASM_EXTERN_GLOBAL:
            wasm_globaltype_delete(wasm_externtype_as_globaltype(externtype));
            break;
        case WASM_EXTERN_MEMORY:
            wasm_memorytype_delete(wasm_externtype_as_memorytype(externtype));
            break;
        case WASM_EXTERN_TABLE:
            wasm_tabletype_delete(wasm_externtype_as_tabletype(externtype));
            break;
        default:
            LOG_WARNING("%s meets unsupported type", __FUNCTION__,
                        externtype);
            break;
    }
}

own wasm_importtype_t *
wasm_importtype_new(own wasm_name_t *module,
                    own wasm_name_t *name,
                    own wasm_externtype_t *externtype)
{
    wasm_importtype_t *importtype = NULL;

    if (!(importtype = malloc_internal(sizeof(wasm_importtype_t)))) {
        goto failed;
    }

    importtype->module_name = module;
    importtype->name = name;
    importtype->externtype = externtype;

    RETURN_OBJ(importtype, wasm_importtype_delete);
}

void
wasm_importtype_delete(own wasm_importtype_t *importtype)
{
    if (!importtype) {
        return;
    }

    DEINIT_VEC(importtype->module_name, wasm_byte_vec_delete);
    DEINIT_VEC(importtype->name, wasm_byte_vec_delete);
    wasm_externtype_delete(importtype->externtype);
    wasm_runtime_free(importtype);
}

own wasm_importtype_t *
wasm_importtype_copy(const wasm_importtype_t *src)
{
    wasm_name_t *module_name = NULL, *name = NULL;
    wasm_externtype_t *externtype = NULL;
    wasm_importtype_t *importtype = NULL;

    bh_assert(src);

    INIT_VEC(module_name, wasm_byte_vec_copy, src->module_name);
    INIT_VEC(name, wasm_byte_vec_copy, src->name);

    if (!(externtype = wasm_externtype_copy(src->externtype))) {
        goto failed;
    }

    if (!(importtype = wasm_importtype_new(module_name, name, externtype))) {
        goto failed;
    }

    return importtype;

failed:
    DEINIT_VEC(module_name, wasm_byte_vec_delete);
    DEINIT_VEC(name, wasm_byte_vec_delete);
    wasm_externtype_delete(externtype);
    wasm_importtype_delete(importtype);
    return NULL;
}

const wasm_name_t *
wasm_importtype_module(const wasm_importtype_t *importtype)
{
    return importtype->module_name;
}

const wasm_name_t *
wasm_importtype_name(const wasm_importtype_t *importtype)
{
    return importtype->name;
}

const wasm_externtype_t *
wasm_importtype_type(const wasm_importtype_t *importtype)
{
    return importtype->externtype;
}

own wasm_exporttype_t *
wasm_exporttype_new(own wasm_name_t *name, own wasm_externtype_t *externtype)
{
    wasm_exporttype_t *exporttype = NULL;

    if (!(exporttype = malloc_internal(sizeof(wasm_exporttype_t)))) {
        goto failed;
    }

    INIT_VEC(exporttype->name, wasm_byte_vec_new, name->size, name->data);

    exporttype->externtype = externtype;

    RETURN_OBJ(exporttype, wasm_exporttype_delete)
}

wasm_exporttype_t *
wasm_exporttype_copy(const wasm_exporttype_t *exporttype)
{
    return wasm_exporttype_new(exporttype->name, exporttype->externtype);
}

void
wasm_exporttype_delete(wasm_exporttype_t *exporttype)
{
    if (!exporttype) {
        return;
    }

    DEINIT_VEC(exporttype->name, wasm_byte_vec_delete);

    wasm_externtype_delete(exporttype->externtype);

    wasm_runtime_free(exporttype);
}

const wasm_name_t *
wasm_exporttype_name(const wasm_exporttype_t *exporttype)
{
    return exporttype->name;
}

const wasm_externtype_t *
wasm_exporttype_type(const wasm_exporttype_t *exporttype)
{
    return exporttype->externtype;
}

/* Runtime Objects */

void
wasm_val_delete(wasm_val_t *v)
{
    FREEIF(v);
}

void
wasm_val_copy(wasm_val_t *out, const wasm_val_t *src)
{
    bh_assert(out && src);
    bh_memcpy_s(out, sizeof(wasm_val_t), src, sizeof(wasm_val_t));
}

bool
wasm_val_same(const wasm_val_t *v1, const wasm_val_t *v2)
{
    if (!v1 && !v2) {
        return true;
    }

    if (!v1 || !v2) {
        return false;
    }

    if (v1->kind != v2->kind) {
        return false;
    }

    switch (v1->kind) {
        case WASM_I32:
            return v1->of.i32 == v2->of.i32;
        case WASM_I64:
            return v1->of.i64 == v2->of.i64;
        case WASM_F32:
            return v1->of.f32 == v2->of.f32;
        case WASM_F64:
            return v1->of.f64 == v2->of.f64;
        case WASM_FUNCREF:
            return v1->of.ref == v2->of.ref;
        default:
            break;
    }
    return false;
}

static wasm_trap_t *
wasm_trap_new_basic(const wasm_message_t *message)
{
    wasm_trap_t *trap;

    if (!(trap = malloc_internal(sizeof(wasm_trap_t)))) {
        goto failed;
    }

    INIT_VEC(trap->message, wasm_byte_vec_new, message->size, message->data);

    RETURN_OBJ(trap, wasm_trap_delete);
}

static wasm_trap_t *
wasm_trap_new_internal(const char *string)
{
    wasm_trap_t *trap;

    if (!(trap = malloc_internal(sizeof(wasm_trap_t)))) {
        goto failed;
    }

    INIT_VEC(trap->message, wasm_byte_vec_new, strlen(string) + 1, string);

    RETURN_OBJ(trap, wasm_trap_delete);
}

wasm_trap_t *
wasm_trap_new(wasm_store_t *store, const wasm_message_t *message)
{
    bh_assert(store && message);
    return wasm_trap_new_basic(message);
}

void
wasm_trap_delete(wasm_trap_t *trap)
{
    if (!trap) {
        return;
    }

    DEINIT_VEC(trap->message, wasm_byte_vec_delete);

    wasm_runtime_free(trap);
}

void
wasm_trap_message(const wasm_trap_t *trap, own wasm_message_t *out)
{
    bh_assert(trap && out);
    wasm_name_copy(out, trap->message);
}

struct wasm_module_ex_t {
    struct WASMModuleCommon *module_comm_rt;
    wasm_byte_vec_t *binary;
};

static inline wasm_module_t *
module_ext_to_module(wasm_module_ex_t *module_ex)
{
    return (wasm_module_t *)module_ex;
}

static inline wasm_module_ex_t *
module_to_module_ext(wasm_module_t *module)
{
    return (wasm_module_ex_t *)module;
}

#if WASM_ENABLE_INTERP != 0
#undef MODULE_RUNTIME
#define MODULE_RUNTIME(module) ((WASMModule *)(*module))
#endif

#if WASM_ENABLE_AOT != 0
#undef MODULE_RUNTIME
#define MODULE_RUNTIME(module) ((AOTModule *)(*module))
#endif

wasm_module_t *
wasm_module_new(wasm_store_t *store, const wasm_byte_vec_t *binary)
{
    char error[128] = { 0 };
    wasm_module_ex_t *module_ex = NULL;
    PackageType pkg_type = Package_Type_Unknown;

    check_engine_and_store(singleton_engine, store);
    bh_assert(binary && binary->data && binary->size);

    if (binary->size > UINT32_MAX) {
        LOG_ERROR("%s failed", __FUNCTION__);
        return NULL;
    }

    pkg_type = get_package_type((uint8 *)binary->data, (uint32)binary->size);
    if (Package_Type_Unknown == pkg_type
        || (Wasm_Module_Bytecode == pkg_type
            && INTERP_MODE != current_runtime_mode())
        || (Wasm_Module_AoT == pkg_type
            && INTERP_MODE == current_runtime_mode())) {
        LOG_ERROR(
          "current runtime mode %d doesn\'t support the package type %d",
          current_runtime_mode(), pkg_type);
        return NULL;
    }

    module_ex = malloc_internal(sizeof(wasm_module_ex_t));
    if (!module_ex) {
        goto failed;
    }

    INIT_VEC(module_ex->binary, wasm_byte_vec_new, binary->size, binary->data);

    module_ex->module_comm_rt = wasm_runtime_load(
      (uint8 *)module_ex->binary->data, (uint32)module_ex->binary->size, error,
      (uint32)sizeof(error));
    if (!(module_ex->module_comm_rt)) {
        LOG_ERROR(error);
        wasm_module_delete_internal(module_ext_to_module(module_ex));
        return NULL;
    }

    /* add it to a watching list in store */
    if (!bh_vector_append((Vector *)store->modules, &module_ex)) {
        goto failed;
    }

    return module_ext_to_module(module_ex);

failed:
    LOG_ERROR("%s failed", __FUNCTION__);
    wasm_module_delete_internal(module_ext_to_module(module_ex));
    return NULL;
}

static void
wasm_module_delete_internal(wasm_module_t *module)
{
    wasm_module_ex_t *module_ex;
    if (!module) {
        return;
    }

    module_ex = module_to_module_ext(module);
    DEINIT_VEC(module_ex->binary, wasm_byte_vec_delete);

    if (module_ex->module_comm_rt) {
        wasm_runtime_unload(module_ex->module_comm_rt);
        module_ex->module_comm_rt = NULL;
    }

    wasm_runtime_free(module_ex);
}

void
wasm_module_delete(wasm_module_t *module)
{
    /* will release module when releasing the store */
}

void
wasm_module_imports(const wasm_module_t *module,
                    own wasm_importtype_vec_t *out)
{
    uint32 i, import_count;
    wasm_name_t *module_name = NULL, *name = NULL;

    bh_assert(out);

#if WASM_ENABLE_INTERP != 0
    import_count = MODULE_RUNTIME(module)->import_count;
#endif

#if WASM_ENABLE_AOT != 0
    /* TODO */
#endif

    wasm_importtype_vec_new_uninitialized(out, import_count);

    /* TODO: module_name ? */

    for (i = 0; i != import_count; ++i) {
#if WASM_ENABLE_INTERP != 0
        WASMImport *import = MODULE_RUNTIME(module)->imports + i;
#endif

#if WASM_ENABLE_AOT != 0
        /* TODO */
#endif
        wasm_externtype_t *externtype = NULL;
        wasm_importtype_t *importtype = NULL;

        INIT_VEC(module_name, wasm_byte_vec_new,
                 strlen(import->u.names.module_name),
                 import->u.names.module_name);
        INIT_VEC(name, wasm_byte_vec_new, strlen(import->u.names.field_name),
                 import->u.names.field_name);

        switch (import->kind) {
            case IMPORT_KIND_FUNC:
            {
                wasm_functype_t *type = NULL;
                WASMType *type_rt;

                type_rt = import->u.function.func_type;

                if (!(type = wasm_functype_new_internal(type_rt))) {
                    goto failed;
                }

                externtype = wasm_functype_as_externtype(type);
                break;
            }
            case IMPORT_KIND_GLOBAL:
            {
                wasm_globaltype_t *type = NULL;
                uint8 val_type_rt = 0;
                bool mutability_rt = 0;

                val_type_rt = import->u.global.type;
                mutability_rt = import->u.global.is_mutable;

                if (!(type = wasm_globaltype_new_internal(val_type_rt,
                                                          mutability_rt))) {
                    goto failed;
                }

                externtype = wasm_globaltype_as_externtype(type);
                break;
            }
            case IMPORT_KIND_TABLE:
            {
                wasm_tabletype_t *type = NULL;
                uint8 elem_type_rt = 0;
                uint32 min_size = 0, max_size = 0;

                elem_type_rt = import->u.table.elem_type;
                min_size = import->u.table.init_size;
                max_size = import->u.table.max_size;

                if (!(type = wasm_tabletype_new_internal(
                        elem_type_rt, min_size, max_size))) {
                    goto failed;
                }

                externtype = wasm_tabletype_as_externtype(type);
                break;
            }
            case IMPORT_KIND_MEMORY:
            {
                wasm_memorytype_t *type = NULL;
                uint32 min_page = 0, max_page = 0;

                min_page = import->u.memory.init_page_count;
                max_page = import->u.memory.max_page_count;

                if (!(type =
                        wasm_memorytype_new_internal(min_page, max_page))) {
                    goto failed;
                }

                externtype = wasm_memorytype_as_externtype(type);
                break;
            }
            default:
            {
                LOG_WARNING("%s meets unsupported type", __FUNCTION__,
                            import->kind);
                goto failed;
            }
        }

        if (!(importtype =
                wasm_importtype_new(module_name, name, externtype))) {
            goto failed;
        }

        if (!bh_vector_append((Vector *)out, &importtype)) {
            goto failed;
        }
    }

    return;

failed:
    DEINIT_VEC(module_name, wasm_byte_vec_delete);
    DEINIT_VEC(name, wasm_byte_vec_delete);
    wasm_importtype_vec_delete(out);
}

void
wasm_module_exports(const wasm_module_t *module, wasm_exporttype_vec_t *out)
{
    uint32 i, export_count;
    wasm_name_t *name = NULL;

    bh_assert(out);

#if WASM_ENABLE_INTERP != 0
    export_count = MODULE_RUNTIME(module)->export_count;
#endif
#if WASM_ENABLE_AOT != 0
    export_count = MODULE_RUNTIME(module)->export_count;
#endif

    wasm_exporttype_vec_new_uninitialized(out, export_count);

    for (i = 0; i != export_count; i++) {
#if WASM_ENABLE_INTERP != 0
        WASMExport *export = MODULE_RUNTIME(module)->exports + i;
#endif
#if WASM_ENABLE_AOT != 0
        WASMExport *export = MODULE_RUNTIME(module)->exports + i;
#endif
        wasm_externtype_t *externtype = NULL;
        wasm_exporttype_t *exporttype = NULL;

        /* byte* -> wasm_byte_vec_t */
        INIT_VEC(name, wasm_byte_vec_new, strlen(export->name), export->name);

        /* WASMExport -> (WASMType, (uint8, bool)) -> (wasm_functype_t, wasm_globaltype_t) -> wasm_externtype_t*/
        switch (export->kind) {
            case EXPORT_KIND_FUNC:
            {
                wasm_functype_t *type = NULL;
                WASMType *type_rt;

                if (!wasm_runtime_get_export_func_type(*module, export,
                                                       &type_rt)) {
                    goto failed;
                }

                if (!(type = wasm_functype_new_internal(type_rt))) {
                    goto failed;
                }

                externtype = wasm_functype_as_externtype(type);
                break;
            }
            case EXPORT_KIND_GLOBAL:
            {
                wasm_globaltype_t *type = NULL;
                uint8 val_type_rt = 0;
                bool mutability_rt = 0;

                if (!wasm_runtime_get_export_global_type(
                      *module, export, &val_type_rt, &mutability_rt)) {
                    goto failed;
                }

                if (!(type = wasm_globaltype_new_internal(val_type_rt,
                                                          mutability_rt))) {
                    goto failed;
                }

                externtype = wasm_globaltype_as_externtype(type);
                break;
            }
            case EXPORT_KIND_MEMORY:
            {
                wasm_memorytype_t *type = NULL;
                uint32 min_page = 0, max_page = 0;

                if (!wasm_runtime_get_export_memory_type(
                      *module, export, &min_page, &max_page)) {
                    goto failed;
                }

                if (!(type =
                        wasm_memorytype_new_internal(min_page, max_page))) {
                    goto failed;
                }

                externtype = wasm_memorytype_as_externtype(type);
                break;
            }
            case EXPORT_KIND_TABLE:
            {
                wasm_tabletype_t *type = NULL;
                uint8 elem_type_rt = 0;
                uint32 min_size = 0, max_size = 0;

                if (!wasm_runtime_get_export_table_type(
                      *module, export, &elem_type_rt, &min_size, &max_size)) {
                    goto failed;
                }

                if (!(type = wasm_tabletype_new_internal(
                        elem_type_rt, min_size, max_size))) {
                    goto failed;
                }

                externtype = wasm_tabletype_as_externtype(type);
                break;
            }
            default:
            {
                LOG_WARNING("%s meets unsupported type", __FUNCTION__,
                            export->kind);
                break;
            }
        }

        if (!(exporttype = wasm_exporttype_new(name, externtype))) {
            goto failed;
        }

        DEINIT_VEC(name, wasm_byte_vec_delete);

        if (!(bh_vector_append((Vector *)out, &exporttype))) {
            goto failed;
        }
    }

    return;

failed:
    DEINIT_VEC(name, wasm_byte_vec_delete);
    wasm_exporttype_vec_delete(out);
}

static uint32
argv_to_params(const uint64 *argv,
               const wasm_valtype_vec_t *param_defs,
               wasm_val_t out[])
{
    size_t i = 0;
    uint32 argc = 0;

    for (i = 0; i < param_defs->num_elems; i++) {
        wasm_valtype_t *param_def = param_defs->data[i];
        wasm_val_t *param = out + i;
        switch (param_def->kind) {
            case WASM_I32:
                param->kind = WASM_I32;
                param->of.i32 = *(uint32 *)(argv + i);
                argc++;
                break;
            case WASM_I64:
                param->kind = WASM_I64;
                param->of.i64 = *(uint64 *)(argv + i);
                argc++;
                break;
            case WASM_F32:
                param->kind = WASM_F32;
                param->of.f32 = *(float32 *)(argv + i);
                argc++;
                break;
            case WASM_F64:
                param->kind = WASM_F64;
                param->of.f64 = *(float64 *)(argv + i);
                argc++;
                break;
            default:
                LOG_WARNING("%s meets unsupported type: %d", __FUNCTION__,
                            param_def->kind);
                goto failed;
        }
    }

    return argc;
failed:
    return 0;
}

static uint32
results_to_argv(const wasm_val_t *results,
                const wasm_valtype_vec_t *result_defs,
                uint64 *out)
{
    size_t i = 0;
    uint32 argc = 0;

    for (i = 0; i < result_defs->num_elems; ++i) {
        wasm_valtype_t *result_def = result_defs->data[i];
        const wasm_val_t *result = results + i;
        switch (result_def->kind) {
            case WASM_I32:
                *(int32 *)(out + i) = result->of.i32;
                argc++;
                break;
            case WASM_I64:
                *(int64 *)(out + i) = result->of.i64;
                argc++;
                break;
            case WASM_F32:
                *(float32 *)(out + i) = result->of.f32;
                argc++;
                break;
            case WASM_F64:
                *(float64 *)(out + i) = result->of.f64;
                argc++;
                break;
            default:
            {
                LOG_WARNING("%s meets unsupported kind", __FUNCTION__,
                            result_def->kind);
                goto failed;
            }
        }
    }

    return argc;
failed:
    return 0;
}

static wasm_trap_t *cur_trap = NULL;
static void
native_func_trampoline(wasm_exec_env_t exec_env, uint64 *argv)
{
    wasm_val_t *params = NULL, *results = NULL;
    uint32 argc = 0;
    const wasm_func_t *func = NULL;
    wasm_trap_t *trap = NULL;
    size_t param_count, result_count;

    func = wasm_runtime_get_function_attachment(exec_env);
    bh_assert(func);

    param_count = wasm_func_param_arity(func);
    if (param_count) {
        if (!argv) {
            goto failed;
        }

        if (!(params = malloc_internal(param_count * sizeof(wasm_val_t)))) {
            goto failed;
        }

        /* argv -> const wasm_val_t params[] */
        if (!(argc = argv_to_params(argv, wasm_functype_params(func->type),
                                    params))) {
            goto failed;
        }
    }

    result_count = wasm_func_result_arity(func);
    if (result_count) {
        if (!argv) {
            goto failed;
        }

        if (!(results = malloc_internal(result_count * sizeof(wasm_val_t)))) {
            goto failed;
        }
    }

    if (func->with_env) {
        trap = func->u.cb_env.cb(func->u.cb_env.env, params, results);
    }
    else {
        trap = func->u.cb(params, results);
    }

    if (trap) {
        wasm_name_t message = { 0 };
        wasm_trap_message(trap, &message);
        if (message.data) {
            LOG_WARNING("got a trap %s", message.data);
            wasm_set_exception((WASMModuleInstance *)exec_env->module_inst,
                               "call failed, meet a wasm_trap_t");
        }
        wasm_name_delete(&message);

        cur_trap = trap;
    }

    if (argv) {
        memset(argv, 0, wasm_func_param_arity(func) * sizeof(uint64));
    }

    /* there is no trap and there is return values */
    if (!trap && result_count) {
        /* wasm_val_t results[] -> argv */
        if (!(argc = results_to_argv(
                results, wasm_functype_results(func->type), argv))) {
            goto failed;
        }
    }

failed:
    FREEIF(params);
    FREEIF(results);
    return;
}

static wasm_func_t *
wasm_func_new_basic(const wasm_functype_t *type,
                    wasm_func_callback_t func_callback)
{
    wasm_func_t *func = NULL;
    bh_assert(type);

    if (!(func = malloc_internal(sizeof(wasm_func_t)))) {
        goto failed;
    }

    func->kind = WASM_EXTERN_FUNC;
    func->with_env = false;
    func->u.cb = func_callback;

    if (!(func->type = wasm_functype_copy(type))) {
        goto failed;
    }

    RETURN_OBJ(func, wasm_func_delete)
}

static wasm_func_t *
wasm_func_new_with_env_basic(const wasm_functype_t *type,
                             wasm_func_callback_with_env_t callback,
                             void *env,
                             void (*finalizer)(void *))
{
    wasm_func_t *func = NULL;

    bh_assert(type);

    if (!(func = malloc_internal(sizeof(wasm_func_t)))) {
        goto failed;
    }

    func->kind = WASM_EXTERN_FUNC;
    func->with_env = true;
    func->u.cb_env.cb = callback;
    func->u.cb_env.env = env;
    func->u.cb_env.finalizer = finalizer;

    if (!(func->type = wasm_functype_copy(type))) {
        goto failed;
    }

    RETURN_OBJ(func, wasm_func_delete)
}

wasm_func_t *
wasm_func_new(wasm_store_t *store,
              const wasm_functype_t *type,
              wasm_func_callback_t callback)
{
    check_engine_and_store(singleton_engine, store);
    return wasm_func_new_basic(type, callback);
}

wasm_func_t *
wasm_func_new_with_env(wasm_store_t *store,
                       const wasm_functype_t *type,
                       wasm_func_callback_with_env_t callback,
                       void *env,
                       void (*finalizer)(void *))
{
    check_engine_and_store(singleton_engine, store);
    return wasm_func_new_with_env_basic(type, callback, env, finalizer);
}

static wasm_func_t *
wasm_func_new_internal(wasm_store_t *store,
                       uint16 func_idx_rt,
                       WASMModuleInstanceCommon *inst_comm_rt)
{
    wasm_func_t *func = NULL;
    WASMType *type_rt = NULL;

    check_engine_and_store(singleton_engine, store);
    bh_assert(inst_comm_rt);

    func = malloc_internal(sizeof(wasm_func_t));
    if (!func) {
        goto failed;
    }

    func->kind = WASM_EXTERN_FUNC;

    if (INTERP_MODE == current_runtime_mode()) {
#if WASM_ENABLE_INTERP != 0
        bh_assert(func_idx_rt
                  < ((WASMModuleInstance *)inst_comm_rt)->function_count);
        WASMFunctionInstance *func_interp =
          ((WASMModuleInstance *)inst_comm_rt)->functions + func_idx_rt;
        type_rt = func_interp->is_import_func
                    ? func_interp->u.func_import->func_type
                    : func_interp->u.func->func_type;
#endif
    }
    else {
#if WASM_ENABLE_AOT != 0
        /* use same index to trace the function type in AOTFuncType **func_types */
        AOTModuleInstance *inst_aot = (AOTModuleInstance *)inst_comm_rt;
        AOTFunctionInstance *func_aot =
          (AOTFunctionInstance *)inst_aot->export_funcs.ptr + func_idx_rt;
        type_rt = func_aot->is_import_func ? func_aot->u.func_import->func_type
                                           : func_aot->u.func.func_type;
#endif
    }

    if (!type_rt) {
        goto failed;
    }

    func->type = wasm_functype_new_internal(type_rt);
    if (!func->type) {
        goto failed;
    }

    /* will add name information when processing "exports" */
    func->module_name = NULL;
    func->name = NULL;
    func->func_idx_rt = func_idx_rt;
    func->inst_comm_rt = inst_comm_rt;
    return func;

failed:
    LOG_DEBUG("%s failed", __FUNCTION__);
    wasm_func_delete(func);
    return NULL;
}

void
wasm_func_delete(wasm_func_t *func)
{
    if (!func) {
        return;
    }

    if (func->type) {
        wasm_functype_delete(func->type);
        func->type = NULL;
    }

    if (func->with_env) {
        if (func->u.cb_env.finalizer) {
            func->u.cb_env.finalizer(func->u.cb_env.env);
            func->u.cb_env.finalizer = NULL;
            func->u.cb_env.env = NULL;
        }
    }

    wasm_runtime_free(func);
}

own wasm_func_t *
wasm_func_copy(const wasm_func_t *func)
{
    wasm_func_t *cloned = NULL;

    bh_assert(func);

    if (!(cloned = func->with_env
                     ? wasm_func_new_with_env_basic(
                       func->type, func->u.cb_env.cb, func->u.cb_env.env,
                       func->u.cb_env.finalizer)
                     : wasm_func_new_basic(func->type, func->u.cb))) {
        goto failed;
    }

    cloned->func_idx_rt = func->func_idx_rt;
    cloned->inst_comm_rt = func->inst_comm_rt;

    RETURN_OBJ(cloned, wasm_func_delete)
}

own wasm_functype_t *
wasm_func_type(const wasm_func_t *func)
{
    bh_assert(func);
    return wasm_functype_copy(func->type);
}

static uint32
params_to_argv(const wasm_val_t *params,
               const wasm_valtype_vec_t *param_defs,
               size_t param_arity,
               uint32 *out)
{
    size_t i = 0;
    uint32 argc = 0;
    const wasm_val_t *param = NULL;

    if (!param_arity) {
        return 0;
    }

    bh_assert(params && param_defs && out);
    bh_assert(param_defs->num_elems == param_arity);

    for (i = 0; out && i < param_arity; ++i) {
        param = params + i;
        bh_assert((*(param_defs->data + i))->kind == param->kind);

        switch (param->kind) {
            case WASM_I32:
                *(int32 *)out = param->of.i32;
                out += 1;
                argc += 1;
                break;
            case WASM_I64:
                *(int64 *)out = param->of.i64;
                out += 2;
                argc += 2;
                break;
            case WASM_F32:
                *(float32 *)out = param->of.f32;
                out += 1;
                argc += 1;
                break;
            case WASM_F64:
                *(float64 *)out = param->of.f64;
                out += 2;
                argc += 2;
                break;
            default:
                LOG_DEBUG("unexpected parameter val type %d", param->kind);
                goto failed;
        }
    }

    return argc;

failed:
    LOG_DEBUG("%s failed", __FUNCTION__);
    return 0;
}

static uint32
argv_to_results(const uint32 *results,
                const wasm_valtype_vec_t *result_defs,
                size_t result_arity,
                wasm_val_t *out)
{
    size_t i = 0;
    uint32 argc = 0;
    const uint32 *result = results;
    const wasm_valtype_t *def = NULL;

    if (!result_arity) {
        return 0;
    }

    bh_assert(results && result_defs && out);
    bh_assert(result_arity == result_defs->num_elems);

    for (i = 0; out && i < result_arity; i++) {
        def = *(result_defs->data + i);

        switch (def->kind) {
            case WASM_I32:
            {
                out->kind = WASM_I32;
                out->of.i32 = *(int32 *)result;
                result += 1;
                break;
            }
            case WASM_I64:
            {
                out->kind = WASM_I64;
                out->of.i64 = *(int64 *)result;
                result += 2;
                break;
            }
            case WASM_F32:
            {
                out->kind = WASM_F32;
                out->of.f32 = *(float32 *)result;
                result += 1;
                break;
            }
            case WASM_F64:
            {
                out->kind = WASM_F64;
                out->of.f64 = *(float64 *)result;
                result += 2;
                break;
            }
            default:
                LOG_WARNING("%s meets unsupported type: %d", __FUNCTION__,
                            def->kind);
                goto failed;
        }
        out++;
        argc++;
    }

    return argc;

failed:
    LOG_DEBUG("%s failed", __FUNCTION__);
    return 0;
}

wasm_trap_t *
wasm_func_call(const wasm_func_t *func,
               const wasm_val_t params[],
               wasm_val_t results[])
{
    /* parameters count as if all are uint32 */
    /* a int64 or float64 parameter means 2 */
    uint32 argc = 0;
    /* a parameter list and a return value list */
    uint32 *argv = NULL;
    WASMFunctionInstanceCommon *func_comm_rt = NULL;
    size_t param_count, result_count, alloc_count;

    bh_assert(func && func->type && func->inst_comm_rt);

    /* TODO: how to check whether its store still exists */

    cur_trap = NULL;

    if (INTERP_MODE == current_runtime_mode()) {
#if WASM_ENABLE_INTERP != 0
        func_comm_rt = ((WASMModuleInstance *)func->inst_comm_rt)->functions
                       + func->func_idx_rt;
#endif
    }
    else {
#if WASM_ENABLE_AOT != 0
        AOTModuleInstance *inst_aot = (AOTModuleInstance *)func->inst_comm_rt;
        func_comm_rt = (AOTFunctionInstance *)inst_aot->export_funcs.ptr
                       + func->func_idx_rt;
#endif
    }
    if (!func_comm_rt) {
        goto failed;
    }

    param_count = wasm_func_param_arity(func);
    result_count = wasm_func_result_arity(func);
    alloc_count = (param_count > result_count) ? param_count : result_count;
    if (alloc_count) {
        if (!(argv = malloc_internal(sizeof(uint64) * alloc_count))) {
            goto failed;
        }
    }

    /* copy parametes */
    if (param_count
        && !(argc = params_to_argv(params, wasm_functype_params(func->type),
                                   param_count, argv))) {
        goto failed;
    }

    if (!wasm_runtime_create_exec_env_and_call_wasm(
          func->inst_comm_rt, func_comm_rt, argc, argv)) {
        if (wasm_runtime_get_exception(func->inst_comm_rt)) {
            LOG_DEBUG(wasm_runtime_get_exception(func->inst_comm_rt));
            goto failed;
        }
    }

    /* copy results */
    if (result_count) {
        if (!(argc = argv_to_results(argv, wasm_functype_results(func->type),
                                     result_count, results))) {
            goto failed;
        }
    }

    FREEIF(argv);
    return NULL;

failed:
    FREEIF(argv);
    if (cur_trap) {
        return cur_trap;
    }
    else {
        if (wasm_runtime_get_exception(func->inst_comm_rt)) {
            return wasm_trap_new_internal(
              wasm_runtime_get_exception(func->inst_comm_rt));
        }
        else {
            return wasm_trap_new_internal("wasm_func_call failed");
        }
    }
}

size_t
wasm_func_param_arity(const wasm_func_t *func)
{
    bh_assert(func && func->type && func->type->params);
    return func->type->params->num_elems;
}

size_t
wasm_func_result_arity(const wasm_func_t *func)
{
    bh_assert(func && func->type && func->type->results);
    return func->type->results->num_elems;
}

wasm_global_t *
wasm_global_new(wasm_store_t *store,
                const wasm_globaltype_t *global_type,
                const wasm_val_t *init)
{
    wasm_global_t *global = NULL;

    check_engine_and_store(singleton_engine, store);
    bh_assert(store && global_type && init);

    global = malloc_internal(sizeof(wasm_global_t));
    if (!global) {
        goto failed;
    }

    global->kind = WASM_EXTERN_GLOBAL;
    global->type = wasm_globaltype_copy(global_type);
    if (!global->type) {
        goto failed;
    }

    global->init = malloc_internal(sizeof(wasm_val_t));
    if (!global->init) {
        goto failed;
    }

    wasm_val_copy(global->init, init);
    /* TODO: how to check if above is failed */

    return global;

failed:
    LOG_DEBUG("%s failed", __FUNCTION__);
    wasm_global_delete(global);
    return NULL;
}

/* almost same with wasm_global_new */
wasm_global_t *
wasm_global_copy(const wasm_global_t *src)
{
    wasm_global_t *global = NULL;

    bh_assert(src);

    global = malloc_internal(sizeof(wasm_global_t));
    if (!global) {
        goto failed;
    }

    global->kind = WASM_EXTERN_GLOBAL;
    global->type = wasm_globaltype_copy(src->type);
    if (!global->type) {
        goto failed;
    }

    global->init = malloc_internal(sizeof(wasm_val_t));
    if (!global->init) {
        goto failed;
    }

    wasm_val_copy(global->init, src->init);

    global->global_idx_rt = src->global_idx_rt;
    global->inst_comm_rt = src->inst_comm_rt;

    return global;

failed:
    LOG_DEBUG("%s failed", __FUNCTION__);
    wasm_global_delete(global);
    return NULL;
}

void
wasm_global_delete(wasm_global_t *global)
{
    if (!global) {
        return;
    }

    if (global->init) {
        wasm_val_delete(global->init);
        global->init = NULL;
    }

    if (global->type) {
        wasm_globaltype_delete(global->type);
        global->type = NULL;
    }

    wasm_runtime_free(global);
}

bool
wasm_global_same(const wasm_global_t *g1, const wasm_global_t *g2)
{
    if (!g1 && !g2) {
        return true;
    }

    if (!g1 || !g2) {
        return false;
    }

    return g1->kind == g2->kind && wasm_globaltype_same(g1->type, g2->type)
           && wasm_val_same(g1->init, g2->init);
}

#if WASM_ENABLE_INTERP != 0
static bool
interp_global_set(const WASMModuleInstance *inst_interp,
                  uint16 global_idx_rt,
                  const wasm_val_t *v)
{
    const WASMGlobalInstance *global_interp =
      inst_interp->globals + global_idx_rt;
    uint8 val_type_rt = global_interp->type;
#if WASM_ENABLE_MULTI_MODULE != 0
    uint8 *data = global_interp->import_global_inst
                    ? global_interp->import_module_inst->global_data
                        + global_interp->import_global_inst->data_offset
                    : inst_interp->global_data + global_interp->data_offset;
#else
    uint8 *data = inst_interp->global_data + global_interp->data_offset;
#endif
    bool ret = true;

    switch (val_type_rt) {
        case VALUE_TYPE_I32:
            bh_assert(WASM_I32 == v->kind);
            *((int32 *)data) = v->of.i32;
            break;
        case VALUE_TYPE_F32:
            bh_assert(WASM_F32 == v->kind);
            *((float32 *)data) = v->of.f32;
            break;
        case VALUE_TYPE_I64:
            bh_assert(WASM_I64 == v->kind);
            *((int64 *)data) = v->of.i64;
            break;
        case VALUE_TYPE_F64:
            bh_assert(WASM_F64 == v->kind);
            *((float64 *)data) = v->of.f64;
            break;
        default:
            LOG_DEBUG("unexpected value type %d", val_type_rt);
            ret = false;
            break;
    }

    return ret;
}

static bool
interp_global_get(const WASMModuleInstance *inst_interp,
                  uint16 global_idx_rt,
                  wasm_val_t *out)
{
    WASMGlobalInstance *global_interp = inst_interp->globals + global_idx_rt;
    uint8 val_type_rt = global_interp->type;
#if WASM_ENABLE_MULTI_MODULE != 0
    uint8 *data = global_interp->import_global_inst
                    ? global_interp->import_module_inst->global_data
                        + global_interp->import_global_inst->data_offset
                    : inst_interp->global_data + global_interp->data_offset;
#else
    uint8 *data = inst_interp->global_data + global_interp->data_offset;
#endif
    bool ret = true;

    switch (val_type_rt) {
        case VALUE_TYPE_I32:
            out->kind = WASM_I32;
            out->of.i32 = *((int32 *)data);
            break;
        case VALUE_TYPE_F32:
            out->kind = WASM_F32;
            out->of.f32 = *((float32 *)data);
            break;
        case VALUE_TYPE_I64:
            out->kind = WASM_I64;
            out->of.i64 = *((int64 *)data);
            break;
        case VALUE_TYPE_F64:
            out->kind = WASM_F64;
            out->of.f64 = *((float64 *)data);
            break;
        default:
            LOG_DEBUG("unexpected value type %d", val_type_rt);
            ret = false;
    }
    return ret;
}
#endif

#if WASM_ENABLE_AOT != 0
static bool
aot_global_set(const AOTModuleInstance *inst_aot,
               uint16 global_idx_rt,
               const wasm_val_t *v)
{
    AOTModule *module_aot = inst_aot->aot_module.ptr;
    uint8 val_type_rt = 0;
    uint32 data_offset = 0;
    void *data = NULL;
    bool ret = true;

    if (global_idx_rt < module_aot->import_global_count) {
        data_offset = module_aot->import_globals[global_idx_rt].data_offset;
        val_type_rt = module_aot->import_globals[global_idx_rt].type;
    }
    else {
        data_offset =
          module_aot->globals[global_idx_rt - module_aot->import_global_count]
            .data_offset;
        val_type_rt =
          module_aot->globals[global_idx_rt - module_aot->import_global_count]
            .type;
    }

    data = (void *)((uint8 *)inst_aot->global_data.ptr + data_offset);
    switch (val_type_rt) {
        case VALUE_TYPE_I32:
            bh_assert(WASM_I32 == v->kind);
            *((int32 *)data) = v->of.i32;
            break;
        case VALUE_TYPE_F32:
            bh_assert(WASM_F32 == v->kind);
            *((float32 *)data) = v->of.f32;
            break;
        case VALUE_TYPE_I64:
            bh_assert(WASM_I64 == v->kind);
            *((int64 *)data) = v->of.i64;
            break;
        case VALUE_TYPE_F64:
            bh_assert(WASM_F64 == v->kind);
            *((float64 *)data) = v->of.f64;
            break;
        default:
            LOG_DEBUG("unexpected value type %d", val_type_rt);
            ret = false;
    }
    return ret;
}

static bool
aot_global_get(const AOTModuleInstance *inst_aot,
               uint16 global_idx_rt,
               wasm_val_t *out)
{
    AOTModule *module_aot = inst_aot->aot_module.ptr;
    uint8 val_type_rt = 0;
    uint32 data_offset = 0;
    void *data = NULL;
    bool ret = true;

    if (global_idx_rt < module_aot->import_global_count) {
        data_offset = module_aot->import_globals[global_idx_rt].data_offset;
        val_type_rt = module_aot->import_globals[global_idx_rt].type;
    }
    else {
        data_offset =
          module_aot->globals[global_idx_rt - module_aot->import_global_count]
            .data_offset;
        val_type_rt =
          module_aot->globals[global_idx_rt - module_aot->import_global_count]
            .type;
    }

    data = (void *)((uint8 *)inst_aot->global_data.ptr + data_offset);
    switch (val_type_rt) {
        case VALUE_TYPE_I32:
            out->kind = WASM_I32;
            out->of.i32 = *((int32 *)data);
            break;
        case VALUE_TYPE_F32:
            out->kind = WASM_F32;
            out->of.f32 = *((float32 *)data);
            break;
        case VALUE_TYPE_I64:
            out->kind = WASM_I64;
            out->of.i64 = *((int64 *)data);
            break;
        case VALUE_TYPE_F64:
            out->kind = WASM_F64;
            out->of.f64 = *((float64 *)data);
            break;
        default:
            LOG_DEBUG("unexpected value type %d", val_type_rt);
            ret = false;
    }
    return ret;
}
#endif

void
wasm_global_set(wasm_global_t *global, const wasm_val_t *v)
{
    bh_assert(global && v);

    if (INTERP_MODE == current_runtime_mode()) {
#if WASM_ENABLE_INTERP != 0
        (void)interp_global_set((WASMModuleInstance *)global->inst_comm_rt,
                                global->global_idx_rt, v);
#endif
    }
    else {
#if WASM_ENABLE_AOT != 0
        (void)aot_global_set((AOTModuleInstance *)global->inst_comm_rt,
                             global->global_idx_rt, v);
#endif
    }
}

void
wasm_global_get(const wasm_global_t *global, wasm_val_t *out)
{
    bh_assert(global && out);

    memset(out, 0, sizeof(wasm_val_t));

    if (INTERP_MODE == current_runtime_mode()) {
#if WASM_ENABLE_INTERP != 0
        (void)interp_global_get((WASMModuleInstance *)global->inst_comm_rt,
                                global->global_idx_rt, out);
#endif
    }
    else {
#if WASM_ENABLE_AOT != 0
        (void)aot_global_get((AOTModuleInstance *)global->inst_comm_rt,
                             global->global_idx_rt, out);
#endif
    }

    bh_assert(global->init->kind == out->kind);
}

static wasm_global_t *
wasm_global_new_internal(wasm_store_t *store,
                         uint16 global_idx_rt,
                         WASMModuleInstanceCommon *inst_comm_rt)
{
    wasm_global_t *global = NULL;
    uint8 val_type_rt = 0;
    bool is_mutable = 0;

    check_engine_and_store(singleton_engine, store);
    bh_assert(inst_comm_rt);

    global = malloc_internal(sizeof(wasm_global_t));
    if (!global) {
        goto failed;
    }

    /*
     * global->module_name = NULL;
     * global->name = NULL;
     */
    global->kind = WASM_EXTERN_GLOBAL;

    if (INTERP_MODE == current_runtime_mode()) {
#if WASM_ENABLE_INTERP != 0
        WASMGlobalInstance *global_interp =
          ((WASMModuleInstance *)inst_comm_rt)->globals + global_idx_rt;
        val_type_rt = global_interp->type;
        is_mutable = global_interp->is_mutable;
#endif
    }
    else {
#if WASM_ENABLE_AOT != 0
        AOTModuleInstance *inst_aot = (AOTModuleInstance *)inst_comm_rt;
        AOTModule *module_aot = inst_aot->aot_module.ptr;
        if (global_idx_rt < module_aot->import_global_count) {
            AOTImportGlobal *global_import_aot =
              module_aot->import_globals + global_idx_rt;
            val_type_rt = global_import_aot->type;
            is_mutable = global_import_aot->is_mutable;
        }
        else {
            AOTGlobal *global_aot =
              module_aot->globals
              + (global_idx_rt - module_aot->import_global_count);
            val_type_rt = global_aot->type;
            is_mutable = global_aot->is_mutable;
        }
#endif
    }

    global->type = wasm_globaltype_new_internal(val_type_rt, is_mutable);
    if (!global->type) {
        goto failed;
    }

    global->init = malloc_internal(sizeof(wasm_val_t));
    if (!global->init) {
        goto failed;
    }

    if (INTERP_MODE == current_runtime_mode()) {
#if WASM_ENABLE_INTERP != 0
        interp_global_get((WASMModuleInstance *)inst_comm_rt, global_idx_rt,
                          global->init);
#endif
    }
    else {
#if WASM_ENABLE_AOT != 0
        aot_global_get((AOTModuleInstance *)inst_comm_rt, global_idx_rt,
                       global->init);
#endif
    }

    global->inst_comm_rt = inst_comm_rt;
    global->global_idx_rt = global_idx_rt;

    return global;

failed:
    LOG_DEBUG("%s failed", __FUNCTION__);
    wasm_global_delete(global);
    return NULL;
}

wasm_globaltype_t *
wasm_global_type(const wasm_global_t *global)
{
    bh_assert(global);
    return wasm_globaltype_copy(global->type);
}

static wasm_table_t *
wasm_table_new_basic(const wasm_tabletype_t *type)
{
    wasm_table_t *table = NULL;

    bh_assert(type);

    if (!(table = malloc_internal(sizeof(wasm_table_t)))) {
        goto failed;
    }

    table->kind = WASM_EXTERN_TABLE;

    if (!(table->type = wasm_tabletype_copy(type))) {
        goto failed;
    }

    RETURN_OBJ(table, wasm_table_delete);
}

static wasm_table_t *
wasm_table_new_internal(wasm_store_t *store,
                        uint16 table_idx_rt,
                        WASMModuleInstanceCommon *inst_comm_rt)
{
    wasm_table_t *table = NULL;
    uint8 val_type_rt = 0;
    uint32 init_size = 0, max_size = 0;

    check_engine_and_store(singleton_engine, store);
    bh_assert(inst_comm_rt);

    if (!(table = malloc_internal(sizeof(wasm_table_t)))) {
        goto failed;
    }

    table->kind = WASM_EXTERN_TABLE;

    if (INTERP_MODE == current_runtime_mode()) {
#if WASM_ENABLE_INTERP != 0
        WASMTableInstance *table_interp =
          ((WASMModuleInstance *)inst_comm_rt)->tables[table_idx_rt];
        val_type_rt = table_interp->elem_type;
        init_size = table_interp->cur_size;
        max_size = table_interp->max_size;
#endif
    }
    else {
#if WASM_ENABLE_AOT != 0
        /* TODO */
#endif
    }

    if (!(table->type =
            wasm_tabletype_new_internal(val_type_rt, init_size, max_size))) {
        goto failed;
    }

    table->inst_comm_rt = inst_comm_rt;
    table->table_idx_rt = table_idx_rt;

    RETURN_OBJ(table, wasm_table_delete);
}

wasm_table_t *
wasm_table_new(wasm_store_t *store,
               const wasm_tabletype_t *table_type,
               wasm_ref_t *init)
{
    (void)init;
    check_engine_and_store(singleton_engine, store);
    return wasm_table_new_basic(table_type);
}

wasm_table_t *
wasm_table_copy(const wasm_table_t *src)
{
    return wasm_table_new_basic(src->type);
}

void
wasm_table_delete(wasm_table_t *table)
{
    if (!table) {
        return;
    }

    if (table->type) {
        wasm_tabletype_delete(table->type);
        table->type = NULL;
    }

    wasm_runtime_free(table);
}

wasm_tabletype_t *
wasm_table_type(const wasm_table_t *table)
{
    bh_assert(table);
    return wasm_tabletype_copy(table->type);
}

static wasm_memory_t *
wasm_memory_new_basic(const wasm_memorytype_t *type)
{
    wasm_memory_t *memory = NULL;

    bh_assert(type);

    if (!(memory = malloc_internal(sizeof(wasm_memory_t)))) {
        goto failed;
    }

    memory->kind = WASM_EXTERN_MEMORY;
    memory->type = wasm_memorytype_copy(type);

    RETURN_OBJ(memory, wasm_memory_delete)
}

wasm_memory_t *
wasm_memory_new(wasm_store_t *store, const wasm_memorytype_t *type)
{
    check_engine_and_store(singleton_engine, store);
    return wasm_memory_new_basic(type);
}

wasm_memory_t *
wasm_memory_copy(const wasm_memory_t *src)
{
    wasm_memory_t *dst = NULL;

    bh_assert(src);

    if (!(dst = wasm_memory_new_basic(src->type))) {
        goto failed;
    }

    dst->memory_idx_rt = src->memory_idx_rt;
    dst->inst_comm_rt = src->inst_comm_rt;

    RETURN_OBJ(dst, wasm_memory_delete)
}

static wasm_memory_t *
wasm_memory_new_internal(wasm_store_t *store,
                         uint16 memory_idx_rt,
                         WASMModuleInstanceCommon *inst_comm_rt)
{
    wasm_memory_t *memory = NULL;
    uint32 min_pages = 0, max_pages = 0;

    check_engine_and_store(singleton_engine, store);
    bh_assert(inst_comm_rt);

    if (!(memory = malloc_internal(sizeof(wasm_memory_t)))) {
        goto failed;
    }

    memory->kind = WASM_EXTERN_MEMORY;

    if (INTERP_MODE == current_runtime_mode()) {
#if WASM_ENABLE_INTERP != 0
        WASMMemoryInstance *memory_interp =
          ((WASMModuleInstance *)inst_comm_rt)->memories[memory_idx_rt];
        min_pages = memory_interp->cur_page_count;
        max_pages = memory_interp->max_page_count;
#endif
    }
    else {
#if WASM_ENABLE_AOT != 0
        /* TODO */
#endif
    }

    if (!(memory->type = wasm_memorytype_new_internal(min_pages, max_pages))) {
        goto failed;
    }

    memory->inst_comm_rt = inst_comm_rt;
    memory->memory_idx_rt = memory_idx_rt;

    RETURN_OBJ(memory, wasm_memory_delete);
}

void
wasm_memory_delete(wasm_memory_t *memory)
{
    if (!memory) {
        return;
    }

    if (memory->type) {
        wasm_memorytype_delete(memory->type);
        memory->type = NULL;
    }

    wasm_runtime_free(memory);
}

wasm_memorytype_t *
wasm_memory_type(const wasm_memory_t *memory)
{
    bh_assert(memory);
    return wasm_memorytype_copy(memory->type);
}

byte_t *
wasm_memory_data(wasm_memory_t *memory)
{
    return (byte_t *)wasm_runtime_get_memory_data(memory->inst_comm_rt,
                                                  memory->memory_idx_rt);
}

size_t
wasm_memory_data_size(const wasm_memory_t *memory)
{
    return wasm_runtime_get_memory_data_size(memory->inst_comm_rt,
                                             memory->memory_idx_rt);
}

#if WASM_ENABLE_INTERP != 0
static bool
interp_link_func(const wasm_instance_t *inst,
                 const WASMModule *module_interp,
                 uint16 func_idx_rt,
                 wasm_func_t *import)
{
    WASMImport *imported_func_interp = NULL;
    wasm_func_t *cloned = NULL;

    bh_assert(inst && module_interp && import);
    bh_assert(func_idx_rt < module_interp->import_function_count);
    bh_assert(WASM_EXTERN_FUNC == import->kind);

    imported_func_interp = module_interp->import_functions + func_idx_rt;
    bh_assert(imported_func_interp);

    if (!(cloned = wasm_func_copy(import))) {
        return false;
    }

    if (!bh_vector_append((Vector *)inst->imports, &cloned)) {
        wasm_func_delete(cloned);
        return false;
    }

    /* add native_func_trampoline as a NativeSymbol */
    imported_func_interp->u.function.call_conv_raw = true;
    imported_func_interp->u.function.attachment = cloned;
    imported_func_interp->u.function.func_ptr_linked = native_func_trampoline;
    import->func_idx_rt = func_idx_rt;

    return true;
}

static bool
interp_link_global(const WASMModule *module_interp,
                   uint16 global_idx_rt,
                   wasm_global_t *import)
{
    WASMImport *imported_global_interp = NULL;

    bh_assert(module_interp && import);
    bh_assert(global_idx_rt < module_interp->import_global_count);
    bh_assert(WASM_EXTERN_GLOBAL == import->kind);

    imported_global_interp = module_interp->import_globals + global_idx_rt;
    bh_assert(imported_global_interp);

    /* set init value */
    switch (wasm_valtype_kind(import->type->val_type)) {
        case WASM_I32:
            bh_assert(VALUE_TYPE_I32 == imported_global_interp->u.global.type);
            imported_global_interp->u.global.global_data_linked.i32 =
              import->init->of.i32;
            break;
        case WASM_I64:
            bh_assert(VALUE_TYPE_I64 == imported_global_interp->u.global.type);
            imported_global_interp->u.global.global_data_linked.i64 =
              import->init->of.i64;
            break;
        case WASM_F32:
            bh_assert(VALUE_TYPE_F32 == imported_global_interp->u.global.type);
            imported_global_interp->u.global.global_data_linked.f32 =
              import->init->of.f32;
            break;
        case WASM_F64:
            bh_assert(VALUE_TYPE_F64 == imported_global_interp->u.global.type);
            imported_global_interp->u.global.global_data_linked.f64 =
              import->init->of.f64;
            break;
        default:
            return false;
    }

    import->global_idx_rt = global_idx_rt;
    imported_global_interp->u.global.is_linked = true;
    return true;
}

static uint32
interp_link(const wasm_instance_t *inst,
            const WASMModule *module_interp,
            wasm_extern_t *imports[])
{
    uint32 i = 0;
    uint32 import_func_i = 0;
    uint32 import_global_i = 0;

    bh_assert(inst && module_interp && imports);

    for (i = 0; i < module_interp->import_count; ++i) {
        wasm_extern_t *import = imports[i];
        WASMImport *import_rt = module_interp->imports + i;

        switch (import_rt->kind) {
            case IMPORT_KIND_FUNC:
            {
                if (!interp_link_func(inst, module_interp, import_func_i++,
                                      wasm_extern_as_func(import))) {
                    goto failed;
                }

                break;
            }
            case IMPORT_KIND_GLOBAL:
            {
                if (!interp_link_global(module_interp, import_global_i++,
                                        wasm_extern_as_global(import))) {
                    goto failed;
                }

                break;
            }
            case IMPORT_KIND_MEMORY:
            case IMPORT_KIND_TABLE:
                ASSERT_NOT_IMPLEMENTED();
                break;
            default:
                LOG_WARNING("%s meets unsupported kind: %d", __FUNCTION__,
                            import_rt->kind);
                goto failed;
        }
    }

    return i;

failed:
    LOG_DEBUG("%s failed", __FUNCTION__);
    return (uint32)-1;
}

static bool
interp_process_export(wasm_store_t *store,
                      const WASMModuleInstance *inst_interp,
                      wasm_extern_vec_t *externals)
{
    WASMExport *exports = NULL;
    WASMExport *export = NULL;
    wasm_extern_t *external = NULL;
    uint32 export_cnt = 0;
    uint32 i = 0;

    bh_assert(store && inst_interp && inst_interp->module && externals);

    exports = inst_interp->module->exports;
    export_cnt = inst_interp->module->export_count;

    for (i = 0; i < export_cnt; ++i) {
        export = exports + i;

        switch (export->kind) {
            case EXPORT_KIND_FUNC:
            {
                wasm_func_t *func;
                if (!(func = wasm_func_new_internal(
                        store, export->index,
                        (WASMModuleInstanceCommon *)inst_interp))) {
                    goto failed;
                }

                external = wasm_func_as_extern(func);
                break;
            }
            case EXPORT_KIND_GLOBAL:
            {
                wasm_global_t *global;
                if (!(global = wasm_global_new_internal(
                        store, export->index,
                        (WASMModuleInstanceCommon *)inst_interp))) {
                    goto failed;
                }

                external = wasm_global_as_extern(global);
                break;
            }
            case EXPORT_KIND_TABLE:
            {
                wasm_table_t *table;
                if (!(table = wasm_table_new_internal(
                        store, export->index,
                        (WASMModuleInstanceCommon *)inst_interp))) {
                    goto failed;
                }

                external = wasm_table_as_extern(table);
                break;
            }
            case EXPORT_KIND_MEMORY:
            {
                wasm_memory_t *memory;
                if (!(memory = wasm_memory_new_internal(
                        store, export->index,
                        (WASMModuleInstanceCommon *)inst_interp))) {
                    goto failed;
                }

                external = wasm_memory_as_extern(memory);
                break;
            }
            default:
                LOG_WARNING("%s meets unsupported kind: %d", __FUNCTION__,
                            export->kind);
                goto failed;
        }

        if (!bh_vector_append((Vector *)externals, &external)) {
            goto failed;
        }
    }

    return true;

failed:
    LOG_DEBUG("%s failed", __FUNCTION__);
    return false;
}
#endif /* WASM_ENABLE_INTERP */

#if WASM_ENABLE_AOT != 0
static bool
aot_link_func(const wasm_instance_t *inst,
              const AOTModule *module_aot,
              uint32 import_func_idx_rt,
              wasm_func_t *import)
{
    AOTImportFunc *import_aot_func = NULL;
    wasm_func_t *cloned = NULL;

    bh_assert(inst && module_aot && import);

    import_aot_func = module_aot->import_funcs + import_func_idx_rt;
    bh_assert(import_aot_func);

    if (!(cloned = wasm_func_copy(import))) {
        return false;
    }

    import_aot_func->call_conv_raw = true;
    import_aot_func->attachment = cloned;
    import_aot_func->func_ptr_linked = native_func_trampoline;
    import->func_idx_rt = import_func_idx_rt;

    return true;
}

static bool
aot_link_global(const AOTModule *module_aot,
                uint16 global_idx_rt,
                wasm_global_t *import)
{
    AOTImportGlobal *import_aot_global = NULL;
    const wasm_valtype_t *val_type = NULL;

    bh_assert(module_aot && import);

    import_aot_global = module_aot->import_globals + global_idx_rt;
    bh_assert(import_aot_global);

    //TODO: import->type ?
    val_type = wasm_globaltype_content(wasm_global_type(import));
    bh_assert(val_type);

    switch (wasm_valtype_kind(val_type)) {
        case WASM_I32:
            bh_assert(VALUE_TYPE_I32 == import_aot_global->type);
            import_aot_global->global_data_linked.i32 = import->init->of.i32;
            break;
        case WASM_I64:
            bh_assert(VALUE_TYPE_I64 == import_aot_global->type);
            import_aot_global->global_data_linked.i64 = import->init->of.i64;
            break;
        case WASM_F32:
            bh_assert(VALUE_TYPE_F32 == import_aot_global->type);
            import_aot_global->global_data_linked.f32 = import->init->of.f32;
            break;
        case WASM_F64:
            bh_assert(VALUE_TYPE_F64 == import_aot_global->type);
            import_aot_global->global_data_linked.f64 = import->init->of.f64;
            break;
        default:
            goto failed;
    }

    import->global_idx_rt = global_idx_rt;
    return true;

failed:
    LOG_DEBUG("%s failed", __FUNCTION__);
    return false;
}

static uint32
aot_link(const wasm_instance_t *inst,
         const AOTModule *module_aot,
         wasm_extern_t *imports[])
{
    uint32 i = 0;
    uint32 import_func_i = 0;
    uint32 import_global_i = 0;
    wasm_extern_t *import = NULL;
    wasm_func_t *func = NULL;
    wasm_global_t *global = NULL;

    bh_assert(inst && module_aot && imports);

    while (import_func_i < module_aot->import_func_count
           || import_global_i < module_aot->import_global_count) {
        import = imports[i++];

        bh_assert(import);

        switch (wasm_extern_kind(import)) {
            case WASM_EXTERN_FUNC:
                bh_assert(import_func_i < module_aot->import_func_count);
                func = wasm_extern_as_func((wasm_extern_t *)import);
                if (!aot_link_func(inst, module_aot, import_func_i++, func)) {
                    goto failed;
                }

                break;
            case WASM_EXTERN_GLOBAL:
                bh_assert(import_global_i < module_aot->import_global_count);
                global = wasm_extern_as_global((wasm_extern_t *)import);
                if (!aot_link_global(module_aot, import_global_i++, global)) {
                    goto failed;
                }

                break;
            case WASM_EXTERN_MEMORY:
            case WASM_EXTERN_TABLE:
                ASSERT_NOT_IMPLEMENTED();
                break;
            default:
                goto failed;
        }
    }

    return i;

failed:
    LOG_DEBUG("%s failed", __FUNCTION__);
    return (uint32)-1;
}

static bool
aot_process_export(wasm_store_t *store,
                   const AOTModuleInstance *inst_aot,
                   wasm_extern_vec_t *externals)
{
    uint32 i = 0;
    uint32 export_func_i = 0;
    wasm_extern_t *external = NULL;
    AOTModule *module_aot = NULL;

    bh_assert(store && inst_aot && externals);

    module_aot = (AOTModule *)inst_aot->aot_module.ptr;
    bh_assert(module_aot);

    for (i = 0; i < module_aot->export_count; ++i) {
        AOTExport *export = module_aot->exports + i;
        wasm_func_t *func = NULL;
        wasm_global_t *global = NULL;

        switch (export->kind) {
            case EXPORT_KIND_FUNC:
                func =
                  wasm_func_new_internal(store, export_func_i++,
                                         (WASMModuleInstanceCommon *)inst_aot);
                if (!func) {
                    goto failed;
                }

                external = wasm_func_as_extern(func);
                break;
            case EXPORT_KIND_GLOBAL:
                global = wasm_global_new_internal(
                  store, export->index, (WASMModuleInstanceCommon *)inst_aot);
                if (!global) {
                    goto failed;
                }

                external = wasm_global_as_extern(global);
                break;
            case EXPORT_KIND_MEMORY:
            case EXPORT_KIND_TABLE:
                break;
            default:
                LOG_WARNING("%s meets unsupported kind: %d", __FUNCTION__,
                            export->kind);
                goto failed;
        }

        if (!bh_vector_append((Vector *)externals, &external)) {
            goto failed;
        }
    }

    return true;

failed:
    LOG_DEBUG("%s failed", __FUNCTION__);
    return false;
}
#endif /* WASM_ENABLE_AOT */

wasm_instance_t *
wasm_instance_new(wasm_store_t *store,
                  const wasm_module_t *module,
                  const wasm_extern_t *const imports[],
                  own wasm_trap_t **traps)
{
    char error[128] = { 0 };
    const uint32 stack_size = 16 * 1024;
    const uint32 heap_size = 16 * 1024;
    uint32 import_count = 0;
    wasm_instance_t *instance = NULL;
    uint32 i = 0;
    (void)traps;

    check_engine_and_store(singleton_engine, store);

    instance = malloc_internal(sizeof(wasm_instance_t));
    if (!instance) {
        goto failed;
    }

    /* link module and imports */
    if (imports) {
        if (INTERP_MODE == current_runtime_mode()) {
#if WASM_ENABLE_INTERP != 0
            import_count = MODULE_RUNTIME(module)->import_count;

            INIT_VEC(instance->imports, wasm_extern_vec_new_uninitialized,
                     import_count);

            if (import_count) {
                uint32 actual_link_import_count = interp_link(
                  instance, MODULE_RUNTIME(module), (wasm_extern_t **)imports);
                /* make sure a complete import list */
                if ((int32)import_count < 0
                    || import_count != actual_link_import_count) {
                    goto failed;
                }
            }
#endif
        }
        else {
#if WASM_ENABLE_AOT != 0
            import_count = MODULE_RUNTIME(module)->import_func_count
                           + MODULE_RUNTIME(module)->import_global_count
                           + MODULE_RUNTIME(module)->import_memory_count
                           + MODULE_RUNTIME(module)->import_table_count;

            INIT_VEC(instance->imports, wasm_extern_vec_new_uninitialized,
                     import_count);

            if (import_count) {
                import_count = aot_link(instance, MODULE_RUNTIME(module),
                                        (wasm_extern_t **)imports);
                if ((int32)import_count < 0) {
                    goto failed;
                }
            }
#endif
        }
    }

    instance->inst_comm_rt = wasm_runtime_instantiate(
      *module, stack_size, heap_size, error, sizeof(error));
    if (!instance->inst_comm_rt) {
        LOG_ERROR(error);
        goto failed;
    }

    /* fill with inst */
    for (i = 0; imports && i < (uint32)import_count; ++i) {
        wasm_extern_t *import = (wasm_extern_t *)imports[i];
        switch (import->kind) {
            case WASM_EXTERN_FUNC:
                wasm_extern_as_func(import)->inst_comm_rt =
                  instance->inst_comm_rt;
                break;
            case WASM_EXTERN_GLOBAL:
                wasm_extern_as_global(import)->inst_comm_rt =
                  instance->inst_comm_rt;
                break;
            case WASM_EXTERN_MEMORY:
                wasm_extern_as_memory(import)->inst_comm_rt =
                  instance->inst_comm_rt;
                break;
            case WASM_EXTERN_TABLE:
                wasm_extern_as_table(import)->inst_comm_rt =
                  instance->inst_comm_rt;
                break;
            default:
                goto failed;
        }
    }

    /* build the exports list */
    if (INTERP_MODE == current_runtime_mode()) {
#if WASM_ENABLE_INTERP != 0
        uint32 export_cnt =
          ((WASMModuleInstance *)instance->inst_comm_rt)->module->export_count;

        INIT_VEC(instance->exports, wasm_extern_vec_new_uninitialized,
                 export_cnt);

        if (!interp_process_export(
              store, (WASMModuleInstance *)instance->inst_comm_rt,
              instance->exports)) {
            goto failed;
        }
#endif
    }
    else {
#if WASM_ENABLE_AOT != 0
        uint32 export_cnt =
          ((AOTModuleInstance *)instance->inst_comm_rt)->export_func_count;

        INIT_VEC(instance->exports, wasm_extern_vec_new_uninitialized,
                 export_cnt);

        if (!aot_process_export(store,
                                (AOTModuleInstance *)instance->inst_comm_rt,
                                instance->exports)) {
            goto failed;
        }
#endif
    }

    /* add it to a watching list in store */
    if (!bh_vector_append((Vector *)store->instances, &instance)) {
        goto failed;
    }

    return instance;

failed:
    LOG_DEBUG("%s failed", __FUNCTION__);
    wasm_instance_delete_internal(instance);
    return NULL;
}

static void
wasm_instance_delete_internal(wasm_instance_t *instance)
{
    if (!instance) {
        return;
    }

    DEINIT_VEC(instance->imports, wasm_extern_vec_delete);
    DEINIT_VEC(instance->exports, wasm_extern_vec_delete);

    if (instance->inst_comm_rt) {
        wasm_runtime_deinstantiate(instance->inst_comm_rt);
        instance->inst_comm_rt = NULL;
    }
    wasm_runtime_free(instance);
}

void
wasm_instance_delete(wasm_instance_t *module)
{
    /* will release instance when releasing the store */
}

void
wasm_instance_exports(const wasm_instance_t *instance,
                      own wasm_extern_vec_t *out)
{
    bh_assert(instance && out);
    wasm_extern_vec_copy(out, instance->exports);
}

wasm_extern_t *
wasm_extern_copy(const wasm_extern_t *src)
{
    wasm_extern_t *dst = NULL;
    bh_assert(src);

    switch (wasm_extern_kind(src)) {
        case WASM_EXTERN_FUNC:
            dst = wasm_func_as_extern(
              wasm_func_copy(wasm_extern_as_func_const(src)));
            break;
        case WASM_EXTERN_GLOBAL:
            dst = wasm_global_as_extern(
              wasm_global_copy(wasm_extern_as_global_const(src)));
            break;
        case WASM_EXTERN_MEMORY:
            dst = wasm_memory_as_extern(
              wasm_memory_copy(wasm_extern_as_memory_const(src)));
            break;
        case WASM_EXTERN_TABLE:
            dst = wasm_table_as_extern(
              wasm_table_copy(wasm_extern_as_table_const(src)));
            break;
        default:
            LOG_WARNING("%s meets unsupported kind: %d", __FUNCTION__,
                        src->kind);
            break;
    }

    if (!dst) {
        goto failed;
    }

    return dst;

failed:
    LOG_DEBUG("%s failed", __FUNCTION__);
    wasm_extern_delete(dst);
    return NULL;
}

void
wasm_extern_delete(wasm_extern_t *external)
{
    if (!external) {
        return;
    }

    switch (wasm_extern_kind(external)) {
        case WASM_EXTERN_FUNC:
            wasm_func_delete(wasm_extern_as_func(external));
            break;
        case WASM_EXTERN_GLOBAL:
            wasm_global_delete(wasm_extern_as_global(external));
            break;
        case WASM_EXTERN_MEMORY:
            wasm_memory_delete(wasm_extern_as_memory(external));
            break;
        case WASM_EXTERN_TABLE:
            wasm_table_delete(wasm_extern_as_table(external));
            break;
        default:
            LOG_WARNING("%s meets unsupported kind: %d", __FUNCTION__,
                        external->kind);
            break;
    }
}

wasm_externkind_t
wasm_extern_kind(const wasm_extern_t *extrenal)
{
    return extrenal->kind;
}

own wasm_externtype_t *
wasm_extern_type(const wasm_extern_t *external)
{
    switch (wasm_extern_kind(external)) {
        case WASM_EXTERN_FUNC:
            return wasm_functype_as_externtype(
              wasm_func_type(wasm_extern_as_func_const(external)));
        case WASM_EXTERN_GLOBAL:
            return wasm_globaltype_as_externtype(
              wasm_global_type(wasm_extern_as_global_const(external)));
        case WASM_EXTERN_MEMORY:
            return wasm_memorytype_as_externtype(
              wasm_memory_type(wasm_extern_as_memory_const(external)));
        case WASM_EXTERN_TABLE:
            return wasm_tabletype_as_externtype(
              wasm_table_type(wasm_extern_as_table_const(external)));
        default:
            LOG_WARNING("%s meets unsupported kind: %d", __FUNCTION__,
                        external->kind);
            break;
    }
    return NULL;
}

#define BASIC_FOUR_LIST(V)                                                    \
    V(func)                                                                   \
    V(global)                                                                 \
    V(memory)                                                                 \
    V(table)

#define WASM_EXTERN_AS_OTHER(name)                                            \
    wasm_##name##_t *wasm_extern_as_##name(wasm_extern_t *external)           \
    {                                                                         \
        return (wasm_##name##_t *)external;                                   \
    }

BASIC_FOUR_LIST(WASM_EXTERN_AS_OTHER)
#undef WASM_EXTERN_AS_OTHER

#define WASM_OTHER_AS_EXTERN(name)                                            \
    wasm_extern_t *wasm_##name##_as_extern(wasm_##name##_t *other)            \
    {                                                                         \
        return (wasm_extern_t *)other;                                        \
    }

BASIC_FOUR_LIST(WASM_OTHER_AS_EXTERN)
#undef WASM_OTHER_AS_EXTERN

#define WASM_EXTERN_AS_OTHER_CONST(name)                                      \
    const wasm_##name##_t *wasm_extern_as_##name##_const(                     \
      const wasm_extern_t *external)                                          \
    {                                                                         \
        return (const wasm_##name##_t *)external;                             \
    }

BASIC_FOUR_LIST(WASM_EXTERN_AS_OTHER_CONST)
#undef WASM_EXTERN_AS_OTHER_CONST

#define WASM_OTHER_AS_EXTERN_CONST(name)                                      \
    const wasm_extern_t *wasm_##name##_as_extern_const(                       \
      const wasm_##name##_t *other)                                           \
    {                                                                         \
        return (const wasm_extern_t *)other;                                  \
    }

BASIC_FOUR_LIST(WASM_OTHER_AS_EXTERN_CONST)
#undef WASM_OTHER_AS_EXTERN_CONST
