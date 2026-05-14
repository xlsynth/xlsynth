# Copyright 2026 The XLS Authors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Rules for static archives that own their full native dependency closure."""

load("@bazel_tools//tools/cpp:toolchain_utils.bzl", "find_cpp_toolchain", "use_cpp_toolchain")
load("@rules_cc//cc/common:cc_common.bzl", "cc_common")
load("@rules_cc//cc/common:cc_info.bzl", "CcInfo")

def _append_unique(files, seen_paths, file):
    if file.path not in seen_paths:
        files.append(file)
        seen_paths[file.path] = True

def _collect_bundle_inputs(compilation_outputs, dep_cc_infos):
    """Returns the object/archive files that must live inside the bundle."""
    files = []
    seen_paths = {}

    for object_file in compilation_outputs.pic_objects:
        _append_unique(files, seen_paths, object_file)
    for object_file in compilation_outputs.objects:
        _append_unique(files, seen_paths, object_file)

    for dep_cc_info in dep_cc_infos:
        for linker_input in dep_cc_info.linking_context.linker_inputs.to_list():
            for library in linker_input.libraries:
                if library.pic_static_library != None:
                    _append_unique(files, seen_paths, library.pic_static_library)
                elif library.static_library != None:
                    _append_unique(files, seen_paths, library.static_library)
                else:
                    fail(
                        "xls_static_library_bundle only accepts static C++ dependencies; " +
                        "found dependency without a static archive from %s" % linker_input.owner,
                    )
    return files

def _xls_static_library_bundle_impl(ctx):
    """Builds one static archive from local sources and dependency archives."""
    cc_toolchain = find_cpp_toolchain(ctx)
    feature_configuration = cc_common.configure_features(
        ctx = ctx,
        cc_toolchain = cc_toolchain,
        requested_features = ctx.features,
        unsupported_features = ctx.disabled_features,
    )

    dep_cc_infos = [dep[CcInfo] for dep in ctx.attr.deps]
    (compilation_context, compilation_outputs) = cc_common.compile(
        name = ctx.label.name,
        actions = ctx.actions,
        feature_configuration = feature_configuration,
        cc_toolchain = cc_toolchain,
        srcs = ctx.files.srcs,
        public_hdrs = ctx.files.hdrs,
        compilation_contexts = [dep.compilation_context for dep in dep_cc_infos],
    )
    archive = ctx.actions.declare_file("lib%s.a" % ctx.label.name)
    bundle_inputs = _collect_bundle_inputs(compilation_outputs, dep_cc_infos)
    ctx.actions.run(
        executable = ctx.executable._bundle_tool,
        inputs = bundle_inputs,
        outputs = [archive],
        arguments = [archive.path] + [file.path for file in bundle_inputs],
        mnemonic = "BundleStaticLibrary",
        progress_message = "Bundling static library %s" % archive.short_path,
    )

    library_to_link = cc_common.create_library_to_link(
        actions = ctx.actions,
        feature_configuration = feature_configuration,
        cc_toolchain = cc_toolchain,
        static_library = archive,
        pic_static_library = archive,
    )
    linker_input = cc_common.create_linker_input(
        owner = ctx.label,
        libraries = depset([library_to_link]),
    )
    linking_context = cc_common.create_linking_context(
        linker_inputs = depset([linker_input]),
    )

    return [
        CcInfo(
            compilation_context = compilation_context,
            linking_context = linking_context,
        ),
        DefaultInfo(files = depset([archive])),
    ]

xls_static_library_bundle = rule(
    implementation = _xls_static_library_bundle_impl,
    doc = "Builds one static archive that owns the C++ closure of its dependencies.",
    attrs = {
        "srcs": attr.label_list(allow_files = [".cc"]),
        "hdrs": attr.label_list(allow_files = [".h"]),
        "deps": attr.label_list(providers = [CcInfo]),
        "_bundle_tool": attr.label(
            default = Label("//xls/build_rules:bundle_static_library"),
            executable = True,
            cfg = "exec",
        ),
    },
    fragments = ["cpp"],
    provides = [CcInfo],
    toolchains = use_cpp_toolchain(),
)
