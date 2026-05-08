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

#ifndef XLS_PUBLIC_STANDALONE_AOT_RUNTIME_H_
#define XLS_PUBLIC_STANDALONE_AOT_RUNTIME_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Opaque owner of the assertion-only runtime state used by standalone AOT
// function artifacts.
struct xls_standalone_aot_runtime;

// Version of the standalone runtime ABI understood by this static artifact.
//
// Generated artifacts and runtimes are intentionally same-version only: callers
// should rebuild artifacts whenever related compiler/runtime code changes
// rather than attempting cross-version execution.
#define XLS_STANDALONE_AOT_ABI_VERSION 1u

// Feature bits that generated standalone artifacts declare at runtime creation.
//
// Traces are intentionally reserved in the first landing even though the
// runtime rejects them today. Current trace lowering needs value-formatting
// support that remains part of the full JIT runtime; keeping the feature bit in
// the ABI now gives the later trace implementation an explicit extension path
// instead of forcing another ownership redesign.
enum xls_standalone_aot_runtime_feature {
  // The artifact may emit assertion callbacks that this runtime records.
  XLS_STANDALONE_AOT_RUNTIME_FEATURE_ASSERTIONS = 1u << 0,
  // Reserved extension bit for a future trace-capable standalone runtime.
  XLS_STANDALONE_AOT_RUNTIME_FEATURE_TRACES = 1u << 1,
};

// Result codes for constructing a standalone AOT runtime context.
enum xls_standalone_aot_runtime_status {
  // Construction succeeded and `out` owns a valid runtime context.
  XLS_STANDALONE_AOT_RUNTIME_STATUS_OK = 0,
  // The artifact/runtime ABI versions differ and must not be mixed.
  XLS_STANDALONE_AOT_RUNTIME_STATUS_UNSUPPORTED_ABI_VERSION = 1,
  // The artifact requested a feature that this runtime does not implement.
  XLS_STANDALONE_AOT_RUNTIME_STATUS_UNSUPPORTED_FEATURE = 2,
  // Runtime-owned storage could not be allocated.
  XLS_STANDALONE_AOT_RUNTIME_STATUS_ALLOCATION_FAILED = 3,
};

// Creates a function-only standalone AOT runtime context.
//
// The first landing accepts assertion-bearing artifacts and rejects artifacts
// that require trace support. The returned object is owned by the caller and
// must be freed with `xls_standalone_aot_runtime_free`. `out` must be non-null;
// passing a null output slot is a caller bug because creation always writes the
// result there, including on failure.
enum xls_standalone_aot_runtime_status xls_standalone_aot_runtime_create(
    uint32_t abi_version, uint32_t required_features,
    struct xls_standalone_aot_runtime** out);

// Releases a runtime context returned by `xls_standalone_aot_runtime_create`.
//
// This also releases all recorded assertion messages. Callers must not pass
// null or use the runtime again afterward; doing so would dereference freed
// storage in the C implementation.
void xls_standalone_aot_runtime_free(
    struct xls_standalone_aot_runtime* runtime);

// Drops all assertion messages while keeping the runtime reusable.
//
// Message pointers previously returned by
// `xls_standalone_aot_runtime_get_assert_message` become invalid immediately
// after this call.
void xls_standalone_aot_runtime_clear_events(
    struct xls_standalone_aot_runtime* runtime);

// Returns the number of assertion messages recorded since the last clear.
//
// `runtime` must be a live runtime returned by `create`; passing null or a
// freed runtime would dereference invalid storage.
size_t xls_standalone_aot_runtime_get_assert_message_count(
    const struct xls_standalone_aot_runtime* runtime);

// Returns an assertion message owned by `runtime`, or `NULL` when `index` is
// out of range. The returned pointer stays valid until events are cleared or
// the runtime is freed. `runtime` must remain live for the whole use of the
// returned pointer.
const char* xls_standalone_aot_runtime_get_assert_message(
    const struct xls_standalone_aot_runtime* runtime, size_t index);

// Invokes a function-only AOT entrypoint through the XLS-owned standalone
// runtime. The function pointer must address an unpacked XLS AOT function
// entrypoint produced for the same ABI version.
//
// The runtime borrows `inputs`, `outputs`, and `temp_buffer`; callers keep
// ownership of all three. Passing a pointer for another signature or ABI would
// make the generated code interpret the argument layout incorrectly, so only
// metadata-matched direct-call artifacts may use this trampoline.
// `assert_messages_count_out` must be non-null so the caller can observe the
// post-call assertion count.
int64_t xls_standalone_aot_entrypoint_trampoline(
    uintptr_t function_ptr, const uint8_t* const* inputs,
    uint8_t* const* outputs, void* temp_buffer,
    struct xls_standalone_aot_runtime* runtime, int64_t continuation_point,
    size_t* assert_messages_count_out);

#ifdef __cplusplus
}
#endif

#endif  // XLS_PUBLIC_STANDALONE_AOT_RUNTIME_H_
