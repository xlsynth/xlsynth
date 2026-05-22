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

import hashlib
from pathlib import Path
import tempfile
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

    def test_write_sha256sum_uses_release_asset_basename(self):
        with tempfile.TemporaryDirectory() as tmp_dir:
            archive = Path(tmp_dir) / "xls-aot-runtime-source.tar.gz"
            archive.write_bytes(b"runtime-source")

            checksum_path = Path(make_local_release.write_sha256sum(str(archive)))

            self.assertEqual(
                checksum_path.read_text(encoding="utf-8"),
                "{}  xls-aot-runtime-source.tar.gz\n".format(
                    hashlib.sha256(b"runtime-source").hexdigest()
                ),
            )

    def test_release_workflow_uploads_runtime_source_asset(self):
        workflow = (
            Path(__file__).parent
            / ".github"
            / "workflows"
            / "build-and-release-dylib.yml"
        ).read_text(encoding="utf-8")

        self.assertIn("//xls/public:standalone_aot_runtime_source_archive", workflow)
        self.assertIn("ubuntu2004-artifacts/xls-aot-runtime-source.tar.gz", workflow)
        self.assertIn(
            "ubuntu2004-artifacts/xls-aot-runtime-source.tar.gz.sha256",
            workflow,
        )


if __name__ == "__main__":
    unittest.main()
