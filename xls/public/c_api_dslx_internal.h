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

#ifndef XLS_PUBLIC_C_API_DSLX_INTERNAL_H_
#define XLS_PUBLIC_C_API_DSLX_INTERNAL_H_

#include <filesystem>
#include <functional>
#include <memory>

#include "xls/public/c_api_dslx.h"

namespace xls::dslx {
class ParametricEnv;
class Span;
class ValueFormatDescriptor;
}  // namespace xls::dslx

namespace xls {

// Unwraps a C-owned or owner-backed parametric environment. A null C handle
// preserves the existing optional-environment calling convention.
const dslx::ParametricEnv* UnwrapDslxParametricEnv(
    const struct xls_dslx_parametric_env* env);

// Installs the existing importer observer for controlled C operation tests.
// Set it before starting concurrent operations on the import context.
void SetDslxImporterStackObserverForTesting(
    struct xls_dslx_import_data* import_data,
    std::function<void(const dslx::Span&, const std::filesystem::path&)>
        observer);

// Notifies a test immediately before metadata waits on its selected owner.
// Install before concurrent operations; the observer must not block.
void SetDslxMetadataLockObserverForTesting(
    struct xls_dslx_import_data* import_data, std::function<void()> observer);

// Observes metadata lifetime without retaining the value or its import context.
std::weak_ptr<const void> GetDslxValueMetadataForTesting(
    const struct xls_dslx_interp_value* value);

// Views an owned handle's descriptor without exposing it in the C ABI. The
// returned pointer is valid only while the handle remains alive.
const dslx::ValueFormatDescriptor* GetDslxValueFormatDescriptorForTesting(
    const struct xls_dslx_interp_value* value);

// Observes already-cached enum metadata under the owner's existing lock. This
// must not create a nominal entry or format descriptor for an uncached type.
std::weak_ptr<const void> GetDslxCachedEnumMetadataForTesting(
    struct xls_dslx_import_data* import_data,
    const struct xls_dslx_type* enum_type);

}  // namespace xls

#endif  // XLS_PUBLIC_C_API_DSLX_INTERNAL_H_
