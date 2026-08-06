#
# Copyright 2020 The XLS Authors
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
"""Tests for xls.fuzzer.run_fuzz_multiprocess."""

import os
import subprocess

from xls.common import runfiles
from xls.common import test_base

RUN_FUZZ_MULTIPROCESS_PATH = runfiles.get_path(
    'xls/fuzzer/run_fuzz_multiprocess'
)
SEED_MANIFEST_PATH = runfiles.get_path(
    'xls/fuzzer/testdata/semantic_sum_phase1/manifest.textproto'
)


class RunFuzzMultiprocessTest(test_base.TestCase):

  def test_two_samples(self):
    crasher_path = self.create_tempdir().full_path
    samples_path = self.create_tempdir().full_path

    subprocess.check_call([
        RUN_FUZZ_MULTIPROCESS_PATH,
        '--seed=42',
        '--crash_path=' + crasher_path,
        '--save_temps_path=' + samples_path,
        '--sample_count=2',
        '--calls_per_sample=3',
        '--worker_count=1',
    ])
    # Crasher path should contain a single file 'test'.
    self.assertSequenceEqual(os.listdir(crasher_path), ('test',))

    # Sample path should have two samples in it.
    self.assertSequenceEqual(
        sorted(os.listdir(samples_path)), ('worker0-sample0', 'worker0-sample1')
    )

    # Validate sample 1 directory.
    sample1_contents = os.listdir(os.path.join(samples_path, 'worker0-sample0'))
    self.assertIn('sample.x', sample1_contents)
    self.assertIn('sample.x.results', sample1_contents)
    self.assertIn('testvector.pbtxt', sample1_contents)
    self.assertIn('sample.ir', sample1_contents)
    self.assertIn('sample.ir.results', sample1_contents)
    self.assertIn('sample.opt.ir', sample1_contents)
    self.assertIn('sample.opt.ir.results', sample1_contents)

    # Codegen was not enabled so there should be no Verilog file.
    self.assertNotIn('sample.v', sample1_contents)
    self.assertNotIn('sample.sv', sample1_contents)

  def test_multiple_workers(self):
    crasher_path = self.create_tempdir().full_path
    samples_path = self.create_tempdir().full_path

    subprocess.check_call([
        RUN_FUZZ_MULTIPROCESS_PATH,
        '--seed=42',
        '--crash_path=' + crasher_path,
        '--save_temps_path=' + samples_path,
        '--sample_count=25',
        '--calls_per_sample=3',
        '--worker_count=10',
    ])

    sample_dirs = os.listdir(samples_path)

    # Sample directory should have 25 samples in it.
    self.assertEqual(len(sample_dirs), 25)

    # Should be samples from each worker.
    for i in range(10):
      self.assertTrue(
          any(d.startswith('worker{}'.format(i)) for d in sample_dirs)
      )

    # Crasher path should contain a single file 'test'.
    self.assertSequenceEqual(os.listdir(crasher_path), ('test',))

  def test_crashers_on_failure(self):
    crasher_path = self.create_tempdir().full_path

    subprocess.check_call([
        RUN_FUZZ_MULTIPROCESS_PATH,
        '--seed=42',
        '--crash_path=' + crasher_path,
        '--sample_count=5',
        '--calls_per_sample=3',
        '--worker_count=3',
        '--force_failure',
    ])

    # Crasher directory should have 5 samples in it plus the `test` file.
    self.assertEqual(len(os.listdir(crasher_path)), 6)

  def test_duration(self):
    samples_path = self.create_tempdir().full_path
    crasher_path = self.create_tempdir().full_path

    subprocess.check_call([
        RUN_FUZZ_MULTIPROCESS_PATH,
        '--seed=42',
        '--crash_path=' + crasher_path,
        '--save_temps_path=' + samples_path,
        '--duration=30s',
        '--calls_per_sample=3',
        '--worker_count=3',
    ])

    # Samples directory should have at least one sample in it.
    self.assertGreater(len(os.listdir(samples_path)), 0)

  def test_semantic_sum_seed_manifest_replay(self):
    crasher_path = self.create_tempdir().full_path
    samples_path = self.create_tempdir().full_path
    summaries_path = self.create_tempdir().full_path

    subprocess.check_call([
        RUN_FUZZ_MULTIPROCESS_PATH,
        '--seed=42',
        '--seed_manifest=' + SEED_MANIFEST_PATH,
        '--require_sum_type',
        '--crash_path=' + crasher_path,
        '--save_temps_path=' + samples_path,
        '--summary_path=' + summaries_path,
        '--sample_count=1',
        '--calls_per_sample=2',
        '--worker_count=1',
    ])

    sample_dirs = os.listdir(samples_path)
    self.assertIn(
        'seed-source_exhaustive_constructor_match_without_wildcard',
        sample_dirs,
    )
    self.assertIn('seed-source_or_pattern_binding_nonfinal', sample_dirs)
    self.assertIn('worker0-sample0', sample_dirs)

    self.assertIn(
        'diagnostic.txt',
        os.listdir(
            os.path.join(
                samples_path, 'seed-source_or_pattern_binding_nonfinal'
            )
        ),
    )
    self.assertIn('source_seed_replay.binarypb', os.listdir(summaries_path))

  def test_imported_parametric_semantic_sum_sample(self):
    crasher_path = self.create_tempdir().full_path
    samples_path = self.create_tempdir().full_path

    subprocess.check_call([
        RUN_FUZZ_MULTIPROCESS_PATH,
        '--seed=42',
        '--require_sum_type',
        '--require_imported_parametric_type',
        '--crash_path=' + crasher_path,
        '--save_temps_path=' + samples_path,
        '--sample_count=1',
        '--calls_per_sample=1',
        '--worker_count=1',
    ])

    self.assertSequenceEqual(os.listdir(crasher_path), ('test',))
    self.assertSequenceEqual(os.listdir(samples_path), ('worker0-sample0',))
    sample_path = os.path.join(samples_path, 'worker0-sample0')
    with open(os.path.join(sample_path, 'sample.x'), encoding='utf-8') as source:
      generated_program = source.read()
    self.assertIn('import float32;', generated_program)
    self.assertIn('float32::F32 {', generated_program)
    self.assertRegex(
        generated_program, r'x[0-9]+::x[0-9]+\([^)]*\.fraction as '
    )

    for artifact in (
        'sample.x.results',
        'sample.ir',
        'sample.ir.results',
        'sample.opt.ir',
        'sample.opt.ir.results',
    ):
      artifact_path = os.path.join(sample_path, artifact)
      self.assertTrue(os.path.isfile(artifact_path), artifact)
      self.assertGreater(os.path.getsize(artifact_path), 0, artifact)

    subprocess.check_call([os.path.join(sample_path, 'run.sh')], cwd=sample_path)

  def test_codegen_and_simulate(self):
    crasher_path = self.create_tempdir().full_path
    samples_path = self.create_tempdir().full_path

    subprocess.check_call([
        RUN_FUZZ_MULTIPROCESS_PATH,
        '--seed=42',
        '--crash_path=' + crasher_path,
        '--save_temps_path=' + samples_path,
        '--sample_count=2',
        '--calls_per_sample=3',
        '--worker_count=1',
        '--codegen',
        '--simulate',
        '--nouse_system_verilog',
    ])
    # Validate sample 1 directory.
    sample1_contents = os.listdir(os.path.join(samples_path, 'worker0-sample0'))
    self.assertIn('sample.x', sample1_contents)
    self.assertIn('sample.x.results', sample1_contents)
    self.assertIn('testvector.pbtxt', sample1_contents)
    self.assertIn('sample.ir', sample1_contents)
    self.assertIn('sample.ir.results', sample1_contents)
    self.assertIn('sample.opt.ir', sample1_contents)
    self.assertIn('sample.opt.ir.results', sample1_contents)
    # Directory should have verilog and simulation results.
    self.assertIn('sample.v', sample1_contents)
    self.assertIn('sample.v.results', sample1_contents)

  def test_codegen_systemverilog(self):
    crasher_path = self.create_tempdir().full_path
    samples_path = self.create_tempdir().full_path

    subprocess.check_call([
        RUN_FUZZ_MULTIPROCESS_PATH,
        '--seed=42',
        '--crash_path=' + crasher_path,
        '--save_temps_path=' + samples_path,
        '--sample_count=2',
        '--calls_per_sample=3',
        '--worker_count=1',
        '--codegen',
        '--use_system_verilog',
    ])
    # Directory should a system verilog file.
    sample1_contents = os.listdir(os.path.join(samples_path, 'worker0-sample0'))
    self.assertIn('sample.sv', sample1_contents)

  def test_codegen_and_simulate_with_proc(self):
    crasher_path = self.create_tempdir().full_path
    samples_path = self.create_tempdir().full_path

    subprocess.check_call([
        RUN_FUZZ_MULTIPROCESS_PATH,
        '--seed=42',
        '--crash_path=' + crasher_path,
        '--save_temps_path=' + samples_path,
        '--sample_count=2',
        '--generate_proc',
        '--proc_ticks=3',
        '--worker_count=1',
        '--codegen',
        '--simulate',
        '--nouse_system_verilog',
    ])
    # Validate sample 1 directory.
    sample1_contents = os.listdir(os.path.join(samples_path, 'worker0-sample0'))
    self.assertIn('sample.x', sample1_contents)
    self.assertIn('sample.x.results', sample1_contents)
    self.assertIn('testvector.pbtxt', sample1_contents)
    self.assertIn('sample.ir', sample1_contents)
    self.assertIn('sample.ir.results', sample1_contents)
    self.assertIn('sample.opt.ir', sample1_contents)
    self.assertIn('sample.opt.ir.results', sample1_contents)
    # Directory should have verilog and simulation results.
    self.assertIn('sample.v', sample1_contents)
    self.assertIn('sample.v.results', sample1_contents)


if __name__ == '__main__':
  test_base.main()
