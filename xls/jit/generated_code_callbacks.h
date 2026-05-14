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

#ifndef XLS_JIT_GENERATED_CODE_CALLBACKS_H_
#define XLS_JIT_GENERATED_CODE_CALLBACKS_H_

#include <cstdint>

namespace xls {

class InterpreterEvents;
struct InstanceContext;

namespace generated_code_callbacks {

// Shared assertion callback used by every XLS runtime that executes the
// generated-code callback ABI.
void RecordAssertion(InstanceContext* thiz, const char* msg,
                     InterpreterEvents* events);

// Shared generated-code scratch allocator used by every XLS runtime that
// executes the generated-code callback ABI.
void* AllocateBuffer(InstanceContext* thiz, int64_t byte_size,
                     int64_t alignment);

// Releases storage returned by `AllocateBuffer`.
void DeallocateBuffer(InstanceContext* thiz, void* ptr);

}  // namespace generated_code_callbacks
}  // namespace xls

#endif  // XLS_JIT_GENERATED_CODE_CALLBACKS_H_
