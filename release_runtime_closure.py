#!/usr/bin/env python3

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

"""Packages the non-system Linux runtime closure for a released libxls shared library."""

import argparse
import gzip
import io
import json
import os
from pathlib import Path
import subprocess
import tarfile

_SYSTEM_LIBRARY_PREFIXES = (
    "/lib/",
    "/lib64/",
    "/usr/lib64/",
    "/usr/lib/x86_64-linux-gnu/",
)


def build_runtime_archive_filename(platform):
    return f"libxls-runtime-{platform}.tar.gz"


def build_runtime_manifest_filename(platform):
    return f"libxls-runtime-{platform}-manifest.json"


def detect_linux_release_platform(os_release_path = Path("/etc/os-release")):
    if not os_release_path.exists():
        return "ubuntu2004"
    contents = os_release_path.read_text(encoding = "utf-8").lower()
    if any(marker in contents for marker in ["rocky", "rhel", "almalinux", "centos"]):
        return "rocky8"
    return "ubuntu2004"


def run_text_command(args):
    return subprocess.run(
        args,
        check = False,
        capture_output = True,
        text = True,
    )


def parse_ldd_output(stdout, subject_path):
    resolved = {}
    missing = []
    for raw_line in stdout.splitlines():
        line = raw_line.strip()
        if not line or "=>" not in line:
            continue
        name, remainder = [piece.strip() for piece in line.split("=>", 1)]
        target = remainder.split("(", 1)[0].strip()
        if target == "not found":
            missing.append(name)
        elif target:
            resolved[name] = Path(target)
    if missing:
        raise RuntimeError(
            "Missing runtime dependencies for {}: {}".format(
                subject_path,
                ", ".join(sorted(missing)),
            )
        )
    return resolved


def resolve_direct_dependencies(shared_library_path):
    result = run_text_command(["ldd", str(shared_library_path)])
    if result.returncode != 0:
        raise RuntimeError(
            "Failed to inspect dynamic dependencies for {}\nstdout:\n{}\nstderr:\n{}".format(
                shared_library_path,
                result.stdout,
                result.stderr,
            )
        )
    return parse_ldd_output(result.stdout, shared_library_path)


def should_bundle_runtime_dependency(dependency_path):
    normalized_path = os.path.realpath(str(dependency_path))
    return not any(
        normalized_path == prefix[:-1] or normalized_path.startswith(prefix)
        for prefix in _SYSTEM_LIBRARY_PREFIXES
    )


def collect_runtime_closure(primary_dso_path, dependency_resolver = resolve_direct_dependencies):
    primary_path = Path(primary_dso_path)
    pending = [primary_path]
    bundled = {}
    visited = set()

    while pending:
        current = pending.pop()
        current_key = str(current.resolve())
        if current_key in visited:
            continue
        visited.add(current_key)

        for dependency_name, dependency_path in dependency_resolver(current).items():
            if not should_bundle_runtime_dependency(dependency_path):
                continue
            bundled_path = bundled.get(dependency_name)
            if bundled_path is not None:
                if bundled_path.resolve() != dependency_path.resolve():
                    raise RuntimeError(
                        "Dependency {} resolved to two different paths: {} and {}".format(
                            dependency_name,
                            bundled_path,
                            dependency_path,
                        )
                    )
                continue
            bundled[dependency_name] = dependency_path
            pending.append(dependency_path)

    return [bundled[name] for name in sorted(bundled)]


def build_runtime_manifest(primary_dso_name, runtime_files):
    return {
        "primary_dso": primary_dso_name,
        "runtime_files": [path.name for path in runtime_files],
    }


def add_file_to_archive(archive, source_path):
    resolved_path = source_path.resolve()
    tar_info = tarfile.TarInfo(name = source_path.name)
    tar_info.mode = 0o644
    tar_info.mtime = 0
    tar_info.uid = 0
    tar_info.gid = 0
    tar_info.uname = ""
    tar_info.gname = ""
    file_bytes = resolved_path.read_bytes()
    tar_info.size = len(file_bytes)
    archive.addfile(tar_info, io.BytesIO(file_bytes))


def create_runtime_archive(archive_path, runtime_files):
    with open(archive_path, "wb") as raw_file:
        with gzip.GzipFile(filename = "", mode = "wb", fileobj = raw_file, mtime = 0) as gzip_file:
            with tarfile.open(fileobj = gzip_file, mode = "w", format = tarfile.PAX_FORMAT) as archive:
                for runtime_file in sorted(runtime_files, key = lambda path: path.name):
                    add_file_to_archive(archive, runtime_file)


def write_runtime_manifest(manifest_path, manifest):
    manifest_path.write_text(
        json.dumps(manifest, indent = 2, sort_keys = True) + "\n",
        encoding = "utf-8",
    )


def package_runtime_closure(primary_dso_path, output_dir, platform):
    primary_path = Path(primary_dso_path)
    target_dir = Path(output_dir)
    target_dir.mkdir(parents = True, exist_ok = True)

    runtime_files = collect_runtime_closure(primary_path)
    manifest = build_runtime_manifest(primary_path.name, runtime_files)

    archive_path = target_dir / build_runtime_archive_filename(platform)
    manifest_path = target_dir / build_runtime_manifest_filename(platform)

    create_runtime_archive(archive_path, runtime_files)
    write_runtime_manifest(manifest_path, manifest)
    return {
        "archive_path": archive_path,
        "manifest_path": manifest_path,
        "runtime_files": runtime_files,
    }


def parse_args(argv):
    parser = argparse.ArgumentParser()
    parser.add_argument("--libxls", required = True)
    parser.add_argument("--output-dir", required = True)
    parser.add_argument("--platform", required = True)
    return parser.parse_args(argv)


def main(argv):
    args = parse_args(argv)
    result = package_runtime_closure(
        primary_dso_path = args.libxls,
        output_dir = args.output_dir,
        platform = args.platform,
    )
    print("Packaged runtime closure:")
    print("  archive: {}".format(result["archive_path"]))
    print("  manifest: {}".format(result["manifest_path"]))
    print("  runtime files: {}".format(", ".join(path.name for path in result["runtime_files"])))


if __name__ == "__main__":
    import sys

    main(sys.argv[1:])
