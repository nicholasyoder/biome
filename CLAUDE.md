# CLAUDE.md

Biome is an opinionated, fixed-policy Wayland compositor on wlroots 0.18. See
`docs/roadmap.md` for planned work, `docs/architecture-notes.md` for settled
design detail, and `docs/history.md` for the original build-out and rationale.

## Git Workflow

Same model in `biome` and `forest`:

- **Major changes:** branch off an up-to-date `develop`, implement and test there, push, and open a PR into `develop` (`gh pr create --base develop`). The user merges PRs and pulls `develop` locally — don't do either.
- **Small tweaks** (roadmap/doc edits, other small standalone doc changes, a code change of a couple of lines): commit straight to `develop` and push — unless a PR is about to be opened, in which case put it on that branch instead.
- `master` only changes at a release (`develop` merged in and tagged by the user). Never commit to or PR against it.
