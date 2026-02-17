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

import unittest

import make_local_release


class MakeLocalReleaseTest(unittest.TestCase):
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
