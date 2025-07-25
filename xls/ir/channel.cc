// Copyright 2020 The XLS Authors
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

#include "xls/ir/channel.h"

#include <cstdint>
#include <iterator>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "absl/algorithm/container.h"
#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"
#include "absl/types/variant.h"
#include "xls/common/casts.h"
#include "xls/common/status/status_macros.h"
#include "xls/ir/channel.pb.h"
#include "xls/ir/channel_ops.h"
#include "xls/ir/type.h"
#include "xls/ir/value_utils.h"

namespace xls {
/* static */ absl::StatusOr<FifoConfig> FifoConfig::FromProto(
    const FifoConfigProto& proto) {
  if (!proto.has_depth()) {
    return absl::InvalidArgumentError("FifoConfigProto.depth is required.");
  }
  return FifoConfig(proto.depth(), proto.bypass(),
                    proto.register_push_outputs(),
                    proto.register_pop_outputs());
}
FifoConfigProto FifoConfig::ToProto(int64_t width) const {
  FifoConfigProto proto;
  proto.set_width(width);
  proto.set_depth(depth_);
  proto.set_bypass(bypass_);
  proto.set_register_push_outputs(register_push_outputs_);
  proto.set_register_pop_outputs(register_pop_outputs_);
  return proto;
}

absl::Status FifoConfig::Validate() const {
  if (depth_ == 0) {
    if (!bypass_) {
      return absl::FailedPreconditionError(
          "Zero-depth FIFOs are direct connections, and hence cannot be "
          "bypass-less");
    }
    if (register_push_outputs_ || register_pop_outputs_) {
      return absl::FailedPreconditionError(
          "Zero-depth FIFOs are memory-less/direct connections, and hence do "
          "not support registers");
    }
  }
  if (depth_ == 1) {
    if (register_pop_outputs_) {
      return absl::FailedPreconditionError(
          "register_pop_outputs adds an extra FIFO entry for the output and "
          "still requires a depth>=1 FIFO in front of the output stage, so "
          "we require depth>=2");
      // Please file a github issue if you have different FIFO needs :)
    }
  }
  return absl::OkStatus();
}

std::string FifoConfig::ToString() const {
  return absl::StrFormat(
      "FifoConfig{ %s }",
      absl::StrJoin(GetDslxKwargs(), ", ", absl::PairFormatter(": ")));
}

std::vector<std::pair<std::string, std::string>> FifoConfig::GetDslxKwargs()
    const {
  if (depth_ == 0) {
    // depth == 0 is a special case where all the other kwargs don't need to be
    // specified because depth 0 only supports one configuration.
    return {{"depth", "0"}};
  }
  std::vector<std::pair<std::string, std::string>> kwargs;
  kwargs.push_back({"depth", absl::StrCat(depth_)});
  kwargs.push_back({"bypass", bypass_ ? "true" : "false"});
  kwargs.push_back(
      {"register_push_outputs", register_push_outputs_ ? "true" : "false"});
  kwargs.push_back(
      {"register_pop_outputs", register_pop_outputs_ ? "true" : "false"});
  return kwargs;
}

namespace {

FlopKindProto ToProtoFlop(std::optional<FlopKind> f) {
  if (!f) {
    return FLOP_KIND_DEFAULT;
  }
  switch (*f) {
    case FlopKind::kNone:
      return FLOP_KIND_NONE;
    case FlopKind::kFlop:
      return FLOP_KIND_FLOP;
    case FlopKind::kSkid:
      return FLOP_KIND_SKID;
    case FlopKind::kZeroLatency:
      return FLOP_KIND_ZERO_LATENCY;
  }
}
}  // namespace

absl::StatusOr<std::optional<FlopKind>> FlopKindFromProto(FlopKindProto f) {
  switch (f) {
    case FLOP_KIND_DEFAULT:
      return std::nullopt;
    case FLOP_KIND_NONE:
      return FlopKind::kNone;
    case FLOP_KIND_FLOP:
      return FlopKind::kFlop;
    case FLOP_KIND_SKID:
      return FlopKind::kSkid;
    case FLOP_KIND_ZERO_LATENCY:
      return FlopKind::kZeroLatency;
    default:
      return absl::InternalError(absl::StrFormat("Unknown flop kind: %d", f));
  }
}

absl::StatusOr<FlopKind> StringToFlopKind(std::string_view str) {
  for (FlopKind f : {FlopKind::kNone, FlopKind::kFlop, FlopKind::kSkid,
                     FlopKind::kZeroLatency}) {
    if (str == FlopKindToString(f)) {
      return f;
    }
  }
  return absl::InvalidArgumentError(
      absl::StrFormat("'%s' is not a valid flop kind", str));
}

/* static */ absl::StatusOr<ChannelConfig> ChannelConfig::FromProto(
    const ChannelConfigProto& proto) {
  std::optional<FifoConfig> fc;
  if (proto.has_fifo()) {
    XLS_ASSIGN_OR_RETURN(fc, FifoConfig::FromProto(proto.fifo()));
  }
  XLS_ASSIGN_OR_RETURN(std::optional<FlopKind> input,
                       FlopKindFromProto(proto.flop_inputs()));
  XLS_ASSIGN_OR_RETURN(std::optional<FlopKind> output,
                       FlopKindFromProto(proto.flop_outputs()));
  return ChannelConfig(std::move(fc), input, output);
}

ChannelConfigProto ChannelConfig::ToProto(int64_t width) const {
  ChannelConfigProto proto;
  if (fifo_config()) {
    *proto.mutable_fifo() = fifo_config()->ToProto(width);
  }
  proto.set_flop_inputs(ToProtoFlop(input_flop_kind()));
  proto.set_flop_outputs(ToProtoFlop(output_flop_kind()));
  return proto;
}

std::string ChannelKindToString(ChannelKind kind) {
  switch (kind) {
    case ChannelKind::kStreaming:
      return "streaming";
    case ChannelKind::kSingleValue:
      return "single_value";
  }
  LOG(FATAL) << "Invalid channel kind: " << static_cast<int64_t>(kind);
}

absl::StatusOr<ChannelKind> StringToChannelKind(std::string_view str) {
  if (str == "streaming") {
    return ChannelKind::kStreaming;
  }
  if (str == "single_value") {
    return ChannelKind::kSingleValue;
  }
  return absl::InvalidArgumentError(
      absl::StrFormat("Invalid channel kind '%s'", str));
}

std::ostream& operator<<(std::ostream& os, ChannelKind kind) {
  os << ChannelKindToString(kind);
  return os;
}

std::string Channel::ToString() const {
  std::string result = absl::StrFormat("chan %s(", name());
  absl::StrAppendFormat(&result, "%s, ", type()->ToString());
  if (!initial_values().empty()) {
    absl::StrAppendFormat(
        &result, "initial_values={%s}, ",
        absl::StrJoin(initial_values(), ", ", UntypedValueFormatter));
  }
  absl::StrAppendFormat(&result, "id=%d, kind=%s, ops=%s", id(),
                        ChannelKindToString(kind_),
                        ChannelOpsToString(supported_ops()));

  if (kind() == ChannelKind::kStreaming) {
    const StreamingChannel* streaming_channel =
        down_cast<const StreamingChannel*>(this);
    absl::StrAppendFormat(
        &result, ", flow_control=%s, strictness=%s",
        FlowControlToString(streaming_channel->GetFlowControl()),
        ChannelStrictnessToString(streaming_channel->GetStrictness()));
    const std::optional<ChannelConfig>& channel_config =
        streaming_channel->channel_config();
    if (channel_config.has_value()) {
      if (channel_config->fifo_config()) {
        absl::StrAppendFormat(
            &result,
            ", fifo_depth=%d, bypass=%s, "
            "register_push_outputs=%s, register_pop_outputs=%s",
            channel_config->fifo_config()->depth(),
            channel_config->fifo_config()->bypass() ? "true" : "false",
            channel_config->fifo_config()->register_push_outputs() ? "true"
                                                                   : "false",
            channel_config->fifo_config()->register_pop_outputs() ? "true"
                                                                  : "false");
      }
      if (channel_config->input_flop_kind().has_value()) {
        absl::StrAppendFormat(
            &result, ", input_flop_kind=%s",
            FlopKindToString(*channel_config->input_flop_kind()));
      }
      if (channel_config->output_flop_kind().has_value()) {
        absl::StrAppendFormat(
            &result, ", output_flop_kind=%s",
            FlopKindToString(*channel_config->output_flop_kind()));
      }
    }
  }

  absl::StrAppend(&result, ")");
  return result;
}

std::string FlowControlToString(FlowControl fc) {
  switch (fc) {
    case FlowControl::kNone:
      return "none";
    case FlowControl::kReadyValid:
      return "ready_valid";
  }
  LOG(FATAL) << "Invalid flow control value: " << static_cast<int64_t>(fc);
}

absl::StatusOr<FlowControl> StringToFlowControl(std::string_view str) {
  if (str == "none") {
    return FlowControl::kNone;
  }
  if (str == "ready_valid") {
    return FlowControl::kReadyValid;
  }
  return absl::InvalidArgumentError(
      absl::StrFormat("Invalid channel kind '%s'", str));
}

std::ostream& operator<<(std::ostream& os, FlowControl fc) {
  os << FlowControlToString(fc);
  return os;
}

absl::StatusOr<ChannelStrictness> ChannelStrictnessFromString(
    std::string_view text) {
  if (text == "proven_mutually_exclusive") {
    return ChannelStrictness::kProvenMutuallyExclusive;
  }
  if (text == "runtime_mutually_exclusive") {
    return ChannelStrictness::kRuntimeMutuallyExclusive;
  }
  if (text == "total_order") {
    return ChannelStrictness::kTotalOrder;
  }
  if (text == "runtime_ordered") {
    return ChannelStrictness::kRuntimeOrdered;
  }
  if (text == "arbitrary_static_order") {
    return ChannelStrictness::kArbitraryStaticOrder;
  }
  return absl::InvalidArgumentError(
      absl::StrFormat("Invalid strictness %s.", text));
}

std::string ChannelStrictnessToString(ChannelStrictness in) {
  if (in == ChannelStrictness::kProvenMutuallyExclusive) {
    return "proven_mutually_exclusive";
  }
  if (in == ChannelStrictness::kRuntimeMutuallyExclusive) {
    return "runtime_mutually_exclusive";
  }
  if (in == ChannelStrictness::kTotalOrder) {
    return "total_order";
  }
  if (in == ChannelStrictness::kRuntimeOrdered) {
    return "runtime_ordered";
  }
  if (in == ChannelStrictness::kArbitraryStaticOrder) {
    return "arbitrary_static_order";
  }
  return "unknown";
}

std::ostream& operator<<(std::ostream& os, ChannelStrictness in) {
  os << ChannelStrictnessToString(in);
  return os;
}

std::string ChannelDirectionToString(ChannelDirection direction) {
  switch (direction) {
    case ChannelDirection::kSend:
      return "send";
    case ChannelDirection::kReceive:
      return "receive";
  }
}

absl::StatusOr<ChannelDirection> ChannelDirectionFromString(
    std::string_view str) {
  if (str == "send") {
    return ChannelDirection::kSend;
  }
  if (str == "receive") {
    return ChannelDirection::kReceive;
  }
  return absl::InvalidArgumentError(
      absl::StrFormat("Invalid direction %s.", str));
}

std::ostream& operator<<(std::ostream& os, ChannelDirection direction) {
  os << ChannelDirectionToString(direction);
  return os;
}

std::unique_ptr<SendChannelInterface> SendChannelInterface::Clone(
    std::optional<std::string_view> new_name) const {
  auto clone = std::make_unique<SendChannelInterface>(new_name.value_or(name()),
                                                      type(), kind());
  clone->SetKind(kind());
  clone->SetStrictness(strictness());
  clone->SetFlowControl(flow_control());
  clone->SetFlopKind(flop_kind());
  return clone;
}

std::unique_ptr<ReceiveChannelInterface> ReceiveChannelInterface::Clone(
    std::optional<std::string_view> new_name) const {
  auto clone = std::make_unique<ReceiveChannelInterface>(
      new_name.value_or(name()), type(), kind());
  clone->SetKind(kind());
  clone->SetStrictness(strictness());
  clone->SetFlowControl(flow_control());
  clone->SetFlopKind(flop_kind());
  return clone;
}

std::string_view ChannelRefName(ChannelRef ref) {
  return absl::visit([](const auto& ch) { return ch->name(); }, ref);
}

Type* ChannelRefType(ChannelRef ref) {
  if (std::holds_alternative<ChannelInterface*>(ref)) {
    return std::get<ChannelInterface*>(ref)->type();
  }
  return std::get<Channel*>(ref)->type();
}

ChannelKind ChannelRefKind(ChannelRef ref) {
  if (std::holds_alternative<ChannelInterface*>(ref)) {
    return std::get<ChannelInterface*>(ref)->kind();
  }
  return std::get<Channel*>(ref)->kind();
}

std::optional<ChannelStrictness> ChannelRefStrictness(ChannelRef ref) {
  if (std::holds_alternative<ChannelInterface*>(ref)) {
    return std::get<ChannelInterface*>(ref)->strictness();
  }

  if (std::get<Channel*>(ref)->kind() == ChannelKind::kStreaming) {
    return down_cast<StreamingChannel*>(std::get<Channel*>(ref))
        ->GetStrictness();
  }
  return std::nullopt;
}

std::string ChannelInterface::ToString() const {
  std::vector<std::string> keyword_strs;
  keyword_strs.push_back(
      absl::StrCat("direction=", ChannelDirectionToString(direction())));
  keyword_strs.push_back(absl::StrCat("kind=", ChannelKindToString(kind())));
  if (strictness_.has_value()) {
    keyword_strs.push_back(absl::StrCat(
        "strictness=", ChannelStrictnessToString(strictness_.value())));
  }
  keyword_strs.push_back(
      absl::StrCat("flow_control=", FlowControlToString(flow_control())));
  keyword_strs.push_back(
      absl::StrCat("flop_kind=", FlopKindToString(flop_kind())));
  return absl::StrFormat("chan_interface %s(%s)", name(),
                         absl::StrJoin(keyword_strs, ", "));
}

std::string ChannelRefToString(ChannelRef ref) {
  if (std::holds_alternative<ChannelInterface*>(ref)) {
    return std::get<ChannelInterface*>(ref)->ToString();
  }
  return std::get<Channel*>(ref)->ToString();
}

FlowControl ChannelRefFlowControl(ChannelRef ref) {
  if (std::holds_alternative<ChannelInterface*>(ref)) {
    return std::get<ChannelInterface*>(ref)->flow_control();
  } else if (StreamingChannel* streaming_channel =
                 dynamic_cast<StreamingChannel*>(std::get<Channel*>(ref));
             streaming_channel != nullptr) {
    return streaming_channel->GetFlowControl();
  }
  return FlowControl::kNone;
}

std::string ChannelConfig::ToString() const {
  return absl::StrFormat(
      "ChannelConfig(%s)",
      absl::StrJoin(GetDslxKwargs(), ", ", absl::PairFormatter("=")));
}
std::vector<std::pair<std::string, std::string>> ChannelConfig::GetDslxKwargs()
    const {
  std::vector<std::pair<std::string, std::string>> kwargs;
  if (fifo_config().has_value()) {
    absl::c_copy(fifo_config()->GetDslxKwargs(), std::back_inserter(kwargs));
  }
  if (input_flop_kind().has_value()) {
    kwargs.push_back({"input_flop_kind", FlopKindToString(*input_flop_kind())});
  }
  if (output_flop_kind().has_value()) {
    kwargs.push_back(
        {"output_flop_kind", FlopKindToString(*output_flop_kind())});
  }
  return kwargs;
}

ChannelRef ToChannelRef(AnyChannelRef ref) {
  if (std::holds_alternative<ChannelRef>(ref)) {
    return std::get<ChannelRef>(ref);
  }
  if (std::holds_alternative<SendChannelRef>(ref)) {
    return std::visit([](const auto& v) -> ChannelRef { return v; },
                      std::get<SendChannelRef>(ref));
  }
  return std::visit([](const auto& v) -> ChannelRef { return v; },
                    std::get<ReceiveChannelRef>(ref));
}

}  // namespace xls
