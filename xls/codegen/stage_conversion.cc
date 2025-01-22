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

#include "xls/codegen/stage_conversion.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_format.h"
#include "absl/types/span.h"
#include "xls/codegen/codegen_options.h"
#include "xls/common/status/ret_check.h"
#include "xls/common/status/status_macros.h"
#include "xls/ir/channel.h"
#include "xls/ir/function_base.h"
#include "xls/ir/function_builder.h"
#include "xls/ir/instantiation.h"
#include "xls/ir/name_uniquer.h"
#include "xls/ir/node.h"
#include "xls/ir/nodes.h"
#include "xls/ir/op.h"
#include "xls/ir/package.h"
#include "xls/ir/proc.h"
#include "xls/ir/source_location.h"
#include "xls/ir/type.h"
#include "xls/ir/value.h"
#include "xls/ir/xls_ir_interface.pb.h"
#include "xls/scheduling/pipeline_schedule.h"

namespace xls::verilog {
namespace {

class StageConversionHandler {
 public:
  // Initialize this object with the proc/function, and conversion options.
  StageConversionHandler(FunctionBase* proc_or_function,
                         const CodegenOptions& options,
                         StageConversionMetadata& metadata)
      : is_proc_(proc_or_function->IsProc()),
        function_base_(proc_or_function),
        options_(options),
        metadata_(metadata) {}

  // Creates input channels associated with the inputs (i.e. Params) for the
  // function.
  absl::Status CreatePipelineInputChannelsForFunction(
      ProcBuilder& pb, ProcMetadata* proc_metadata) {
    XLS_RET_CHECK(proc_metadata != nullptr);

    if (is_proc_) {
      return absl::OkStatus();
    }

    Function* f = function_base_->AsFunctionOrDie();

    for (Param* param : f->params()) {
      XLS_ASSIGN_OR_RETURN(
          ReceiveChannelReference * chan,
          pb.AddInputChannel(param->GetName(), param->GetType(),
                             ChannelKind::kStreaming));
      proc_metadata->AssociateReceiveChannelReference(
          param, chan, SpecialUseMetadata::Purpose::kExternalInput);
    }

    return absl::OkStatus();
  }

  // Creates input channels associated with the inputs (i.e. Params) for the
  // function.
  absl::Status CreatePipelineOutputChannelsForFunction(
      ProcBuilder& pb, ProcMetadata* proc_metadata) {
    XLS_RET_CHECK(proc_metadata != nullptr);

    if (is_proc_) {
      return absl::OkStatus();
    }

    Function* f = function_base_->AsFunctionOrDie();

    std::string_view output_port_name = options_.output_port_name();
    Node* return_value = f->return_value();
    Type* return_type = return_value->GetType();

    XLS_ASSIGN_OR_RETURN(SendChannelReference * chan,
                         pb.AddOutputChannel(output_port_name, return_type,
                                             ChannelKind::kStreaming));
    proc_metadata->AssociateSendChannelReference(
        return_value, chan, SpecialUseMetadata::Purpose::kExternalOutput);

    return absl::OkStatus();
  }

  // Add datapath input/output channels between stages. A channel is needed for
  // each node which is scheduled at or before a cycle and has a use after this
  // cycle.
  absl::Status CreatePipelineDatapathChannels(const PipelineSchedule& schedule,
                                              ProcBuilder& pb,
                                              ProcMetadata* proc_metadata) {
    XLS_RET_CHECK(proc_metadata != nullptr);

    NameUniquer uniquer(/*separator=*/"__", /*reserved_names=*/{});

    // No need to create datapath channels for single stage pipelines.
    if (schedule.length() <= 1) {
      return absl::OkStatus();
    }

    // TODO(tedhong): 2024-12-19 - Optimize this by creating a data structure
    // that maps nodes to the cycles it is live in and reuse here and in
    // CreateDatapathChannelsForStage().
    for (int64_t stage = 0; stage < schedule.length() - 1; ++stage) {
      std::vector<Node*> live_out_of_cycle = schedule.GetLiveOutOfCycle(stage);

      for (Node* node : live_out_of_cycle) {
        std::string ch_name = uniquer.GetSanitizedUniqueName(absl::StrFormat(
            "__stage_%d_to_%d_%s", stage, stage + 1, node->GetName()));
        Type* type = node->GetType();

        XLS_ASSIGN_OR_RETURN(
            ChannelReferences chan,
            pb.AddChannel(ch_name, type, ChannelKind::kStreaming));
        SpecialUseMetadata* special_use_metadata =
            proc_metadata->AssociateChannelReferences(
                node, chan, SpecialUseMetadata::Purpose::kDatapathIO);

        // All channels out of a given stage share the same flow control.
        special_use_metadata->SetGroupId(stage);

        // Datapath output channels all fire after all other channels of a
        // stage.
        special_use_metadata->SetStrictlyAfterAll(true);
      }
    }

    return absl::OkStatus();
  }

  // Instantiate and stitch all stages in the pipeline together.
  absl::Status InstantiateAndStitchPipeline(const PipelineSchedule& schedule,
                                            ProcBuilder& pb,
                                            ProcMetadata* proc_metadata) {
    XLS_RET_CHECK(proc_metadata != nullptr);

    // Instantiate each stage and use the names of channels to associate
    XLS_ASSIGN_OR_RETURN(std::vector<ProcMetadata*> stages,
                         metadata_.GetChildrenOf(proc_metadata));
    for (ProcMetadata* stage_proc_metadata : stages) {
      Proc* stage_proc = stage_proc_metadata->proc();
      std::string stage_name =
          absl::StrFormat("__%s_inst_", stage_proc->name());

      XLS_ASSIGN_OR_RETURN(
          std::vector<ChannelReference*> channel_references,
          AssociateStageChannelReferences(stage_proc_metadata, proc_metadata));

      XLS_RET_CHECK_OK(
          pb.InstantiateProc(stage_name, stage_proc, channel_references));
    }

    return absl::OkStatus();
  }

  // Add datapath input/output channel interfaces for a given stage. A channel
  // is needed for each node which is scheduled at or before a cycle and has a
  // use after this cycle.
  absl::Status CreateDatapathChannelsForStage(const PipelineSchedule& schedule,
                                              int64_t stage, ProcBuilder& pb,
                                              ProcMetadata* proc_metadata) {
    XLS_RET_CHECK(proc_metadata != nullptr);

    NameUniquer uniquer(/*separator=*/"__", /*reserved_names=*/{});

    // No need to create datapath channels for single stage pipelines.
    if (schedule.length() <= 1) {
      return absl::OkStatus();
    }

    // Create channels for nodes from the prior stage.
    for (Node* node : schedule.GetLiveOutOfCycle(stage - 1)) {
      std::string ch_name = uniquer.GetSanitizedUniqueName(absl::StrFormat(
          "__stage_%d_to_%d_%s", stage - 1, stage, node->GetName()));
      Type* type = node->GetType();

      XLS_ASSIGN_OR_RETURN(
          ReceiveChannelReference * chan,
          pb.AddInputChannel(ch_name, type, ChannelKind::kStreaming));
      SpecialUseMetadata* special_use_metadata =
          proc_metadata->AssociateReceiveChannelReference(
              node, chan, SpecialUseMetadata::Purpose::kDatapathInput);

      // All channels into a given stage share the same flow control.
      special_use_metadata->SetGroupId(stage);
    }

    // Create channels for nodes to next stage.
    for (Node* node : schedule.GetLiveOutOfCycle(stage)) {
      std::string ch_name = uniquer.GetSanitizedUniqueName(absl::StrFormat(
          "__stage_%d_to_%d_%s", stage, stage + 1, node->GetName()));
      Type* type = node->GetType();

      XLS_ASSIGN_OR_RETURN(
          SendChannelReference * chan,
          pb.AddOutputChannel(ch_name, type, ChannelKind::kStreaming));
      SpecialUseMetadata* special_use_metadata =
          proc_metadata->AssociateSendChannelReference(
              node, chan, SpecialUseMetadata::Purpose::kDatapathOutput);

      // All channels out of a given stage share the same flow control.
      special_use_metadata->SetGroupId(stage);
    }

    return absl::OkStatus();
  }

  // Create internal nodes for the given stage.
  absl::Status ProcessNodesForStage(const PipelineSchedule& schedule,
                                    int64_t stage, ProcBuilder& pb,
                                    ProcMetadata* proc_metadata) {
    XLS_RET_CHECK(proc_metadata != nullptr);

    absl::Span<Node* const> sorted_nodes = schedule.nodes_in_cycle(stage);

    for (Node* node : sorted_nodes) {
      if (node->Is<Param>()) {
        XLS_RET_CHECK(!is_proc_);
        XLS_RET_CHECK_OK(ReceiveStageInput(node, pb, proc_metadata));
      } else {
        XLS_RET_CHECK_OK(HandleGeneralNode(node, pb, proc_metadata));
      }
    }

    // Process all nodes that need to be passed to the next stage.
    std::vector<Node*> live_out_of_cycle = schedule.GetLiveOutOfCycle(stage);
    for (Node* node : live_out_of_cycle) {
      if (schedule.cycle(node) != stage) {
        XLS_RET_CHECK_OK(ReceiveStageInput(node, pb, proc_metadata));
      }

      XLS_RET_CHECK_OK(SendStageOutput(node, pb, proc_metadata));
    }

    return absl::OkStatus();
  }

  // Create a send node for the return node of a function.
  absl::Status ProcessReturnNodeForFunction(ProcBuilder& pb,
                                            ProcMetadata* proc_metadata) {
    XLS_RET_CHECK(proc_metadata != nullptr);

    if (is_proc_) {
      return absl::OkStatus();
    }

    Function* f = function_base_->AsFunctionOrDie();

    Node* return_value = f->return_value();
    XLS_ASSIGN_OR_RETURN(
        std::vector<SpecialUseMetadata*> return_metadata,
        proc_metadata->FindSpecialUseMetadata(
            return_value, static_cast<SpecialUseMetadata::Purpose>(
                              SpecialUseMetadata::Purpose::kExternalOutput)));
    XLS_RET_CHECK_EQ(return_metadata.size(), 1);

    SendChannelReference* chan = return_metadata[0]->GetSendChannelReference();
    Node* node = proc_metadata->GetFromOrigMapping(return_value);

    std::string send_name = absl::StrFormat("__send_%s", node->GetName());
    BValue token = pb.Literal(Value::Token(), SourceInfo());
    pb.Send(chan, token, BValue(node, &pb), return_value->loc(), send_name);

    return absl::OkStatus();
  }

 private:
  absl::StatusOr<std::vector<ChannelReference*>>
  AssociateStageChannelReferences(ProcMetadata* stage_proc_metadata,
                                  ProcMetadata* proc_metadata) {
    std::vector<ChannelReference*> channel_references;

    Proc* top_proc = proc_metadata->proc();

    for (ChannelReference* const interface_channel :
         stage_proc_metadata->proc()->interface()) {
      std::string_view interface_channel_name = interface_channel->name();
      XLS_ASSIGN_OR_RETURN(
          SpecialUseMetadata * stage_interface_metadata,
          stage_proc_metadata->FindSpecialUseMetadataForChannel(
              interface_channel));

      uint32_t purpose = stage_interface_metadata->purpose();

      // TODO(tedhong): 2024-12-19 Create a path/id scheme so that we don't need
      // to rely on string matching to associate channels.
      if (purpose & (SpecialUseMetadata::Purpose::kDatapathInput |
                     SpecialUseMetadata::Purpose::kExternalInput)) {
        XLS_ASSIGN_OR_RETURN(
            ReceiveChannelReference * ref,
            top_proc->GetReceiveChannelReference(interface_channel_name));
        channel_references.push_back(ref);
      } else if (purpose & (SpecialUseMetadata::Purpose::kDatapathOutput |
                            SpecialUseMetadata::Purpose::kExternalOutput)) {
        XLS_ASSIGN_OR_RETURN(
            SendChannelReference * ref,
            top_proc->GetSendChannelReference(interface_channel_name));
        channel_references.push_back(ref);
      } else {
        return absl::InternalError(
            absl::StrFormat("Interface channel %s has unsupported purpose %d",
                            interface_channel->name(), purpose));
      }
    }

    return channel_references;
  }

  absl::StatusOr<Node*> ReceiveStageInput(Node* orig_node, ProcBuilder& pb,
                                          ProcMetadata* proc_metadata) {
    XLS_ASSIGN_OR_RETURN(
        std::vector<SpecialUseMetadata*> io_metadata,
        proc_metadata->FindSpecialUseMetadata(
            orig_node, static_cast<SpecialUseMetadata::Purpose>(
                           SpecialUseMetadata::Purpose::kDatapathInput |
                           SpecialUseMetadata::Purpose::kExternalInput)));

    if (io_metadata.size() != 1) {
      return absl::InternalError(absl::StrFormat(
          "Stage %s does not have or has ambiguous source for node %s",
          pb.proc()->name(), orig_node->ToString()));
    }

    ReceiveChannelReference* chan =
        io_metadata[0]->GetReceiveChannelReference();

    std::string recv_name = absl::StrFormat("__recv_%s", orig_node->GetName());
    BValue token = pb.Literal(Value::Token(), SourceInfo());
    BValue res = pb.Receive(chan, token, orig_node->loc(), recv_name);
    BValue recv_value = pb.TupleIndex(res, 1, orig_node->loc());

    proc_metadata->RecordNodeMapping(orig_node, recv_value.node());

    return recv_value.node();
  }

  absl::Status SendStageOutput(Node* orig_node, ProcBuilder& pb,
                               ProcMetadata* proc_metadata) {
    if (is_proc_) {
      return absl::OkStatus();
    }

    if (!proc_metadata->HasFromOrigMapping(orig_node)) {
      return absl::InternalError(
          absl::StrFormat("Stage %s has no mapping for node %s",
                          pb.proc()->name(), orig_node->ToString()));
    }
    Node* node = proc_metadata->GetFromOrigMapping(orig_node);

    XLS_ASSIGN_OR_RETURN(
        std::vector<SpecialUseMetadata*> io_metadata,
        proc_metadata->FindSpecialUseMetadata(
            orig_node, static_cast<SpecialUseMetadata::Purpose>(
                           SpecialUseMetadata::Purpose::kDatapathOutput |
                           SpecialUseMetadata::Purpose::kExternalOutput)));

    for (SpecialUseMetadata* m : io_metadata) {
      SendChannelReference* chan = m->GetSendChannelReference();

      std::string send_name = absl::StrFormat("__send_%s", node->GetName());
      BValue token = pb.Literal(Value::Token(), SourceInfo());
      pb.Send(chan, token, BValue(node, &pb), orig_node->loc(), send_name);
    }

    return absl::OkStatus();
  }

  // Clone the operation from the source to the block as is.
  absl::StatusOr<Node*> HandleGeneralNode(Node* node, ProcBuilder& pb,
                                          ProcMetadata* proc_metadata) {
    std::vector<Node*> new_operands;
    for (Node* operand : node->operands()) {
      Node* stage_operand;
      if (proc_metadata->HasFromOrigMapping(operand)) {
        stage_operand = proc_metadata->GetFromOrigMapping(operand);
      } else {
        // No prior mapping exists, so this must be a datapath input.
        XLS_ASSIGN_OR_RETURN(stage_operand,
                             ReceiveStageInput(operand, pb, proc_metadata));
      }

      new_operands.push_back(stage_operand);
    }

    XLS_ASSIGN_OR_RETURN(Node * res,
                         node->CloneInNewFunction(new_operands, pb.proc()));

    proc_metadata->RecordNodeMapping(node, res);

    return res;
  }

  bool is_proc_;
  FunctionBase* function_base_;
  const CodegenOptions& options_;
  StageConversionMetadata& metadata_;
};

}  // namespace

absl::StatusOr<std::vector<SpecialUseMetadata*>>
ProcMetadata::FindSpecialUseMetadata(Node* orig_node,
                                     SpecialUseMetadata::Purpose purpose) {
  auto metadata_map_iterator = orig_node_to_metadata_map_.find(orig_node);

  if (metadata_map_iterator == orig_node_to_metadata_map_.end()) {
    return absl::InternalError(
        absl::StrFormat("FindSpecialUseMetadata() has no map for node %s",
                        orig_node->ToString()));
  }

  std::vector<SpecialUseMetadata*> ret;

  for (SpecialUseMetadata* m : metadata_map_iterator->second) {
    if ((m->purpose() & purpose) != 0) {
      ret.push_back(m);
    }
  }

  return ret;
}

absl::StatusOr<SpecialUseMetadata*>
ProcMetadata::FindSpecialUseMetadataForChannel(ChannelReference* chan_ref) {
  auto metadata_map_iterator = channel_ref_to_metadata_map_.find(chan_ref);

  if (metadata_map_iterator == channel_ref_to_metadata_map_.end()) {
    return absl::InternalError(
        absl::StrFormat("FindSpecialUseMetadata() has no map for reference %s",
                        chan_ref->ToString()));
  }

  return metadata_map_iterator->second;
}

absl::StatusOr<ProcMetadata*> StageConversionMetadata::GetTopProcMetadata(
    FunctionBase* orig) {
  auto metadata_map_iterator = orig_to_metadata_map_.find(orig);

  if (metadata_map_iterator == orig_to_metadata_map_.end()) {
    return absl::InternalError(absl::StrFormat(
        "GetTopProcMetadata() has no map for function base %s", orig->name()));
  }

  for (ProcMetadata* m : metadata_map_iterator->second) {
    if (m->IsTop()) {
      return m;
    }
  }

  return absl::InternalError(
      absl::StrFormat("GetTopProcMetadata() has no top proc for function "
                      "base %s",
                      orig->name()));
}

absl::StatusOr<std::vector<ProcMetadata*>>
StageConversionMetadata::GetChildrenOf(ProcMetadata* proc_m) {
  std::vector<ProcMetadata*> ret;

  for (std::unique_ptr<ProcMetadata>& p : proc_metadata_) {
    if (p->parent() == proc_m) {
      ret.push_back(p.get());
    }
  }

  return ret;
}

absl::Status SingleFunctionBaseToPipelinedStages(
    std::string_view top_name, const PipelineSchedule& schedule,
    const CodegenOptions& options, StageConversionMetadata& metadata) {
  FunctionBase* function_base = schedule.function_base();
  XLS_RET_CHECK(function_base->IsProc() || function_base->IsFunction());
  Package* package = function_base->package();

  StageConversionHandler stage_conversion(function_base, options, metadata);

  // Create top-level proc and fill out interfaces.
  std::string pipelined_top_name =
      absl::StrFormat("%s_pipeline_%d_", top_name, schedule.length());
  ProcBuilder top_builder(NewStyleProc{}, pipelined_top_name, package,
                          /*should_verify=*/false);
  ProcMetadata* top_metadata =
      metadata.AssociateWithNewPipeline(top_builder.proc(), function_base);

  // Create I/O channels and datapath channels
  XLS_RET_CHECK_OK(stage_conversion.CreatePipelineInputChannelsForFunction(
      top_builder, top_metadata));
  XLS_RET_CHECK_OK(stage_conversion.CreatePipelineOutputChannelsForFunction(
      top_builder, top_metadata));
  XLS_RET_CHECK_OK(stage_conversion.CreatePipelineDatapathChannels(
      schedule, top_builder, top_metadata));

  // Create a proc for each stage.
  NameUniquer uniquer(/*separator=*/"__", /*reserved_names=*/{});

  for (int64_t stage = 0; stage < schedule.length(); ++stage) {
    std::string stage_name = uniquer.GetSanitizedUniqueName(
        absl::StrFormat("%s_stage_%d_", top_name, stage));
    ProcBuilder stage_builder(NewStyleProc{}, stage_name, package,
                              /*should_verify=*/false);

    ProcMetadata* stage_metadata = metadata.AssociateWithNewStage(
        stage_builder.proc(), function_base, top_metadata);

    // If this is the first stage, add in input channels.
    if (stage == 0) {
      XLS_RET_CHECK_OK(stage_conversion.CreatePipelineInputChannelsForFunction(
          stage_builder, stage_metadata));
    }

    // If this is the last stage, add in output channels.
    if (stage == schedule.length() - 1) {
      XLS_RET_CHECK_OK(stage_conversion.CreatePipelineOutputChannelsForFunction(
          stage_builder, stage_metadata));
    }

    // Create datapath input and output channels for this stage.
    XLS_RET_CHECK_OK(stage_conversion.CreateDatapathChannelsForStage(
        schedule, stage, stage_builder, stage_metadata));

    // Create internal nodes for this stage.
    XLS_RET_CHECK_OK(stage_conversion.ProcessNodesForStage(
        schedule, stage, stage_builder, stage_metadata));

    if (stage == schedule.length() - 1) {
      XLS_RET_CHECK_OK(stage_conversion.ProcessReturnNodeForFunction(
          stage_builder, stage_metadata));
    }

    XLS_RET_CHECK_OK(stage_builder.Build());
  }

  // Stitch each stage together.
  XLS_RET_CHECK_OK(stage_conversion.InstantiateAndStitchPipeline(
      schedule, top_builder, top_metadata));

  XLS_RET_CHECK_OK(top_builder.Build());

  return absl::OkStatus();
}

}  // namespace xls::verilog