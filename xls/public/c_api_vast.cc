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

#include "xls/public/c_api_vast.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "absl/types/span.h"
#include "xls/codegen/vast/vast.h"
#include "xls/ir/bits.h"
#include "xls/ir/format_preference.h"
#include "xls/ir/source_location.h"
#include "xls/public/c_api_format_preference.h"
#include "xls/public/c_api_impl_helpers.h"

extern "C" {

struct xls_vast_verilog_file* xls_vast_make_verilog_file(
    xls_vast_file_type file_type) {
  auto* value = new xls::verilog::VerilogFile(
      static_cast<xls::verilog::FileType>(file_type));
  return reinterpret_cast<xls_vast_verilog_file*>(value);
}

void xls_vast_verilog_file_free(struct xls_vast_verilog_file* f) {
  delete reinterpret_cast<xls::verilog::VerilogFile*>(f);
}

struct xls_vast_verilog_module* xls_vast_verilog_file_add_module(
    struct xls_vast_verilog_file* f, const char* name) {
  auto* cpp_file = reinterpret_cast<xls::verilog::VerilogFile*>(f);
  xls::verilog::Module* cpp_module =
      cpp_file->AddModule(name, xls::SourceInfo());
  return reinterpret_cast<xls_vast_verilog_module*>(cpp_module);
}

void xls_vast_verilog_file_add_include(struct xls_vast_verilog_file* f,
                                       const char* path) {
  auto* cpp_file = reinterpret_cast<xls::verilog::VerilogFile*>(f);
  cpp_file->AddInclude(path, xls::SourceInfo());
}

struct xls_vast_logic_ref* xls_vast_verilog_module_add_input(
    struct xls_vast_verilog_module* m, const char* name,
    struct xls_vast_data_type* type) {
  auto* cpp_module = reinterpret_cast<xls::verilog::Module*>(m);
  auto* cpp_type = reinterpret_cast<xls::verilog::DataType*>(type);
  xls::verilog::LogicRef* logic_ref =
      cpp_module->AddInput(name, cpp_type, xls::SourceInfo());
  return reinterpret_cast<xls_vast_logic_ref*>(logic_ref);
}

struct xls_vast_logic_ref* xls_vast_verilog_module_add_output(
    struct xls_vast_verilog_module* m, const char* name,
    struct xls_vast_data_type* type) {
  auto* cpp_module = reinterpret_cast<xls::verilog::Module*>(m);
  auto* cpp_type = reinterpret_cast<xls::verilog::DataType*>(type);
  xls::verilog::LogicRef* logic_ref =
      cpp_module->AddOutput(name, cpp_type, xls::SourceInfo());
  return reinterpret_cast<xls_vast_logic_ref*>(logic_ref);
}

struct xls_vast_logic_ref* xls_vast_verilog_module_add_wire(
    struct xls_vast_verilog_module* m, const char* name,
    struct xls_vast_data_type* type) {
  auto* cpp_module = reinterpret_cast<xls::verilog::Module*>(m);
  auto* cpp_type = reinterpret_cast<xls::verilog::DataType*>(type);
  xls::verilog::LogicRef* logic_ref =
      cpp_module->AddWire(name, cpp_type, xls::SourceInfo());
  return reinterpret_cast<xls_vast_logic_ref*>(logic_ref);
}

char* xls_vast_verilog_file_emit(const struct xls_vast_verilog_file* f) {
  const auto* cpp_file = reinterpret_cast<const xls::verilog::VerilogFile*>(f);
  std::string result = cpp_file->Emit();
  return xls::ToOwnedCString(result);
}

struct xls_vast_data_type* xls_vast_verilog_file_make_scalar_type(
    struct xls_vast_verilog_file* f) {
  auto* cpp_file = reinterpret_cast<xls::verilog::VerilogFile*>(f);
  xls::verilog::DataType* type = cpp_file->ScalarType(xls::SourceInfo());
  return reinterpret_cast<xls_vast_data_type*>(type);
}

struct xls_vast_data_type* xls_vast_verilog_file_make_bit_vector_type(
    struct xls_vast_verilog_file* f, int64_t bit_count, bool is_signed) {
  auto* cpp_file = reinterpret_cast<xls::verilog::VerilogFile*>(f);
  xls::verilog::DataType* type =
      cpp_file->BitVectorType(bit_count, xls::SourceInfo(), is_signed);
  return reinterpret_cast<xls_vast_data_type*>(type);
}

struct xls_vast_data_type* xls_vast_verilog_file_make_extern_package_type(
    struct xls_vast_verilog_file* f, const char* package_name,
    const char* entity_name) {
  auto* cpp_file = reinterpret_cast<xls::verilog::VerilogFile*>(f);
  xls::verilog::DataType* type =
      cpp_file->Make<xls::verilog::ExternPackageType>(
          xls::SourceInfo(), package_name, entity_name);
  return reinterpret_cast<xls_vast_data_type*>(type);
}

struct xls_vast_data_type* xls_vast_verilog_file_make_packed_array_type(
    struct xls_vast_verilog_file* f, struct xls_vast_data_type* element_type,
    const int64_t* packed_dims, size_t packed_dims_count) {
  auto* cpp_file = reinterpret_cast<xls::verilog::VerilogFile*>(f);
  auto* cpp_element_type =
      reinterpret_cast<xls::verilog::DataType*>(element_type);
  absl::Span<const int64_t> dims(packed_dims, packed_dims_count);
  xls::verilog::DataType* type = cpp_file->Make<xls::verilog::PackedArrayType>(
      xls::SourceInfo(), cpp_element_type, dims, /*dims_are_max=*/false);
  return reinterpret_cast<xls_vast_data_type*>(type);
}

void xls_vast_verilog_module_add_member_instantiation(
    struct xls_vast_verilog_module* m, struct xls_vast_instantiation* member) {
  auto* cpp_module = reinterpret_cast<xls::verilog::Module*>(m);
  auto* cpp_instantiation =
      reinterpret_cast<xls::verilog::Instantiation*>(member);
  cpp_module->AddModuleMember(cpp_instantiation);
}

void xls_vast_verilog_module_add_member_continuous_assignment(
    struct xls_vast_verilog_module* m,
    struct xls_vast_continuous_assignment* member) {
  auto* cpp_module = reinterpret_cast<xls::verilog::Module*>(m);
  auto* cpp_member =
      reinterpret_cast<xls::verilog::ContinuousAssignment*>(member);
  cpp_module->AddModuleMember(cpp_member);
}

struct xls_vast_literal* xls_vast_verilog_file_make_plain_literal(
    struct xls_vast_verilog_file* f, int32_t value) {
  auto* cpp_file = reinterpret_cast<xls::verilog::VerilogFile*>(f);
  xls::verilog::Literal* cpp_literal =
      cpp_file->PlainLiteral(value, xls::SourceInfo());
  return reinterpret_cast<xls_vast_literal*>(cpp_literal);
}

bool xls_vast_verilog_file_make_literal(struct xls_vast_verilog_file* f,
                                        struct xls_bits* bits,
                                        xls_format_preference format_preference,
                                        bool emit_bit_count, char** error_out,
                                        struct xls_vast_literal** literal_out) {
  auto* cpp_file = reinterpret_cast<xls::verilog::VerilogFile*>(f);
  auto* cpp_bits = reinterpret_cast<xls::Bits*>(bits);
  xls::FormatPreference cpp_pref;
  if (!xls::FormatPreferenceFromC(format_preference, &cpp_pref, error_out)) {
    return false;
  }
  xls::verilog::Literal* cpp_literal = cpp_file->Make<xls::verilog::Literal>(
      xls::SourceInfo(), *cpp_bits, cpp_pref, emit_bit_count);
  *error_out = nullptr;
  *literal_out = reinterpret_cast<xls_vast_literal*>(cpp_literal);
  return true;
}

struct xls_vast_continuous_assignment*
xls_vast_verilog_file_make_continuous_assignment(
    struct xls_vast_verilog_file* f, struct xls_vast_expression* lhs,
    struct xls_vast_expression* rhs) {
  auto* cpp_file = reinterpret_cast<xls::verilog::VerilogFile*>(f);
  auto* cpp_lhs = reinterpret_cast<xls::verilog::Expression*>(lhs);
  auto* cpp_rhs = reinterpret_cast<xls::verilog::Expression*>(rhs);
  auto* cpp_assignment = cpp_file->Make<xls::verilog::ContinuousAssignment>(
      xls::SourceInfo(), cpp_lhs, cpp_rhs);
  return reinterpret_cast<xls_vast_continuous_assignment*>(cpp_assignment);
}

struct xls_vast_instantiation* xls_vast_verilog_file_make_instantiation(
    struct xls_vast_verilog_file* f, const char* module_name,
    const char* instance_name, const char** parameter_port_names,
    struct xls_vast_expression** parameter_expressions, size_t parameter_count,
    const char** connection_port_names,
    struct xls_vast_expression** connection_expressions,
    size_t connection_count) {
  auto* cpp_file = reinterpret_cast<xls::verilog::VerilogFile*>(f);

  std::vector<xls::verilog::Connection> parameters;
  parameters.reserve(parameter_count);
  for (size_t i = 0; i < parameter_count; ++i) {
    auto* cpp_expression =
        reinterpret_cast<xls::verilog::Expression*>(parameter_expressions[i]);
    parameters.push_back(
        xls::verilog::Connection{parameter_port_names[i], cpp_expression});
  }

  std::vector<xls::verilog::Connection> connections;
  connections.reserve(connection_count);
  for (size_t i = 0; i < connection_count; ++i) {
    auto* cpp_expression =
        reinterpret_cast<xls::verilog::Expression*>(connection_expressions[i]);
    connections.push_back(
        xls::verilog::Connection{connection_port_names[i], cpp_expression});
  }

  auto* cpp_instantiation = cpp_file->Make<xls::verilog::Instantiation>(
      xls::SourceInfo(), module_name, instance_name,
      absl::MakeConstSpan(parameters), absl::MakeConstSpan(connections));
  return reinterpret_cast<xls_vast_instantiation*>(cpp_instantiation);
}

struct xls_vast_expression* xls_vast_literal_as_expression(
    struct xls_vast_literal* v) {
  auto* cpp_literal = reinterpret_cast<xls::verilog::Literal*>(v);
  auto* cpp_expression = static_cast<xls::verilog::Expression*>(cpp_literal);
  return reinterpret_cast<xls_vast_expression*>(cpp_expression);
}

struct xls_vast_expression* xls_vast_logic_ref_as_expression(
    struct xls_vast_logic_ref* v) {
  auto* cpp_v = reinterpret_cast<xls::verilog::LogicRef*>(v);
  auto* cpp_expression = static_cast<xls::verilog::Expression*>(cpp_v);
  return reinterpret_cast<xls_vast_expression*>(cpp_expression);
}

struct xls_vast_expression* xls_vast_slice_as_expression(
    struct xls_vast_slice* v) {
  auto* cpp_v = reinterpret_cast<xls::verilog::Slice*>(v);
  auto* cpp_expression = static_cast<xls::verilog::Expression*>(cpp_v);
  return reinterpret_cast<xls_vast_expression*>(cpp_expression);
}

struct xls_vast_expression* xls_vast_concat_as_expression(
    struct xls_vast_concat* v) {
  auto* cpp_v = reinterpret_cast<xls::verilog::Concat*>(v);
  auto* cpp_expression = static_cast<xls::verilog::Expression*>(cpp_v);
  return reinterpret_cast<xls_vast_expression*>(cpp_expression);
}

struct xls_vast_indexable_expression*
xls_vast_logic_ref_as_indexable_expression(
    struct xls_vast_logic_ref* logic_ref) {
  auto* cpp_logic_ref = reinterpret_cast<xls::verilog::LogicRef*>(logic_ref);
  auto* cpp_indexable_expression =
      static_cast<xls::verilog::IndexableExpression*>(cpp_logic_ref);
  return reinterpret_cast<xls_vast_indexable_expression*>(
      cpp_indexable_expression);
}

struct xls_vast_indexable_expression* xls_vast_index_as_indexable_expression(
    struct xls_vast_index* index) {
  auto* cpp_index = reinterpret_cast<xls::verilog::Index*>(index);
  auto* cpp_indexable_expression =
      static_cast<xls::verilog::IndexableExpression*>(cpp_index);
  return reinterpret_cast<xls_vast_indexable_expression*>(
      cpp_indexable_expression);
}

struct xls_vast_slice* xls_vast_verilog_file_make_slice_i64(
    struct xls_vast_verilog_file* f,
    struct xls_vast_indexable_expression* subject, int64_t hi, int64_t lo) {
  auto* cpp_file = reinterpret_cast<xls::verilog::VerilogFile*>(f);
  auto* cpp_subject =
      reinterpret_cast<xls::verilog::IndexableExpression*>(subject);
  xls::verilog::Slice* cpp_slice =
      cpp_file->Slice(cpp_subject, hi, lo, xls::SourceInfo());
  return reinterpret_cast<xls_vast_slice*>(cpp_slice);
}

struct xls_vast_slice* xls_vast_verilog_file_make_slice(
    struct xls_vast_verilog_file* f,
    struct xls_vast_indexable_expression* subject,
    struct xls_vast_expression* hi, struct xls_vast_expression* lo) {
  auto* cpp_file = reinterpret_cast<xls::verilog::VerilogFile*>(f);
  auto* cpp_subject =
      reinterpret_cast<xls::verilog::IndexableExpression*>(subject);
  auto* cpp_hi = reinterpret_cast<xls::verilog::Expression*>(hi);
  auto* cpp_lo = reinterpret_cast<xls::verilog::Expression*>(lo);
  xls::verilog::Slice* cpp_slice =
      cpp_file->Slice(cpp_subject, cpp_hi, cpp_lo, xls::SourceInfo());
  return reinterpret_cast<xls_vast_slice*>(cpp_slice);
}

struct xls_vast_concat* xls_vast_verilog_file_make_concat(
    struct xls_vast_verilog_file* f, struct xls_vast_expression** elements,
    size_t element_count) {
  auto* cpp_file = reinterpret_cast<xls::verilog::VerilogFile*>(f);
  std::vector<xls::verilog::Expression*> cpp_elements;
  cpp_elements.reserve(element_count);
  for (size_t i = 0; i < element_count; ++i) {
    auto* cpp_element =
        reinterpret_cast<xls::verilog::Expression*>(elements[i]);
    cpp_elements.push_back(cpp_element);
  }
  xls::verilog::Concat* cpp_concat =
      cpp_file->Make<xls::verilog::Concat>(xls::SourceInfo(), cpp_elements);
  return reinterpret_cast<xls_vast_concat*>(cpp_concat);
}

struct xls_vast_index* xls_vast_verilog_file_make_index_i64(
    struct xls_vast_verilog_file* f,
    struct xls_vast_indexable_expression* subject, int64_t index) {
  auto* cpp_file = reinterpret_cast<xls::verilog::VerilogFile*>(f);
  auto* cpp_subject =
      reinterpret_cast<xls::verilog::IndexableExpression*>(subject);
  xls::verilog::Index* cpp_index =
      cpp_file->Index(cpp_subject, index, xls::SourceInfo());
  return reinterpret_cast<xls_vast_index*>(cpp_index);
}

struct xls_vast_index* xls_vast_verilog_file_make_index(
    struct xls_vast_verilog_file* f,
    struct xls_vast_indexable_expression* subject,
    struct xls_vast_expression* index) {
  auto* cpp_file = reinterpret_cast<xls::verilog::VerilogFile*>(f);
  auto* cpp_subject =
      reinterpret_cast<xls::verilog::IndexableExpression*>(subject);
  auto* cpp_index = reinterpret_cast<xls::verilog::Expression*>(index);
  xls::verilog::Index* result =
      cpp_file->Index(cpp_subject, cpp_index, xls::SourceInfo());
  return reinterpret_cast<xls_vast_index*>(result);
}

}  // extern "C"
