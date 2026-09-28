# MojoRecomp Agent Guide

This file defines the operating rules for AI coding agents working in this
repository. It is intentionally focused on decisions and guardrails. User-facing
setup and product information belongs in `README.md`.

## Project identity and scope

- The project name is **MojoRecomp**. The desktop application is
  **MojoRecomp Launcher**.
- Do not introduce or restore the former project name in code, paths, UI,
  metadata, package names, documentation, or generated output.
- MojoRecomp is an independent native recompilation project for Xbox 360 Crash
  titles. It is not affiliated with or endorsed by the games' rights holders.
- **Crash of the Titans (COT)** is the current playable target.
- **Crash: Mind over Mutant (MOM)** has a separate launcher profile and runtime
  identity, but is not yet a playable target.
- Keep maintained source code, project-controlled identifiers, comments, UI
  text, configuration, tests, logs, and documentation in English.
- English is mandatory for class, function, method, variable, constant, field,
  enum, test, file, and directory names created by the project; comments;
  diagnostics; console/log output; test fixtures and synthetic path names; build
  scripts; and developer-facing messages. Do not introduce Portuguese or any
  other non-English prose into maintained code merely because the project owner
  speaks that language. Non-English text is allowed only when it is intentional
  localization content or a proper name/title that must retain its original
  spelling.
- Do not use emoji or decorative Unicode glyphs in maintained source code,
  identifiers, comments, tests, logs, diagnostics, scripts, configuration, or
  developer-facing runtime/build output. Prefer plain ASCII punctuation there
  unless a non-ASCII character is required by an intentional localization or
  character-encoding test. Markdown documentation may use emoji or Unicode
  symbols when they improve readability, such as status indicators or checkmarks.

## Non-negotiable Git and collaboration rules

Read-only Git commands such as `git status`, `git diff`, `git log`, and
`git ls-files` are allowed. Every state-changing or remote Git action requires
an explicit instruction from the project owner for that exact action.

- Never run `git add`, `git commit`, `git push`, `git tag`, or publish a release
  unless explicitly requested.
- Never create an issue or pull request. If the owner later wants one, prepare a
  local summary or patch for human review instead of publishing it. Do not
  contact upstream projects or their maintainers on your own.
- Never force-push or change Git remotes.
- Do not create or switch branches/worktrees, stash changes, merge, rebase,
  amend, reset, restore, checkout files, or clean the worktree unless the owner
  explicitly requests that operation.
- A request to "finish", "fix", "update", or "clean up" is not authorization
  to stage, commit, push, open a PR, or modify the index.
- Preserve unrelated user changes. Inspect the relevant diff before editing and
  never discard work merely to obtain a clean status.
- The historical Git index is protected and may retain references to removed
  private or diagnostic material. Do not publish, commit, rewrite, unstage, or
  clean that state without a specific owner-approved plan.
- If a future request explicitly authorizes a commit, first inspect the exact
  staged set and ensure the protected historical index and private material are
  excluded. Do not assume the existing index is safe to commit.

## Legal and data boundary

The repository must remain useful without distributing copyrighted game data.
Users provide their own legally obtained supported game input.

Never add, stage, package, upload, quote, or expose:

- Xbox 360 disc images, archives, executables, title updates, keys, decrypted
  intermediates, movies, audio, textures, or other original game assets;
- extracted or managed game directories, including `games/`;
- saves, settings, user content, caches, or any `userdata/` contents;
- generated PPC translation units or generated guest images;
- build directories, packaged applications, executables, DLLs, symbols, maps,
  crash dumps, shader dumps, logs, screenshots, captures, or support bundles;
- local toolchain/dependency trees; or
- `.private/` contents, agent handoffs, operational notes, or other private
  development state.

Do not use real user saves or installed game data in automated tests. Use
isolated temporary fixtures and synthetic inputs. Do not weaken ignore rules or
release validation to make a local build artifact appear publishable.

Third-party license and notice files are required compliance material, not
documentation clutter. Preserve them and their provenance. Original MojoRecomp
code is licensed under the ISC License in the repository root unless an
individual file states different terms. Do not apply that ISC grant to
third-party code/tools, generated guest/PPC or original game code, game data,
launcher game artwork, names, characters, logos, trademarks, or any material
owned by another rights holder.

## Repository map and ownership

- `launcher/`: MojoRecomp Launcher, built with Tauri, Rust, Svelte, TypeScript,
  and Vite. It owns setup/import, validation, settings, launch/monitoring,
  updates, logs, support tooling, and title selection.
- `runtime/`: native C++ host/runtime for the recompiled guest. It owns kernel
  services, VFS, timing, threads/fibers, input, audio, video, and Vulkan/Xenos
  presentation. Runtime tests live under `runtime/tests/`.
- `config/`: maintained XenonRecomp configuration and switch tables. Treat these
  inputs, not generated PPC output, as the source of truth for recompilation.
- `patches/`: reviewed patches for pinned XenonRecomp/XenosRecomp dependencies.
  Update the maintained patch instead of hand-editing a disposable dependency
  checkout.
- `tools/`: analysis and recompilation helpers. Windows entry points are `.bat`.
- `launcher/resources/`: title metadata, required notices, verified license
  texts, and distribution metadata. Do not delete license files to reduce file
  count.
- `ppc/`: local generated guest code. It is not maintained public source.
- `thirdparty/`: local pinned tools/dependencies. It is not a place for direct,
  permanent project changes.
- `.private/`: ignored local operational state. It must never enter public
  source, patches, logs, reports, prompts, or release artifacts.
- `version.toml`: source of truth for suite, launcher, and per-title versions.

Avoid adding planning diaries, migration reports, agent handoffs, or duplicate
status Markdown files. Update `README.md` or this file only when the information
belongs there, and create new documentation only when the owner asks for it.

## Architecture rules

- Prefer general, title-independent runtime behavior over scene-, address-,
  ordinal-, vendor-, or machine-specific hacks.
- Keep title-specific hooks and addresses explicit and isolated. Do not make MOM
  silently inherit COT behavior; share code only after both titles prove the
  same stable contract.
- Do not manually edit generated PPC output when a configuration, generator,
  patch, or runtime fix is the durable source-level solution.
- Keep guest simulation/timing independent from host rendering performance. COT
  remains a 30 FPS title unless a separately designed and validated feature
  changes that contract.
- Preserve game behavior and visual intent. A change that fixes one scene while
  regressing another is not complete.
- Hardware behavior claims require evidence. Form a hypothesis, reproduce the
  issue, prefer an isolated probe/test, and validate representative game scenes.
- Unsupported GPU capabilities must fail or fall back safely; do not infer
  capabilities from vendor names alone.
- Launcher updates must preserve the configured game library and the separate
  Saved Games and Local AppData trees. Legacy portable `games/` and `userdata/`
  directories are migration inputs only and must never be discarded silently.
  Uninstall and cleanup paths must make any user-data deletion explicit and
  narrowly scoped.
- Do not replace the currently owner-approved launcher artwork or remove its
  hash/provenance validation without explicit owner approval.

## Windows scripting policy

MojoRecomp uses batch entry points on Windows.

- Never add, restore, invoke, or document a `.ps1` project script.
- Use `.bat` for maintained Windows automation and `.cmd` executables such as
  `npm.cmd` when required by an installed tool.
- Keep batch scripts non-interactive where practical, fail on errors, quote
  paths, and work when the repository path contains spaces.
- Do not duplicate substantial build logic across batch files. Keep the logic in
  the owning build/tool configuration and use `.bat` as the stable entry point.

## Canonical commands

Run commands from the repository root unless a command changes directory.

```bat
setup.bat
tools\analyze.bat
tools\recompile.bat
build-smoke.bat
```

Launcher checks and production build:

```bat
cd launcher
npm.cmd ci
npm.cmd run check
npm.cmd run generate:notices
cargo test --manifest-path src-tauri\Cargo.toml
npm.cmd run build:production
```

Public release packaging requires the release asset base and notes URLs. Artwork,
clean-install behavior, and representative gameplay/visual behavior remain manual
release review responsibilities and must be reported accurately rather than encoded
as environment-variable acknowledgements.

Do not run setup, dependency installation, a full rebuild, packaging, or the
entire test suite by habit. Use the smallest command that meaningfully validates
the changed area, and state clearly when a required check could not be run.

## Change workflow

1. Read this file and the relevant section of `README.md`.
2. Inspect `git status` and the relevant diff without altering the index.
3. Identify the maintained source of truth before editing. Generated output,
   local dependency copies, and packaged files are usually not that source.
4. Make the smallest coherent change and preserve unrelated work.
5. Add or update an isolated regression test/probe when behavior changes and a
   stable automated assertion is possible.
6. Run focused validation for the affected layer.
7. Review the final diff for private data, game assets, generated output,
   obsolete naming, accidental license changes, and unrelated edits.
8. Report what changed, what was validated, and what still requires manual
   verification. Do not stage or commit the result.

## Validation by area

- Runtime C++ or CMake: run the relevant focused test/probe, then
  `build-smoke.bat` when the change can affect integration or linking.
- Recompiler configuration or switch tables: run `tools\analyze.bat`,
  `tools\recompile.bat`, and the relevant smoke gate. Generated output remains
  local.
- Launcher Svelte/TypeScript/CSS: run `npm.cmd run check`; build the launcher
  when bundling or runtime integration is affected.
- Launcher Rust/Tauri: run focused Cargo tests, then the launcher build when
  IPC, setup, filesystem, process, or packaging behavior changes.
- Versions, bundled resources, notices, or packaging: regenerate notices if
  dependency inputs changed and run the repository's release validation/build
  flow. Never solve a validation failure by removing required notices or guards.
- Documentation-only changes: inspect links, paths, command names, naming, and
  consistency; do not trigger unrelated heavy builds.

Automated tests do not replace manual gameplay and visual validation. Manual
visual validation is still pending for the current release state. Do not claim
that rendering, aspect ratios, UI presentation, or representative gameplay are
fully approved solely because compilation or probes passed.

## Completion criteria

A task is complete only when the requested behavior is implemented at the
correct source layer, scoped checks pass (or failures are reported honestly),
the diff contains no private/game/generated/build material, and remaining manual
validation or licensing decisions are named explicitly. Do not claim release
readiness while required manual visual/gameplay validation remains pending.
