/*
 * Copyright (C) 2019 Intel Corporation. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include "aot_compiler.h"
#include "../aot/aot_runtime.h"

#define PUT_U64_TO_ADDR(addr, value) do {       \
    union { uint64 val; uint32 parts[2]; } u;   \
    u.val = (value);                            \
    ((uint32*)(addr))[0] = u.parts[0];          \
    ((uint32*)(addr))[1] = u.parts[1];          \
  } while (0)

#define CHECK_SIZE(size)    do {                         \
    if (size == (uint32)-1) {                            \
        aot_set_last_error("get symbol size failed.");   \
        return (uint32)-1;                               \
    }                                                    \
  } while (0)

/* Internal function in object file */
typedef struct AOTObjectFunc {
    char *func_name;
    uint64 text_offset;
} AOTObjectFunc;

/* Symbol table list node */
typedef struct AOTSymbolNode {
    struct AOTSymbolNode *next;
    uint32 str_len;
    char *symbol;
} AOTSymbolNode;

typedef struct AOTSymbolList {
    AOTSymbolNode *head;
    AOTSymbolNode *end;
    uint32 len;
} AOTSymbolList;

/* AOT object data */
typedef struct AOTObjectData {
    LLVMMemoryBufferRef mem_buf;
    LLVMBinaryRef binary;

    AOTTargetInfo target_info;

    void *text;
    uint32 text_size;

    /* literal data and size */
    void *literal;
    uint32 literal_size;

    AOTObjectDataSection *data_sections;
    uint32 data_sections_count;

    AOTObjectFunc *funcs;
    uint32 func_count;

    AOTSymbolList symbol_list;
    AOTRelocationGroup *relocation_groups;
    uint32 relocation_group_count;
} AOTObjectData;

#if 0
static void dump_buf(uint8 *buf, uint32 size, char *title)
{
    int i;
    printf("------ %s -------", title);
    for (i = 0; i < size; i++) {
        if ((i % 16) == 0)
            printf("\n");
        printf("%02x ", (unsigned char)buf[i]);
    }
    printf("\n\n");
}
#endif

static bool
is_32bit_binary(LLVMBinaryRef binary)
{
    LLVMBinaryType type = LLVMBinaryGetType(binary);
    return (type == LLVMBinaryTypeELF32L || type == LLVMBinaryTypeELF32B);
}

static bool
is_little_endian_binary(LLVMBinaryRef binary)
{
    LLVMBinaryType type = LLVMBinaryGetType(binary);
    return (type == LLVMBinaryTypeELF32L || type == LLVMBinaryTypeELF64L);
}

static bool
str_starts_with(const char *str, const char *prefix)
{
    size_t len_pre = strlen(prefix), len_str = strlen(str);
    return (len_str >= len_pre) && !memcmp(str, prefix, len_pre);
}

static uint32
get_file_header_size()
{
    /* magic number (4 bytes) + version (4 bytes) */
    return sizeof(uint32) + sizeof(uint32);
}

static uint32
get_string_size(const char *s)
{
    /* string size (2 bytes) + string content without '\0' */
    return (uint32)sizeof(uint16) + (uint32)strlen(s);
}

static uint32
get_target_info_section_size()
{
    return sizeof(AOTTargetInfo);
}

static uint32
get_mem_init_data_size(AOTMemInitData *mem_init_data)
{
    /* init expr type (4 bytes) + init expr value (8 bytes)
       + byte count (4 bytes) + bytes */
    uint32 total_size =
        (uint32)(sizeof(uint32) + sizeof(uint64)
                 + sizeof(uint32) + mem_init_data->byte_count);

    /* bulk_memory enabled:
        is_passive (4 bytes) + memory_index (4 bytes)
       bulk memory disabled:
        placeholder (4 bytes) + placeholder (4 bytes)
    */
    total_size += (sizeof(uint32) + sizeof(uint32));

    return total_size;
}

static uint32
get_mem_init_data_list_size(AOTMemInitData **mem_init_data_list,
                            uint32 mem_init_data_count)
{
    AOTMemInitData **mem_init_data = mem_init_data_list;
    uint32 size = 0, i;

    for (i = 0; i < mem_init_data_count; i++, mem_init_data++) {
        size = align_uint(size, 4);
        size += get_mem_init_data_size(*mem_init_data);
    }
    return size;
}

static uint32
get_import_memory_size(AOTCompData *comp_data)
{
    /* currently we only emit import_memory_count = 0 */
    return sizeof(uint32);
}

static uint32
get_memory_size(AOTCompData *comp_data)
{
    /* memory_count + count * (memory_flags + num_bytes_per_page +
                               init_page_count + max_page_count) */
    return (uint32)(sizeof(uint32)
                    + comp_data->memory_count * sizeof(uint32) * 4);
}

static uint32
get_mem_info_size(AOTCompData *comp_data)
{
    /* import_memory_size + memory_size
       + init_data_count + init_data_list */
    return get_import_memory_size(comp_data)
           + get_memory_size(comp_data)
           + (uint32)sizeof(uint32)
           + get_mem_init_data_list_size(comp_data->mem_init_data_list,
                                         comp_data->mem_init_data_count);
}

static uint32
get_table_init_data_size(AOTTableInitData *table_init_data)
{
    /*
     * mode (4 bytes), elem_type (4 bytes), do not need is_dropped field
     *
     * table_index(4 bytes) + init expr type (4 bytes) + init expr value (8 bytes)
     * + func index count (4 bytes) + func indexes
     */
    return (uint32)(sizeof(uint32) * 2 + sizeof(uint32) + sizeof(uint32)
                    + sizeof(uint64) + sizeof(uint32)
                    + sizeof(uint32) * table_init_data->func_index_count);
}

static uint32
get_table_init_data_list_size(AOTTableInitData **table_init_data_list,
                              uint32 table_init_data_count)
{
    /*
     * ------------------------------
     * | table_init_data_count
     * ------------------------------
     * |                     | U32 mode
     * | AOTTableInitData[N] | U32 elem_type
     * |                     | U32 table_index
     * |                     | U32 offset.init_expr_type
     * |                     | U64 offset.u.i64
     * |                     | U32 func_index_count
     * |                     | U32[func_index_count]
     * ------------------------------
     */
    AOTTableInitData **table_init_data = table_init_data_list;
    uint32 size = 0, i;

    size = (uint32)sizeof(uint32);

    for (i = 0; i < table_init_data_count; i++, table_init_data++) {
        size = align_uint(size, 4);
        size += get_table_init_data_size(*table_init_data);
    }
    return size;
}

static uint32
get_import_table_size(AOTCompData *comp_data)
{
    /*
     * ------------------------------
     * | import_table_count
     * ------------------------------
     * |                  | U32 table_init_size
     * |                  | ----------------------
     * | AOTImpotTable[N] | U32 table_init_size
     * |                  | ----------------------
     * |                  | U32 possible_grow (convenient than U8)
     * ------------------------------
     */
    return (uint32)(sizeof(uint32)
                    + comp_data->import_table_count
                        * (sizeof(uint32) * 3));
}

static uint32
get_table_size(AOTCompData *comp_data)
{
    /*
     * ------------------------------
     * | table_count
     * ------------------------------
     * |             | U32 elem_type
     * | AOTTable[N] | U32 table_flags
     * |             | U32 table_init_size
     * |             | U32 table_max_size
     * |             | U32 possible_grow (convenient than U8)
     * ------------------------------
     */
    return (uint32)(sizeof(uint32)
                    + comp_data->table_count
                        * (sizeof(uint32) * 5));
}

static uint32
get_table_info_size(AOTCompData *comp_data)
{
    /*
     * ------------------------------
     * | import_table_count
     * ------------------------------
     * |
     * | AOTImportTable[import_table_count]
     * |
     * ------------------------------
     * | table_count
     * ------------------------------
     * |
     * | AOTTable[table_count]
     * |
     * ------------------------------
     * | table_init_data_count
     * ------------------------------
     * |
     * | AOTTableInitData*[table_init_data_count]
     * |
     * ------------------------------
     */
    return get_import_table_size(comp_data) + get_table_size(comp_data)
           + get_table_init_data_list_size(comp_data->table_init_data_list,
                                           comp_data->table_init_data_count);
}

static uint32
get_func_type_size(AOTFuncType *func_type)
{
    /* param count + result count + types */
    return (uint32)sizeof(uint32) * 2
           + func_type->param_count + func_type->result_count;
}

static uint32
get_func_types_size(AOTFuncType **func_types, uint32 func_type_count)
{
    AOTFuncType **func_type = func_types;
    uint32 size = 0, i;

    for (i = 0; i < func_type_count; i++, func_type++) {
        size = align_uint(size, 4);
        size += get_func_type_size(*func_type);
    }
    return size;
}

static uint32
get_func_type_info_size(AOTCompData *comp_data)
{
    /* func type count + func type list */
    return (uint32)sizeof(uint32)
           + get_func_types_size(comp_data->func_types,
                                 comp_data->func_type_count);
}

static uint32
get_import_global_size(AOTImportGlobal *import_global)
{
    /* type (1 byte) + is_mutable (1 byte) + module_name + global_name */
    uint32 size = (uint32)sizeof(uint8) * 2
                  + get_string_size(import_global->module_name);
    size = align_uint(size, 2);
    size += get_string_size(import_global->global_name);
    return size;
}

static uint32
get_import_globals_size(AOTImportGlobal *import_globals,
                        uint32 import_global_count)
{
    AOTImportGlobal *import_global = import_globals;
    uint32 size = 0, i;

    for (i = 0; i < import_global_count; i++, import_global++) {
        size = align_uint(size, 2);
        size += get_import_global_size(import_global);
    }
    return size;
}

static uint32
get_import_global_info_size(AOTCompData *comp_data)
{
    /* import global count + import globals */
    return (uint32)sizeof(uint32)
           + get_import_globals_size(comp_data->import_globals,
                                     comp_data->import_global_count);
}

static uint32
get_global_size(AOTGlobal *global)
{
    if (global->init_expr.init_expr_type != INIT_EXPR_TYPE_V128_CONST)
        /* type (1 byte) + is_mutable (1 byte)
           + init expr type (2 byes) + init expr value (8 byes) */
        return sizeof(uint8) * 2 + sizeof(uint16) + sizeof(uint64);
    else
        /* type (1 byte) + is_mutable (1 byte)
           + init expr type (2 byes) + v128 value (16 byes) */
        return sizeof(uint8) * 2 + sizeof(uint16) + sizeof(uint64) * 2;
}

static uint32
get_globals_size(AOTGlobal *globals, uint32 global_count)
{
    AOTGlobal *global = globals;
    uint32 size = 0, i;

    for (i = 0; i < global_count; i++, global++) {
        size = align_uint(size, 4);
        size += get_global_size(global);
    }
    return size;
}

static uint32
get_global_info_size(AOTCompData *comp_data)
{
    /* global count + globals */
    return (uint32)sizeof(uint32)
           + get_globals_size(comp_data->globals,
                              comp_data->global_count);
}

static uint32
get_import_func_size(AOTImportFunc *import_func)
{
    /* type index (2 bytes) + module_name + func_name */
    uint32 size = (uint32)sizeof(uint16)
                  + get_string_size(import_func->module_name);
    size = align_uint(size, 2);
    size += get_string_size(import_func->func_name);
    return size;
}

static uint32
get_import_funcs_size(AOTImportFunc *import_funcs,
                      uint32 import_func_count)
{
    AOTImportFunc *import_func = import_funcs;
    uint32 size = 0, i;

    for (i = 0; i < import_func_count; i++, import_func++) {
        size = align_uint(size, 2);
        size += get_import_func_size(import_func);
    }
    return size;
}

static uint32
get_import_func_info_size(AOTCompData *comp_data)
{
    /* import func count + import funcs */
    return (uint32)sizeof(uint32)
           + get_import_funcs_size(comp_data->import_funcs,
                                   comp_data->import_func_count);
}

static uint32
get_object_data_section_size(AOTObjectDataSection *data_section)
{
    /* name + size + data */
    uint32 size = get_string_size(data_section->name);
    size = align_uint(size, 4);
    size += (uint32)sizeof(uint32);
    size += data_section->size;
    return size;
}

static uint32
get_object_data_sections_size(AOTObjectDataSection *data_sections,
                              uint32 data_sections_count)
{
    AOTObjectDataSection *data_section = data_sections;
    uint32 size = 0, i;

    for (i = 0; i < data_sections_count; i++, data_section++) {
        size = align_uint(size, 2);
        size += get_object_data_section_size(data_section);
    }
    return size;
}

static uint32
get_object_data_section_info_size(AOTObjectData *obj_data)
{
    /* data sections count + data sections */
    return (uint32)sizeof(uint32)
           + get_object_data_sections_size(obj_data->data_sections,
                                           obj_data->data_sections_count);
}

static uint32
get_init_data_section_size(AOTCompData *comp_data, AOTObjectData *obj_data)
{
    uint32 size = 0;

    size += get_mem_info_size(comp_data);

    size = align_uint(size, 4);
    size += get_table_info_size(comp_data);

    size = align_uint(size, 4);
    size += get_func_type_info_size(comp_data);

    size = align_uint(size, 4);
    size += get_import_global_info_size(comp_data);

    size = align_uint(size, 4);
    size += get_global_info_size(comp_data);

    size = align_uint(size, 4);
    size += get_import_func_info_size(comp_data);

    /* func count + start func index */
    size = align_uint(size, 4);
    size += (uint32)sizeof(uint32) * 2;

    /* aux data/heap/stack data */
    size += sizeof(uint32) * 7;

    size += get_object_data_section_info_size(obj_data);
    return size;
}

static uint32
get_text_section_size(AOTObjectData *obj_data)
{
    return (sizeof(uint32) + obj_data->literal_size + obj_data->text_size + 3) & ~3;
}

static uint32
get_func_section_size(AOTCompData *comp_data, AOTObjectData *obj_data)
{
    /* text offsets + function type indexs */
    uint32 size = 0;

    if (is_32bit_binary(obj_data->binary))
        size = (uint32)sizeof(uint32) * comp_data->func_count;
    else
        size = (uint32)sizeof(uint64) * comp_data->func_count;

    size += (uint32)sizeof(uint32) * comp_data->func_count;
    return size;
}

static uint32
get_export_size(AOTExport *export)
{
    /* export index + export kind + 1 byte padding + export name */
    return (uint32)sizeof(uint32) + sizeof(uint8) + 1
           + get_string_size(export->name);
}

static uint32
get_exports_size(AOTExport *exports, uint32 export_count)
{
    AOTExport *export = exports;
    uint32 size = 0, i;

    for (i = 0; i < export_count; i++, export++) {
        size = align_uint(size, 4);
        size += get_export_size(export);
    }
    return size;
}

static uint32
get_export_section_size(AOTCompData *comp_data)
{
    /* export count + exports */
    return (uint32)sizeof(uint32)
           + get_exports_size(comp_data->wasm_module->exports,
                              comp_data->wasm_module->export_count);
}

static uint32
get_relocation_size(AOTRelocation *relocation, bool is_32bin)
{
    /* offset + addend + relocation type + symbol name */
    uint32 size = 0;
    if (is_32bin)
        size = sizeof(uint32) * 2; /* offset and addend */
    else
        size = sizeof(uint64) * 2; /* offset and addend */
    size += (uint32)sizeof(uint32); /* relocation type */
    size += (uint32)sizeof(uint32); /* symbol name index */
    return size;
}

static uint32
get_relocations_size(AOTRelocation *relocations,
                     uint32 relocation_count,
                     bool is_32bin)
{
    AOTRelocation *relocation = relocations;
    uint32 size = 0, i;

    for (i = 0; i < relocation_count; i++, relocation++) {
        size = align_uint(size, 4);
        size += get_relocation_size(relocation, is_32bin);
    }
    return size;
}

static uint32
get_relocation_group_size(AOTRelocationGroup *relocation_group,
                          bool is_32bin)
{
    uint32 size = 0;
    /* section name index + relocation count + relocations */
    size += (uint32)sizeof(uint32);
    size += (uint32)sizeof(uint32);
    size += get_relocations_size(relocation_group->relocations,
                                 relocation_group->relocation_count,
                                 is_32bin);
    return size;
}

static uint32
get_relocation_groups_size(AOTRelocationGroup *relocation_groups,
                           uint32 relocation_group_count,
                           bool is_32bin)
{
    AOTRelocationGroup *relocation_group = relocation_groups;
    uint32 size = 0, i;

    for (i = 0; i < relocation_group_count; i++, relocation_group++) {
        size = align_uint(size, 4);
        size += get_relocation_group_size(relocation_group, is_32bin);
    }
    return size;
}

/* return the index (in order of insertion) of the symbol,
   create if not exits, -1 if failed */
static uint32
get_relocation_symbol_index(const char *symbol_name,
                            bool *is_new,
                            AOTSymbolList *symbol_list)
{
    AOTSymbolNode *sym;
    uint32 index = 0;

    sym = symbol_list->head;
    while (sym) {
        if (!strcmp(sym->symbol, symbol_name)) {
            if (is_new)
                *is_new = false;
            return index;
        }

        sym = sym->next;
        index ++;
    }

    /* Not found in symbol_list, add it */
    sym = wasm_runtime_malloc(sizeof(AOTSymbolNode));
    if (!sym) {
        return (uint32)-1;
    }

    memset(sym, 0, sizeof(AOTSymbolNode));
    sym->symbol = (char *)symbol_name;
    sym->str_len = (uint32)strlen(symbol_name);

    if (!symbol_list->head) {
        symbol_list->head = symbol_list->end = sym;
    }
    else {
        symbol_list->end->next = sym;
        symbol_list->end = sym;
    }
    symbol_list->len ++;

    if (is_new)
        *is_new = true;
    return index;
}

static uint32
get_relocation_symbol_size(AOTRelocation *relocation,
                           AOTSymbolList *symbol_list)
{
    uint32 size = 0, index = 0;
    bool is_new = false;

    index = get_relocation_symbol_index(relocation->symbol_name, &is_new, symbol_list);
    CHECK_SIZE(index);

    if (is_new) {
        size += (uint32)sizeof(uint16);
        size += (uint32)strlen(relocation->symbol_name);
        size = align_uint(size, 2);
    }

    relocation->symbol_index = index;
    return size;
}

static uint32
get_relocations_symbol_size(AOTRelocation *relocations,
                            uint32 relocation_count,
                            AOTSymbolList *symbol_list)
{
    AOTRelocation *relocation = relocations;
    uint32 size = 0, curr_size, i;

    for (i = 0; i < relocation_count; i++, relocation++) {
        curr_size = get_relocation_symbol_size(relocation, symbol_list);
        CHECK_SIZE(curr_size);

        size += curr_size;
    }
    return size;
}

static uint32
get_relocation_group_symbol_size(AOTRelocationGroup *relocation_group,
                                 AOTSymbolList *symbol_list)
{
    uint32 size = 0, index = 0, curr_size;
    bool is_new = false;

    index = get_relocation_symbol_index(relocation_group->section_name,
                                        &is_new,
                                        symbol_list);
    CHECK_SIZE(index);

    if (is_new) {
        size += (uint32)sizeof(uint16);
        size += (uint32)strlen(relocation_group->section_name);
        size = align_uint(size, 2);
    }

    relocation_group->name_index = index;

    curr_size = get_relocations_symbol_size(relocation_group->relocations,
                                            relocation_group->relocation_count,
                                            symbol_list);
    CHECK_SIZE(curr_size);
    size += curr_size;

    return size;
}

static uint32
get_relocation_groups_symbol_size(AOTRelocationGroup *relocation_groups,
                                  uint32 relocation_group_count,
                                  AOTSymbolList *symbol_list)
{
    AOTRelocationGroup *relocation_group = relocation_groups;
    uint32 size = 0, curr_size, i;

    for (i = 0; i < relocation_group_count; i++, relocation_group++) {
        curr_size = get_relocation_group_symbol_size(relocation_group,
                                                     symbol_list);
        CHECK_SIZE(curr_size);
        size += curr_size;
    }
    return size;
}

static uint32
get_symbol_size_from_symbol_list(AOTSymbolList *symbol_list)
{
    AOTSymbolNode *sym;
    uint32 size = 0;

    sym = symbol_list->head;
    while (sym) {
        /* (uint16)str_len + str */
        size += (uint32)sizeof(uint16) + sym->str_len;
        size = align_uint(size, 2);
        sym = sym->next;
    }

    return size;
}

static uint32
get_relocation_section_symbol_size(AOTObjectData *obj_data)
{
    AOTRelocationGroup *relocation_groups = obj_data->relocation_groups;
    uint32 relocation_group_count = obj_data->relocation_group_count;
    uint32 string_count = 0, symbol_table_size = 0;

    /* section size will be calculated twice,
       get symbol size from symbol list directly in the second calculation */
    if (obj_data->symbol_list.len > 0) {
        symbol_table_size =
                get_symbol_size_from_symbol_list(&obj_data->symbol_list);
    }
    else {
        symbol_table_size =
                get_relocation_groups_symbol_size(relocation_groups,
                                                  relocation_group_count,
                                                  &obj_data->symbol_list);
    }
    CHECK_SIZE(symbol_table_size);
    string_count = obj_data->symbol_list.len;

    /* string_count + string_offsets + total_string_len + [str (string_len + str)] */
    return (uint32)(sizeof(uint32) + sizeof(uint32) * string_count
                    + sizeof(uint32) + symbol_table_size);
}

static uint32
get_relocation_section_size(AOTObjectData *obj_data)
{
    AOTRelocationGroup *relocation_groups = obj_data->relocation_groups;
    uint32 relocation_group_count = obj_data->relocation_group_count;
    uint32 symbol_table_size = 0;

    symbol_table_size = get_relocation_section_symbol_size(obj_data);
    CHECK_SIZE(symbol_table_size);
    symbol_table_size = align_uint(symbol_table_size, 4);

    /* relocation group count + symbol_table + relocation groups */
    return (uint32)sizeof(uint32) + symbol_table_size
           + get_relocation_groups_size(relocation_groups,
                                        relocation_group_count,
                                        is_32bit_binary(obj_data->binary));
}

static uint32
get_aot_file_size(AOTCompContext *comp_ctx, AOTCompData *comp_data,
                  AOTObjectData *obj_data)
{
    uint32 size = 0;

    /* aot file header */
    size += get_file_header_size();

    /* target info section */
    size = align_uint(size, 4);
    /* section id + section size */
    size += (uint32)sizeof(uint32) * 2;
    size += get_target_info_section_size();

    /* init data section */
    size = align_uint(size, 4);
    /* section id + section size */
    size += (uint32)sizeof(uint32) * 2;
    size += get_init_data_section_size(comp_data, obj_data);

    /* text section */
    size = align_uint(size, 4);
    /* section id + section size */
    size += (uint32)sizeof(uint32) * 2;
    size += get_text_section_size(obj_data);

    /* function section */
    size = align_uint(size, 4);
    /* section id + section size */
    size += (uint32)sizeof(uint32) * 2;
    size += get_func_section_size(comp_data, obj_data);

    /* export section */
    size = align_uint(size, 4);
    /* section id + section size */
    size += (uint32)sizeof(uint32) * 2;
    size += get_export_section_size(comp_data);

    /* relocation section */
    size = align_uint(size, 4);
    /* section id + section size */
    size += (uint32)sizeof(uint32) * 2;
    size += get_relocation_section_size(obj_data);

    return size;
}

#define exchange_uint8(p_data) (void)0

static void
exchange_uint16(uint8 *p_data)
{
    uint8 value = *p_data;
    *p_data = *(p_data + 1);
    *(p_data + 1) = value;
}

static void
exchange_uint32(uint8 *p_data)
{
    uint8 value = *p_data;
    *p_data = *(p_data + 3);
    *(p_data + 3) = value;

    value = *(p_data + 1);
    *(p_data + 1) = *(p_data + 2);
    *(p_data + 2) = value;
}

static void
exchange_uint64(uint8 *pData)
{
    uint32 value;

    value = *(uint32 *)pData;
    *(uint32 *)pData = *(uint32 *)(pData + 4);
    *(uint32 *)(pData + 4) = value;
    exchange_uint32(pData);
    exchange_uint32(pData + 4);
}

static void
exchange_uint128(uint8 *pData)
{
    /* swap high 64bit and low 64bit */
    uint64 value = *(uint64*)pData;
    *(uint64*)pData = *(uint64*)(pData + 8);
    *(uint64*)(pData + 8) = value;
    /* exchange high 64bit */
    exchange_uint64(pData);
    /* exchange low 64bit */
    exchange_uint64(pData + 8);
}

static union {
    int a;
    char b;
} __ue = { .a = 1 };

#define is_little_endian() (__ue.b == 1)

#define CHECK_BUF(length) do {              \
  if (buf + offset + length > buf_end) {    \
    aot_set_last_error("buf overflow");     \
    return false;                           \
  }                                         \
} while (0)

#define EMIT_U8(v)  do {                    \
    CHECK_BUF(1);                           \
    *(uint8*)(buf + offset) = (uint8)v;     \
    offset++;                               \
  } while (0)

#define EMIT_U16(v)  do {                   \
    uint16 t = (uint16)v;                   \
    CHECK_BUF(2);                           \
    if (!is_little_endian())                \
      exchange_uint16((uint8*)&t);          \
    *(uint16*)(buf + offset) = t;           \
    offset += (uint32)sizeof(uint16);       \
  } while (0)

#define EMIT_U32(v)  do {                   \
    uint32 t = (uint32)v;                   \
    CHECK_BUF(4);                           \
    if (!is_little_endian())                \
      exchange_uint32((uint8*)&t);          \
    *(uint32*)(buf + offset) = t;           \
    offset += (uint32)sizeof(uint32);       \
  } while (0)

#define EMIT_U64(v)  do {                   \
    uint64 t = (uint64)v;                   \
    CHECK_BUF(8);                           \
    if (!is_little_endian())                \
      exchange_uint64((uint8*)&t);          \
    PUT_U64_TO_ADDR(buf + offset, t);       \
    offset += (uint32)sizeof(uint64);       \
  } while (0)

#define EMIT_V128(v)  do {                  \
    uint64 *t = (uint64*)v.i64x2;           \
    CHECK_BUF(16);                          \
    if (!is_little_endian())                \
        exchange_uint128((uint8 *)t);       \
    PUT_U64_TO_ADDR(buf + offset, t[0]);    \
    offset += (uint32)sizeof(uint64);       \
    PUT_U64_TO_ADDR(buf + offset, t[1]);    \
    offset += (uint32)sizeof(uint64);       \
  } while (0)

#define EMIT_BUF(v, len)  do {              \
    CHECK_BUF(len);                         \
    memcpy(buf + offset, v, len);           \
    offset += len;                          \
  } while (0)

#define EMIT_STR(s)  do {                   \
    uint32 str_len = (uint32)strlen(s);     \
    EMIT_U16(str_len);                      \
    EMIT_BUF(s, str_len);                   \
  } while (0)

static bool
aot_emit_file_header(uint8 *buf, uint8 *buf_end, uint32 *p_offset,
                     AOTCompData *comp_data, AOTObjectData *obj_data)
{
    uint32 offset = *p_offset;
    uint32 aot_curr_version = AOT_CURRENT_VERSION;

    EMIT_U8('\0');
    EMIT_U8('a');
    EMIT_U8('o');
    EMIT_U8('t');

    EMIT_U32(aot_curr_version);

    *p_offset = offset;
    return true;
}

static bool
aot_emit_target_info_section(uint8 *buf, uint8 *buf_end, uint32 *p_offset,
                             AOTCompData *comp_data, AOTObjectData *obj_data)
{
    uint32 offset = *p_offset;
    uint32 section_size = get_target_info_section_size();
    AOTTargetInfo *target_info = &obj_data->target_info;

    *p_offset = offset = align_uint(offset, 4);

    EMIT_U32(AOT_SECTION_TYPE_TARGET_INFO);
    EMIT_U32(section_size);

    EMIT_U16(target_info->bin_type);
    EMIT_U16(target_info->abi_type);
    EMIT_U16(target_info->e_type);
    EMIT_U16(target_info->e_machine);
    EMIT_U32(target_info->e_version);
    EMIT_U32(target_info->e_flags);
    EMIT_U32(target_info->reserved);
    EMIT_BUF(target_info->arch, sizeof(target_info->arch));

    if (offset - *p_offset != section_size + sizeof(uint32) * 2) {
        aot_set_last_error("emit target info failed.");
        return false;
    }

    *p_offset = offset;

    return true;
}

static bool
aot_emit_mem_info(uint8 *buf, uint8 *buf_end, uint32 *p_offset,
                  AOTCompContext *comp_ctx, AOTCompData *comp_data,
                  AOTObjectData *obj_data)
{
    uint32 offset = *p_offset, i;
    AOTMemInitData **init_datas = comp_data->mem_init_data_list;

    *p_offset = offset = align_uint(offset, 4);

    /* Emit import memory count, only emit 0 currently.
       TODO: emit the actual import memory count and
             the full import memory info. */
    EMIT_U32(0);

    /* Emit memory count */
    EMIT_U32(comp_data->memory_count);
    /* Emit memory items */
    for (i = 0; i < comp_data->memory_count; i++) {
        EMIT_U32(comp_data->memories[i].memory_flags);
        EMIT_U32(comp_data->memories[i].num_bytes_per_page);
        EMIT_U32(comp_data->memories[i].mem_init_page_count);
        EMIT_U32(comp_data->memories[i].mem_max_page_count);
    }

    /* Emit mem init data count */
    EMIT_U32(comp_data->mem_init_data_count);
    /* Emit mem init data items */
    for (i = 0; i < comp_data->mem_init_data_count; i++) {
        offset = align_uint(offset, 4);
#if WASM_ENABLE_BULK_MEMORY != 0
        if (comp_ctx->enable_bulk_memory) {
            EMIT_U32(init_datas[i]->is_passive);
            EMIT_U32(init_datas[i]->memory_index);
        }
        else
#endif
        {
            /* emit two placeholder to keep the same size */
            EMIT_U32(0);
            EMIT_U32(0);
        }
        EMIT_U32(init_datas[i]->offset.init_expr_type);
        EMIT_U64(init_datas[i]->offset.u.i64);
        EMIT_U32(init_datas[i]->byte_count);
        EMIT_BUF(init_datas[i]->bytes, init_datas[i]->byte_count);
    }

    if (offset - *p_offset != get_mem_info_size(comp_data)) {
        aot_set_last_error("emit memory info failed.");
        return false;
    }

    *p_offset = offset;

    return true;
}

static bool
aot_emit_table_info(uint8 *buf, uint8 *buf_end, uint32 *p_offset,
                    AOTCompData *comp_data, AOTObjectData *obj_data)
{
    uint32 offset = *p_offset, i, j;
    AOTTableInitData **init_datas = comp_data->table_init_data_list;

    *p_offset = offset = align_uint(offset, 4);

    /* Emit import table count */
    EMIT_U32(comp_data->import_table_count);
    /* Emit table items */
    for (i = 0; i < comp_data->import_table_count; i++) {
        /* TODO:
         * EMIT_STR(comp_data->import_tables[i].module_name );
         * EMIT_STR(comp_data->import_tables[i].table_name);
         */
        EMIT_U32(comp_data->import_tables[i].table_init_size);
        EMIT_U32(comp_data->import_tables[i].table_max_size);
        EMIT_U32(comp_data->import_tables[i].possible_grow & 0x000000FF);
    }

    /* Emit table count */
    EMIT_U32(comp_data->table_count);
    /* Emit table items */
    for (i = 0; i < comp_data->table_count; i++) {
        EMIT_U32(comp_data->tables[i].elem_type);
        EMIT_U32(comp_data->tables[i].table_flags);
        EMIT_U32(comp_data->tables[i].table_init_size);
        EMIT_U32(comp_data->tables[i].table_max_size);
        EMIT_U32(comp_data->tables[i].possible_grow & 0x000000FF);
    }

    /* Emit table init data count */
    EMIT_U32(comp_data->table_init_data_count);
    /* Emit table init data items */
    for (i = 0; i < comp_data->table_init_data_count; i++) {
        offset = align_uint(offset, 4);
        EMIT_U32(init_datas[i]->mode);
        EMIT_U32(init_datas[i]->elem_type);
        EMIT_U32(init_datas[i]->table_index);
        EMIT_U32(init_datas[i]->offset.init_expr_type);
        EMIT_U64(init_datas[i]->offset.u.i64);
        EMIT_U32(init_datas[i]->func_index_count);
        for (j = 0; j < init_datas[i]->func_index_count; j++)
            EMIT_U32(init_datas[i]->func_indexes[j]);
    }

    if (offset - *p_offset != get_table_info_size(comp_data)) {
        aot_set_last_error("emit table info failed.");
        return false;
    }

    *p_offset = offset;

    return true;
}

static bool
aot_emit_func_type_info(uint8 *buf, uint8 *buf_end, uint32 *p_offset,
                        AOTCompData *comp_data, AOTObjectData *obj_data)
{
    uint32 offset = *p_offset, i;
    AOTFuncType **func_types = comp_data->func_types;

    *p_offset = offset = align_uint(offset, 4);

    EMIT_U32(comp_data->func_type_count);

    for (i = 0; i < comp_data->func_type_count; i++) {
        offset = align_uint(offset, 4);
        EMIT_U32(func_types[i]->param_count);
        EMIT_U32(func_types[i]->result_count);
        EMIT_BUF(func_types[i]->types,
                 func_types[i]->param_count + func_types[i]->result_count);
    }

    if (offset - *p_offset != get_func_type_info_size(comp_data)) {
        aot_set_last_error("emit function type info failed.");
        return false;
    }

    *p_offset = offset;

    return true;
}

static bool
aot_emit_import_global_info(uint8 *buf, uint8 *buf_end, uint32 *p_offset,
                            AOTCompData *comp_data, AOTObjectData *obj_data)
{
    uint32 offset = *p_offset, i;
    AOTImportGlobal *import_global = comp_data->import_globals;

    *p_offset = offset = align_uint(offset, 4);

    EMIT_U32(comp_data->import_global_count);

    for (i = 0; i < comp_data->import_global_count; i++, import_global++) {
        offset = align_uint(offset, 2);
        EMIT_U8(import_global->type);
        EMIT_U8(import_global->is_mutable);
        EMIT_STR(import_global->module_name);
        offset = align_uint(offset, 2);
        EMIT_STR(import_global->global_name);
    }

    if (offset - *p_offset != get_import_global_info_size(comp_data)) {
        aot_set_last_error("emit import global info failed.");
        return false;
    }

    *p_offset = offset;

    return true;
}

static bool
aot_emit_global_info(uint8 *buf, uint8 *buf_end, uint32 *p_offset,
                     AOTCompData *comp_data, AOTObjectData *obj_data)
{
    uint32 offset = *p_offset, i;
    AOTGlobal *global = comp_data->globals;

    *p_offset = offset = align_uint(offset, 4);

    EMIT_U32(comp_data->global_count);

    for (i = 0; i < comp_data->global_count; i++, global++) {
        offset = align_uint(offset, 4);
        EMIT_U8(global->type);
        EMIT_U8(global->is_mutable);
        EMIT_U16(global->init_expr.init_expr_type);
        if (global->init_expr.init_expr_type != INIT_EXPR_TYPE_V128_CONST)
            EMIT_U64(global->init_expr.u.i64);
        else
            EMIT_V128(global->init_expr.u.v128);
    }

    if (offset - *p_offset != get_global_info_size(comp_data)) {
        aot_set_last_error("emit global info failed.");
        return false;
    }

    *p_offset = offset;

    return true;
}

static bool
aot_emit_import_func_info(uint8 *buf, uint8 *buf_end, uint32 *p_offset,
                          AOTCompData *comp_data, AOTObjectData *obj_data)
{
    uint32 offset = *p_offset, i;
    AOTImportFunc *import_func = comp_data->import_funcs;

    *p_offset = offset = align_uint(offset, 4);

    EMIT_U32(comp_data->import_func_count);

    for (i = 0; i < comp_data->import_func_count; i++, import_func++) {
        offset = align_uint(offset, 2);
        EMIT_U16(import_func->func_type_index);
        EMIT_STR(import_func->module_name);
        offset = align_uint(offset, 2);
        EMIT_STR(import_func->func_name);
    }

    if (offset - *p_offset != get_import_func_info_size(comp_data)) {
        aot_set_last_error("emit import function info failed.");
        return false;
    }

    *p_offset = offset;

    return true;
}

static bool
aot_emit_object_data_section_info(uint8 *buf, uint8 *buf_end, uint32 *p_offset,
                                  AOTObjectData *obj_data)
{
    uint32 offset = *p_offset, i;
    AOTObjectDataSection *data_section = obj_data->data_sections;

    *p_offset = offset = align_uint(offset, 4);

    EMIT_U32(obj_data->data_sections_count);

    for (i = 0; i < obj_data->data_sections_count; i++, data_section++) {
        offset = align_uint(offset, 2);
        EMIT_STR(data_section->name);
        offset = align_uint(offset, 4);
        EMIT_U32(data_section->size);
        EMIT_BUF(data_section->data, data_section->size);
    }

    if (offset - *p_offset != get_object_data_section_info_size(obj_data)) {
        aot_set_last_error("emit object data section info failed.");
        return false;
    }

    *p_offset = offset;

    return true;
}

static bool
aot_emit_init_data_section(uint8 *buf, uint8 *buf_end, uint32 *p_offset,
                           AOTCompContext *comp_ctx, AOTCompData *comp_data,
                           AOTObjectData *obj_data)
{
    uint32 section_size = get_init_data_section_size(comp_data, obj_data);
    uint32 offset = *p_offset;

    *p_offset = offset = align_uint(offset, 4);

    EMIT_U32(AOT_SECTION_TYPE_INIT_DATA);
    EMIT_U32(section_size);

    if (!aot_emit_mem_info(buf, buf_end, &offset, comp_ctx, comp_data, obj_data)
        || !aot_emit_table_info(buf, buf_end, &offset, comp_data, obj_data)
        || !aot_emit_func_type_info(buf, buf_end, &offset, comp_data, obj_data)
        || !aot_emit_import_global_info(buf, buf_end, &offset, comp_data, obj_data)
        || !aot_emit_global_info(buf, buf_end, &offset, comp_data, obj_data)
        || !aot_emit_import_func_info(buf, buf_end, &offset, comp_data, obj_data))
        return false;

    offset = align_uint(offset, 4);
    EMIT_U32(comp_data->func_count);
    EMIT_U32(comp_data->start_func_index);

    EMIT_U32(comp_data->aux_data_end_global_index);
    EMIT_U32(comp_data->aux_data_end);
    EMIT_U32(comp_data->aux_heap_base_global_index);
    EMIT_U32(comp_data->aux_heap_base);
    EMIT_U32(comp_data->aux_stack_top_global_index);
    EMIT_U32(comp_data->aux_stack_bottom);
    EMIT_U32(comp_data->aux_stack_size);

    if (!aot_emit_object_data_section_info(buf, buf_end, &offset, obj_data))
        return false;

    if (offset - *p_offset != section_size + sizeof(uint32) * 2) {
        aot_set_last_error("emit init data section failed.");
        return false;
    }

    *p_offset = offset;

    return true;
}

static bool
aot_emit_text_section(uint8 *buf, uint8 *buf_end, uint32 *p_offset,
                      AOTCompData *comp_data, AOTObjectData *obj_data)
{
    uint32 section_size = get_text_section_size(obj_data);
    uint32 offset = *p_offset;
    uint8 placeholder = 0;

    *p_offset = offset = align_uint(offset, 4);

    EMIT_U32(AOT_SECTION_TYPE_TEXT);
    EMIT_U32(section_size);
    EMIT_U32(obj_data->literal_size);
    if (obj_data->literal_size > 0)
        EMIT_BUF(obj_data->literal, obj_data->literal_size);
    EMIT_BUF(obj_data->text, obj_data->text_size);

    while (offset & 3)
        EMIT_BUF(&placeholder, 1);

    if (offset - *p_offset != section_size + sizeof(uint32) * 2) {
        aot_set_last_error("emit text section failed.");
        return false;
    }

    *p_offset = offset;

    return true;
}

static bool
aot_emit_func_section(uint8 *buf, uint8 *buf_end, uint32 *p_offset,
                      AOTCompData *comp_data, AOTObjectData *obj_data)
{
    uint32 section_size = get_func_section_size(comp_data, obj_data);
    uint32 i, offset = *p_offset;
    AOTObjectFunc *func = obj_data->funcs;
    AOTFunc **funcs = comp_data->funcs;

    *p_offset = offset = align_uint(offset, 4);

    EMIT_U32(AOT_SECTION_TYPE_FUNCTION);
    EMIT_U32(section_size);

    for (i = 0; i < obj_data->func_count; i++, func++) {
        if (is_32bit_binary(obj_data->binary))
            EMIT_U32(func->text_offset);
        else
            EMIT_U64(func->text_offset);
    }

    for (i = 0; i < comp_data->func_count; i++)
        EMIT_U32(funcs[i]->func_type_index);

    if (offset - *p_offset != section_size + sizeof(uint32) * 2) {
        aot_set_last_error("emit function section failed.");
        return false;
    }

    *p_offset = offset;

    return true;
}

static bool
aot_emit_export_section(uint8 *buf, uint8 *buf_end, uint32 *p_offset,
                        AOTCompData *comp_data, AOTObjectData *obj_data)
{
    uint32 section_size = get_export_section_size(comp_data);
    AOTExport *export = comp_data->wasm_module->exports;
    uint32 export_count = comp_data->wasm_module->export_count;
    uint32 i, offset = *p_offset;

    *p_offset = offset = align_uint(offset, 4);

    EMIT_U32(AOT_SECTION_TYPE_EXPORT);
    EMIT_U32(section_size);
    EMIT_U32(export_count);

    for (i = 0; i < export_count; i++, export++) {
        offset = align_uint(offset, 4);
        EMIT_U32(export->index);
        EMIT_U8(export->kind);
        EMIT_U8(0);
        EMIT_STR(export->name);
    }

    if (offset - *p_offset != section_size + sizeof(uint32) * 2) {
        aot_set_last_error("emit export section failed.");
        return false;
    }

    *p_offset = offset;

    return true;
}

static bool
aot_emit_relocation_symbol_table(uint8 *buf, uint8 *buf_end, uint32 *p_offset,
                                 AOTCompData *comp_data, AOTObjectData *obj_data)
{
    uint32 symbol_offset = 0, total_string_len = 0;
    uint32 offset = *p_offset;
    AOTSymbolNode *sym;

    EMIT_U32(obj_data->symbol_list.len);

    /* emit symbol offsets */
    sym = (AOTSymbolNode *)(obj_data->symbol_list.head);
    while(sym) {
        EMIT_U32(symbol_offset);
        /* string_len + str[0 .. string_len - 1] */
        symbol_offset += (uint32)sizeof(uint16) + sym->str_len;
        symbol_offset = align_uint(symbol_offset, 2);
        sym = sym->next;
    }

    /* emit total string len */
    total_string_len = symbol_offset;
    EMIT_U32(total_string_len);

    /* emit symbols */
    sym = (AOTSymbolNode *)(obj_data->symbol_list.head);
    while (sym) {
        EMIT_STR(sym->symbol);
        offset = align_uint(offset, 2);
        sym = sym->next;
    }

    *p_offset = offset;
    return true;
}

static bool
aot_emit_relocation_section(uint8 *buf, uint8 *buf_end, uint32 *p_offset,
                            AOTCompData *comp_data, AOTObjectData *obj_data)
{
    uint32 section_size = get_relocation_section_size(obj_data);
    uint32 i, offset = *p_offset;
    AOTRelocationGroup *relocation_group = obj_data->relocation_groups;

    if (section_size == (uint32)-1)
        return false;

    *p_offset = offset = align_uint(offset, 4);

    EMIT_U32(AOT_SECTION_TYPE_RELOCATION);
    EMIT_U32(section_size);

    aot_emit_relocation_symbol_table(buf, buf_end, &offset, comp_data, obj_data);

    offset = align_uint(offset, 4);
    EMIT_U32(obj_data->relocation_group_count);

    /* emit each relocation group */
    for (i = 0; i < obj_data->relocation_group_count; i++, relocation_group++) {
        AOTRelocation *relocation = relocation_group->relocations;
        uint32 j;

        offset = align_uint(offset, 4);
        EMIT_U32(relocation_group->name_index);
        offset = align_uint(offset, 4);
        EMIT_U32(relocation_group->relocation_count);

        /* emit each relocation */
        for (j = 0; j < relocation_group->relocation_count; j++, relocation++) {
            offset = align_uint(offset, 4);
            if (is_32bit_binary(obj_data->binary)) {
                EMIT_U32(relocation->relocation_offset);
                EMIT_U32(relocation->relocation_addend);
            }
            else {
                EMIT_U64(relocation->relocation_offset);
                EMIT_U64(relocation->relocation_addend);
            }
            EMIT_U32(relocation->relocation_type);
            EMIT_U32(relocation->symbol_index);
        }
    }

    if (offset - *p_offset != section_size + sizeof(uint32) * 2) {
        aot_set_last_error("emit relocation section failed.");
        return false;
    }

    *p_offset = offset;
    return true;
}

typedef uint32 U32;
typedef int32  I32;
typedef uint16 U16;
typedef uint8  U8;

struct coff_hdr {
    U16 u16Machine;
    U16 u16NumSections;
    U32 u32DateTimeStamp;
    U32 u32SymTblPtr;
    U32 u32NumSymbols;
    U16 u16PeHdrSize;
    U16 u16Characs;
};

#define IMAGE_FILE_MACHINE_AMD64  0x8664
#define IMAGE_FILE_MACHINE_I386   0x014c
#define IMAGE_FILE_MACHINE_IA64   0x0200

#define AOT_COFF_BIN_TYPE 6

#define EI_NIDENT 16

typedef uint32  elf32_word;
typedef int32   elf32_sword;
typedef uint16  elf32_half;
typedef uint32  elf32_off;
typedef uint32  elf32_addr;

struct elf32_ehdr {
    unsigned char e_ident[EI_NIDENT];    /* ident bytes */
    elf32_half e_type;                   /* file type */
    elf32_half e_machine;                /* target machine */
    elf32_word e_version;                /* file version */
    elf32_addr e_entry;                  /* start address */
    elf32_off e_phoff;                   /* phdr file offset */
    elf32_off e_shoff;                   /* shdr file offset */
    elf32_word e_flags;                  /* file flags */
    elf32_half e_ehsize;                 /* sizeof ehdr */
    elf32_half e_phentsize;              /* sizeof phdr */
    elf32_half e_phnum;                  /* number phdrs */
    elf32_half e_shentsize;              /* sizeof shdr */
    elf32_half e_shnum;                  /* number shdrs */
    elf32_half e_shstrndx;               /* shdr string index */
};

struct elf32_rel {
    elf32_addr r_offset;
    elf32_word r_info;
} elf32_rel;

struct elf32_rela {
    elf32_addr r_offset;
    elf32_word r_info;
    elf32_sword r_addend;
} elf32_rela;

typedef uint32  elf64_word;
typedef int32   elf64_sword;
typedef uint64  elf64_xword;
typedef int64   elf64_sxword;
typedef uint16  elf64_half;
typedef uint64  elf64_off;
typedef uint64  elf64_addr;

struct elf64_ehdr {
    unsigned char e_ident[EI_NIDENT];    /* ident bytes */
    elf64_half e_type;                   /* file type */
    elf64_half e_machine;                /* target machine */
    elf64_word e_version;                /* file version */
    elf64_addr e_entry;                  /* start address */
    elf64_off e_phoff;                   /* phdr file offset */
    elf64_off e_shoff;                   /* shdr file offset */
    elf64_word e_flags;                  /* file flags */
    elf64_half e_ehsize;                 /* sizeof ehdr */
    elf64_half e_phentsize;              /* sizeof phdr */
    elf64_half e_phnum;                  /* number phdrs */
    elf64_half e_shentsize;              /* sizeof shdr */
    elf64_half e_shnum;                  /* number shdrs */
    elf64_half e_shstrndx;               /* shdr string index */
};

typedef struct elf64_rel {
    elf64_addr r_offset;
    elf64_xword r_info;
} elf64_rel;

typedef struct elf64_rela {
    elf64_addr r_offset;
    elf64_xword r_info;
    elf64_sxword r_addend;
} elf64_rela;


#define SET_TARGET_INFO(f, v, type, little) do {    \
    type tmp = elf_header->v;                       \
    if ((little && !is_little_endian())             \
        || (!little && is_little_endian()))         \
        exchange_##type((uint8*)&tmp);              \
    obj_data->target_info.f = tmp;                  \
  } while (0)

static bool
aot_resolve_target_info(AOTCompContext *comp_ctx, AOTObjectData *obj_data)
{
    LLVMBinaryType bin_type = LLVMBinaryGetType(obj_data->binary);
    const uint8 *elf_buf = (uint8 *)LLVMGetBufferStart(obj_data->mem_buf);
    uint32 elf_size = (uint32)LLVMGetBufferSize(obj_data->mem_buf);

    if (bin_type != LLVMBinaryTypeCOFF
        && bin_type != LLVMBinaryTypeELF32L
        && bin_type != LLVMBinaryTypeELF32B
        && bin_type != LLVMBinaryTypeELF64L
        && bin_type != LLVMBinaryTypeELF64B
        && bin_type != LLVMBinaryTypeMachO32L
        && bin_type != LLVMBinaryTypeMachO32B
        && bin_type != LLVMBinaryTypeMachO64L
        && bin_type != LLVMBinaryTypeMachO64B) {
        aot_set_last_error("invaid llvm binary bin_type.");
        return false;
    }

    obj_data->target_info.bin_type = bin_type - LLVMBinaryTypeELF32L;

    if (bin_type == LLVMBinaryTypeCOFF) {
        struct coff_hdr  * coff_header;

        if (!elf_buf || elf_size < sizeof(struct coff_hdr)) {
            aot_set_last_error("invalid coff_hdr buffer.");
            return false;
        }
        coff_header = (struct coff_hdr *)elf_buf;
        obj_data->target_info.e_type = 1;
        obj_data->target_info.e_machine = coff_header->u16Machine;
        obj_data->target_info.e_version = 1;
        obj_data->target_info.e_flags = 0;

        if (coff_header->u16Machine == IMAGE_FILE_MACHINE_AMD64)
            obj_data->target_info.bin_type = AOT_COFF_BIN_TYPE;
    }
    else if (bin_type == LLVMBinaryTypeELF32L
             || bin_type == LLVMBinaryTypeELF32B) {
        struct elf32_ehdr *elf_header;
        bool is_little_bin = bin_type == LLVMBinaryTypeELF32L;

        if (!elf_buf || elf_size < sizeof(struct elf32_ehdr)) {
            aot_set_last_error("invalid elf32 buffer.");
            return false;
        }

        elf_header = (struct elf32_ehdr *)elf_buf;
        SET_TARGET_INFO(e_type, e_type, uint16, is_little_bin);
        SET_TARGET_INFO(e_machine, e_machine, uint16, is_little_bin);
        SET_TARGET_INFO(e_version, e_version, uint32, is_little_bin);
        SET_TARGET_INFO(e_flags, e_flags, uint32, is_little_bin);
    }
    else if (bin_type == LLVMBinaryTypeELF64L
             || bin_type == LLVMBinaryTypeELF64B) {
        struct elf64_ehdr *elf_header;
        bool is_little_bin = bin_type == LLVMBinaryTypeELF64L;

        if (!elf_buf || elf_size < sizeof(struct elf64_ehdr)) {
            aot_set_last_error("invalid elf64 buffer.");
            return false;
        }

        elf_header = (struct elf64_ehdr *)elf_buf;
        SET_TARGET_INFO(e_type, e_type, uint16, is_little_bin);
        SET_TARGET_INFO(e_machine, e_machine, uint16, is_little_bin);
        SET_TARGET_INFO(e_version, e_version, uint32, is_little_bin);
        SET_TARGET_INFO(e_flags, e_flags, uint32, is_little_bin);
    }
    else if (bin_type == LLVMBinaryTypeMachO32L
             || bin_type == LLVMBinaryTypeMachO32B) {
        /* TODO: parse file type of Mach-O 32 */
        aot_set_last_error("invaid llvm binary bin_type.");
        return false;
    }
    else if (bin_type == LLVMBinaryTypeMachO64L
             || bin_type == LLVMBinaryTypeMachO64B) {
        /* TODO: parse file type of Mach-O 64 */
        aot_set_last_error("invaid llvm binary bin_type.");
        return false;
    }


    strncpy(obj_data->target_info.arch, comp_ctx->target_arch,
            sizeof(obj_data->target_info.arch));

    return true;
}

static bool
aot_resolve_text(AOTObjectData *obj_data)
{
    LLVMSectionIteratorRef sec_itr;
    char *name;

    if (!(sec_itr = LLVMObjectFileCopySectionIterator(obj_data->binary))) {
        aot_set_last_error("llvm get section iterator failed.");
        return false;
    }
    while (!LLVMObjectFileIsSectionIteratorAtEnd(obj_data->binary, sec_itr)) {
        if ((name = (char *)LLVMGetSectionName(sec_itr)) && !strcmp(name, ".text")) {
            obj_data->text = (char *)LLVMGetSectionContents(sec_itr);
            obj_data->text_size = (uint32)LLVMGetSectionSize(sec_itr);
            break;
        }
        LLVMMoveToNextSection(sec_itr);
    }
    LLVMDisposeSectionIterator(sec_itr);

    return true;
}

static bool
aot_resolve_literal(AOTObjectData *obj_data)
{
    LLVMSectionIteratorRef sec_itr;
    char *name;

    if (!(sec_itr = LLVMObjectFileCopySectionIterator(obj_data->binary))) {
        aot_set_last_error("llvm get section iterator failed.");
        return false;
    }
    while (!LLVMObjectFileIsSectionIteratorAtEnd(obj_data->binary, sec_itr)) {
        if ((name = (char *)LLVMGetSectionName(sec_itr)) && !strcmp(name, ".literal")) {
            obj_data->literal = (char *)LLVMGetSectionContents(sec_itr);
            obj_data->literal_size = (uint32)LLVMGetSectionSize(sec_itr);
            break;
        }
        LLVMMoveToNextSection(sec_itr);
    }
    LLVMDisposeSectionIterator(sec_itr);

    return true;
}

static bool
get_relocations_count(LLVMSectionIteratorRef sec_itr, uint32 *p_count);

static bool
is_data_section(LLVMSectionIteratorRef sec_itr, char *section_name)
{
    uint32 relocation_count = 0;

    return (!strcmp(section_name, ".data")
            || !strcmp(section_name, ".sdata")
            || !strcmp(section_name, ".rodata")
            /* ".rodata.cst4/8/16/.." */
            || !strncmp(section_name, ".rodata.cst", strlen(".rodata.cst"))
            || (!strcmp(section_name, ".rdata")
                && get_relocations_count(sec_itr, &relocation_count)
                && relocation_count > 0));
}

static bool
get_object_data_sections_count(AOTObjectData *obj_data, uint32 *p_count)
{
    LLVMSectionIteratorRef sec_itr;
    char *name;
    uint32 count = 0;

    if (!(sec_itr = LLVMObjectFileCopySectionIterator(obj_data->binary))) {
        aot_set_last_error("llvm get section iterator failed.");
        return false;
    }
    while (!LLVMObjectFileIsSectionIteratorAtEnd(obj_data->binary, sec_itr)) {
        if ((name = (char *)LLVMGetSectionName(sec_itr))
            && (is_data_section(sec_itr, name))) {
            count++;
        }
        LLVMMoveToNextSection(sec_itr);
    }
    LLVMDisposeSectionIterator(sec_itr);

    *p_count = count;
    return true;
}

static bool
aot_resolve_object_data_sections(AOTObjectData *obj_data)
{
    LLVMSectionIteratorRef sec_itr;
    char *name;
    AOTObjectDataSection *data_section;
    uint32 sections_count;
    uint32 size;

    if (!get_object_data_sections_count(obj_data, &sections_count)) {
        return false;
    }

    if (sections_count > 0) {
        size = (uint32)sizeof(AOTObjectDataSection) * sections_count;
        if (!(data_section = obj_data->data_sections = wasm_runtime_malloc(size))) {
            aot_set_last_error("allocate memory for data sections failed.");
            return false;
        }
        memset(obj_data->data_sections, 0, size);
        obj_data->data_sections_count = sections_count;

        if (!(sec_itr = LLVMObjectFileCopySectionIterator(obj_data->binary))) {
            aot_set_last_error("llvm get section iterator failed.");
            return false;
        }
        while (!LLVMObjectFileIsSectionIteratorAtEnd(obj_data->binary, sec_itr)) {
            if ((name = (char *)LLVMGetSectionName(sec_itr))
                && (is_data_section(sec_itr, name))) {
                data_section->name = name;
                data_section->data = (uint8 *)LLVMGetSectionContents(sec_itr);
                data_section->size = (uint32)LLVMGetSectionSize(sec_itr);
                data_section++;
            }
            LLVMMoveToNextSection(sec_itr);
        }
        LLVMDisposeSectionIterator(sec_itr);
    }

    return true;
}

static bool
aot_resolve_functions(AOTCompContext *comp_ctx, AOTObjectData *obj_data)
{
    AOTObjectFunc *func;
    LLVMSymbolIteratorRef sym_itr;
    char *name, *prefix = AOT_FUNC_PREFIX;
    uint32 func_index, total_size;

    /* allocate memory for aot function */
    obj_data->func_count = comp_ctx->comp_data->func_count;
    if (obj_data->func_count) {
        total_size = (uint32)sizeof(AOTObjectFunc) * obj_data->func_count;
        if (!(obj_data->funcs = wasm_runtime_malloc(total_size))) {
            aot_set_last_error("allocate memory for functions failed.");
            return false;
        }
        memset(obj_data->funcs, 0, total_size);
    }

    if (!(sym_itr = LLVMObjectFileCopySymbolIterator(obj_data->binary))) {
        aot_set_last_error("llvm get symbol iterator failed.");
        return false;
    }

    while (!LLVMObjectFileIsSymbolIteratorAtEnd(obj_data->binary, sym_itr)) {
        if ((name = (char *)LLVMGetSymbolName(sym_itr))
            && str_starts_with(name, prefix)) {
            func_index = (uint32)atoi(name + strlen(prefix));
            if (func_index < obj_data->func_count) {
                func = obj_data->funcs + func_index;
                func->func_name = name;
                func->text_offset = LLVMGetSymbolAddress(sym_itr);
            }
        }
        LLVMMoveToNextSymbol(sym_itr);
    }
    LLVMDisposeSymbolIterator(sym_itr);

    return true;
}

static bool
get_relocations_count(LLVMSectionIteratorRef sec_itr, uint32 *p_count)
{
    uint32 relocation_count = 0;
    LLVMRelocationIteratorRef rel_itr;

    if (!(rel_itr = LLVMGetRelocations(sec_itr))) {
        aot_set_last_error("llvm get relocations failed.");
        LLVMDisposeSectionIterator(sec_itr);
        return false;
    }

    while (!LLVMIsRelocationIteratorAtEnd(sec_itr, rel_itr)) {
        relocation_count++;
        LLVMMoveToNextRelocation(rel_itr);
    }
    LLVMDisposeRelocationIterator(rel_itr);

    *p_count = relocation_count;
    return true;
}

static bool
aot_resolve_object_relocation_group(AOTObjectData *obj_data,
                                    AOTRelocationGroup *group,
                                    LLVMSectionIteratorRef rel_sec)
{
    LLVMRelocationIteratorRef rel_itr;
    AOTRelocation *relocation = group->relocations;
    uint32 size;
    bool is_binary_32bit = is_32bit_binary(obj_data->binary);
    bool is_binary_little_endian = is_little_endian_binary(obj_data->binary);
    bool has_addend = str_starts_with(group->section_name, ".rela");
    uint8 *rela_content = NULL;

    /* calculate relocations count and allcate memory */
    if (!get_relocations_count(rel_sec, &group->relocation_count))
        return false;
    if (group->relocation_count == 0) {
        aot_set_last_error("invalid relocations count");
        return false;
    }
    size = (uint32)sizeof(AOTRelocation) * group->relocation_count;
    if (!(relocation = group->relocations = wasm_runtime_malloc(size))) {
        aot_set_last_error("allocate memory for relocations failed.");
        return false;
    }
    memset(group->relocations, 0, size);

    if (has_addend) {
        uint64 rela_content_size;
        /* LLVM doesn't provide C API to get relocation addend. So we have to
         * parse it manually. */
        rela_content = (uint8 *)LLVMGetSectionContents(rel_sec);
        rela_content_size = LLVMGetSectionSize(rel_sec);
        if (is_binary_32bit)
            size = (uint32)sizeof(struct elf32_rela) * group->relocation_count;
        else
            size = (uint32)sizeof(struct elf64_rela) * group->relocation_count;
        if (rela_content_size != (uint64)size) {
            aot_set_last_error("invalid relocation section content.");
            return false;
        }
    }

    /* pares each relocation */
    if (!(rel_itr = LLVMGetRelocations(rel_sec))) {
        aot_set_last_error("llvm get relocations failed.");
        return false;
    }
    while (!LLVMIsRelocationIteratorAtEnd(rel_sec, rel_itr)) {
        uint64 offset = LLVMGetRelocationOffset(rel_itr);
        uint64 type = LLVMGetRelocationType(rel_itr);
        LLVMSymbolIteratorRef rel_sym = LLVMGetRelocationSymbol(rel_itr);

        if (!rel_sym) {
            aot_set_last_error("llvm get relocation symbol failed.");
            goto fail;
        }

        /* parse relocation addend from reloction content */
        if (has_addend) {
            if (is_binary_32bit) {
                uint32 addend = (uint32)(((struct elf32_rela *)rela_content)->r_addend);
                if (is_binary_little_endian != is_little_endian())
                    exchange_uint32((uint8 *)&addend);
                relocation->relocation_addend = (uint64)addend;
                rela_content += sizeof(struct elf32_rela);
            }
            else {
                uint64 addend = (uint64)(((struct elf64_rela *)rela_content)->r_addend);
                if (is_binary_little_endian != is_little_endian())
                    exchange_uint64((uint8 *)&addend);
                relocation->relocation_addend = addend;
                rela_content += sizeof(struct elf64_rela);
            }
        }

        /* set relocation fields */
        relocation->relocation_offset = offset;
        relocation->relocation_type = (uint32)type;
        relocation->symbol_name = (char *)LLVMGetSymbolName(rel_sym);

        /* for ".LCPIxxx" relocation, transform the symbol name to real
         * section name and set addend to the symbol address */
        if (relocation->symbol_name
            && str_starts_with(relocation->symbol_name, ".LCPI")) {
            /* change relocation->relocation_addend and relocation->symbol_name */
            LLVMSectionIteratorRef contain_section;
            if (!(contain_section
                  = LLVMObjectFileCopySectionIterator(obj_data->binary))) {
                aot_set_last_error("llvm get section iterator failed.");
                goto fail;
            }
            LLVMMoveToContainingSection(contain_section, rel_sym);
            if (LLVMObjectFileIsSectionIteratorAtEnd(obj_data->binary, contain_section)) {
                LLVMDisposeSectionIterator(contain_section);
                aot_set_last_error("llvm get containing section failed.");
                goto fail;
            }
            relocation->relocation_addend += LLVMGetSymbolAddress(rel_sym);
            relocation->symbol_name = (char *)LLVMGetSectionName(contain_section);
            LLVMDisposeSectionIterator(contain_section);
        }

        LLVMDisposeSymbolIterator(rel_sym);
        LLVMMoveToNextRelocation(rel_itr);
        relocation++;
    }
    LLVMDisposeRelocationIterator(rel_itr);
    return true;

fail:
    LLVMDisposeRelocationIterator(rel_itr);
    return false;
}

static bool
is_relocation_section_name(char *section_name)
{
    return (!strcmp(section_name, ".rela.text")
            || !strcmp(section_name, ".rel.text")
            || !strcmp(section_name, ".rela.literal")
            || !strcmp(section_name, ".rela.data")
            || !strcmp(section_name, ".rel.data")
            || !strcmp(section_name, ".rela.sdata")
            || !strcmp(section_name, ".rel.sdata")
            || !strcmp(section_name, ".rela.rodata")
            || !strcmp(section_name, ".rel.rodata")
            /* ".rela.rodata.cst4/8/16/.." */
            || !strncmp(section_name, ".rela.rodata.cst",
                        strlen(".rela.rodata.cst"))
            /* ".rel.rodata.cst4/8/16/.." */
            || !strncmp(section_name, ".rel.rodata.cst",
                        strlen(".rel.rodata.cst")));
}

static bool
is_relocation_section(LLVMSectionIteratorRef sec_itr)
{
    uint32 count = 0;
    char *name = (char *)LLVMGetSectionName(sec_itr);
    if (name) {
        if (is_relocation_section_name(name))
            return true;
        else if ((!strcmp(name, ".text") || !strcmp(name, ".rdata"))
                 && get_relocations_count(sec_itr, &count) && count > 0)
            return true;
    }
    return false;
}

static bool
get_relocation_groups_count(AOTObjectData *obj_data, uint32 *p_count)
{
    uint32 count = 0;
    LLVMSectionIteratorRef sec_itr;

    if (!(sec_itr = LLVMObjectFileCopySectionIterator(obj_data->binary))) {
        aot_set_last_error("llvm get section iterator failed.");
        return false;
    }
    while (!LLVMObjectFileIsSectionIteratorAtEnd(obj_data->binary, sec_itr)) {
        if (is_relocation_section(sec_itr)) {
            count++;
        }
        LLVMMoveToNextSection(sec_itr);
    }
    LLVMDisposeSectionIterator(sec_itr);

    *p_count = count;
    return true;
}

static bool
aot_resolve_object_relocation_groups(AOTObjectData *obj_data)
{
    LLVMSectionIteratorRef sec_itr;
    AOTRelocationGroup *relocation_group;
    uint32 group_count;
    char *name;
    uint32 size;

    /* calculate relocation groups count and allcate memory */
    if (!get_relocation_groups_count(obj_data, &group_count))
        return false;

    if (0 == (obj_data->relocation_group_count = group_count))
        return true;

    size = (uint32)sizeof(AOTRelocationGroup) * group_count;
    if (!(relocation_group = obj_data->relocation_groups = wasm_runtime_malloc(size))) {
        aot_set_last_error("allocate memory for relocation groups failed.");
        return false;
    }

    memset(obj_data->relocation_groups, 0, size);

    /* resolve each relocation group */
    if (!(sec_itr = LLVMObjectFileCopySectionIterator(obj_data->binary))) {
        aot_set_last_error("llvm get section iterator failed.");
        return false;
    }
    while (!LLVMObjectFileIsSectionIteratorAtEnd(obj_data->binary, sec_itr)) {
        if (is_relocation_section(sec_itr)) {
            name = (char *)LLVMGetSectionName(sec_itr);
            relocation_group->section_name = name;
            if (!aot_resolve_object_relocation_group(
                    obj_data,
                    relocation_group,
                    sec_itr)) {
                LLVMDisposeSectionIterator(sec_itr);
                return false;
            }
            relocation_group++;
        }
        LLVMMoveToNextSection(sec_itr);
    }
    LLVMDisposeSectionIterator(sec_itr);

    return true;
}

static void
destroy_relocation_groups(AOTRelocationGroup *relocation_groups,
                          uint32 relocation_group_count)
{
    uint32 i;
    AOTRelocationGroup *relocation_group = relocation_groups;

    for (i = 0; i < relocation_group_count; i++, relocation_group++)
        if (relocation_group->relocations)
            wasm_runtime_free(relocation_group->relocations);
    wasm_runtime_free(relocation_groups);
}

static void
destroy_relocation_symbol_list(AOTSymbolList *symbol_list)
{
    AOTSymbolNode *elem;

    elem = symbol_list->head;
    while (elem) {
        AOTSymbolNode *next = elem->next;
        wasm_runtime_free(elem);
        elem = next;
    }
}

static void
aot_obj_data_destroy(AOTObjectData *obj_data)
{
    if (obj_data->binary)
        LLVMDisposeBinary(obj_data->binary);
    if (obj_data->mem_buf)
        LLVMDisposeMemoryBuffer(obj_data->mem_buf);
    if (obj_data->funcs)
        wasm_runtime_free(obj_data->funcs);
    if (obj_data->data_sections)
        wasm_runtime_free(obj_data->data_sections);
    if (obj_data->relocation_groups)
        destroy_relocation_groups(obj_data->relocation_groups,
                                  obj_data->relocation_group_count);
    if (obj_data->symbol_list.len)
        destroy_relocation_symbol_list(&obj_data->symbol_list);
    wasm_runtime_free(obj_data);
}

static AOTObjectData *
aot_obj_data_create(AOTCompContext *comp_ctx)
{
    char *err = NULL;
    AOTObjectData *obj_data;

    bh_print_time("Begin to emit object file to buffer");

    if (!(obj_data = wasm_runtime_malloc(sizeof(AOTObjectData)))) {
        aot_set_last_error("allocate memory failed.");
        return false;
    }
    memset(obj_data, 0, sizeof(AOTObjectData));

    if (LLVMTargetMachineEmitToMemoryBuffer(comp_ctx->target_machine,
                                            comp_ctx->module,
                                            LLVMObjectFile,
                                            &err,
                                            &obj_data->mem_buf) != 0) {
        if (err) {
            LLVMDisposeMessage(err);
            err = NULL;
        }
        aot_set_last_error("llvm emit to memory buffer failed.");
        goto fail;
    }

    if (!(obj_data->binary =
                LLVMCreateBinary(obj_data->mem_buf, NULL, &err))) {
        if (err) {
            LLVMDisposeMessage(err);
            err = NULL;
        }
        aot_set_last_error("llvm create binary failed.");
        goto fail;
    }

    bh_print_time("Begin to resolve object file info");

    /* resolve target info/text/relocations/functions */
    if (!aot_resolve_target_info(comp_ctx, obj_data)
        || !aot_resolve_text(obj_data)
        || !aot_resolve_literal(obj_data)
        || !aot_resolve_object_data_sections(obj_data)
        || !aot_resolve_object_relocation_groups(obj_data)
        || !aot_resolve_functions(comp_ctx, obj_data))
        goto fail;

    return obj_data;

fail:
    aot_obj_data_destroy(obj_data);
    return NULL;
}

uint8*
aot_emit_aot_file_buf(AOTCompContext *comp_ctx,
                      AOTCompData *comp_data,
                      uint32 *p_aot_file_size)
{
    AOTObjectData *obj_data = aot_obj_data_create(comp_ctx);
    uint8 *aot_file_buf, *buf, *buf_end;
    uint32 aot_file_size, offset = 0;

    if (!obj_data)
        return NULL;

    aot_file_size = get_aot_file_size(comp_ctx, comp_data, obj_data);

    if (!(buf = aot_file_buf = wasm_runtime_malloc(aot_file_size))) {
        aot_set_last_error("allocate memory failed.");
        goto fail1;
    }

    memset(aot_file_buf, 0, aot_file_size);
    buf_end = buf + aot_file_size;

    if (!aot_emit_file_header(buf, buf_end, &offset, comp_data, obj_data)
        || !aot_emit_target_info_section(buf, buf_end, &offset, comp_data, obj_data)
        || !aot_emit_init_data_section(buf, buf_end, &offset, comp_ctx, comp_data, obj_data)
        || !aot_emit_text_section(buf, buf_end, &offset, comp_data, obj_data)
        || !aot_emit_func_section(buf, buf_end, &offset, comp_data, obj_data)
        || !aot_emit_export_section(buf, buf_end, &offset, comp_data, obj_data)
        || !aot_emit_relocation_section(buf, buf_end, &offset, comp_data, obj_data))
        goto fail2;

#if 0
    dump_buf(buf, offset, "sections");
#endif

    if (offset != aot_file_size) {
        aot_set_last_error("emit aot file failed.");
        goto fail2;
    }

    *p_aot_file_size = aot_file_size;

    aot_obj_data_destroy(obj_data);
    return aot_file_buf;

fail2:
    wasm_runtime_free(aot_file_buf);

fail1:
    aot_obj_data_destroy(obj_data);
    return NULL;
}

bool
aot_emit_aot_file(AOTCompContext *comp_ctx, AOTCompData *comp_data,
                  const char *file_name)
{
    uint8 *aot_file_buf;
    uint32 aot_file_size;
    bool ret = false;
    FILE *file;

    bh_print_time("Begin to emit AOT file");

    if (!(aot_file_buf = aot_emit_aot_file_buf(comp_ctx, comp_data,
                                               &aot_file_size))) {
        return false;
    }

    /* write buffer to file */
    if (!(file = fopen(file_name, "wb"))) {
        aot_set_last_error("open or create aot file failed.");
        goto fail1;
    }
    if (!fwrite(aot_file_buf, aot_file_size, 1, file)) {
        aot_set_last_error("write to aot file failed.");
        goto fail2;
    }

    ret = true;

fail2:
    fclose(file);

fail1:
    wasm_runtime_free(aot_file_buf);

    return ret;
}
