// Copyright 2022 The XLS Authors
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

#include "xls/contrib/xlscc/translator_types.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "absl/container/btree_map.h"
#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"
#include "clang/include/clang/AST/Decl.h"
#include "clang/include/clang/AST/DeclTemplate.h"
#include "clang/include/clang/AST/TemplateBase.h"
#include "clang/include/clang/Basic/LLVM.h"
#include "xls/common/status/ret_check.h"
#include "xls/common/status/status_macros.h"
#include "xls/contrib/xlscc/metadata_output.pb.h"
#include "xls/contrib/xlscc/tracked_bvalue.h"
#include "xls/interpreter/ir_interpreter.h"
#include "xls/ir/bits.h"
#include "xls/ir/dfs_visitor.h"
#include "xls/ir/node.h"
#include "xls/ir/nodes.h"
#include "xls/ir/op.h"
#include "xls/ir/package.h"
#include "xls/ir/source_location.h"
#include "xls/ir/type.h"
#include "xls/ir/value.h"
#include "xls/ir/value_utils.h"

using ::std::shared_ptr;
using ::std::string;
using ::std::vector;

namespace xlscc {

absl::StatusOr<xls::Value> TranslatorTypeInterface::GetStructFieldXLS(
    xls::Value val, int index, const CStructType& type) {
  CHECK_LT(index, type.fields().size());
  if (type.no_tuple_flag()) {
    return val;
  }
  XLS_ASSIGN_OR_RETURN(std::vector<xls::Value> values, val.GetElements());
  return values.at(type.fields().size() - 1 - index);
}

CType::~CType() = default;

bool CType::operator!=(const CType& o) const { return !(*this == o); }

int CType::GetBitWidth() const { return 0; }

absl::StatusOr<bool> CType::ContainsLValues(
    TranslatorTypeInterface& translator) const {
  return false;
}

CType::operator std::string() const { return "CType"; }

xls::Type* CType::GetXLSType(xls::Package* /*package*/) const {
  LOG(FATAL) << "GetXLSType() unsupported in CType base class";
  return nullptr;
}

bool CType::StoredAsXLSBits() const { return false; }

absl::Status CType::GetMetadata(
    TranslatorTypeInterface& translator, xlscc_metadata::Type* output,
    absl::flat_hash_set<const clang::NamedDecl*>& aliases_used) const {
  return absl::UnimplementedError(
      "GetMetadata unsupported in CType base class");
}

absl::Status CType::GetMetadataValue(TranslatorTypeInterface& translator,
                                     const ConstValue const_value,
                                     xlscc_metadata::Value* output) const {
  return absl::UnimplementedError(
      "GetMetadataValue unsupported in CType base class");
}

CVoidType::~CVoidType() = default;

int CVoidType::GetBitWidth() const {
  CHECK(false);
  return 0;
}

absl::Status CVoidType::GetMetadata(
    TranslatorTypeInterface& translator, xlscc_metadata::Type* output,
    absl::flat_hash_set<const clang::NamedDecl*>& aliases_used) const {
  (void)output->mutable_as_void();
  return absl::OkStatus();
}

absl::Status CVoidType::GetMetadataValue(TranslatorTypeInterface& translator,
                                         const ConstValue const_value,
                                         xlscc_metadata::Value* output) const {
  return absl::OkStatus();
}

CVoidType::operator std::string() const { return "void"; }

bool CVoidType::operator==(const CType& o) const { return o.Is<CVoidType>(); }

CBitsType::CBitsType(int width) : width_(width) {}

CBitsType::~CBitsType() = default;

int CBitsType::GetBitWidth() const { return width_; }

CBitsType::operator std::string() const {
  return absl::StrFormat("bits[%d]", width_);
}

absl::Status CBitsType::GetMetadata(
    TranslatorTypeInterface& translator, xlscc_metadata::Type* output,
    absl::flat_hash_set<const clang::NamedDecl*>& aliases_used) const {
  output->mutable_as_bits()->set_width(width_);
  return absl::OkStatus();
}

absl::Status CBitsType::GetMetadataValue(TranslatorTypeInterface& translator,
                                         const ConstValue const_value,
                                         xlscc_metadata::Value* output) const {
  vector<uint8_t> bytes = const_value.rvalue().bits().ToBytes();
  // Bits::ToBytes is little-endian, data is stored in the proto as
  // big-endian.
  std::reverse(bytes.begin(), bytes.end());
  output->set_as_bits(bytes.data(), bytes.size());
  return absl::OkStatus();
}

bool CBitsType::StoredAsXLSBits() const { return true; }

bool CBitsType::operator==(const CType& o) const {
  if (!o.Is<CBitsType>()) {
    return false;
  }
  const auto* o_derived = o.As<CBitsType>();
  return width_ == o_derived->width_;
}

CIntType::~CIntType() = default;

CIntType::CIntType(int width, bool is_signed, bool is_declared_as_char)
    : width_(width),
      is_signed_(is_signed),
      is_declared_as_char_(is_declared_as_char) {}

xls::Type* CIntType::GetXLSType(xls::Package* package) const {
  return package->GetBitsType(width_);
}

bool CIntType::operator==(const CType& o) const {
  if (!o.Is<CIntType>()) {
    return false;
  }
  const auto* o_derived = o.As<CIntType>();
  if (width_ != o_derived->width_) {
    return false;
  }
  return is_signed_ == o_derived->is_signed_;
}

int CIntType::GetBitWidth() const { return width_; }

bool CIntType::StoredAsXLSBits() const { return true; }

CIntType::operator std::string() const {
  const std::string pre = is_signed_ ? "" : "unsigned_";
  if (width_ == 32) {
    return pre + "int";
  }
  if (width_ == 1) {
    return pre + "pseudobool";
  }
  if (width_ == 64) {
    return pre + "int64_t";
  }
  if (width_ == 16) {
    return pre + "short";
  }
  if (width_ == 8) {
    return pre + (is_declared_as_char() ? "char" : "int8_t");
  }
  return absl::StrFormat("native_int[%d]", width_);
}

absl::Status CIntType::GetMetadata(
    TranslatorTypeInterface& translator, xlscc_metadata::Type* output,
    absl::flat_hash_set<const clang::NamedDecl*>& aliases_used) const {
  output->mutable_as_int()->set_width(width_);
  output->mutable_as_int()->set_is_signed(is_signed_);
  if (width_ == 8) {
    output->mutable_as_int()->set_is_declared_as_char(is_declared_as_char_);
  }
  output->mutable_as_int()->set_is_synthetic(false);
  return absl::OkStatus();
}

absl::Status CIntType::GetMetadataValue(TranslatorTypeInterface& translator,
                                        const ConstValue const_value,
                                        xlscc_metadata::Value* output) const {
  auto value = const_value.rvalue();
  CHECK(value.IsBits());
  const xls::Bits& bits = value.bits();
  std::vector<uint8_t> bytes = bits.ToBytes();
  CHECK_GT(bytes.size(), 0);
  output->mutable_as_int()->set_big_endian_bytes(bytes.data(), bytes.size());
  return absl::OkStatus();
}

CFloatType::~CFloatType() = default;

CFloatType::CFloatType(bool double_precision)
    : double_precision_(double_precision) {}

xls::Type* CFloatType::GetXLSType(xls::Package* package) const {
  return package->GetBitsType(GetBitWidth());
}

bool CFloatType::operator==(const CType& o) const {
  if (!o.Is<CFloatType>()) {
    return false;
  }
  const auto* o_derived = o.As<CFloatType>();
  if (double_precision_ != o_derived->double_precision_) {
    return false;
  }
  return true;
}

// We just always store as double
int CFloatType::GetBitWidth() const {
  // Both double, int64_t, and fixed 32.32 forms
  return 64 * 3;
}

bool CFloatType::StoredAsXLSBits() const { return true; }

CFloatType::operator std::string() const {
  return double_precision_ ? "double" : "float";
}

absl::Status CFloatType::GetMetadata(
    TranslatorTypeInterface& translator, xlscc_metadata::Type* output,
    absl::flat_hash_set<const clang::NamedDecl*>& aliases_used) const {
  output->mutable_as_float()->set_is_double_precision(double_precision_);
  return absl::OkStatus();
}

absl::Status CFloatType::GetMetadataValue(TranslatorTypeInterface& translator,
                                          const ConstValue const_value,
                                          xlscc_metadata::Value* output) const {
  std::shared_ptr<CFloatType> float_type =
      std::dynamic_pointer_cast<CFloatType>(const_value.type());
  CHECK_NE(float_type, nullptr);
  xls::Value value = const_value.rvalue();
  CHECK(value.IsBits());
  const xls::Bits& bits = value.bits();
  vector<uint8_t> bytes = bits.ToBytes();
  double double_value = 0.0;
  CHECK_GE(bytes.size(), sizeof(double_value));
  memcpy(&double_value, bytes.data(), sizeof(double_value));
  output->mutable_as_float()->set_value(double_value);
  return absl::OkStatus();
}

CEnumType::~CEnumType() = default;

CEnumType::CEnumType(std::string name, int width, bool is_signed,
                     absl::btree_map<std::string, int64_t> variants_by_name)
    : CIntType(width, is_signed),
      name_(std ::move(name)),
      variants_by_name_(std::move(variants_by_name)) {
  for (const auto& variant : variants_by_name_) {
    if (!variants_by_value_.contains(variant.second)) {
      variants_by_value_.insert({variant.second, std::vector<std::string>()});
    }
    variants_by_value_[variant.second].push_back(variant.first);
  }
}

xls::Type* CEnumType::GetXLSType(xls::Package* package) const {
  return package->GetBitsType(width_);
}

bool CEnumType::operator==(const CType& o) const {
  if (!o.Is<CEnumType>()) {
    return false;
  }
  const auto* o_derived = o.As<CEnumType>();
  if (width_ != o_derived->width_) {
    return false;
  }
  return is_signed_ == o_derived->is_signed_;
}

int CEnumType::GetBitWidth() const { return width_; }

bool CEnumType::StoredAsXLSBits() const { return true; }

CEnumType::operator std::string() const {
  return absl::StrFormat("enum[%d]", width_);
}

absl::Status CEnumType::GetMetadata(
    TranslatorTypeInterface& translator, xlscc_metadata::Type* output,
    absl::flat_hash_set<const clang::NamedDecl*>& aliases_used) const {
  output->mutable_as_enum()->set_name(name_);
  output->mutable_as_enum()->set_width(width_);
  output->mutable_as_enum()->set_is_signed(is_signed_);
  absl::btree_map<int64_t, xlscc_metadata::EnumVariant*> proto_variants;
  for (const auto& [variant_name, variant_value] : variants_by_name_) {
    if (!proto_variants.contains(variant_value)) {
      auto proto_variant = output->mutable_as_enum()->add_variants();
      proto_variant->set_value(variant_value);
      proto_variants.insert({variant_value, proto_variant});
    }
    proto_variants[variant_value]->add_name(variant_name);
  }
  return absl::OkStatus();
}

absl::Status CEnumType::GetMetadataValue(TranslatorTypeInterface& translator,
                                         const ConstValue const_value,
                                         xlscc_metadata::Value* output) const {
  auto value = const_value.rvalue();
  CHECK(value.IsBits());
  if (is_signed()) {
    XLS_ASSIGN_OR_RETURN(int64_t signed_value, value.bits().ToInt64());
    auto variant = variants_by_value_.find(signed_value);
    output->mutable_as_enum()->mutable_variant()->set_value(variant->first);
    for (const auto& variant_name : variant->second) {
      output->mutable_as_enum()->mutable_variant()->add_name(variant_name);
    }
  } else {
    XLS_ASSIGN_OR_RETURN(uint64_t unsigned_value, value.bits().ToUint64());
    auto variant = variants_by_value_.find(unsigned_value);
    output->mutable_as_enum()->mutable_variant()->set_value(variant->first);
    for (const auto& variant_name : variant->second) {
      output->mutable_as_enum()->mutable_variant()->add_name(variant_name);
    }
  }
  return absl::OkStatus();
}

CBoolType::~CBoolType() = default;

int CBoolType::GetBitWidth() const { return 1; }

CBoolType::operator std::string() const { return "bool"; }

absl::Status CBoolType::GetMetadata(
    TranslatorTypeInterface& translator, xlscc_metadata::Type* output,
    absl::flat_hash_set<const clang::NamedDecl*>& aliases_used) const {
  (void)output->mutable_as_bool();
  return absl::OkStatus();
}

absl::Status CBoolType::GetMetadataValue(TranslatorTypeInterface& translator,
                                         const ConstValue const_value,
                                         xlscc_metadata::Value* output) const {
  auto value = const_value.rvalue();
  CHECK(value.IsBits());
  XLS_ASSIGN_OR_RETURN(uint64_t bool_value, value.bits().ToUint64());
  output->set_as_bool(bool_value == 1);
  return absl::OkStatus();
}

bool CBoolType::operator==(const CType& o) const { return o.Is<CBoolType>(); }

bool CBoolType::StoredAsXLSBits() const { return true; }

CInstantiableTypeAlias::CInstantiableTypeAlias(const clang::NamedDecl* base)
    : base_(base) {}

const clang::NamedDecl* CInstantiableTypeAlias::base() const { return base_; }

CInstantiableTypeAlias::operator std::string() const {
  return absl::StrFormat("{%s}", base_->getNameAsString());
}

absl::Status CInstantiableTypeAlias::GetMetadata(
    TranslatorTypeInterface& translator, xlscc_metadata::Type* output,
    absl::flat_hash_set<const clang::NamedDecl*>& aliases_used) const {
  auto temp_alias = std::make_shared<CInstantiableTypeAlias>(base_);
  absl::StatusOr<xlscc_metadata::IntType> is_synthetic_int =
      translator.GenerateSyntheticInt(temp_alias);

  if (is_synthetic_int.ok()) {
    // hls_synthetic_int case
    *output->mutable_as_int() = is_synthetic_int.value();
    return absl::OkStatus();
  }

  // Recurse for aliases used in referenced struct
  {
    XLS_ASSIGN_OR_RETURN(auto resolved,
                         translator.ResolveTypeInstance(temp_alias));
    xlscc_metadata::Type dummy_type;
    XLS_RETURN_IF_ERROR(
        resolved->GetMetadata(translator, &dummy_type, aliases_used));
  }

  aliases_used.insert(base_);

  output->mutable_as_inst()->mutable_name()->set_name(base_->getNameAsString());
  output->mutable_as_inst()->mutable_name()->set_fully_qualified_name(
      base_->getQualifiedNameAsString());
  output->mutable_as_inst()->mutable_name()->set_id(
      reinterpret_cast<uint64_t>(base_));

  if (auto special =
          clang::dyn_cast<const clang::ClassTemplateSpecializationDecl>(
              base_)) {
    const clang::TemplateArgumentList& arguments = special->getTemplateArgs();
    for (int argi = 0; argi < arguments.size(); ++argi) {
      const clang::TemplateArgument& arg = arguments.get(argi);
      xlscc_metadata::TemplateArgument* proto_arg =
          output->mutable_as_inst()->add_args();
      switch (arg.getKind()) {
        case clang::TemplateArgument::ArgKind::Integral:
          proto_arg->set_as_integral(arg.getAsIntegral().getExtValue());
          break;
        case clang::TemplateArgument::ArgKind::Type: {
          xls::SourceInfo loc = translator.GetLoc(*base_);
          XLS_ASSIGN_OR_RETURN(
              std::shared_ptr<CType> arg_ctype,
              translator.TranslateTypeFromClang(arg.getAsType(), loc));
          xlscc_metadata::Type* as_type = proto_arg->mutable_as_type();
          XLS_RETURN_IF_ERROR(
              arg_ctype->GetMetadata(translator, as_type, aliases_used));
          break;
        }
        default:
          return absl::UnimplementedError(
              absl::StrFormat("Unimplemented template argument kind %i",
                              static_cast<int>(arg.getKind())));
      }
    }
  }

  return absl::OkStatus();
}

absl::Status CInstantiableTypeAlias::GetMetadataValue(
    TranslatorTypeInterface& translator, const ConstValue const_value,
    xlscc_metadata::Value* output) const {
  bool write_as_bits = false;
  std::shared_ptr<CInstantiableTypeAlias> inst(
      new CInstantiableTypeAlias(base_));
  if (translator.GenerateSyntheticInt(inst).ok()) {
    write_as_bits = true;
  }

  std::shared_ptr<CType> resolved_type = translator.GetCTypeForAlias(inst);
  auto struct_type =
      std::dynamic_pointer_cast<const CStructType>(resolved_type);

  // Handle __xls_bits
  if (struct_type == nullptr) {
    CHECK_EQ(base_->getNameAsString(), "__xls_bits");
    write_as_bits = true;
  }

  if (write_as_bits) {
    CHECK(const_value.rvalue().IsBits());
    vector<uint8_t> bytes = const_value.rvalue().bits().ToBytes();
    // Bits::ToBytes is little-endian, data is stored in the proto as
    // big-endian.
    std::reverse(bytes.begin(), bytes.end());
    output->set_as_bits(bytes.data(), bytes.size());

    return absl::OkStatus();
  }

  XLS_RETURN_IF_ERROR(
      struct_type->GetMetadataValue(translator, const_value, output));
  return absl::OkStatus();
}

int CInstantiableTypeAlias::GetBitWidth() const {
  LOG(FATAL) << "GetBitWidth() unsupported for CInstantiableTypeAlias";
  return 0;
}

bool CInstantiableTypeAlias::operator==(const CType& o) const {
  if (!o.Is<CInstantiableTypeAlias>()) {
    return false;
  }
  const auto* o_derived = o.As<CInstantiableTypeAlias>();
  return base_ == o_derived->base_;
}

absl::StatusOr<bool> CInstantiableTypeAlias::ContainsLValues(
    TranslatorTypeInterface& translator) const {
  auto temp_alias = std::make_shared<CInstantiableTypeAlias>(base_);
  XLS_ASSIGN_OR_RETURN(auto resolved,
                       translator.ResolveTypeInstance(temp_alias));
  XLS_ASSIGN_OR_RETURN(bool ret, resolved->ContainsLValues(translator));
  return ret;
}

CStructType::CStructType(std::vector<std::shared_ptr<CField>> fields,
                         bool no_tuple_flag, bool synthetic_int_flag)
    : no_tuple_flag_(no_tuple_flag),
      synthetic_int_flag_(synthetic_int_flag),
      fields_(fields) {
  for (const std::shared_ptr<CField>& pf : fields) {
    CHECK(!fields_by_name_.contains(pf->name()));
    fields_by_name_[pf->name()] = pf;
  }
}

absl::Status CStructType::GetMetadata(
    TranslatorTypeInterface& translator, xlscc_metadata::Type* output,
    absl::flat_hash_set<const clang::NamedDecl*>& aliases_used) const {
  // Synthetic int classes / structs should never be emitted
  CHECK(!synthetic_int_flag_);

  output->mutable_as_struct()->set_no_tuple(no_tuple_flag_);

  absl::flat_hash_map<
      int, std::pair<const clang::NamedDecl*, std::shared_ptr<CField>>>
      fields_by_index;

  auto size = fields_by_name_.size();
  for (std::pair<const clang::NamedDecl*, std::shared_ptr<CField>> field :
       fields_by_name_) {
    fields_by_index[size - 1 - field.second->index()] = field;
  }

  for (int i = 0; i < fields_by_name_.size(); ++i) {
    std::pair<const clang::NamedDecl*, std::shared_ptr<CField>> field =
        fields_by_index[i];
    XLS_RETURN_IF_ERROR(field.second->GetMetadata(
        translator, output->mutable_as_struct()->add_fields(), aliases_used));
  }

  return absl::OkStatus();
}

absl::Status CStructType::GetMetadataValue(
    TranslatorTypeInterface& translator, const ConstValue const_value,
    xlscc_metadata::Value* output) const {
  output->mutable_as_struct()->set_no_tuple(no_tuple_flag_);

  absl::flat_hash_map<
      int, std::pair<const clang::NamedDecl*, std::shared_ptr<CField>>>
      fields_by_index;

  for (const auto& field : fields_by_name_) {
    fields_by_index[field.second->index()] = field;
  }
  for (int i = 0; i < fields_by_name_.size(); ++i) {
    auto field = fields_by_index[i];
    auto struct_field_value = output->mutable_as_struct()->add_fields();
    auto name_out = struct_field_value->mutable_name();
    name_out->set_fully_qualified_name(field.first->getNameAsString());
    name_out->set_name(field.first->getNameAsString());
    name_out->set_id(reinterpret_cast<uint64_t>(field.first));
    XLS_ASSIGN_OR_RETURN(xls::Value elem_value,
                         TranslatorTypeInterface::GetStructFieldXLS(
                             const_value.rvalue(), i, *this));
    XLS_RETURN_IF_ERROR(field.second->type()->GetMetadataValue(
        translator, ConstValue(elem_value, field.second->type()),
        struct_field_value->mutable_value()));
  }
  return absl::OkStatus();
}

bool CStructType::no_tuple_flag() const { return no_tuple_flag_; }

bool CStructType::synthetic_int_flag() const { return synthetic_int_flag_; }

int CStructType::GetBitWidth() const {
  int ret = 0;
  for (const std::shared_ptr<CField>& field : fields_) {
    ret += field->type()->GetBitWidth();
  }
  return ret;
}

absl::StatusOr<bool> CStructType::ContainsLValues(
    TranslatorTypeInterface& translator) const {
  for (const std::shared_ptr<CField>& field : fields_) {
    XLS_ASSIGN_OR_RETURN(bool ret, field->type()->ContainsLValues(translator));

    if (ret) {
      return true;
    }
  }
  return false;
}

CStructType::operator std::string() const {
  std::ostringstream ostr;
  ostr << "{";
  if (no_tuple_flag_) {
    ostr << " notuple ";
  }
  if (synthetic_int_flag_) {
    ostr << " synthetic_int ";
  }
  for (const std::shared_ptr<CField>& it : fields_) {
    ostr << "[" << it->index() << "] "
         << (it->name() != nullptr ? it->name()->getNameAsString()
                                   : std::string("nullptr"))
         << ": " << string(*it->type()) << " ";
  }
  ostr << "}";
  return ostr.str();
}

bool CStructType::operator==(const CType& o) const {
  LOG(FATAL) << "operator== unsupported on structs";
  return false;
}

const std::vector<std::shared_ptr<CField>>& CStructType::fields() const {
  return fields_;
}

const absl::flat_hash_map<const clang::NamedDecl*, std::shared_ptr<CField>>&
CStructType::fields_by_name() const {
  return fields_by_name_;
}

std::shared_ptr<CField> CStructType::get_field(
    const clang::NamedDecl* name) const {
  auto found = fields_by_name_.find(name);
  if (found == fields_by_name_.end()) {
    return std::shared_ptr<CField>();
  }
  return found->second;
}

absl::StatusOr<int64_t> CStructType::count_lvalue_compounds(
    TranslatorTypeInterface& translator) const {
  int64_t ret = 0;
  for (const auto& field : fields_) {
    XLS_ASSIGN_OR_RETURN(bool field_has_lval,
                         field->type()->ContainsLValues(translator));
    if (field_has_lval) {
      ++ret;
    }
  }
  return ret;
}

CInternalTuple::CInternalTuple(std::vector<std::shared_ptr<CType>> fields)
    : fields_(fields) {}

absl::Status CInternalTuple::GetMetadata(
    TranslatorTypeInterface& translator, xlscc_metadata::Type* output,
    absl::flat_hash_set<const clang::NamedDecl*>& aliases_used) const {
  for (const auto& field : fields_) {
    XLS_RETURN_IF_ERROR(field->GetMetadata(
        translator, output->mutable_as_tuple()->add_fields(), aliases_used));
  }
  return absl::OkStatus();
}

absl::Status CInternalTuple::GetMetadataValue(
    TranslatorTypeInterface& translator, const ConstValue const_value,
    xlscc_metadata::Value* output) const {
  for (int i = 0; i < fields_.size(); ++i) {
    auto field = fields_[i];
    auto struct_field_value = output->mutable_as_tuple()->add_fields();
    XLS_ASSIGN_OR_RETURN(std::vector<xls::Value> values,
                         const_value.rvalue().GetElements());
    xls::Value elem_value = values.at(this->fields().size() - 1 - i);
    XLS_RETURN_IF_ERROR(
        field->GetMetadataValue(translator, ConstValue(elem_value, field),
                                struct_field_value->mutable_value()));
  }
  return absl::OkStatus();
}

int CInternalTuple::GetBitWidth() const {
  int ret = 0;
  for (const std::shared_ptr<CType>& field : fields_) {
    ret += field->GetBitWidth();
  }
  return ret;
}

CInternalTuple::operator std::string() const {
  std::ostringstream ostr;
  ostr << "InternalTuple{";
  for (int i = 0; i < fields_.size(); ++i) {
    const std::shared_ptr<CType>& field = fields_[i];
    if (i > 0) {
      ostr << ", ";
    }
    ostr << "[" << i << "] " << ": " << string(*field);
  }
  ostr << "}";
  return ostr.str();
}

bool CInternalTuple::operator==(const CType& o) const {
  if (!o.Is<CInternalTuple>()) {
    return false;
  }
  auto obj = o.As<CInternalTuple>();
  for (int i = 0; i < fields_.size(); ++i) {
    if (*obj->fields_[i] != *fields_[i]) {
      return false;
    }
  }
  return true;
}

const std::vector<std::shared_ptr<CType>>& CInternalTuple::fields() const {
  return fields_;
}

CField::CField(const clang::NamedDecl* name, int index,
               std::shared_ptr<CType> type)
    : name_(name), index_(index), type_(type) {}

int CField::index() const { return index_; }

const clang::NamedDecl* CField::name() const { return name_; }

std::shared_ptr<CType> CField::type() const { return type_; }

CArrayType::CArrayType(std::shared_ptr<CType> element, int size, bool use_tuple)
    : element_(element), size_(size), use_tuple_(use_tuple) {
  // XLS doesn't support 0 sized arrays
  CHECK_GT(size_, 0);
}

bool CArrayType::operator==(const CType& o) const {
  if (!o.Is<CArrayType>()) {
    return false;
  }
  const auto* o_derived = o.As<CArrayType>();
  return *element_ == *o_derived->element_ && size_ == o_derived->size_ &&
         use_tuple_ == o_derived->use_tuple_;
}

int CArrayType::GetBitWidth() const { return size_ * element_->GetBitWidth(); }

absl::StatusOr<bool> CArrayType::ContainsLValues(
    TranslatorTypeInterface& translator) const {
  XLS_ASSIGN_OR_RETURN(bool ret, element_->ContainsLValues(translator));
  return ret;
}

absl::Status CField::GetMetadata(
    TranslatorTypeInterface& translator, xlscc_metadata::StructField* output,
    absl::flat_hash_set<const clang::NamedDecl*>& aliases_used) const {
  output->set_name(name_->getNameAsString());
  XLS_RETURN_IF_ERROR(
      type_->GetMetadata(translator, output->mutable_type(), aliases_used));
  return absl::OkStatus();
}

int CArrayType::GetSize() const { return size_; }

std::shared_ptr<CType> CArrayType::GetElementType() const { return element_; }

CArrayType::operator std::string() const {
  return absl::StrFormat("%s[%i]%s", string(*element_), size_,
                         use_tuple_ ? " (tup)" : "");
}

absl::Status CArrayType::GetMetadata(
    TranslatorTypeInterface& translator, xlscc_metadata::Type* output,
    absl::flat_hash_set<const clang::NamedDecl*>& aliases_used) const {
  output->mutable_as_array()->set_size(size_);
  output->mutable_as_array()->set_use_tuple(use_tuple_);
  XLS_RETURN_IF_ERROR(element_->GetMetadata(
      translator, output->mutable_as_array()->mutable_element_type(),
      aliases_used));
  return absl::OkStatus();
}

absl::Status CArrayType::GetMetadataValue(TranslatorTypeInterface& translator,
                                          const ConstValue const_value,
                                          xlscc_metadata::Value* output) const {
  vector<xls::Value> values = const_value.rvalue().GetElements().value();
  for (auto& val : values) {
    XLS_RETURN_IF_ERROR(element_->GetMetadataValue(
        translator, ConstValue(val, element_),
        output->mutable_as_array()->add_element_values()));
  }
  return absl::OkStatus();
}

bool CArrayType::GetUseTuple() const { return use_tuple_; }

CPointerType::CPointerType(std::shared_ptr<CType> pointee_type)
    : pointee_type_(pointee_type) {}

bool CPointerType::operator==(const CType& o) const {
  if (!o.Is<CPointerType>()) {
    return false;
  }
  const auto* o_derived = o.As<CPointerType>();
  return *pointee_type_ == *o_derived->pointee_type_;
}

int CPointerType::GetBitWidth() const { return 0; }

absl::StatusOr<bool> CPointerType::ContainsLValues(
    TranslatorTypeInterface& translator) const {
  return true;
}

std::shared_ptr<CType> CPointerType::GetPointeeType() const {
  return pointee_type_;
}

CPointerType::operator std::string() const {
  return absl::StrFormat("%s*", string(*pointee_type_));
}

absl::Status CPointerType::GetMetadata(
    TranslatorTypeInterface& translator, xlscc_metadata::Type* output,
    absl::flat_hash_set<const clang::NamedDecl*>& aliases_used) const {
  return absl::UnavailableError(
      "Can't generate externally useful metadata for pointers");
}

absl::Status CPointerType::GetMetadataValue(
    TranslatorTypeInterface& translator, const ConstValue const_value,
    xlscc_metadata::Value* output) const {
  return absl::UnavailableError(
      "Can't generate externally useful metadata for pointers");
}

CReferenceType::CReferenceType(std::shared_ptr<CType> pointee_type)
    : pointee_type_(pointee_type) {}

bool CReferenceType::operator==(const CType& o) const {
  if (!o.Is<CReferenceType>()) {
    return false;
  }
  const auto* o_derived = o.As<CReferenceType>();
  return *pointee_type_ == *o_derived->pointee_type_;
}

int CReferenceType::GetBitWidth() const { return pointee_type_->GetBitWidth(); }

absl::StatusOr<bool> CReferenceType::ContainsLValues(
    TranslatorTypeInterface& translator) const {
  return true;
}

std::shared_ptr<CType> CReferenceType::GetPointeeType() const {
  return pointee_type_;
}

CReferenceType::operator std::string() const {
  return absl::StrFormat("%s&", string(*pointee_type_));
}

absl::Status CReferenceType::GetMetadata(
    TranslatorTypeInterface& translator, xlscc_metadata::Type* output,
    absl::flat_hash_set<const clang::NamedDecl*>& aliases_used) const {
  return absl::UnavailableError(
      "Can't generate externally useful metadata for references");
}

absl::Status CReferenceType::GetMetadataValue(
    TranslatorTypeInterface& translator, const ConstValue const_value,
    xlscc_metadata::Value* output) const {
  return absl::UnavailableError(
      "Can't generate externally useful metadata for references");
}

CChannelType::CChannelType(std::shared_ptr<CType> item_type,  // OpType op_type,
                           int64_t memory_size)
    : item_type_(item_type),
      op_type_(OpType::kNull),
      memory_size_(memory_size) {}

CChannelType::CChannelType(std::shared_ptr<CType> item_type,
                           int64_t memory_size, OpType op_type)
    : item_type_(item_type), op_type_(op_type), memory_size_(memory_size) {}

bool CChannelType::operator==(const CType& o) const {
  if (!o.Is<CChannelType>()) {
    return false;
  }
  const auto* o_derived = o.As<CChannelType>();
  return *item_type_ == *o_derived->item_type_ &&
         op_type_ == o_derived->op_type_ &&
         memory_size_ == o_derived->memory_size_;
}

int CChannelType::GetBitWidth() const { return item_type_->GetBitWidth(); }

std::shared_ptr<CType> CChannelType::GetItemType() const { return item_type_; }

CChannelType::operator std::string() const {
  if (op_type_ == OpType::kRead || op_type_ == OpType::kWrite) {
    return absl::StrFormat("memory<%s,%i>", string(*item_type_), memory_size_);
  }
  return absl::StrFormat("channel<%s,%s>", string(*item_type_),
                         (op_type_ == OpType::kRecv)
                             ? "recv"
                             : ((op_type_ == OpType::kSend) ? "send" : "null"));
}

absl::Status CChannelType::GetMetadata(
    TranslatorTypeInterface& translator, xlscc_metadata::Type* output,
    absl::flat_hash_set<const clang::NamedDecl*>& aliases_used) const {
  XLS_RETURN_IF_ERROR(item_type_->GetMetadata(
      translator, output->mutable_as_channel()->mutable_item_type(),
      aliases_used));
  output->mutable_as_channel()->set_memory_size(
      static_cast<int32_t>(memory_size_));

  return absl::OkStatus();
}

absl::Status CChannelType::GetMetadataValue(
    TranslatorTypeInterface& translator, const ConstValue const_value,
    xlscc_metadata::Value* output) const {
  return absl::OkStatus();
}

absl::StatusOr<bool> CChannelType::ContainsLValues(
    TranslatorTypeInterface& translator) const {
  return true;
}

OpType CChannelType::GetOpType() const { return op_type_; }

int64_t CChannelType::GetMemorySize() const { return memory_size_; }

int64_t CChannelType::GetMemoryAddressWidth() const {
  return MemoryAddressWidth(memory_size_);
}

std::optional<int64_t> CChannelType::GetMemoryMaskWidth() const {
  // TODO(google/xls#861): support masked memory operations.
  // Setting mask width to nullopt elides the mask ports.
  return std::nullopt;
}

std::shared_ptr<CType> CChannelType::MemoryAddressType(int64_t memory_size) {
  return std::make_shared<CIntType>(MemoryAddressWidth(memory_size), false);
}

int64_t CChannelType::MemoryAddressWidth(int64_t memory_size) {
  CHECK_GT(memory_size, 0);
  return std::ceil(std::log2(memory_size));
}

absl::StatusOr<xls::Type*> CChannelType::GetReadRequestType(
    xls::Package* package, xls::Type* item_type) const {
  xls::Type* addr_type = package->GetBitsType(GetMemoryAddressWidth());
  xls::Type* mask_type =
      GetMemoryMaskWidth().has_value()
          ? package->GetBitsType(GetMemoryMaskWidth().value())
          : static_cast<xls::Type*>(package->GetTupleType({}));
  return package->GetTupleType({addr_type, mask_type});
}

absl::StatusOr<xls::Type*> CChannelType::GetReadResponseType(
    xls::Package* package, xls::Type* item_type) const {
  return package->GetTupleType({item_type});
}

absl::StatusOr<xls::Type*> CChannelType::GetWriteRequestType(
    xls::Package* package, xls::Type* item_type) const {
  xls::Type* addr_type = package->GetBitsType(GetMemoryAddressWidth());
  xls::Type* mask_type =
      GetMemoryMaskWidth().has_value()
          ? package->GetBitsType(GetMemoryMaskWidth().value())
          : static_cast<xls::Type*>(package->GetTupleType({}));

  return package->GetTupleType({addr_type, item_type, mask_type});
}

absl::StatusOr<xls::Type*> CChannelType::GetWriteResponseType(
    xls::Package* package, xls::Type* item_type) const {
  return package->GetTupleType({});
}

void GetAllBValuesForLValue(std::shared_ptr<LValue> lval,
                            std::vector<const TrackedBValue*>& out) {
  if (lval == nullptr) {
    return;
  }
  if (lval->is_select()) {
    out.push_back(&lval->cond());
    GetAllBValuesForLValue(lval->lvalue_true(), out);
    GetAllBValuesForLValue(lval->lvalue_false(), out);
  }
  for (const auto& [_, lval] : lval->get_compounds()) {
    GetAllBValuesForLValue(lval, out);
  }
}

void GetAllBValuesForCValue(const CValue& cval,
                            std::vector<const TrackedBValue*>& out) {
  if (cval.rvalue().valid()) {
    out.push_back(&cval.rvalue());
  }
  GetAllBValuesForLValue(cval.lvalue(), out);
}

std::vector<const CValue*> OrderCValuesFunc::operator()(
    const absl::flat_hash_set<const CValue*>& cvals_unordered) {
  std::vector<const CValue*> ret;
  ret.insert(ret.end(), cvals_unordered.begin(), cvals_unordered.end());

  std::sort(ret.begin(), ret.end(), [](const CValue* a, const CValue* b) {
    std::vector<const TrackedBValue*> all_bvals_for_a;
    GetAllBValuesForCValue(*a, all_bvals_for_a);
    std::vector<const TrackedBValue*> all_bvals_for_b;
    GetAllBValuesForCValue(*b, all_bvals_for_b);
    std::vector<int64_t> seq_numbers_for_a;
    seq_numbers_for_a.reserve(all_bvals_for_a.size());
    for (const TrackedBValue* bval : all_bvals_for_a) {
      seq_numbers_for_a.push_back(bval->sequence_number());
    }
    std::vector<int64_t> seq_numbers_for_b;
    seq_numbers_for_b.reserve(all_bvals_for_b.size());
    for (const TrackedBValue* bval : all_bvals_for_b) {
      seq_numbers_for_b.push_back(bval->sequence_number());
    }
    return seq_numbers_for_a < seq_numbers_for_b;
  });
  return ret;
}

std::vector<const std::shared_ptr<LValue>*> OrderLValuesFunc::operator()(
    const absl::flat_hash_set<const std::shared_ptr<LValue>*>&
        lvals_unordered) {
  std::vector<const std::shared_ptr<LValue>*> ret;
  ret.insert(ret.end(), lvals_unordered.begin(), lvals_unordered.end());

  std::sort(
      ret.begin(), ret.end(),
      [](const std::shared_ptr<LValue>* a, const std::shared_ptr<LValue>* b) {
        std::vector<const TrackedBValue*> all_bvals_for_a;
        GetAllBValuesForLValue(*a, all_bvals_for_a);
        std::vector<const TrackedBValue*> all_bvals_for_b;
        GetAllBValuesForLValue(*b, all_bvals_for_b);
        std::vector<int64_t> seq_numbers_for_a;
        std::vector<int64_t> seq_numbers_for_b;
        seq_numbers_for_a.reserve(all_bvals_for_a.size());
        for (const TrackedBValue* bval : all_bvals_for_a) {
          seq_numbers_for_a.push_back(bval->sequence_number());
        }
        seq_numbers_for_b.reserve(all_bvals_for_b.size());
        for (const TrackedBValue* bval : all_bvals_for_b) {
          seq_numbers_for_b.push_back(bval->sequence_number());
        }
        return seq_numbers_for_a < seq_numbers_for_b;
      });

  return ret;
}

int Debug_CountNodes(const xls::Node* node,
                     std::set<const xls::Node*>& visited) {
  if (visited.find(node) != visited.end()) {
    return 0;
  }
  visited.insert(node);

  int ret = 1;
  for (const xls::Node* child : node->operands()) {
    ret += Debug_CountNodes(child, visited);
  }
  return ret;
}

std::string Debug_NodeToInfix(TrackedBValue bval) {
  if (bval.node() == nullptr) {
    return "[null]";
  }
  int64_t n_printed = 0;
  return Debug_NodeToInfix(bval.node(), n_printed);
}

std::string Debug_NodeToInfix(const xls::Node* node, int64_t& n_printed) {
  ++n_printed;
  if (n_printed > 100) {
    return "[...]";
  }

  if (node->Is<xls::Literal>()) {
    const xls::Literal* literal = node->As<xls::Literal>();
    if (literal->value().kind() == xls::ValueKind::kBits) {
      return absl::StrFormat("%lu", literal->value().bits().ToUint64().value());
    }
  }
  if (node->Is<xls::Param>()) {
    const xls::Param* param = node->As<xls::Param>();
    return param->GetName();
  }
  if (node->Is<xls::TupleIndex>()) {
    const xls::TupleIndex* ti = node->As<xls::TupleIndex>();
    xls::Node* tup = ti->operand(xls::TupleIndex::kArgOperand);
    return absl::StrFormat("%s(%i)", Debug_NodeToInfix(tup, n_printed),
                           ti->index());
  }
  if (node->Is<xls::Tuple>()) {
    const xls::Tuple* tp = node->As<xls::Tuple>();
    std::vector<std::string> operand_strings;
    for (const xls::Node* op : tp->operands()) {
      operand_strings.push_back(Debug_NodeToInfix(op, n_printed));
    }
    return std::string("(") + absl::StrJoin(operand_strings, ", ") + ")";
  }
  if (node->Is<xls::UnOp>()) {
    const xls::UnOp* op = node->As<xls::UnOp>();
    if (op->op() == xls::Op::kNot) {
      return absl::StrFormat("!%s",
                             Debug_NodeToInfix(op->operand(0), n_printed));
    }
  }
  if (node->op() == xls::Op::kSGt || node->op() == xls::Op::kUGt) {
    return absl::StrFormat("(%s > %s)",
                           Debug_NodeToInfix(node->operand(0), n_printed),
                           Debug_NodeToInfix(node->operand(1), n_printed));
  }
  if (node->op() == xls::Op::kSGe || node->op() == xls::Op::kUGe) {
    return absl::StrFormat("(%s > %s)",
                           Debug_NodeToInfix(node->operand(0), n_printed),
                           Debug_NodeToInfix(node->operand(1), n_printed));
  }
  if (node->op() == xls::Op::kSLt || node->op() == xls::Op::kULt) {
    return absl::StrFormat("(%s < %s)",
                           Debug_NodeToInfix(node->operand(0), n_printed),
                           Debug_NodeToInfix(node->operand(1), n_printed));
  }
  if (node->op() == xls::Op::kSLe || node->op() == xls::Op::kULe) {
    return absl::StrFormat("(%s <= %s)",
                           Debug_NodeToInfix(node->operand(0), n_printed),
                           Debug_NodeToInfix(node->operand(1), n_printed));
  }
  if (node->op() == xls::Op::kEq) {
    return absl::StrFormat("(%s == %s)",
                           Debug_NodeToInfix(node->operand(0), n_printed),
                           Debug_NodeToInfix(node->operand(1), n_printed));
  }
  if (node->op() == xls::Op::kAnd) {
    if (node->operand_count() == 2) {
      return absl::StrFormat("(%s & %s)",
                             Debug_NodeToInfix(node->operand(0), n_printed),
                             Debug_NodeToInfix(node->operand(1), n_printed));
    } else {
      return absl::StrFormat(
          "and(%s)",
          absl::StrJoin(node->operands(), ", ",
                        [&n_printed](std::string* out, const xls::Node* node) {
                          absl::StrAppend(out,
                                          Debug_NodeToInfix(node, n_printed));
                        }));
    }
  }
  if (node->op() == xls::Op::kOr && node->operand_count() == 2) {
    return absl::StrFormat("(%s | %s)",
                           Debug_NodeToInfix(node->operand(0), n_printed),
                           Debug_NodeToInfix(node->operand(1), n_printed));
  }
  if (node->op() == xls::Op::kAdd && node->operand_count() == 2) {
    return absl::StrFormat("(%s + %s)",
                           Debug_NodeToInfix(node->operand(0), n_printed),
                           Debug_NodeToInfix(node->operand(1), n_printed));
  }
  if (node->op() == xls::Op::kSignExt) {
    return absl::StrFormat("%s",
                           Debug_NodeToInfix(node->operand(0), n_printed));
  }
  if (node->op() == xls::Op::kSel) {
    return absl::StrFormat("(%s ? %s : %s)",
                           Debug_NodeToInfix(node->operand(0), n_printed),
                           Debug_NodeToInfix(node->operand(2), n_printed),
                           Debug_NodeToInfix(node->operand(1), n_printed));
  }
  if (node->op() == xls::Op::kStateRead) {
    const xls::StateRead* sr = node->As<xls::StateRead>();
    return absl::StrFormat("%s", sr->state_element()->name());
  }

  return absl::StrFormat("[unsupported %s / %s]", node->GetName(),
                         typeid(*node).name());
}

std::string Debug_GenerateReadableTypeName(xls::Type* type) {
  constexpr int64_t max_type_len = 64;
  std::string type_str = type->ToString();
  if (type_str.size() > max_type_len) {
    type_str = type_str.substr(0, max_type_len);
  }
  return type_str;
}

void LogContinuations(const xlscc::GeneratedFunction& func) {
  absl::flat_hash_map<const ContinuationValue*, const GeneratedFunctionSlice*>
      slices_by_continuation_out;
  for (const GeneratedFunctionSlice& slice : func.slices) {
    for (const ContinuationValue& continuation_out : slice.continuations_out) {
      slices_by_continuation_out[&continuation_out] = &slice;
    }
  }
  auto decl_names_string =
      [](const absl::flat_hash_set<const clang::NamedDecl*>& decls) {
        std::vector<std::string> decl_names;
        for (const clang::NamedDecl* decl : decls) {
          decl_names.push_back(decl->getNameAsString());
        }
        return absl::StrJoin(decl_names, ",");
      };
  int64_t slice_index = 0;
  for (const GeneratedFunctionSlice& slice : func.slices) {
    LOG(INFO) << "";
    LOG(INFO) << absl::StrFormat("Slice[%li]: %s %p after %s:", slice_index,
                                 slice.function->name().c_str(), &slice,
                                 slice.after_op == nullptr
                                     ? "(first)"
                                     : Debug_OpName(*slice.after_op).c_str());
    for (const ContinuationInput& continuation_in : slice.continuations_in) {
      LOG(INFO) << absl::StrFormat(
          "  in: %p.%s on param %s/%p top decls %s has %li users, direct-in? "
          "%i",
          slices_by_continuation_out.at(continuation_in.continuation_out),
          continuation_in.name.c_str(),
          continuation_in.input_node->name().data(), continuation_in.input_node,
          decl_names_string(continuation_in.decls),
          continuation_in.input_node->users().size(),
          (int)continuation_in.continuation_out->direct_in);
    }
    LOG(INFO) << "";
    for (const ContinuationValue& continuation_out : slice.continuations_out) {
      const std::string type_str = Debug_GenerateReadableTypeName(
          continuation_out.output_node->GetType());

      LOG(INFO) << absl::StrFormat(
          "  out: %s top decls %s has %li users, type = %s, literal = %s, "
          "direct-in? %i",
          continuation_out.name.c_str(),
          decl_names_string(continuation_out.decls),
          continuation_out.output_node->users().size(), type_str.c_str(),
          continuation_out.literal.has_value()
              ? continuation_out.literal.value().ToString().c_str()
              : "(none)",
          (int)continuation_out.direct_in);
    }
    ++slice_index;
  }
}

std::string Debug_OpName(const IOOp& op) {
  if (op.op == OpType::kTrace) {
    return "trace";
  }
  if (op.op == OpType::kLoopBegin || op.op == OpType::kLoopEndJump) {
    return op.op == OpType::kLoopBegin ? "begin" : "jump";
  }
  if (op.channel != nullptr) {
    std::string op_type_name;
    switch (op.op) {
      case OpType::kSend:
        op_type_name = "send";
        break;
      case OpType::kRecv:
        op_type_name = "recv";
        break;
      case OpType::kRead:
        op_type_name = "read";
        break;
      case OpType::kWrite:
        op_type_name = "write";
        break;
      default:
        LOG(FATAL) << absl::StrFormat("Op type doesn't make sense here: %i",
                                      op.op);
    }
    return absl::StrFormat("%s_%s", op.channel->unique_name, op_type_name);
  }
  if (!op.final_param_name.empty()) {
    return op.final_param_name;
  }
  CHECK_EQ("Unable to form name for op", nullptr);
  return "TODO_OpName";
}

namespace {
class ShortCircuitVisitor : public xls::DfsVisitorWithDefault {
 public:
  static std::optional<xls::Value> TryResolveAsValue(xls::Node* node) {
    xls::IrInterpreter ir_interpreter;
    absl::Status status = node->Accept(&ir_interpreter);
    if (status.ok()) {
      return ir_interpreter.ResolveAsValue(node);
    }
    return std::nullopt;
  }

  absl::Status HandleNaryAnd(xls::NaryOp* op) final {
    xls::IrInterpreter ir_interpreter;
    absl::Status status = op->Accept(&ir_interpreter);
    if (status.ok()) {
      const xls::Value& value = ir_interpreter.ResolveAsValue(op);
      XLS_ASSIGN_OR_RETURN(xls::Node * replacement,
                           op->ReplaceUsesWithNew<xls::Literal>(value));
      rewrites_[op] = replacement;
      return absl::OkStatus();
    }
    std::vector<xls::Node*> new_operands;
    new_operands.reserve(op->operand_count());
    for (xls::Node* operand : op->operands()) {
      std::optional<xls::Value> const_val = TryResolveAsValue(operand);
      if (!const_val.has_value()) {
        new_operands.push_back(operand);
        continue;
      }
      if (const_val->IsAllOnes()) {
        continue;
      }
      if (const_val->IsAllZeros()) {
        XLS_ASSIGN_OR_RETURN(
            xls::Node * replacement,
            op->ReplaceUsesWithNew<xls::Literal>(ZeroOfType(op->GetType())));
        rewrites_[op] = replacement;
        return absl::OkStatus();
      }
      new_operands.push_back(operand);
    }
    XLS_RET_CHECK_GT(new_operands.size(), 0);
    if (new_operands.size() == 1) {
      rewrites_[op] = new_operands[0];
      return op->ReplaceUsesWith(new_operands[0]);
    }
    if (new_operands.size() != op->operand_count()) {
      XLS_ASSIGN_OR_RETURN(
          xls::Node * replacement,
          op->ReplaceUsesWithNew<xls::NaryOp>(new_operands, op->op()));
      rewrites_[op] = replacement;
    }
    return absl::OkStatus();
  }
  absl::Status HandleNaryOr(xls::NaryOp* op) final {
    xls::IrInterpreter ir_interpreter;
    absl::Status status = op->Accept(&ir_interpreter);
    if (status.ok()) {
      const xls::Value& value = ir_interpreter.ResolveAsValue(op);
      XLS_ASSIGN_OR_RETURN(xls::Node * replacement,
                           op->ReplaceUsesWithNew<xls::Literal>(value));
      rewrites_[op] = replacement;
      return absl::OkStatus();
    }
    std::vector<xls::Node*> new_operands;
    new_operands.reserve(op->operand_count());
    for (xls::Node* operand : op->operands()) {
      std::optional<xls::Value> const_val = TryResolveAsValue(operand);
      if (!const_val.has_value()) {
        new_operands.push_back(operand);
        continue;
      }
      if (const_val->IsAllZeros()) {
        continue;
      }
      if (const_val->IsAllOnes()) {
        XLS_ASSIGN_OR_RETURN(
            xls::Node * replacement,
            op->ReplaceUsesWithNew<xls::Literal>(AllOnesOfType(op->GetType())));
        rewrites_[op] = replacement;
        return absl::OkStatus();
      }
      new_operands.push_back(operand);
    }
    XLS_RET_CHECK_GT(new_operands.size(), 0);
    if (new_operands.size() == 1) {
      rewrites_[op] = new_operands[0];
      return op->ReplaceUsesWith(new_operands[0]);
    }
    if (new_operands.size() != op->operand_count()) {
      XLS_ASSIGN_OR_RETURN(
          xls::Node * replacement,
          op->ReplaceUsesWithNew<xls::NaryOp>(new_operands, op->op()));
      rewrites_[op] = replacement;
    }
    return absl::OkStatus();
  }
  absl::Status DefaultHandler(xls::Node* node) final {
    return absl::OkStatus();
  }

  const absl::flat_hash_map<xls::Node*, xls::Node*>& rewrites() const {
    return rewrites_;
  }

 private:
  absl::flat_hash_map<xls::Node*, xls::Node*> rewrites_;
};
}  // namespace

absl::Status ShortCircuitBVal(TrackedBValue& bval, const xls::SourceInfo& loc) {
  ShortCircuitVisitor visitor;
  XLS_RETURN_IF_ERROR(bval.node()->Accept(&visitor));
  auto iter = visitor.rewrites().find(bval.node());
  if (iter != visitor.rewrites().end()) {
    bval = TrackedBValue(iter->second, bval.builder());
  }
  return absl::OkStatus();
}

}  //  namespace xlscc
