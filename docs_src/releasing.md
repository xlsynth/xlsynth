# Releasing

## xlsynth release branches

`release-base` points to **one fork-specific commit** whose parent is an exact
`google/xls` upstream commit. That fork-specific commit collects the changes
needed by xlsynth that will not be submitted upstream. `release-candidate`
builds on `release-base`, keeping changes intended for upstream submission as
separate commits.

When adding a fork-only change without advancing the upstream base:

1. Open a PR targeting `release-base` and squash-merge it after review.
2. Fold the resulting commit into the existing fork-specific commit. Preserve
   its upstream parent and the combined file contents, leaving one fork-specific
   commit on `release-base`.
3. Rebase the pending commits on `release-candidate` from the `release-base`
   commit they previously used onto the updated `release-base`. Preserve their
   changes and order.
4. Push the rewritten `release-base` and `release-candidate` branches, using
   `--force-with-lease` with explicit expected remote tips.

After the squash merge and before rewriting, record both remote branch tips and
the base used by `release-candidate`, and preserve rollback refs. Use those
recorded remote tips as the expected values for `--force-with-lease`. Before
pushing, verify the upstream parent, the combined file contents on
`release-base`, and the changes and order of the rebased candidate commits.

A squash merge alone temporarily leaves two fork-specific commits on
`release-base`; folding them together restores the convention. For example,
suppose upstream commit `U` has fork commit `B` above it, and a PR adds commit
`D`. Combine `B` and `D` into `B'`, whose parent is still `U`. A pending
candidate commit `C` then moves from above `B` to above `B'` and remains
a separate commit.

## xlsynth release versions

This convention applies to `vX.Y.Z` release tags in
[`xlsynth/xlsynth`](https://github.com/xlsynth/xlsynth), including `libxls` and
the accompanying tools. Here, `Y` is the minor version and `Z` is the patch
version.

The **upstream base** of a release is the exact `google/xls` commit on which its
fork-specific changes are based. Compare the upstream base of the release line
being updated with that of the proposed release. The tips of `release-base` and
`release-candidate` can change without changing this upstream base.

- When advancing the upstream base, increment the minor version and reset the
  patch version to zero: `vX.Y.Z` becomes `vX.(Y+1).0`.
- Use a patch-version bump only for a small patch to an existing minor release
  line while keeping its upstream base unchanged: `vX.Y.Z` becomes `vX.Y.(Z+1)`.
  This also applies when patching an older minor release line.

Keeping the upstream base fixed is necessary for a patch release; it does not
make every change suitable for one. Patch releases are for small changes that
keep the release line stable.

For example, starting from `v0.55.2` based on upstream commit `A`:

| Proposed release                      | Version   |
| ------------------------------------- | --------- |
| Small patch, still based on `A`       | `v0.55.3` |
| Sync the upstream base forward to `B` | `v0.56.0` |

These are the fork's release conventions. The following sections describe the
upstream Google XLS release process.

## Versioning

Upstream Google XLS uses the following versioning scheme:
`v${SEM_VER}-${COMMITS_COUNT_SINCE_LAST_ANNOTATED_TAG}-g${GIT_HASH)`

> The [v0.0.0](https://github.com/google/xls/tree/v0.0.0) annotated tag points
> to the initial commit to bootstrap the versioning scheme using
> [git-describe](https://git-scm.com/docs/git-describe).

Released binaries are stamped with the `{version}` using
[`--embed_label`](https://bazel.build/reference/command-line-reference#:~:text=%2D%2Dembed_label%3D%3Ca%20one,label%20in%20binary)
during `bazel build` invocations.

Soft-tags (non-annotated) are used to mark released versions.

## Packaging

The `//dist:xls_dist_tar` target uses
[@rules_pkg](https://github.com/bazelbuild/rules_pkg/)/pkg_tar to create an
archive named `xls-{version}-{os}-{arch}.tar.gz` with the following layout:

```
xls-{version}-{os}-{arch}/codegen_main
xls-{version}-{os}-{arch}/interpreter_main
xls-{version}-{os}-{arch}/ir_converter_main
xls-{version}-{os}-{arch}/opt_main
xls-{version}-{os}-{arch}/proto_to_dslx_main
...
xls-{version}-{os}-{arch}/LICENSE
xls-{version}-{os}-{arch}/THIRD_PARTY_NOTICES.txt
```

`{version}`, `{os}` and `{arch}` placeholders get replaced by
[user-defined build settings](https://bazel.build/extending/config#user-defined-build-settings)
passed during `bazel build` invocations.

## Automation

The
[Nightly Ubuntu 22.04](https://github.com/google/xls/actions/workflows/nightly-ubuntu-22.04.yml)
workflow automates the release process.

The `Dist` step builds the `//dist:xls_dist_tar` target w/ the following flags:

flag               | value
------------------ | -------------------------
`--embed_label`    | `$VERSION`
`--//dist:version` | `$VERSION`
`--//dist:os`      | `downcased($RUNNER_OS)`
`--//dist:arch`    | `downcased($RUNNER_ARCH)`

The `Release` step uploads the release artefacts to
https://github.com/google/xls/releases using
[softprops/action-gh-release@v1](https://github.com/marketplace/actions/gh-release)
and creates new soft-tag corresponding to `{version}`.
