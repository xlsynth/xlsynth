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

#include <cstddef>
#include <cstdint>
#include <string>

#include "gtest/gtest.h"
#include "xls/jit/jit_callback_abi.h"

namespace {

int64_t FakeAssertEntrypoint(const uint8_t* const* inputs,
                             uint8_t* const* outputs, void* temp_buffer,
                             xls::InterpreterEvents* events,
                             xls::InstanceContext* instance_context,
                             xls::JitRuntime* jit_runtime,
                             int64_t continuation_point) {
  outputs[0][0] = inputs[0][0] + 1;
  auto* vtable = reinterpret_cast<xls::InstanceContextVTable*>(instance_context);
  vtable->record_assertion(instance_context, "boom", events);
  return 0;
}

int64_t FakeAlignedAllocationEntrypoint(
    const uint8_t* const* inputs, uint8_t* const* outputs, void* temp_buffer,
    xls::InterpreterEvents* events, xls::InstanceContext* instance_context,
    xls::JitRuntime* jit_runtime, int64_t continuation_point) {
  auto* vtable = reinterpret_cast<xls::InstanceContextVTable*>(instance_context);
  void* buffer = vtable->allocate_buffer(instance_context, /*byte_size=*/17,
                                         /*alignment=*/64);
  outputs[0][0] = reinterpret_cast<uintptr_t>(buffer) % 64 == 0 ? 1 : 0;
  vtable->deallocate_buffer(instance_context, buffer);
  return 0;
}

TEST(XlsStandaloneAotRuntimeTest, RunsDirectFunctionAndCollectsAssertions) {
  xls_standalone_aot_runtime* runtime = nullptr;
  ASSERT_EQ(
      xls_standalone_aot_runtime_create(
          XLS_STANDALONE_AOT_ABI_VERSION,
          XLS_STANDALONE_AOT_RUNTIME_FEATURE_ASSERTIONS, &runtime),
      XLS_STANDALONE_AOT_RUNTIME_STATUS_OK);
  ASSERT_NE(runtime, nullptr);

  uint8_t input = 41;
  uint8_t output = 0;
  const uint8_t* inputs[1] = {&input};
  uint8_t* outputs[1] = {&output};
  uint8_t temp_buffer[1] = {0};
  size_t assert_messages_count = 0;

  EXPECT_EQ(xls_standalone_aot_entrypoint_trampoline(
                reinterpret_cast<uintptr_t>(&FakeAssertEntrypoint), inputs,
                outputs, temp_buffer, runtime, /*continuation_point=*/0,
                &assert_messages_count),
            0);
  EXPECT_EQ(output, 42);
  EXPECT_EQ(assert_messages_count, 1);
  EXPECT_EQ(xls_standalone_aot_runtime_get_assert_message_count(runtime), 1);
  EXPECT_EQ(
      std::string{xls_standalone_aot_runtime_get_assert_message(runtime, 0)},
      "boom");

  xls_standalone_aot_runtime_clear_events(runtime);
  EXPECT_EQ(xls_standalone_aot_runtime_get_assert_message_count(runtime), 0);
  xls_standalone_aot_runtime_free(runtime);
}

TEST(XlsStandaloneAotRuntimeTest, PreservesRequestedHeapAlignment) {
  xls_standalone_aot_runtime* runtime = nullptr;
  ASSERT_EQ(
      xls_standalone_aot_runtime_create(XLS_STANDALONE_AOT_ABI_VERSION,
                                        /*required_features=*/0, &runtime),
      XLS_STANDALONE_AOT_RUNTIME_STATUS_OK);
  ASSERT_NE(runtime, nullptr);

  uint8_t input = 0;
  uint8_t output = 0;
  const uint8_t* inputs[1] = {&input};
  uint8_t* outputs[1] = {&output};
  uint8_t temp_buffer[1] = {0};
  size_t assert_messages_count = 0;

  EXPECT_EQ(
      xls_standalone_aot_entrypoint_trampoline(
          reinterpret_cast<uintptr_t>(&FakeAlignedAllocationEntrypoint), inputs,
          outputs, temp_buffer, runtime, /*continuation_point=*/0,
          &assert_messages_count),
      0);
  EXPECT_EQ(output, 1);
  EXPECT_EQ(assert_messages_count, 0);

  xls_standalone_aot_runtime_free(runtime);
}

TEST(XlsStandaloneAotRuntimeTest, RejectsFutureTraceRequirementForNow) {
  xls_standalone_aot_runtime* runtime = nullptr;
  EXPECT_EQ(xls_standalone_aot_runtime_create(
                XLS_STANDALONE_AOT_ABI_VERSION,
                XLS_STANDALONE_AOT_RUNTIME_FEATURE_TRACES, &runtime),
            XLS_STANDALONE_AOT_RUNTIME_STATUS_UNSUPPORTED_FEATURE);
  EXPECT_EQ(runtime, nullptr);
}

TEST(XlsStandaloneAotRuntimeTest, RejectsUnknownAbiVersion) {
  xls_standalone_aot_runtime* runtime = nullptr;
  EXPECT_EQ(xls_standalone_aot_runtime_create(
                XLS_STANDALONE_AOT_ABI_VERSION + 1,
                XLS_STANDALONE_AOT_RUNTIME_FEATURE_ASSERTIONS, &runtime),
            XLS_STANDALONE_AOT_RUNTIME_STATUS_UNSUPPORTED_ABI_VERSION);
  EXPECT_EQ(runtime, nullptr);
}

}  // namespace
