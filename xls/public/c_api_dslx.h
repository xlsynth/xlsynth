// Copyright 2024 The XLS Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// DSLX (Domain Specific Language "X") APIs
//
// Note that these are expected to be *less* stable than other public C APIs,
// as they are exposing a useful implementation library present within XLS.
//
// Per usual, in a general sense, no promises are made around API or ABI
// stability overall. However, seems worth noting these are effectively
// "protected" APIs, use with particular caution around stability. See
// `xls/protected/BUILD` for how we tend to think about "protected" APIs in the
// project.

#ifndef XLS_PUBLIC_C_API_DSLX_H_
#define XLS_PUBLIC_C_API_DSLX_H_

#include <stddef.h>  // NOLINT(modernize-deprecated-headers)
#include <stdint.h>  // NOLINT(modernize-deprecated-headers)

#ifndef XLS_DLL_EXPORT
#define XLS_DLL_EXPORT __attribute__((visibility("default")))
#endif  // XLS_DLL_EXPORT

extern "C" {

typedef int32_t xls_dslx_type_definition_kind;
enum {
  xls_dslx_type_definition_kind_type_alias,
  xls_dslx_type_definition_kind_struct_def,
  xls_dslx_type_definition_kind_enum_def,
  xls_dslx_type_definition_kind_colon_ref,
  xls_dslx_type_definition_kind_proc_def,
};

// Opaque structs.
struct xls_dslx_typechecked_module;
struct xls_dslx_import_data;
struct xls_dslx_module;
struct xls_dslx_type_definition;
struct xls_dslx_struct_def;
struct xls_dslx_enum_def;
struct xls_dslx_type_alias;
struct xls_dslx_type_info;
struct xls_dslx_type;
struct xls_dslx_type_annotation;

XLS_DLL_EXPORT
struct xls_dslx_import_data* xls_dslx_import_data_create(
    const char* dslx_stdlib_path, const char* additional_search_paths[],
    size_t additional_search_paths_count);

XLS_DLL_EXPORT
void xls_dslx_import_data_free(struct xls_dslx_import_data*);

XLS_DLL_EXPORT
bool xls_dslx_parse_and_typecheck(
    const char* text, const char* path, const char* module_name,
    struct xls_dslx_import_data* import_data, char** error_out,
    struct xls_dslx_typechecked_module** result_out);

XLS_DLL_EXPORT
void xls_dslx_typechecked_module_free(struct xls_dslx_typechecked_module* tm);

XLS_DLL_EXPORT
struct xls_dslx_module* xls_dslx_typechecked_module_get_module(
    struct xls_dslx_typechecked_module*);
XLS_DLL_EXPORT
struct xls_dslx_type_info* xls_dslx_typechecked_module_get_type_info(
    struct xls_dslx_typechecked_module*);

XLS_DLL_EXPORT
int64_t xls_dslx_module_get_type_definition_count(
    struct xls_dslx_module* module);

XLS_DLL_EXPORT
xls_dslx_type_definition_kind xls_dslx_module_get_type_definition_kind(
    struct xls_dslx_module* module, int64_t i);

XLS_DLL_EXPORT
struct xls_dslx_struct_def* xls_dslx_module_get_type_definition_as_struct_def(
    struct xls_dslx_module* module, int64_t i);

XLS_DLL_EXPORT
struct xls_dslx_enum_def* xls_dslx_module_get_type_definition_as_enum_def(
    struct xls_dslx_module* module, int64_t i);

XLS_DLL_EXPORT
struct xls_dslx_type_alias* xls_dslx_module_get_type_definition_as_type_alias(
    struct xls_dslx_module* module, int64_t i);

// -- type_definition

struct xls_dslx_colon_ref* xls_dslx_type_defintion_get_colon_ref(
    struct xls_dslx_type_definition*);

// -- struct_def

// Note: the return value is owned by the caller and must be freed via
// `xls_c_str_free`.
XLS_DLL_EXPORT
char* xls_dslx_struct_def_get_identifier(struct xls_dslx_struct_def*);

XLS_DLL_EXPORT
bool xls_dslx_struct_def_is_parametric(struct xls_dslx_struct_def*);
XLS_DLL_EXPORT
int64_t xls_dslx_struct_def_get_member_count(struct xls_dslx_struct_def*);

XLS_DLL_EXPORT
struct xls_dslx_struct_member* xls_dslx_struct_def_get_member(
    struct xls_dslx_struct_def*, int64_t);

// Note: return value is owned by the caller, free via `xls_c_str_free`.
XLS_DLL_EXPORT
char* xls_dslx_struct_member_get_name(struct xls_dslx_struct_member*);

XLS_DLL_EXPORT
struct xls_dslx_type_annotation* xls_dslx_struct_member_get_type(
    struct xls_dslx_struct_member*);

// -- enum_def (AST node)

XLS_DLL_EXPORT
char* xls_dslx_enum_def_get_identifier(struct xls_dslx_enum_def*);

XLS_DLL_EXPORT
int64_t xls_dslx_enum_def_get_member_count(struct xls_dslx_enum_def*);

XLS_DLL_EXPORT
struct xls_dslx_enum_member* xls_dslx_enum_def_get_member(
    struct xls_dslx_enum_def*, int64_t);

struct xls_dslx_type_annotation* xls_dslx_enum_def_get_underlying(
    struct xls_dslx_enum_def*);

// Note: return value is owned by the caller, free via `xls_c_str_free`.
XLS_DLL_EXPORT
char* xls_dslx_enum_member_get_name(struct xls_dslx_enum_member*);

struct xls_dslx_expr* xls_dslx_enum_member_get_value(
    struct xls_dslx_enum_member*);

// -- type_annotation

// Attempts to convert the given type annotation to a TypeRefTypeAnnotation --
// returns nullptr if the conversion is not viable.
struct xls_dslx_type_ref_type_annotation*
xls_dslx_type_annotation_get_type_ref_type_annotation(
    struct xls_dslx_type_annotation*);

// -- type_ref_type_annotation

struct xls_dslx_type_ref* xls_dslx_type_ref_type_annotation_get_type_ref(
    struct xls_dslx_type_ref_type_annotation*);

// -- type_ref

struct xls_dslx_type_definition* xls_dslx_type_ref_get_type_definition(
    struct xls_dslx_type_ref*);

// -- type_definition

struct xls_dslx_colon_ref* xls_dslx_type_definition_get_colon_ref(
    struct xls_dslx_type_definition*);

// -- colon_ref

struct xls_dslx_import* xls_dslx_colon_ref_resolve_import_subject(
    struct xls_dslx_colon_ref*);

// -- type_alias

// Note: the return value is owned by the caller and must be freed via
// `xls_c_str_free`.
char* xls_dslx_type_alias_get_identifier(struct xls_dslx_type_alias*);

struct xls_dslx_type_annotation* xls_dslx_type_alias_get_type_annotation(
    struct xls_dslx_type_alias*);

// -- interp_value

bool xls_dslx_interp_value_convert_to_ir(struct xls_dslx_interp_value* v,
                                         char** error_out,
                                         struct xls_value** result_out);

void xls_dslx_interp_value_free(struct xls_dslx_interp_value*);

// -- type_info (deduced type information)

// Note: if there is no type information available for the given entity these
// may return null; however, if type checking has completed successfully this
// should not occur in practice.

const struct xls_dslx_type* xls_dslx_type_info_get_type_struct_def(
    struct xls_dslx_type_info*, struct xls_dslx_struct_def*);

const struct xls_dslx_type* xls_dslx_type_info_get_type_struct_member(
    struct xls_dslx_type_info*, struct xls_dslx_struct_member*);

const struct xls_dslx_type* xls_dslx_type_info_get_type_enum_def(
    struct xls_dslx_type_info*, struct xls_dslx_enum_def*);

const struct xls_dslx_type* xls_dslx_type_info_get_type_type_annotation(
    struct xls_dslx_type_info*, struct xls_dslx_type_annotation*);

// Note: the outparam is owned by the caller and must be freed via
// `xls_dslx_interp_value_free`.
bool xls_dslx_type_info_get_const_expr(
    struct xls_dslx_type_info* type_info, struct xls_dslx_expr* expr,
    char** error_out, struct xls_dslx_interp_value** result_out);

// -- type (deduced type information)

bool xls_dslx_type_get_total_bit_count(const struct xls_dslx_type*,
                                       char** error_out, int64_t* result_out);

// Returns whether the given type is a bits-like type with signedness 'true'.
bool xls_dslx_type_is_signed_bits(const struct xls_dslx_type*, char** error_out,
                                  bool* result_out);

bool xls_dslx_type_to_string(const struct xls_dslx_type*, char** error_out,
                             char** result_out);

// Note: on success the caller owns `is_signed` and `size` and must free them
// via `xls_dslx_type_dim_free`.
bool xls_dslx_type_is_bits_like(struct xls_dslx_type*,
                                struct xls_dslx_type_dim** is_signed,
                                struct xls_dslx_type_dim** size);

bool xls_dslx_type_is_enum(const struct xls_dslx_type*);

bool xls_dslx_type_is_struct(const struct xls_dslx_type*);

bool xls_dslx_type_is_array(const struct xls_dslx_type*);

// Precondition: xls_dslx_type_is_enum
struct xls_dslx_enum_def* xls_dslx_type_get_enum_def(struct xls_dslx_type*);

// Precondition: xls_dslx_type_is_struct
struct xls_dslx_struct_def* xls_dslx_type_get_struct_def(struct xls_dslx_type*);

// Precondition: xls_dslx_type_is_array
struct xls_dslx_type* xls_dslx_type_array_get_element_type(struct xls_dslx_type*);

// Note: returned xls_dslx_type_dim is owned by the caller and must be
// deallocated.
struct xls_dslx_type_dim* xls_dslx_type_array_get_size(struct xls_dslx_type*);

// -- type_dim (deduced type information)

bool xls_dslx_type_dim_is_parametric(struct xls_dslx_type_dim*);

bool xls_dslx_type_dim_get_as_bool(struct xls_dslx_type_dim*,
                                   char** error_out, bool* result_out);

bool xls_dslx_type_dim_get_as_int64(struct xls_dslx_type_dim*,
                                    char** error_out, int64_t* result_out);

void xls_dslx_type_dim_free(struct xls_dslx_type_dim*);

// Precondition: `type` must be an array type.
struct xls_dslx_type* xls_dslx_type_array_get_element_type(
    struct xls_dslx_type* type);

struct xls_dslx_type_dim* xls_dslx_type_array_get_size(
    struct xls_dslx_type* type);

}  // extern "C"

#endif  // XLS_PUBLIC_C_API_DSLX_H_
