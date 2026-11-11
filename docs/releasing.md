# Releasing

urlmagic4 uses [semantic versioning](https://semver.org) (starting at 4.0.0, for IIS 4) and
[Changesets](https://github.com/changesets/changesets) for the changelog.

## Day to day

* Every pull request that changes behaviour adds a changeset in `.changeset/` (see the README
  there): `patch` for fixes, `minor` for features, `major` for breaking changes.
* On every push to `trunk`, the **Release** workflow opens or updates a **Version Packages** pull
  request. It bumps `package.json`, writes `CHANGELOG.md` and deletes the used changesets.

## Cutting a release

1. Merge the Version Packages pull request (if there is one).
2. GitHub → Actions → **Release** → *Run workflow* on `trunk`.

The workflow builds the DLLs and creates the GitHub release `v<version>` with
`urlmagic4-<version>.zip` attached and that version's changelog section as the notes. If the
tag already exists, nothing is published.

## Before 4.0.0

`package.json` already says 4.0.0 and nothing has been released. Don't add changesets until
4.0.0 is out (they would bump it to 4.0.1 or 4.1.0); edit the 4.0.0 section of
`CHANGELOG.md` directly instead.
