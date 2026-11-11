# Releasing

urlmagic4 uses [semantic versioning](https://semver.org), starting at 4.0.0 (for IIS 4), and
[release-please](https://github.com/googleapis/release-please) to cut releases from
[Conventional Commits](https://www.conventionalcommits.org).

## How versions are chosen

Commit messages on `trunk` decide the next version:

| Commit | Next version |
|---|---|
| `fix: ...` | patch (4.0.0 → 4.0.1) |
| `feat: ...` | minor (4.0.0 → 4.1.0) |
| `feat!: ...` or a `BREAKING CHANGE:` footer | major (4.0.0 → 5.0.0) |
| `docs:`, `chore:`, `build:`, `ci:`, `test:`, `refactor:`, `style:` | no release on their own |

## Cutting a release

1. On every push to `trunk`, the **Release** workflow opens or updates a release pull request
   ("chore(trunk): release 4.1.0"). It bumps `version.txt` and adds the new section to
   `CHANGELOG.md`.
2. Merge it when you want to release. The workflow tags `v<version>`, creates the GitHub
   release, builds the installer `urlmagic<version>.exe`, e.g. `urlmagic4.0.0.exe` (and a zip with it plus the docs) and attaches them.

The version in `version.txt` is stamped into the DLLs and the installer (and its file name) by the Makefile.

## First release

`release-please-config.json` says `"release-as": "4.0.0"` so the first release is 4.0.0.
Remove that line after 4.0.0 is out; from then on the commits decide.

## One-time repository setting

GitHub → Settings → Actions → General → **Allow GitHub Actions to create and approve pull
requests** must be on, or the release pull request can't be opened.
