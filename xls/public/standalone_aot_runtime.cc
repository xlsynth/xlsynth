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
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <string>

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

// Records one assertion failure into runtime-owned storage.
void RecordAssertion(xls::InstanceContext* thiz, const char* msg,
                     xls::InterpreterEvents* events);

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

// Returns whether `value` is a valid power-of-two alignment.
bool IsPowerOfTwo(uint64_t value) {
  return value != 0 && (value & (value - 1)) == 0;
}

// Allocates generated-code scratch storage while preserving the requested
// alignment on both ordinary and over-aligned paths.
void* AllocateBuffer(xls::InstanceContext* thiz, int64_t byte_size,
                     int64_t alignment) {
  if (byte_size < 0 || alignment <= 0 ||
      !IsPowerOfTwo(static_cast<uint64_t>(alignment))) {
    std::abort();
  } else if (alignment <= alignof(std::max_align_t)) {
    void* buffer = std::malloc(byte_size);
    if (buffer == nullptr) {
      std::abort();
    }
    return buffer;
  } else {
    if (byte_size > std::numeric_limits<int64_t>::max() - alignment + 1) {
      std::abort();
    }
    const int64_t adjusted_size =
        ((byte_size + alignment - 1) / alignment) * alignment;
    void* buffer = std::aligned_alloc(alignment, adjusted_size);
    if (buffer == nullptr) {
      std::abort();
    }
    return buffer;
  }
}

// Releases storage previously returned by `AllocateBuffer`.
void DeallocateBuffer(xls::InstanceContext* thiz, void* buffer) {
  std::free(buffer);
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
            &RecordAssertion, &UnsupportedQueueReceiveWrapper,
            &UnsupportedQueueSendWrapper, &UnsupportedRecordActiveNextValue,
            &UnsupportedRecordNodeResult, &AllocateBuffer, &DeallocateBuffer,
            &UnsupportedRecordActiveRegisterWrite) {}
};

// Feature bits this first standalone runtime can actually execute.
constexpr uint32_t kSupportedFeatures =
    XLS_STANDALONE_AOT_RUNTIME_FEATURE_ASSERTIONS;

}  // namespace

// Runtime object whose first bytes are the callback table seen by generated
// code. The assertion vector owns copied message strings until clear/free.
struct xls_standalone_aot_runtime {
  StandaloneRuntimeVTable vtable;
  char** assert_messages;
  size_t assert_messages_count;
  size_t assert_messages_capacity;
};

namespace {

// Copies one assertion message into runtime-owned storage that remains valid
// until the next clear/free operation.
void RecordAssertion(xls::InstanceContext* thiz, const char* msg,
                     xls::InterpreterEvents* events) {
  auto* runtime = reinterpret_cast<xls_standalone_aot_runtime*>(thiz);
  if (runtime->assert_messages_count == runtime->assert_messages_capacity) {
    const size_t new_capacity =
        runtime->assert_messages_capacity == 0
            ? 4
            : runtime->assert_messages_capacity * 2;
    if (new_capacity < runtime->assert_messages_capacity ||
        new_capacity > std::numeric_limits<size_t>::max() / sizeof(char*)) {
      std::abort();
    }
    void* new_messages = std::realloc(
        runtime->assert_messages, new_capacity * sizeof(char*));
    if (new_messages == nullptr) {
      std::abort();
    }
    runtime->assert_messages = static_cast<char**>(new_messages);
    runtime->assert_messages_capacity = new_capacity;
  }

  const size_t message_size = std::strlen(msg) + 1;
  char* message_copy = static_cast<char*>(std::malloc(message_size));
  if (message_copy == nullptr) {
    std::abort();
  }
  std::memcpy(message_copy, msg, message_size);
  runtime->assert_messages[runtime->assert_messages_count] = message_copy;
  ++runtime->assert_messages_count;
}

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
    void* storage = std::malloc(sizeof(xls_standalone_aot_runtime));
    if (storage == nullptr) {
      *out = nullptr;
      return XLS_STANDALONE_AOT_RUNTIME_STATUS_ALLOCATION_FAILED;
    }
    auto* runtime = static_cast<xls_standalone_aot_runtime*>(storage);
    new (&runtime->vtable) StandaloneRuntimeVTable();
    runtime->assert_messages = nullptr;
    runtime->assert_messages_count = 0;
    runtime->assert_messages_capacity = 0;
    *out = runtime;
    return XLS_STANDALONE_AOT_RUNTIME_STATUS_OK;
  }
}

// Releases runtime-owned assertion storage and then the runtime object itself.
void xls_standalone_aot_runtime_free(
    struct xls_standalone_aot_runtime* runtime) {
  xls_standalone_aot_runtime_clear_events(runtime);
  std::free(runtime->assert_messages);
  runtime->vtable.~StandaloneRuntimeVTable();
  std::free(runtime);
}

// Clears event payloads without shrinking capacity so repeated calls can reuse
// one runtime allocation.
void xls_standalone_aot_runtime_clear_events(
    struct xls_standalone_aot_runtime* runtime) {
  for (size_t i = 0; i < runtime->assert_messages_count; ++i) {
    std::free(runtime->assert_messages[i]);
  }
  runtime->assert_messages_count = 0;
}

// Exposes the number of assertion messages currently retained by the runtime.
size_t xls_standalone_aot_runtime_get_assert_message_count(
    const struct xls_standalone_aot_runtime* runtime) {
  return runtime->assert_messages_count;
}

// Returns one retained assertion message, or null when the caller asks past the
// retained range.
const char* xls_standalone_aot_runtime_get_assert_message(
    const struct xls_standalone_aot_runtime* runtime, size_t index) {
  if (index >= runtime->assert_messages_count) {
    return nullptr;
  } else {
    return runtime->assert_messages[index];
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
  // Assertion-only artifacts need the callback table but do not require the
  // full InterpreterEvents object. Trace support is the expected future reason
  // to add a real event sink back to this standalone runtime.
  int64_t continuation = entrypoint(
      inputs, outputs, temp_buffer, /*events=*/nullptr,
      reinterpret_cast<xls::InstanceContext*>(&runtime->vtable),
      /*jit_runtime=*/nullptr, continuation_point);
  *assert_messages_count_out = runtime->assert_messages_count;
  return continuation;
}
