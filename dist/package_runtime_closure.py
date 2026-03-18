#!/usr/bin/env python3

"""CLI wrapper for packaging non-system Linux DSOs needed to load libxls."""

import argparse
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import release_runtime_closure


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--libxls", required = True)
    parser.add_argument("--output-dir", required = True)
    parser.add_argument("--platform", default = "")
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    libxls_path = Path(args.libxls)
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents = True, exist_ok = True)
    platform = args.platform or release_runtime_closure.detect_linux_release_platform()
    result = release_runtime_closure.package_runtime_closure(libxls_path, output_dir, platform)
    print("Wrote {}".format(result["archive_path"]))
    print("Wrote {}".format(result["manifest_path"]))


if __name__ == "__main__":
    main()
