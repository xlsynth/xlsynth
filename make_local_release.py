# Copyright 2025 The XLS Authors
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

import os
import shutil
import subprocess
import sys
import glob
import re

# List of Bazel targets to build
COMMON_TARGETS = [
    "//xls/dev_tools:check_ir_equivalence_main",
    "//xls/dslx:interpreter_main",
    "//xls/dslx/lsp:dslx_ls",
    "//xls/tools:opt_main",
    "//xls/tools:codegen_main",
    "//xls/dslx/ir_convert:ir_converter_main",
    "//xls/tools:delay_info_main",
    "//xls/dslx:dslx_fmt",
    "//xls/dslx:prove_quickcheck_main",
    "//xls/dslx/type_system:typecheck_main",
    "//xls/tools:block_to_verilog_main"
]

LINUX_TARGETS = COMMON_TARGETS + ["//xls/public:libxls.so"]
MACOS_TARGETS = COMMON_TARGETS + ["//xls/public:libxls.dylib"]

# Smoke test target to validate the C API symbols
SMOKE_TEST_TARGETS = [
    "//xls/public:test_c_api_symbols",
    "//xls/public:c_api_vast_test",
    "//xls/public:c_api_test",
    "//xls/dslx/type_system:typecheck_main"
]

CXX20_FLAGS = [
    "--cxxopt=-std=c++20",
    "--host_cxxopt=-std=c++20",
]

IMPORT_RE = re.compile(r'^\s*import\s+"([^"]+)";\s*$')
AOT_ENTRYPOINT_PROTO = "xls/jit/aot_entrypoint.proto"


def collect_proto_dependencies(proto_path):
    """Returns all local proto dependencies reachable from `proto_path`."""
    all_protos = set()
    stack = [proto_path]
    while stack:
        current = stack.pop()
        if current in all_protos:
            continue
        all_protos.add(current)
        with open(current, "r", encoding="utf-8") as f:
            for line in f:
                match = IMPORT_RE.match(line)
                if not match:
                    continue
                imported = match.group(1)
                # We only bundle local proto dependencies from this tree.
                if os.path.exists(imported):
                    stack.append(imported)
    return all_protos


def copy_flattened_protos(output_dir, root_proto):
    """Copies `root_proto` and local dependencies into output_dir/proto."""
    proto_paths = collect_proto_dependencies(root_proto)
    by_basename = {}
    for proto_path in sorted(proto_paths):
        basename = os.path.basename(proto_path)
        existing = by_basename.get(basename)
        if existing and existing != proto_path:
            raise ValueError(
                f"Cannot flatten protos: {existing} and {proto_path} share {basename}"
            )
        by_basename[basename] = proto_path

    proto_out_dir = os.path.join(output_dir, "proto")
    os.makedirs(proto_out_dir, exist_ok=True)
    for proto_path in sorted(proto_paths):
        destination = os.path.join(proto_out_dir, os.path.basename(proto_path))
        with open(proto_path, "r", encoding="utf-8") as src:
            content = src.read()
        # Rewrite local imports to the flattened import path.
        for dep_path in sorted(proto_paths):
            old_import = f'import "{dep_path}";'
            new_import = f'import "{os.path.basename(dep_path)}";'
            content = content.replace(old_import, new_import)
        with open(destination, "w", encoding="utf-8") as dst:
            dst.write(content)
        print(f"Copied {proto_path} to {destination}")


def get_release_targets(is_macos, override_targets=None):
    """Returns the Bazel targets to build for the requested release mode."""
    if override_targets:
        return list(override_targets)
    elif is_macos:
        return MACOS_TARGETS
    else:
        return LINUX_TARGETS


def get_run_tests_default(is_macos, override_targets=None):
    """Returns whether smoke tests should run for the requested build."""
    if is_macos:
        return False
    elif override_targets:
        return False
    else:
        return True


def get_default_compiler_env(environ=None, which=shutil.which):
    """Returns compiler env defaults when the generic clang names exist."""
    environment = dict(os.environ if environ is None else environ)
    if "CC" in environment or "CXX" in environment:
        return {}

    clang = which("clang")
    clangxx = which("clang++")
    if clang and clangxx:
        return {"CC": "clang", "CXX": "clang++"}
    else:
        return {}


def add_cxx20_flags(command):
    """Inserts the repo's C++20 flags after the Bazel mode selection."""
    return command[:4] + CXX20_FLAGS + command[4:]


def get_bazel_command(subcommand, mode, targets):
    """Returns the Bazel command for the requested subcommand and targets."""
    if mode == "dbg-asan":
        command = ["bazel", subcommand, "-c", "dbg", "--config=asan"]
    elif mode == "dbg":
        command = ["bazel", subcommand, "-c", "dbg"]
    elif mode == "opt":
        command = ["bazel", subcommand, "-c", "opt"]
    else:
        raise ValueError(f"Unsupported mode: {mode}")
    return add_cxx20_flags(command) + list(targets)

# Function to get the current git hash and cleanliness status
def get_git_info():
    try:
        git_hash = subprocess.check_output(["git", "rev-parse", "HEAD"]).decode().strip()
        git_status = subprocess.check_output(["git", "status", "--porcelain"]).decode().strip()
        clean_status = "clean" if not git_status else "dirty"
        return git_hash, clean_status
    except subprocess.CalledProcessError as e:
        print(f"Failed to retrieve git information: {e}")
        return "unknown", "unknown"

# Main function to handle the release process.
# Now accepts an optional `mode` argument to change the Bazel command.
def make_local_release(output_dir, targets, run_tests=True, mode="opt"):
    # Ensure the output directory exists and is writable
    if os.path.exists(output_dir):
        try:
            shutil.rmtree(output_dir)
        except PermissionError as e:
            print(f"Permission denied while removing existing directory: {e}")
            sys.exit(1)
    os.makedirs(output_dir, exist_ok=True)

    # Build the targets using Bazel; adjust flags based on the mode.
    # Include the smoke test targets in the build to ensure they compile with the rest.
    build_targets = list(targets)
    build_command = get_bazel_command("build", mode, build_targets)
    build_env = os.environ.copy()
    build_env.update(get_default_compiler_env(build_env))

    try:
        subprocess.run(build_command, check=True, env=build_env)
    except subprocess.CalledProcessError as e:
        print(f"Build failed: {e}")
        sys.exit(1)

    if run_tests:
        test_command = get_bazel_command("test", mode, SMOKE_TEST_TARGETS)

        try:
            subprocess.run(test_command, check=True, env=build_env)
        except subprocess.CalledProcessError as e:
            print(f"Smoke test failed: {e}")
            sys.exit(1)


    # Copy built binaries to the output directory
    for target in targets:
        # Convert Bazel target path to the corresponding path in bazel-bin
        binary_path = os.path.join(
            "bazel-bin", target.lstrip("//").replace(":", "/")
        )

        # Ensure the binary exists
        if not os.path.exists(binary_path):
            print(f"Binary not found: {binary_path}")
            continue

        # Copy the binary, overwriting if it exists
        destination_path = os.path.join(output_dir, os.path.basename(binary_path))
        if destination_path.endswith('/interpreter_main'):
            destination_path = destination_path.replace('/interpreter_main', '/dslx_interpreter_main')

        try:
            shutil.copy2(binary_path, destination_path)
            print(f"Copied {binary_path} to {destination_path}")
        except PermissionError as e:
            print(f"Permission denied while copying {binary_path}: {e}")
            sys.exit(1)

    # Copy the standard library files to the output directory in the same relpath locations
    # they were at in the source tree.
    stdlib_src_dir = 'xls/dslx/stdlib'
    stdlib_dst_dir = os.path.join(output_dir, stdlib_src_dir)
    os.makedirs(stdlib_dst_dir, exist_ok=True)
    for stdlib_path in glob.glob(os.path.join(stdlib_src_dir, '*.x')):
        rel_path = os.path.relpath(stdlib_path)
        dst_path = os.path.join(output_dir, rel_path)
        os.makedirs(os.path.dirname(dst_path), exist_ok=True)
        try:
            shutil.copy2(stdlib_path, dst_path)
        except FileNotFoundError as e:
            print(f"Standard library file not found {stdlib_path}: {e}")
            sys.exit(1)

    # Copy the AOT entrypoint proto and all local dependencies into a flat
    # proto directory for downstream consumers.
    try:
        copy_flattened_protos(output_dir, AOT_ENTRYPOINT_PROTO)
    except (FileNotFoundError, ValueError) as e:
        print(f"Failed to copy protos for release: {e}")
        sys.exit(1)

    # Write git information to a file
    git_hash, clean_status = get_git_info()
    git_info_path = os.path.join(output_dir, "git_info.txt")
    with open(git_info_path, "w") as f:
        f.write(f"Git Hash: {git_hash}\n")
        f.write(f"Repository Status: {clean_status}\n")
    print(f"Git information saved to {git_info_path}")

    # Produce a diff against the main branch, excluding MODULE.bazel.lock, and
    # write it to the release directory so that users can quickly see local
    # changes relative to upstream.
    diff_path = os.path.join(output_dir, "diff_vs_main.patch")
    try:
        diff_bytes = subprocess.check_output(
            [
                "git",
                "diff",
                "main",
                "--",
                ":(exclude)MODULE.bazel.lock",
                ":(exclude)make_local_release.py",
                ":(exclude).github/workflows/build-and-release-dylib.yml",
            ]
        )
        with open(diff_path, "wb") as f:
            f.write(diff_bytes)
        print(f"Diff against main written to {diff_path}")
    except subprocess.CalledProcessError as e:
        print(f"Failed to generate diff against main: {e}")

# New main() function using optparse
def main():
    from optparse import OptionParser

    usage = "usage: %prog [options] output_directory"
    parser = OptionParser(usage=usage)
    parser.add_option("-m", "--mode", dest="mode", default="opt", choices=["opt", "dbg-asan", "dbg"],
                      help="Build mode: opt (default) or dbg-asan")
    parser.add_option("--macos", action="store_true", default=False, help="Build for macOS")
    parser.add_option(
        "-t",
        "--target",
        action="append",
        dest="targets",
        default=[],
        help=(
            "Override the default release target set. May be repeated. When "
            "used, smoke tests are skipped."
        ),
    )
    options, args = parser.parse_args()

    if len(args) != 1:
        parser.error("You must specify exactly one output directory")
    output_dir = args[0]

    targets = get_release_targets(options.macos, options.targets)
    run_tests = get_run_tests_default(options.macos, options.targets)
    make_local_release(output_dir, targets, run_tests=run_tests, mode=options.mode)

if __name__ == "__main__":
    main()
