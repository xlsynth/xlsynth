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

# List of Bazel targets to build
targets = [
    "//xls/public:libxls.so",
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

# Smoke test target to validate the C API symbols
smoke_test_targets = [
    "//xls/public:test_c_api_symbols",
    "//xls/public:c_api_vast_test",
    "//xls/public:c_api_test",
]

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
def make_local_release(output_dir, mode="opt"):
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
    build_targets = targets + smoke_test_targets
    if mode == "dbg-asan":
        build_command = [
            "CC=clang", "CXX=clang++", "bazel", "build", "-c", "dbg", "--config=asan"
        ] + build_targets
    elif mode == "dbg":
        build_command = [
            "CC=clang", "CXX=clang++", "bazel", "build", "-c", "dbg"
        ] + build_targets
    elif mode == "opt":
        build_command = [
            "CC=clang", "CXX=clang++", "bazel", "build", "-c", "opt"
        ] + build_targets
    else:
        print(f"Unsupported mode: {mode}")
        sys.exit(1)

    try:
        subprocess.run(" ".join(build_command), shell=True, check=True)
    except subprocess.CalledProcessError as e:
        print(f"Build failed: {e}")
        sys.exit(1)

    # Run smoke tests with the same compilation mode.
    if mode == "dbg-asan":
        test_command = [
            "CC=clang", "CXX=clang++", "bazel", "test", "-c", "dbg", "--config=asan",
        ] + smoke_test_targets
    elif mode == "dbg":
        test_command = [
            "CC=clang", "CXX=clang++", "bazel", "test", "-c", "dbg",
        ] + smoke_test_targets
    elif mode == "opt":
        test_command = [
            "CC=clang", "CXX=clang++", "bazel", "test", "-c", "opt",
        ] + smoke_test_targets
    try:
        subprocess.run(" ".join(test_command), shell=True, check=True)
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
    options, args = parser.parse_args()

    if len(args) != 1:
        parser.error("You must specify exactly one output directory")
    output_dir = args[0]

    make_local_release(output_dir, mode=options.mode)

if __name__ == "__main__":
    main()
