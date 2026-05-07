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

// Lightweight ABI declarations shared by LLVM-emitted code and runtime
// implementations that need to satisfy the generated callback table without
// depending on the heavyweight JIT runtime itself.

#ifndef XLS_JIT_JIT_CALLBACK_ABI_H_
#define XLS_JIT_JIT_CALLBACK_ABI_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace xls {

class InterpreterEvents;
class JitRuntime;
struct InstanceContext;

// Manual vtable of an InstanceContext. Called directly from LLVM-generated
// code, so the slot order is part of the ABI shared by the compiler and runtime
// implementations.
struct InstanceContextVTable {
 public:
  using PerformStringStepFn = void (*)(InstanceContext* thiz, char* step_string,
                                       std::string* buffer);
  using PerformFormatStepFn =
      void (*)(InstanceContext* thiz, JitRuntime* runtime,
               const uint8_t* type_proto_data, int64_t type_proto_data_size,
               const uint8_t* value, uint64_t format_u64, std::string* buffer);
  using RecordTraceFn = void (*)(InstanceContext* thiz, std::string* buffer,
                                 int64_t verbosity, InterpreterEvents* events);
  using CreateTraceBufferFn = std::string* (*)(InstanceContext* thiz);
  using RecordAssertionFn = void (*)(InstanceContext* thiz, const char* msg,
                                     InterpreterEvents* events);
  using QueueReceiveWrapperFn = bool (*)(InstanceContext* thiz,
                                         int64_t queue_index, uint8_t* buffer);
  using QueueSendWrapperFn = void (*)(InstanceContext* instance_context,
                                      int64_t queue_index, const uint8_t* data);
  using RecordActiveNextValueFn = void (*)(InstanceContext* thiz,
                                           int64_t param_id, int64_t next_id);
  using RecordNodeResultFn = void (*)(InstanceContext* thiz, int64_t node_ptr,
                                      const uint8_t* data);
  using AllocateBufferFn = void* (*)(InstanceContext* thiz, int64_t byte_size,
                                     int64_t alignment);
  using DeallocateBufferFn = void (*)(InstanceContext* thiz, void* buffer);
  using RecordActiveRegisterWriteFn = void (*)(InstanceContext* thiz,
                                               int64_t register_no,
                                               int64_t register_write_no);

  explicit InstanceContextVTable();
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

  const PerformStringStepFn perform_string_step;
  const PerformFormatStepFn perform_format_step;
  const RecordTraceFn record_trace;
  const CreateTraceBufferFn create_trace_buffer;
  const RecordAssertionFn record_assertion;
  const QueueReceiveWrapperFn queue_receive_wrapper;
  const QueueSendWrapperFn queue_send_wrapper;
  const RecordActiveNextValueFn record_active_next_value;
  const RecordNodeResultFn record_node_result;
  const AllocateBufferFn allocate_buffer;
  const DeallocateBufferFn deallocate_buffer;
  const RecordActiveRegisterWriteFn record_active_register_write;

  static constexpr int64_t kVTableLength = 12;
  using VTableArrayType = std::array<void (*)(), kVTableLength>;
};

static_assert(sizeof(InstanceContextVTable) ==
              sizeof(InstanceContextVTable::VTableArrayType));

}  // namespace xls

#endif  // XLS_JIT_JIT_CALLBACK_ABI_H_
