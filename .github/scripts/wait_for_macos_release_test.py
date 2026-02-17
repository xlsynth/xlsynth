# Copyright 2026 The XLS Authors
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     https://www.apache.org/licenses/LICENSE-2.0
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

import wait_for_macos_release as worker


REPO = "xlsynth/xlsynth"
SHA = "2c20e4754452cf1d08ca96ad63e648de06626f0e"
REF = "v0.64.0"
RUN_ID = 53000
RUN_API = f"repos/{REPO}/actions/runs/{RUN_ID}"

# GitHub returned this failure with an unassigned runner on v0.64.0 and v0.65.0.
CAPACITY_FAILURE = {
    "annotation_level": "failure",
    "message": "The job was not acquired by Runner of type hosted even after multiple attempts",
}
CAPACITY_NOTICE = {
    "annotation_level": "notice",
    "message": "Due to capacity constraints, jobs targeting macOS arm64 runners may experience longer queue times.",
}


def run(attempt=1, **overrides):
    return {
        "id": RUN_ID,
        "repository": {"full_name": REPO},
        "head_repository": {"full_name": REPO},
        "head_sha": SHA,
        "head_branch": REF,
        "event": "push",
        "path": ".github/workflows/build-release-macos.yml",
        "status": "completed",
        "conclusion": "success",
        "run_attempt": attempt,
        **overrides,
    }


def jobs(attempt=1, conclusion="cancelled", **overrides):
    assigned = conclusion == "success"
    job = {
        "id": 40 + attempt,
        "run_id": RUN_ID,
        "head_sha": SHA,
        "name": "build-osx-arm64",
        "status": "completed",
        "run_attempt": attempt,
        "conclusion": conclusion,
        "runner_id": 42 if assigned else 0,
        "runner_name": "GitHub Actions assigned" if assigned else "",
        "steps": [{"name": "Upload artifacts", "conclusion": "success"}] if assigned else [],
        "check_run_url": f"https://api.github.com/repos/{REPO}/check-runs/{90 + attempt}",
        **overrides,
    }
    return {"total_count": 1, "jobs": [job]}


def discovery(*runs):
    return [{"workflow_runs": list(runs)}]


class WaitForMacosReleaseTest(unittest.TestCase):
    def setUp(self):
        api_patch = mock.patch.object(worker, "gh_api")
        self.api = api_patch.start()
        self.addCleanup(api_patch.stop)
        sleep_patch = mock.patch.object(worker.time, "sleep")
        self.sleep = sleep_patch.start()
        self.addCleanup(sleep_patch.stop)

    def posts(self):
        return [c.args[0] for c in self.api.call_args_list if c.kwargs.get("method") == "POST"]

    def test_waits_for_matching_worker_and_outputs_its_artifact_run_id(self):
        other_sources = {
            "head_branch": "release-preflight/other",
            "head_sha": "0" * 40,
            "path": ".github/workflows/build-and-release-dylib.yml",
            "event": "workflow_dispatch",
            "repository": {"full_name": "other/xls"},
            "head_repository": {"full_name": "other/xls"},
        }
        unrelated = discovery(
            *(run(id=i, **{key: value}) for i, (key, value) in enumerate(other_sources.items()))
        )
        self.api.side_effect = [
            unrelated,
            unrelated + discovery(run()),
            run(status="in_progress", conclusion=None),
            run(),
            jobs(conclusion="success"),
        ]
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "github_output"
            with mock.patch.dict(
                os.environ,
                {
                    "GITHUB_REPOSITORY": REPO,
                    "GITHUB_SHA": SHA,
                    "GITHUB_REF_NAME": REF,
                    "GITHUB_OUTPUT": str(output),
                },
            ):
                worker.main()
            self.assertEqual(output.read_text(), f"run_id={RUN_ID}\n")
        self.api.assert_any_call(f"{RUN_API}/attempts/1/jobs?per_page=100")
        self.assertGreaterEqual(self.sleep.call_count, 2)
        self.assertEqual(self.posts(), [])

    def test_ambiguity_or_changed_source_cannot_supply_artifacts(self):
        scenarios = [
            [discovery(run(), run(id=RUN_ID + 1))],
            [discovery(run()), run(head_sha="0" * 40)],
            [discovery(run()), run(), jobs(conclusion="success", run_id=RUN_ID + 1)],
            [discovery(run()), run(), jobs(conclusion="success", run_attempt=2)],
            [discovery(run()), run(), jobs(conclusion="success", name="build-rocky8")],
            [discovery(run()), run(), jobs(conclusion="success", head_sha="0" * 40)],
        ]
        for responses in scenarios:
            with self.subTest(responses=responses):
                self.api.side_effect = responses
                with self.assertRaises(RuntimeError):
                    worker.wait_for_worker(REPO, SHA, REF)
                self.assertEqual(self.posts(), [])

    def test_only_unassigned_capacity_failure_can_retry(self):
        for overrides, annotations in [
            ({}, [CAPACITY_NOTICE]),
            ({}, [{"annotation_level": "failure", "message": "Request was cancelled."}]),
            ({"runner_id": 42}, [CAPACITY_FAILURE]),
            ({"runner_name": "GitHub Actions assigned"}, [CAPACITY_FAILURE]),
            ({"steps": [{"name": "Build for arm64", "conclusion": "failure"}]}, [CAPACITY_FAILURE]),
        ]:
            with self.subTest(job=overrides, annotations=annotations):
                self.api.return_value = [annotations]
                self.assertFalse(worker.acquisition_failed(REPO, jobs(**overrides)["jobs"][0]))

    def test_retries_only_the_macos_job_up_to_three_total_attempts(self):
        responses = [discovery(run())]
        for attempt in (1, 2):
            failed = run(attempt, conclusion="failure")
            responses.extend(
                [
                    failed,
                    jobs(attempt),
                    [[CAPACITY_NOTICE], [CAPACITY_FAILURE]],
                    failed,
                    None,
                    run(attempt + 1, status="queued", conclusion=None),
                ]
            )
        responses.extend([run(3, conclusion="failure"), jobs(3)])
        self.api.side_effect = responses
        with self.assertRaises(RuntimeError):
            worker.wait_for_worker(REPO, SHA, REF)
        self.assertEqual(
            self.posts(), [f"repos/{REPO}/actions/jobs/{job}/rerun" for job in (41, 42)]
        )
        self.api.assert_any_call(
            f"repos/{REPO}/check-runs/91/annotations?per_page=100", paginate=True
        )

    def test_rerun_races_keep_one_request_per_failed_attempt(self):
        first = run(conclusion="failure")
        retry = run(2, status="queued", conclusion=None)
        failed = [discovery(first), first, jobs(), [[CAPACITY_FAILURE]]]
        success = [run(2), jobs(2, conclusion="success")]
        for transition, expected_posts in [
            ([retry], []),  # Another caller requested the retry first.
            (
                [first, worker.GitHubAPIError("response lost"), first, retry],
                [f"repos/{REPO}/actions/jobs/41/rerun"],
            ),
        ]:
            with self.subTest(transition=transition):
                self.api.reset_mock()
                self.api.side_effect = failed + transition + success
                self.assertEqual(worker.wait_for_worker(REPO, SHA, REF), RUN_ID)
                self.assertEqual(self.posts(), expected_posts)

    def test_manual_success_after_auto_retry_limit_supplies_artifacts(self):
        self.api.side_effect = [discovery(run(4)), run(4), jobs(4, conclusion="success")]
        self.assertEqual(worker.wait_for_worker(REPO, SHA, REF), RUN_ID)
        self.assertEqual(self.posts(), [])

    def test_lost_post_response_without_new_attempt_is_bounded(self):
        first = run(conclusion="failure")
        self.api.side_effect = [
            discovery(first),
            first,
            jobs(),
            [[CAPACITY_FAILURE]],
            first,
            worker.GitHubAPIError("response lost"),
            first,
            first,
        ]
        with mock.patch.object(worker, "RERUN_POLLS", 2):
            with self.assertRaisesRegex(RuntimeError, "No new macOS worker attempt"):
                worker.wait_for_worker(REPO, SHA, REF)
        self.assertEqual(self.posts(), [f"repos/{REPO}/actions/jobs/41/rerun"])


if __name__ == "__main__":
    unittest.main()
