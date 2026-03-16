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

"""Tests for the local release helper's target and toolchain selection."""

import unittest
from unittest import mock

import make_local_release


class MakeLocalReleaseTest(unittest.TestCase):
  """Tests for `make_local_release.py` pure selection logic."""

  def test_get_release_targets_defaults_to_linux_targets(self):
    self.assertEqual(
        make_local_release.get_release_targets(False),
        make_local_release.LINUX_TARGETS,
    )

  def test_get_release_targets_defaults_to_macos_targets(self):
    self.assertEqual(
        make_local_release.get_release_targets(True),
        make_local_release.MACOS_TARGETS,
    )

  def test_get_release_targets_uses_override_targets(self):
    override_targets = ["//xls/dslx:interpreter_main"]
    self.assertEqual(
        make_local_release.get_release_targets(False, override_targets),
        override_targets,
    )

  def test_get_run_tests_default_uses_linux_release_defaults(self):
    self.assertTrue(make_local_release.get_run_tests_default(False))

  def test_get_run_tests_default_skips_macos_tests(self):
    self.assertFalse(make_local_release.get_run_tests_default(True))

  def test_get_run_tests_default_skips_tests_for_override_targets(self):
    self.assertFalse(
        make_local_release.get_run_tests_default(
            False, ["//xls/dslx:interpreter_main"]
        )
    )

  def test_get_default_compiler_env_uses_generic_clang_when_available(self):
    which = mock.Mock(side_effect=["/usr/bin/clang", "/usr/bin/clang++"])
    self.assertEqual(
        make_local_release.get_default_compiler_env({}, which=which),
        {"CC": "clang", "CXX": "clang++"},
    )

  def test_get_default_compiler_env_respects_existing_env(self):
    which = mock.Mock(side_effect=AssertionError("which should not be called"))
    self.assertEqual(
        make_local_release.get_default_compiler_env(
            {"CC": "clang-18", "CXX": "clang++-18"}, which=which
        ),
        {},
    )

  def test_get_default_compiler_env_skips_missing_generic_clang(self):
    which = mock.Mock(side_effect=[None, None])
    self.assertEqual(
        make_local_release.get_default_compiler_env({}, which=which),
        {},
    )

  def test_get_bazel_command_keeps_cxx20_flags(self):
    self.assertEqual(
        make_local_release.get_bazel_command(
            "build", "opt", ["//xls/dslx:interpreter_main"]
        ),
        [
            "bazel",
            "build",
            "-c",
            "opt",
            "--cxxopt=-std=c++20",
            "--host_cxxopt=-std=c++20",
            "//xls/dslx:interpreter_main",
        ],
    )


if __name__ == "__main__":
  unittest.main()
