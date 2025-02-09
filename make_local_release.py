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
    "//xls/dslx/type_system:typecheck_main"
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

# Main function to handle the release process
def make_local_release(output_dir):
    # Ensure the output directory exists and is writable
    if os.path.exists(output_dir):
        try:
            shutil.rmtree(output_dir)
        except PermissionError as e:
            print(f"Permission denied while removing existing directory: {e}")
            sys.exit(1)
    os.makedirs(output_dir, exist_ok=True)

    # Build the targets using Bazel
    build_command = [
        "CC=clang", "CXX=clang++", "bazel", "build", "-c", "opt"
    ] + targets

    try:
        subprocess.run(" ".join(build_command), shell=True, check=True)
    except subprocess.CalledProcessError as e:
        print(f"Build failed: {e}")
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

    # Write git information to a file
    git_hash, clean_status = get_git_info()
    git_info_path = os.path.join(output_dir, "git_info.txt")
    with open(git_info_path, "w") as f:
        f.write(f"Git Hash: {git_hash}\n")
        f.write(f"Repository Status: {clean_status}\n")
    print(f"Git information saved to {git_info_path}")

if __name__ == "__main__":
    if len(sys.argv) != 2:
        print("Usage: python3 make_local_release.py <output_directory>")
        sys.exit(1)

    output_directory = sys.argv[1]
    make_local_release(output_directory)

