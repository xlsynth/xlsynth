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

// VAST (Verilog AST) APIs
//
// Note that these are expected to be *less* stable than other public C APIs,
// as they are exposing a useful implementation library present within XLS.
//
// Per usual, in a general sense, no promises are made around API or ABI
// stability overall. However, seems worth noting these are effectively
// "protected" APIs, use with particular caution around stability. See
// `xls/protected/BUILD` for how we tend to think about "protected" APIs in the
// project.

#ifndef XLS_PUBLIC_C_API_VAST_H_
#define XLS_PUBLIC_C_API_VAST_H_

#include <stddef.h>  // NOLINT(modernize-deprecated-headers)
#include <stdint.h>  // NOLINT(modernize-deprecated-headers)

#include "xls/public/c_api_format_preference.h"

extern "C" {

// Opaque structs.
struct xls_vast_verilog_file;
struct xls_vast_verilog_module;
struct xls_vast_node;
struct xls_vast_expression;
struct xls_vast_logic_ref;
struct xls_vast_data_type;
struct xls_vast_indexable_expression;
struct xls_vast_slice;
struct xls_vast_literal;
struct xls_vast_instantiation;
struct xls_vast_continuous_assignment;

// Note: We define the enum with a fixed width integer type for clarity of the
// exposed ABI.
typedef int32_t xls_vast_file_type;
enum {
  xls_vast_file_type_verilog,
  xls_vast_file_type_system_verilog,
};

// Note: caller owns the returned verilog file object, to be freed by
// `xls_vast_verilog_file_free`.
struct xls_vast_verilog_file* xls_vast_make_verilog_file(
    xls_vast_file_type file_type);

void xls_vast_verilog_file_free(struct xls_vast_verilog_file* f);

struct xls_vast_verilog_module* xls_vast_verilog_file_add_module(
    struct xls_vast_verilog_file* f, const char* name);

// -- VerilogFile::Make*Type

struct xls_vast_data_type* xls_vast_verilog_file_make_scalar_type(
    struct xls_vast_verilog_file* f);

struct xls_vast_data_type* xls_vast_verilog_file_make_bit_vector_type(
    struct xls_vast_verilog_file* f, int64_t bit_count, bool is_signed);

struct xls_vast_data_type* xls_vast_verilog_file_make_extern_package_type(
    struct xls_vast_verilog_file* f, const char* package_name,
    const char* entity_name);

struct xls_vast_data_type* xls_vast_verilog_file_make_packed_array_type(
    struct xls_vast_verilog_file* f, xls_vast_data_type* element_type,
    const int64_t* packed_dims, size_t packed_dims_count);

// -- Module::Add*

void xls_vast_verilog_module_add_member_instantiation(
    struct xls_vast_verilog_module* m, struct xls_vast_instantiation* member);
void xls_vast_verilog_module_add_member_continuous_assignment(
    struct xls_vast_verilog_module* m,
    struct xls_vast_continuous_assignment* member);

struct xls_vast_logic_ref* xls_vast_verilog_module_add_input(
    struct xls_vast_verilog_module* m, const char* name,
    struct xls_vast_data_type* type);
struct xls_vast_logic_ref* xls_vast_verilog_module_add_output(
    struct xls_vast_verilog_module* m, const char* name,
    struct xls_vast_data_type* type);
struct xls_vast_logic_ref* xls_vast_verilog_module_add_wire(
    struct xls_vast_verilog_module* m, const char* name,
    struct xls_vast_data_type* type);
// TODO(cdleary): 2024-09-05 Add xls_vast_verilog_module_add_wire_with_expr

struct xls_vast_continuous_assignment*
xls_vast_verilog_file_make_continuous_assignment(
    struct xls_vast_verilog_file* f, struct xls_vast_expression* lhs,
    struct xls_vast_expression* rhs);

struct xls_vast_instantiation* xls_vast_verilog_file_make_instantiation(
    struct xls_vast_verilog_file* f, const char* module_name,
    const char* instance_name, const char** parameter_port_names,
    struct xls_vast_expression** parameter_expressions, size_t parameter_count,
    const char** connection_port_names,
    struct xls_vast_expression** connection_expressions,
    size_t connection_count);

void xls_vast_verilog_file_add_include(struct xls_vast_verilog_file* f,
                                       const char* path);

struct xls_vast_concat* xls_vast_verilog_file_make_concat(
    struct xls_vast_verilog_file* f, struct xls_vast_expression** elements,
    size_t element_count);

struct xls_vast_slice* xls_vast_verilog_file_make_slice_i64(
    struct xls_vast_verilog_file* f,
    struct xls_vast_indexable_expression* subject, int64_t hi, int64_t lo);

struct xls_vast_slice* xls_vast_verilog_file_make_slice(
    struct xls_vast_verilog_file* f,
    struct xls_vast_indexable_expression* subject,
    struct xls_vast_expression* hi, struct xls_vast_expression* lo);

struct xls_vast_index* xls_vast_verilog_file_make_index_i64(
    struct xls_vast_verilog_file* f,
    struct xls_vast_indexable_expression* subject, int64_t index);

struct xls_vast_index* xls_vast_verilog_file_make_index(
    struct xls_vast_verilog_file* f,
    struct xls_vast_indexable_expression* subject,
    struct xls_vast_expression* index);

struct xls_vast_literal* xls_vast_verilog_file_make_plain_literal(
    struct xls_vast_verilog_file* f, int32_t value);

// Creates a VAST literal with an arbitrary bit count.
//
// Returns an error if the given format preference is invalid.
bool xls_vast_verilog_file_make_literal(struct xls_vast_verilog_file* f,
                                        struct xls_bits* bits,
                                        xls_format_preference format_preference,
                                        bool emit_bit_count, char** error_out,
                                        struct xls_vast_literal** literal_out);

// Casts to turn the given node to an expression, where possible.
struct xls_vast_expression* xls_vast_literal_as_expression(
    struct xls_vast_literal* v);
struct xls_vast_expression* xls_vast_logic_ref_as_expression(
    struct xls_vast_logic_ref* v);
struct xls_vast_expression* xls_vast_slice_as_expression(
    struct xls_vast_slice* v);
struct xls_vast_expression* xls_vast_concat_as_expression(
    struct xls_vast_concat* v);

struct xls_vast_indexable_expression*
xls_vast_logic_ref_as_indexable_expression(
    struct xls_vast_logic_ref* logic_ref);

struct xls_vast_indexable_expression* xls_vast_index_as_indexable_expression(
    struct xls_vast_index* index);

// Emits/formats the contents of the given verilog file to a string.
//
// Note: caller owns the returned string, to be freed by `xls_c_str_free`.
char* xls_vast_verilog_file_emit(const struct xls_vast_verilog_file* f);

}  // extern "C"

#endif  // XLS_PUBLIC_C_API_VAST_H_
