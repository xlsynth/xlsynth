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

import os
from pathlib import Path
import tempfile
import unittest
from unittest import mock

import make_local_release


class MakeLocalReleaseTest(unittest.TestCase):
    def test_local_release_builds_and_packages_eval_proc_main(self):
        with tempfile.TemporaryDirectory() as temporary_dir:
            original_dir = os.getcwd()
            self.addCleanup(os.chdir, original_dir)
            os.chdir(temporary_dir)
            for target in make_local_release.MACOS_TARGETS:
                binary_path = Path("bazel-bin") / target[2:].replace(":", "/")
                binary_path.parent.mkdir(parents=True, exist_ok=True)
                binary_path.write_bytes(b"test executable")
                binary_path.chmod(0o755)

            with mock.patch.object(make_local_release.subprocess, "run") as build:
                with mock.patch.object(
                    make_local_release.subprocess, "check_output", return_value=b""
                ):
                    make_local_release.make_local_release(
                        "release", make_local_release.MACOS_TARGETS, run_tests=False
                    )

            self.assertIn("//xls/tools:eval_proc_main", build.call_args[0][0])
            executable = Path("release/eval_proc_main")
            self.assertEqual(executable.read_bytes(), b"test executable")
            self.assertTrue(os.access(executable, os.X_OK))
            os.chdir(original_dir)

    def test_workflow_builds_and_publishes_eval_proc_main_on_all_platforms(self):
        workflows = Path(__file__).resolve().parent / ".github/workflows"
        macos = (workflows / "build-release-macos.yml").read_text()
        release = (workflows / "build-and-release-dylib.yml").read_text()
        linux, publication = release.split("\n  release:\n", 1)
        rocky, ubuntu = linux.split("\n  build-ubuntu2004:\n", 1)
        for build, directory, suffix, checksum_command in (
            (macos, "osx-artifacts-arm64", "arm64", "shasum -a 256"),
            (rocky, "rocky8-artifacts", "rocky8", "sha256sum"),
            (ubuntu, "ubuntu2004-artifacts", "ubuntu2004", "sha256sum"),
        ):
            with self.subTest(platform=suffix):
                self.assertEqual(build.count("//xls/tools:eval_proc_main \\\n"), 1)
                asset = f"{directory}/eval_proc_main-{suffix}"
                self.assertRegex(
                    build, rf"cp bazel-bin/xls/tools/eval_proc_main\s+{asset}\n"
                )
                self.assertIn(
                    f"{checksum_command} {asset} > {asset}.sha256", build
                )
                self.assertIn(f"          {asset} \\\n", publication)
                self.assertIn(f"          {asset}.sha256 \\\n", publication)

    def test_parse_cli_args_accepts_release_target(self):
        options, output_dir = make_local_release.parse_cli_args(
            ["--release-target", "ubuntu2004", "/tmp/output"]
        )

        self.assertEqual(options.release_target.cli_name, "ubuntu2004")
        self.assertEqual(output_dir, "/tmp/output")

    def test_parse_cli_args_accepts_macos_release_target(self):
        options, _ = make_local_release.parse_cli_args(
            ["--release-target", "macos-arm64", "/tmp/output"]
        )

        self.assertEqual(options.release_target.cli_name, "macos-arm64")

    def test_parse_cli_args_rejects_legacy_macos_flag(self):
        with self.assertRaises(SystemExit):
            make_local_release.parse_cli_args(["--macos", "/tmp/output"])

    def test_parse_cli_args_rejects_unknown_release_target(self):
        with self.assertRaises(SystemExit):
            make_local_release.parse_cli_args(
                ["--release-target", "ubuntu2404", "/tmp/output"]
            )

    def test_parse_cli_args_accepts_release_target_mismatch_override(self):
        options, _ = make_local_release.parse_cli_args(
            [
                "--release-target",
                "ubuntu2004",
                "--allow-release-target-mismatch",
                "/tmp/output",
            ]
        )

        self.assertTrue(options.allow_release_target_mismatch)

    def test_parse_cli_args_rejects_release_target_mismatch_override_for_macos(self):
        with self.assertRaises(SystemExit):
            make_local_release.parse_cli_args(
                [
                    "--release-target",
                    "macos-arm64",
                    "--allow-release-target-mismatch",
                    "/tmp/output",
                ]
            )


if __name__ == "__main__":
    unittest.main()
