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

#include "xls/public/standalone_aot_runtime.h"

#include <bit>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <string>

#include "xls/ir/events.h"
#include "xls/jit/aot_runtime_callbacks.h"
#include "xls/jit/jit_callback_abi.h"

namespace {

// Native function shape emitted for function-only standalone AOT artifacts.
using StandaloneEntrypoint = int64_t (*)(
    const uint8_t* const* inputs, uint8_t* const* outputs, void* temp_buffer,
    xls::InterpreterEvents* events, xls::InstanceContext* instance_context,
    xls::JitRuntime* jit_runtime, int64_t continuation_point);

// Fails fast if generated code reaches a callback that the negotiated feature
// set promised would be unreachable in this first standalone runtime.
[[noreturn]] void UnsupportedCallback() { std::abort(); }

// Trace string fragments are unsupported until trace lowering is added here.
void UnsupportedPerformStringStep(xls::InstanceContext* thiz, char* step_string,
                                  std::string* buffer) {
  UnsupportedCallback();
}

// Trace value formatting still depends on the full JIT runtime today.
void UnsupportedPerformFormatStep(xls::InstanceContext* thiz,
                                  xls::JitRuntime* runtime,
                                  const uint8_t* type_proto_data,
                                  int64_t type_proto_data_size,
                                  const uint8_t* value, uint64_t format_u64,
                                  std::string* buffer) {
  UnsupportedCallback();
}

// Completed traces are not part of the assertion-only runtime contract.
void UnsupportedRecordTrace(xls::InstanceContext* thiz, std::string* buffer,
                            int64_t verbosity,
                            xls::InterpreterEvents* events) {
  UnsupportedCallback();
}

// Trace buffers are intentionally unavailable until trace support lands.
std::string* UnsupportedCreateTraceBuffer(xls::InstanceContext* thiz) {
  UnsupportedCallback();
}

// Proc queue operations are invalid for function-only standalone artifacts.
bool UnsupportedQueueReceiveWrapper(xls::InstanceContext* thiz,
                                    int64_t queue_index, uint8_t* buffer) {
  UnsupportedCallback();
}

// Proc queue operations are invalid for function-only standalone artifacts.
void UnsupportedQueueSendWrapper(xls::InstanceContext* thiz,
                                 int64_t queue_index, const uint8_t* data) {
  UnsupportedCallback();
}

// Proc bookkeeping is invalid for function-only standalone artifacts.
void UnsupportedRecordActiveNextValue(xls::InstanceContext* thiz,
                                      int64_t param_id, int64_t next_id) {
  UnsupportedCallback();
}

// Observer callbacks are outside the standalone runtime contract.
void UnsupportedRecordNodeResult(xls::InstanceContext* thiz, int64_t node_ptr,
                                 const uint8_t* data) {
  UnsupportedCallback();
}

// Block bookkeeping is invalid for function-only standalone artifacts.
void UnsupportedRecordActiveRegisterWrite(xls::InstanceContext* thiz,
                                          int64_t register_no,
                                          int64_t register_write_no) {
  UnsupportedCallback();
}

// Callback table installed behind the ABI-visible prefix of the runtime
// object. Unsupported slots stay present so generated-code offsets remain ABI
// compatible, but they abort if a non-negotiated feature somehow reaches them.
struct StandaloneRuntimeVTable final : xls::InstanceContextVTable {
  StandaloneRuntimeVTable()
      : xls::InstanceContextVTable(
            &UnsupportedPerformStringStep, &UnsupportedPerformFormatStep,
            &UnsupportedRecordTrace, &UnsupportedCreateTraceBuffer,
            &xls::aot_runtime_callbacks::RecordAssertion,
            &UnsupportedQueueReceiveWrapper,
            &UnsupportedQueueSendWrapper, &UnsupportedRecordActiveNextValue,
            &UnsupportedRecordNodeResult,
            &xls::aot_runtime_callbacks::AllocateBuffer,
            &xls::aot_runtime_callbacks::DeallocateBuffer,
            &UnsupportedRecordActiveRegisterWrite) {}
};

// Feature bits this first standalone runtime can actually execute.
constexpr uint32_t kSupportedFeatures =
    XLS_STANDALONE_AOT_RUNTIME_FEATURE_ASSERTIONS;

}  // namespace

// Runtime object whose first bytes are the callback table seen by generated
// code. Assertion storage reuses the same `InterpreterEvents` path as the full
// JIT runtime so standalone execution does not grow a second event model.
struct xls_standalone_aot_runtime {
  StandaloneRuntimeVTable vtable;
  xls::InterpreterEvents events;
};

// Validates feature negotiation before allocating the standalone runtime
// object, so unreachable callbacks remain a construction-time contract.
enum xls_standalone_aot_runtime_status xls_standalone_aot_runtime_create(
    uint32_t abi_version, uint32_t required_features,
    struct xls_standalone_aot_runtime** out) {
  if (abi_version != XLS_STANDALONE_AOT_ABI_VERSION) {
    *out = nullptr;
    return XLS_STANDALONE_AOT_RUNTIME_STATUS_UNSUPPORTED_ABI_VERSION;
  } else if ((required_features & ~kSupportedFeatures) != 0) {
    *out = nullptr;
    return XLS_STANDALONE_AOT_RUNTIME_STATUS_UNSUPPORTED_FEATURE;
  } else {
    auto* runtime = new (std::nothrow) xls_standalone_aot_runtime;
    if (runtime == nullptr) {
      *out = nullptr;
      return XLS_STANDALONE_AOT_RUNTIME_STATUS_ALLOCATION_FAILED;
    }
    *out = runtime;
    return XLS_STANDALONE_AOT_RUNTIME_STATUS_OK;
  }
}

// Releases the runtime object and its owned event storage.
void xls_standalone_aot_runtime_free(
    struct xls_standalone_aot_runtime* runtime) {
  delete runtime;
}

// Clears event payloads while keeping one reusable runtime object alive.
void xls_standalone_aot_runtime_clear_events(
    struct xls_standalone_aot_runtime* runtime) {
  runtime->events.Clear();
}

// Exposes the number of assertion messages currently retained by the runtime.
size_t xls_standalone_aot_runtime_get_assert_message_count(
    const struct xls_standalone_aot_runtime* runtime) {
  return runtime->events.GetAssertMessageCount();
}

// Returns one retained assertion message, or null when the caller asks past the
// retained range.
const char* xls_standalone_aot_runtime_get_assert_message(
    const struct xls_standalone_aot_runtime* runtime, size_t index) {
  const std::string* message = runtime->events.GetAssertMessage(index);
  if (message == nullptr) {
    return nullptr;
  } else {
    return message->c_str();
  }
}

// Calls one metadata-matched direct-call artifact through the standalone ABI
// and reports the assertion count observed after that invocation.
int64_t xls_standalone_aot_entrypoint_trampoline(
    uintptr_t function_ptr, const uint8_t* const* inputs,
    uint8_t* const* outputs, void* temp_buffer,
    struct xls_standalone_aot_runtime* runtime, int64_t continuation_point,
    size_t* assert_messages_count_out) {
  StandaloneEntrypoint entrypoint =
      std::bit_cast<StandaloneEntrypoint>(function_ptr);
  int64_t continuation = entrypoint(
      inputs, outputs, temp_buffer, &runtime->events,
      reinterpret_cast<xls::InstanceContext*>(&runtime->vtable),
      /*jit_runtime=*/nullptr, continuation_point);
  *assert_messages_count_out = runtime->events.GetAssertMessageCount();
  return continuation;
}
