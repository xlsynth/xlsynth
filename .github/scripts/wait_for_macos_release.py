#!/usr/bin/env python3
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

"""Waits for the macOS release build triggered by the same push."""

import json
import os
import re
import subprocess
import sys
import time
from urllib.parse import urlsplit


WORKFLOW = ".github/workflows/build-release-macos.yml"
JOB_NAME = "build-osx-arm64"
ACQUISITION_ERROR = (
    "The job was not acquired by Runner of type hosted even after multiple attempts"
)
MAX_ATTEMPTS = 3
POLL_SECONDS = 30
DISCOVERY_POLLS = 10
RERUN_POLLS = 10


class GitHubAPIError(RuntimeError):
    pass


def gh_api(endpoint, *, method="GET", paginate=False):
    """Reads JSON pages, or requests a rerun once even after a transport error."""
    command = ["gh", "api", "--method", method, endpoint]
    if paginate:
        command.extend(["--paginate", "--jq", "@json"])
    for attempt in range(3):
        try:
            result = subprocess.run(
                command, capture_output=True, text=True, timeout=90, check=False
            )
            if result.returncode == 0:
                if paginate:
                    return [json.loads(line) for line in result.stdout.splitlines()]
                return json.loads(result.stdout) if result.stdout.strip() else None
            detail = result.stderr.strip()
            http = re.search(r"\bHTTP (\d{3})\b", detail)
            transient = (
                http is None
                or int(http[1]) >= 500
                or http[1] == "429"
                or (http[1] == "403" and "rate limit" in detail.lower())
            )
        except subprocess.TimeoutExpired:
            detail = "request timed out"
            transient = True
        if method != "GET" or not transient or attempt == 2:
            raise GitHubAPIError(f"{method} {endpoint}: {detail}")
        time.sleep(5 * (attempt + 1))


def matches_source(run, repo, sha, ref):
    return (
        run["path"] == WORKFLOW
        and run["event"] == "push"
        and run["head_sha"] == sha
        and run["head_branch"] == ref
        and run["repository"]["full_name"] == repo
        and (run.get("head_repository") or {}).get("full_name") == repo
    )


def find_worker(repo, sha, ref):
    # Repo-wide discovery also works when the worker YAML lives on the release
    # branch and the repository's default branch mirrors upstream.
    endpoint = f"repos/{repo}/actions/runs?event=push&head_sha={sha}&per_page=100"
    for poll in range(DISCOVERY_POLLS):
        pages = gh_api(endpoint, paginate=True)
        ids = {
            run["id"]
            for page in pages
            for run in page["workflow_runs"]
            if matches_source(run, repo, sha, ref)
        }
        if len(ids) > 1:
            raise RuntimeError(
                f"Multiple macOS worker runs for {ref} at {sha}: {sorted(ids)}"
            )
        if ids:
            return ids.pop()
        if poll + 1 < DISCOVERY_POLLS:
            time.sleep(POLL_SECONDS)
    raise RuntimeError(f"No macOS worker run for {ref} at {sha}")


def acquisition_failed(repo, job):
    if (
        job["conclusion"] not in ("cancelled", "failure")
        or job["runner_id"]
        or job["runner_name"]
        or job["steps"] != []
    ):
        return False
    path = urlsplit(job["check_run_url"]).path
    if not re.fullmatch(rf"/repos/{re.escape(repo)}/check-runs/\d+", path):
        raise RuntimeError(f"Invalid check run URL for job {job['id']}")
    pages = gh_api(f"{path.lstrip('/')}/annotations?per_page=100", paginate=True)
    return any(
        entry["annotation_level"] == "failure"
        and entry["message"].strip().rstrip(".") == ACQUISITION_ERROR
        for page in pages
        for entry in page
    )


def wait_for_worker(repo, sha, ref):
    run_id = find_worker(repo, sha, ref)
    endpoint = f"repos/{repo}/actions/runs/{run_id}"
    print(
        f"Waiting for macOS worker https://github.com/{repo}/actions/runs/{run_id}",
        flush=True,
    )
    while True:
        run = gh_api(endpoint)
        if run["id"] != run_id or not matches_source(run, repo, sha, ref):
            raise RuntimeError("macOS worker source changed")
        attempt = run["run_attempt"]
        if run["status"] != "completed":
            time.sleep(POLL_SECONDS)
            continue
        listing = gh_api(f"{endpoint}/attempts/{attempt}/jobs?per_page=100")
        if listing["total_count"] != 1 or len(listing["jobs"]) != 1:
            raise RuntimeError(f"Expected exactly one job in worker attempt {attempt}")
        job = listing["jobs"][0]
        if (
            job["name"] != JOB_NAME
            or job["status"] != "completed"
            or job["run_attempt"] != attempt
            or job["run_id"] != run_id
            or job["head_sha"] != sha
        ):
            raise RuntimeError(f"Unexpected job in macOS worker attempt {attempt}")
        if run["conclusion"] == "success" and job["conclusion"] == "success":
            return run_id
        if (
            run["conclusion"] not in ("cancelled", "failure")
            or attempt >= MAX_ATTEMPTS
            or not acquisition_failed(repo, job)
        ):
            raise RuntimeError(
                f"macOS worker attempt {attempt} ended with {run['conclusion']}"
            )

        # Someone may have manually rerun the worker while we inspected the
        # annotations. A lost POST response can also mean the rerun was accepted.
        if gh_api(endpoint)["run_attempt"] > attempt:
            continue
        print(f"Retrying hosted runner acquisition after attempt {attempt}", flush=True)
        try:
            gh_api(f"repos/{repo}/actions/jobs/{job['id']}/rerun", method="POST")
            detail = "rerun accepted"
        except GitHubAPIError as error:
            detail = str(error)
        for _ in range(RERUN_POLLS):
            time.sleep(POLL_SECONDS)
            if gh_api(endpoint)["run_attempt"] > attempt:
                break
        else:
            raise RuntimeError(f"No new macOS worker attempt after {detail}")


def main():
    run_id = wait_for_worker(
        os.environ["GITHUB_REPOSITORY"],
        os.environ["GITHUB_SHA"],
        os.environ["GITHUB_REF_NAME"],
    )
    with open(os.environ["GITHUB_OUTPUT"], "a", encoding="utf-8") as output:
        print(f"run_id={run_id}", file=output)


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, KeyError, ValueError, OSError) as error:
        sys.exit(f"macOS release build failed: {error}")
