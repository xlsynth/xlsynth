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

import tarfile
import tempfile
from pathlib import Path
import unittest

import release_runtime_closure


class ReleaseRuntimeClosureTest(unittest.TestCase):
    def test_parse_ldd_output_collects_resolved_paths(self):
        output = """
            linux-vdso.so.1 (0x00007ffd7c7eb000)
            libc++.so.1 => /usr/lib/llvm-18/lib/libc++.so.1 (0x0000700000000000)
            libc.so.6 => /lib/x86_64-linux-gnu/libc.so.6 (0x0000700000001000)
            /lib64/ld-linux-x86-64.so.2 (0x0000700000002000)
        """

        resolved = release_runtime_closure.parse_ldd_output(output, Path("/tmp/libxls.so"))

        self.assertEqual(
            resolved,
            {
                "libc++.so.1": Path("/usr/lib/llvm-18/lib/libc++.so.1"),
                "libc.so.6": Path("/lib/x86_64-linux-gnu/libc.so.6"),
            },
        )

    def test_parse_ldd_output_rejects_missing_dependencies(self):
        output = "libc++.so.1 => not found\n"

        with self.assertRaises(RuntimeError):
            release_runtime_closure.parse_ldd_output(output, Path("/tmp/libxls.so"))

    def test_collect_runtime_closure_skips_system_path_dependencies(self):
        dependency_graph = {
            "/tmp/libxls.so": {
                "libc++.so.1": Path("/toolchain/lib/libc++.so.1"),
                "libstdc++.so.6": Path("/lib64/libstdc++.so.6"),
                "libm.so.6": Path("/lib/libm.so.6"),
            },
            "/toolchain/lib/libc++.so.1": {
                "libc++abi.so.1": Path("/toolchain/lib/libc++abi.so.1"),
                "libunwind.so.1": Path("/toolchain/lib/libunwind.so.1"),
                "libgcc_s.so.1": Path("/lib/libgcc_s.so.1"),
            },
            "/toolchain/lib/libc++abi.so.1": {},
            "/toolchain/lib/libunwind.so.1": {
                "libpthread.so.0": Path("/lib/libpthread.so.0"),
            },
        }

        closure = release_runtime_closure.collect_runtime_closure(
            Path("/tmp/libxls.so"),
            dependency_resolver = lambda path: dependency_graph[str(path)],
        )

        self.assertEqual(
            [path.name for path in closure],
            ["libc++.so.1", "libc++abi.so.1", "libunwind.so.1"],
        )

    def test_create_runtime_archive_is_deterministic(self):
        with tempfile.TemporaryDirectory() as tmp_dir:
            tmp_path = Path(tmp_dir)
            runtime_one = tmp_path / "libc++.so.1"
            runtime_two = tmp_path / "libc++abi.so.1"
            runtime_one.write_bytes(b"runtime-one")
            runtime_two.write_bytes(b"runtime-two")
            archive_path = tmp_path / "libxls-runtime-ubuntu2004.tar.gz"
            second_archive_path = tmp_path / "libxls-runtime-ubuntu2004-copy.tar.gz"

            release_runtime_closure.create_runtime_archive(
                archive_path,
                [runtime_two, runtime_one],
            )
            release_runtime_closure.create_runtime_archive(
                second_archive_path,
                [runtime_two, runtime_one],
            )

            with tarfile.open(archive_path, "r:gz") as archive:
                members = archive.getmembers()
                self.assertEqual(
                    [member.name for member in members],
                    ["libc++.so.1", "libc++abi.so.1"],
                )
                self.assertTrue(all(member.mtime == 0 for member in members))
            self.assertEqual(archive_path.read_bytes(), second_archive_path.read_bytes())


if __name__ == "__main__":
    unittest.main()
