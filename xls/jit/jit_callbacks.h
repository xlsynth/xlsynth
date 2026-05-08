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

#ifndef XLS_JIT_JIT_CALLBACKS_H_
#define XLS_JIT_JIT_CALLBACKS_H_

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/types/span.h"
#include "xls/ir/events.h"
#include "xls/ir/proc_elaboration.h"
#include "xls/ir/type.h"
#include "xls/ir/type_manager.h"
#include "xls/jit/jit_callback_abi.h"
#include "xls/jit/jit_channel_queue.h"
#include "xls/jit/jit_runtime.h"
#include "xls/jit/observer.h"

namespace xls {

// Full in-process execution context used by ordinary JIT evaluation.
//
// The ABI-visible callback prefix now lives in `jit_callback_abi.h` so
// standalone runtimes can satisfy generated code without depending on the rest
// of this type. The remaining fields here are intentionally heavyweight JIT
// state for proc/block execution, type materialization, and observers.
struct InstanceContext {
 public:
  // Creates the minimal full-JIT context used by function evaluation.
  static InstanceContext CreateForFunc() { return InstanceContext(); }
  // Creates the minimal full-JIT context used by block evaluation.
  static InstanceContext CreateForBlock() { return InstanceContext(); }
  // Creates a proc context with the instance and compile-time queue ordering
  // expected by generated send/receive callbacks.
  static InstanceContext CreateForProc(ProcInstance* inst,
                                       std::vector<JitChannelQueue*> queues) {
    InstanceContext ret;
    ret.instance = inst;
    ret.channel_queues = std::move(queues);
    return ret;
  }

  // Offsets in the ABI prefix that LLVM-generated code uses to load callbacks.
  static constexpr int64_t kPerformStringStepOffset =
      offsetof(InstanceContextVTable, perform_string_step);
  static constexpr int64_t kPerformFormatStepOffset =
      offsetof(InstanceContextVTable, perform_format_step);
  static constexpr int64_t kRecordTraceOffset =
      offsetof(InstanceContextVTable, record_trace);
  static constexpr int64_t kCreateTraceBufferOffset =
      offsetof(InstanceContextVTable, create_trace_buffer);
  static constexpr int64_t kRecordAssertionOffset =
      offsetof(InstanceContextVTable, record_assertion);
  static constexpr int64_t kQueueReceiveWrapperOffset =
      offsetof(InstanceContextVTable, queue_receive_wrapper);
  static constexpr int64_t kQueueSendWrapperOffset =
      offsetof(InstanceContextVTable, queue_send_wrapper);
  static constexpr int64_t kRecordActiveNextValueOffset =
      offsetof(InstanceContextVTable, record_active_next_value);
  static constexpr int64_t kRecordNodeResultOffset =
      offsetof(InstanceContextVTable, record_node_result);
  static constexpr int64_t kAllocateBufferOffset =
      offsetof(InstanceContextVTable, allocate_buffer);
  static constexpr int64_t kDeallocateBufferOffset =
      offsetof(InstanceContextVTable, deallocate_buffer);
  static constexpr int64_t kRecordActiveRegisterWrite =
      offsetof(InstanceContextVTable, record_active_register_write);
  static constexpr int64_t kVTableLength = InstanceContextVTable::kVTableLength;
  using VTableArrayType = InstanceContextVTable::VTableArrayType;

  // Returns whether an offset is one of the ABI-visible callback slots.
  static constexpr bool IsVtableOffset(int64_t v) {
    return v == kPerformFormatStepOffset || v == kPerformStringStepOffset ||
           v == kRecordTraceOffset || v == kCreateTraceBufferOffset ||
           v == kRecordAssertionOffset || v == kQueueReceiveWrapperOffset ||
           v == kQueueSendWrapperOffset || v == kRecordActiveNextValueOffset ||
           v == kRecordNodeResultOffset || v == kAllocateBufferOffset ||
           v == kDeallocateBufferOffset || v == kRecordActiveRegisterWrite;
  }

  // Materializes the serialized type needed by format callbacks for this
  // in-process runtime instance.
  Type* ParseTypeFromProto(absl::Span<uint8_t const> data);

  InstanceContextVTable vtable;

  // The proc instance being evaluated (if we are evaluating a proc).
  ProcInstance* instance = nullptr;

  // The active next values for each parameter (if we are evaluating a proc).
  // Map from node-id of the param to node-id of the next
  absl::flat_hash_map<int64_t, absl::flat_hash_set<int64_t>> active_next_values;

  // The active register writes for each register (if we are evaluating a
  // block). Map from register index (in Block::GetRegisters) to register write
  // index (in Block::GetRegisterWrites). This is only recorded for registers
  // which have multiple writes.
  absl::flat_hash_map<int64_t, std::vector<int64_t>> active_register_writes;

  // The channel queues used by the proc instance. The order of queues is
  // assigned at JIT compile time. The indices of particular queues is baked
  // into the JITted code for sends and receives.
  std::vector<JitChannelQueue*> channel_queues;

  // Arena used to materialize types that are passed to callbacks.
  std::unique_ptr<TypeManager> type_manager = std::make_unique<TypeManager>();

  RuntimeObserver* observer = nullptr;
};

static_assert(offsetof(InstanceContext, vtable) == 0);

}  // namespace xls

#endif  // XLS_JIT_JIT_CALLBACKS_H_
