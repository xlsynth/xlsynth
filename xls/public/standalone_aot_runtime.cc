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
#include <vector>

#include "xls/jit/generated_code_callback_abi.h"

struct xls_standalone_aot_runtime;

namespace {

// Native function shape emitted for function-only standalone AOT artifacts.
using StandaloneAotFunctionEntrypoint = int64_t (*)(
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

void RecordAssertion(xls::InstanceContext* thiz, const char* msg,
                     xls::InterpreterEvents* events);
void* AllocateBuffer(xls::InstanceContext* thiz, int64_t byte_size,
                     int64_t alignment);
void DeallocateBuffer(xls::InstanceContext* thiz, void* ptr);

// Callback table installed behind the ABI-visible prefix of the runtime object.
// Unsupported slots stay present so generated-code offsets remain ABI
// compatible, but they abort if a non-negotiated feature somehow reaches them.
struct StandaloneAotCallbackTable final : xls::InstanceContextVTable {
  StandaloneAotCallbackTable()
      : xls::InstanceContextVTable(
            &UnsupportedPerformStringStep, &UnsupportedPerformFormatStep,
            &UnsupportedRecordTrace, &UnsupportedCreateTraceBuffer,
            &RecordAssertion, &UnsupportedQueueReceiveWrapper,
            &UnsupportedQueueSendWrapper, &UnsupportedRecordActiveNextValue,
            &UnsupportedRecordNodeResult, &AllocateBuffer, &DeallocateBuffer,
            &UnsupportedRecordActiveRegisterWrite) {}
};

// Feature bits this first standalone runtime can actually execute.
constexpr uint32_t kSupportedFeatures =
    XLS_STANDALONE_AOT_RUNTIME_FEATURE_ASSERTIONS;

int64_t RoundUpToAlignment(int64_t byte_size, int64_t alignment) {
  int64_t remainder = byte_size % alignment;
  if (remainder == 0) {
    return byte_size;
  } else {
    return byte_size + alignment - remainder;
  }
}

}  // namespace

// Runtime object whose first bytes are the callback table seen by generated
// code. The standalone source bundle owns its assertion storage instead of
// depending on XLS event types that would pull a wider native closure into
// mixed Bazel final links.
struct xls_standalone_aot_runtime {
  StandaloneAotCallbackTable vtable;
  std::vector<std::string> assert_messages;
};

namespace {

xls_standalone_aot_runtime* RuntimeFromContext(xls::InstanceContext* thiz) {
  return reinterpret_cast<xls_standalone_aot_runtime*>(thiz);
}

void RecordAssertion(xls::InstanceContext* thiz, const char* msg,
                     xls::InterpreterEvents* events) {
  RuntimeFromContext(thiz)->assert_messages.push_back(msg);
}

void* AllocateBuffer(xls::InstanceContext* thiz, int64_t byte_size,
                     int64_t alignment) {
  int64_t adjusted_size = RoundUpToAlignment(byte_size, alignment);
  void* buffer = std::aligned_alloc(alignment, adjusted_size);
  if (buffer == nullptr) {
    std::abort();
  } else {
    return buffer;
  }
}

void DeallocateBuffer(xls::InstanceContext* thiz, void* ptr) { std::free(ptr); }

}  // namespace

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
    } else {
      *out = runtime;
      return XLS_STANDALONE_AOT_RUNTIME_STATUS_OK;
    }
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
  runtime->assert_messages.clear();
}

// Exposes the number of assertion messages currently retained by the runtime.
size_t xls_standalone_aot_runtime_get_assert_message_count(
    const struct xls_standalone_aot_runtime* runtime) {
  return runtime->assert_messages.size();
}

// Returns one retained assertion message, or null when the caller asks past the
// retained range.
const char* xls_standalone_aot_runtime_get_assert_message(
    const struct xls_standalone_aot_runtime* runtime, size_t index) {
  if (index >= runtime->assert_messages.size()) {
    return nullptr;
  } else {
    return runtime->assert_messages[index].c_str();
  }
}

// Calls one metadata-matched direct-call artifact through the standalone ABI
// and reports the assertion count observed after that invocation.
int64_t xls_standalone_aot_entrypoint_trampoline(
    uintptr_t function_ptr, const uint8_t* const* inputs,
    uint8_t* const* outputs, void* temp_buffer,
    struct xls_standalone_aot_runtime* runtime, int64_t continuation_point,
    size_t* assert_messages_count_out) {
  StandaloneAotFunctionEntrypoint entrypoint =
      std::bit_cast<StandaloneAotFunctionEntrypoint>(function_ptr);
  int64_t continuation = entrypoint(
      inputs, outputs, temp_buffer, /*events=*/nullptr,
      reinterpret_cast<xls::InstanceContext*>(&runtime->vtable),
      /*jit_runtime=*/nullptr, continuation_point);
  *assert_messages_count_out = runtime->assert_messages.size();
  return continuation;
}
