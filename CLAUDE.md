# CLAUDE.md

Biome is an opinionated, fixed-policy Wayland compositor on wlroots 0.20. See
`docs/roadmap.md` for planned work, `docs/architecture-notes.md` for settled
design detail, and `docs/history.md` for the original build-out and rationale.

## Local Install Layout (dev machine)

Build into `build/` (`cmake --build build`). Two binaries, deliberately different:

- **Session:** `/usr/bin/biome` is a symlink to `build/core/biome`, so a rebuild takes effect at the next login. `startforest-wayland` runs it.
- **Greeter:** `/usr/local/bin/biome-greeter` is a static copy (`sudo install -m755 build/core/biome /usr/local/bin/biome-greeter`), refreshed by hand only when wanted. greetd's user can't exec out of `$HOME`, and `install` gives it the `bin_t` SELinux label (a `cp` from home keeps `user_home_t` and fails).

greetd is started via the `greetd.service.d/50-forest.conf` drop-in with `--config /etc/forest/greetd.toml` (not `/etc/greetd/config.toml`, which is unused); its command is `/usr/local/bin/biome-greeter -s /usr/bin/forest-greeter`.

## Debian Packaging

`debian/` is kept in the repo for a possible return to Debian, but Fedora
(`packaging/biome.spec`) is the real target. When a change affects packaging
(new binaries or files, dependencies, man pages), update `debian/` alongside the
spec, best effort. Don't build or test Debian packages; leave `debian/changelog`
for release time.

## Git Workflow

Same model in `biome` and `forest`:

- **Major changes:** branch off an up-to-date `develop`, implement and test there, push, and open a PR into `develop` (`gh pr create --base develop`). The user merges PRs and pulls `develop` locally — don't do either.
- **Small tweaks** (roadmap/doc edits, other small standalone doc changes, a code change of a couple of lines): commit straight to `develop` and push — unless a PR is about to be opened, in which case put it on that branch instead.
- `master` only changes at a release (`develop` merged in and tagged by the user). Never commit to or PR against it.
- **Commit once a change is settled, not on every edit.** If you've asked the user to verify something or answer a question that could change the edit, leave it uncommitted until they reply. Then fold the follow-up into the same commit.
