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

#include "xls/jit/generated_code_callbacks.h"

#include <cstdlib>

#include "absl/log/check.h"
#include "xls/common/math_util.h"
#include "xls/ir/events.h"

namespace xls::generated_code_callbacks {

void RecordAssertion(InstanceContext* thiz, const char* msg,
                     InterpreterEvents* events) {
  events->AddAssertMessage(msg);
}

void* AllocateBuffer(InstanceContext* thiz, int64_t byte_size,
                     int64_t alignment) {
  // The c11 std §7.22.3 states that std::aligned_alloc must be called with
  // size as a multiple of the alignment. The c17 standard removes this
  // simply saying that bad alignments fail. For all allocators except the ASAN
  // one the looser rules are followed and any relatively normal alignment llvm
  // supports is allowed for any size but asan specifically follows the older
  // spec. Overallocate to support this behavior.
  int64_t adj_size = RoundUpToNearest(byte_size, alignment);
  void* res = std::aligned_alloc(alignment, adj_size);
  CHECK(res != nullptr) << "Null alloc with " << byte_size
                        << " (adjusted to: " << adj_size
                        << ") aligned at: " << alignment;
  return res;
}

void DeallocateBuffer(InstanceContext* thiz, void* ptr) { free(ptr); }

}  // namespace xls::generated_code_callbacks
