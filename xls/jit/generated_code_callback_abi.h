// Copyright 2026 The XLS Authors
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

// Lightweight generated-code callback-table ABI shared by LLVM-emitted AOT
// code and every runtime that can host it.
//
// This header intentionally exposes only the binary contract that generated
// code dereferences directly. Full JIT execution still lives in
// `jit_callbacks.h`; standalone runtimes include this header so they can satisfy
// the generated layout without linking the heavyweight JIT implementation.

#ifndef XLS_JIT_GENERATED_CODE_CALLBACK_ABI_H_
#define XLS_JIT_GENERATED_CODE_CALLBACK_ABI_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace xls {

class InterpreterEvents;
class JitRuntime;
struct InstanceContext;

// Manual vtable prefix expected at the start of every generated-code instance
// context.
//
// LLVM-generated code loads these slots by byte offset rather than through C++
// virtual dispatch, so slot order, field count, and function signatures are all
// ABI. A runtime may reject a feature at construction time and install aborting
// handlers for unreachable slots, but it must still preserve this exact table
// shape for artifacts built against the matching ABI version.
struct InstanceContextVTable {
 public:
  // Appends one already-formatted trace fragment to an in-progress trace buffer.
  using PerformStringStepFn = void (*)(InstanceContext* thiz, char* step_string,
                                       std::string* buffer);
  // Formats one value fragment into an in-progress trace buffer.
  using PerformFormatStepFn =
      void (*)(InstanceContext* thiz, JitRuntime* runtime,
               const uint8_t* type_proto_data, int64_t type_proto_data_size,
               const uint8_t* value, uint64_t format_u64, std::string* buffer);
  // Publishes one completed trace buffer to the runtime event sink.
  using RecordTraceFn = void (*)(InstanceContext* thiz, std::string* buffer,
                                 int64_t verbosity, InterpreterEvents* events);
  // Creates the mutable buffer used while lowering one trace statement.
  using CreateTraceBufferFn = std::string* (*)(InstanceContext* thiz);
  // Publishes one failed assertion message to the runtime event sink.
  using RecordAssertionFn = void (*)(InstanceContext* thiz, const char* msg,
                                     InterpreterEvents* events);
  // Receives one raw proc value from the queue selected at AOT compile time.
  using QueueReceiveWrapperFn = bool (*)(InstanceContext* thiz,
                                         int64_t queue_index, uint8_t* buffer);
  // Sends one raw proc value to the queue selected at AOT compile time.
  using QueueSendWrapperFn = void (*)(InstanceContext* instance_context,
                                      int64_t queue_index, const uint8_t* data);
  // Records the selected `next_value` node for proc execution bookkeeping.
  using RecordActiveNextValueFn = void (*)(InstanceContext* thiz,
                                           int64_t param_id, int64_t next_id);
  // Reports one computed node result to an optional runtime observer.
  using RecordNodeResultFn = void (*)(InstanceContext* thiz, int64_t node_ptr,
                                      const uint8_t* data);
  // Allocates aligned scratch storage requested by generated code.
  using AllocateBufferFn = void* (*)(InstanceContext* thiz, int64_t byte_size,
                                     int64_t alignment);
  // Releases scratch storage returned by `AllocateBufferFn`.
  using DeallocateBufferFn = void (*)(InstanceContext* thiz, void* buffer);
  // Records the chosen register-write node for block execution bookkeeping.
  using RecordActiveRegisterWriteFn = void (*)(InstanceContext* thiz,
                                               int64_t register_no,
                                               int64_t register_write_no);

  // Builds the full-JIT callback table used by the ordinary in-process runtime.
  explicit InstanceContextVTable();
  // Builds a callback table for runtimes that provide their own implementations.
  InstanceContextVTable(PerformStringStepFn perform_string_step,
                        PerformFormatStepFn perform_format_step,
                        RecordTraceFn record_trace,
                        CreateTraceBufferFn create_trace_buffer,
                        RecordAssertionFn record_assertion,
                        QueueReceiveWrapperFn queue_receive_wrapper,
                        QueueSendWrapperFn queue_send_wrapper,
                        RecordActiveNextValueFn record_active_next_value,
                        RecordNodeResultFn record_node_result,
                        AllocateBufferFn allocate_buffer,
                        DeallocateBufferFn deallocate_buffer,
                        RecordActiveRegisterWriteFn
                            record_active_register_write)
      : perform_string_step(perform_string_step),
        perform_format_step(perform_format_step),
        record_trace(record_trace),
        create_trace_buffer(create_trace_buffer),
        record_assertion(record_assertion),
        queue_receive_wrapper(queue_receive_wrapper),
        queue_send_wrapper(queue_send_wrapper),
        record_active_next_value(record_active_next_value),
        record_node_result(record_node_result),
        allocate_buffer(allocate_buffer),
        deallocate_buffer(deallocate_buffer),
        record_active_register_write(record_active_register_write) {}

  // Appends one literal trace fragment.
  const PerformStringStepFn perform_string_step;
  // Appends one formatted-value trace fragment.
  const PerformFormatStepFn perform_format_step;
  // Publishes one finished trace statement.
  const RecordTraceFn record_trace;
  // Allocates the trace buffer consumed by `record_trace`.
  const CreateTraceBufferFn create_trace_buffer;
  // Publishes one failed assertion message.
  const RecordAssertionFn record_assertion;
  // Receives one raw value from a proc channel queue.
  const QueueReceiveWrapperFn queue_receive_wrapper;
  // Sends one raw value to a proc channel queue.
  const QueueSendWrapperFn queue_send_wrapper;
  // Records one active proc `next_value`.
  const RecordActiveNextValueFn record_active_next_value;
  // Reports one raw node result to an observer.
  const RecordNodeResultFn record_node_result;
  // Allocates generated-code scratch storage.
  const AllocateBufferFn allocate_buffer;
  // Releases generated-code scratch storage.
  const DeallocateBufferFn deallocate_buffer;
  // Records one active block register write.
  const RecordActiveRegisterWriteFn record_active_register_write;

  // Number of ABI slots that generated code may index by offset.
  static constexpr int64_t kVTableLength = 12;
  // Plain function-pointer view used to validate the ABI layout size.
  using VTableArrayType = std::array<void (*)(), kVTableLength>;
};

// Generated code assumes the manual table is exactly a dense pointer array.
static_assert(sizeof(InstanceContextVTable) ==
              sizeof(InstanceContextVTable::VTableArrayType));

}  // namespace xls

#endif  // XLS_JIT_GENERATED_CODE_CALLBACK_ABI_H_
