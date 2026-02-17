#!/usr/bin/env python3

"""CLI wrapper for packaging non-system Linux DSOs needed to load libxls."""

from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import release_runtime_closure


def main() -> None:
    args = release_runtime_closure.parse_args(sys.argv[1:])
    libxls_path = Path(args.libxls)
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents = True, exist_ok = True)
    release_target = release_runtime_closure.validate_linux_release_target(
        args.release_target
    )
    result = release_runtime_closure.package_runtime_closure(
        libxls_path,
        output_dir,
        release_target,
        allow_release_target_mismatch = args.allow_release_target_mismatch,
    )
    print("Wrote {}".format(result["archive_path"]))
    print("Wrote {}".format(result["manifest_path"]))


if __name__ == "__main__":
    main()
