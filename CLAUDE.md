# MiniSkirt — notes for Claude sessions

This repository is public. Only put files here that are fine to publish: source, build settings, scripts that build or package the mod, and the files that go into the release zip.

## Private files go in `dev/`
- `dev/` is ignored by this repository and has its own private git repository. Put everything that is not for publishing there: working notes and hand-offs, plans, one-off check scripts, survey tools and their results, the outfit viewer, the release checklist, files extracted from the game.
- Read `dev/HANDOFF.md`, `dev/NOTES_MULTI_RUNTIME.md` and `dev/RELEASE.md` first when they exist. New notes are written there, not here.
- Commit changes in `dev/` to the `dev/` repository, and changes to public files to this repository.
- Do not write paths that only exist on this PC into public files. Local paths go in `local.ps1` (ignored; see `local.example.ps1`).
