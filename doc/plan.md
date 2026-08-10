# GitBoard C++ Implementation Plan

## Goal

Implement the GitBoard command line tool described in `doc/spec.md` as a modern
C++ application built with Bazel. The first supported target is macOS. Linux and
Windows support should follow once the macOS CLI is functional and tested.

The implementation should preserve the documented task file format:

- Markdown files under `tasks/`.
- YAML front matter delimited by `---`.
- Task IDs like `TASK-001`.
- Base64-encoded command inputs where specified.
- JSON output for `list` and `batch`.
- Git-backed `publish` and `sync` behavior.

## Requirement Summary

### Global Behavior

- Support `--project-root PATH`.
- Resolve relative project roots from the current working directory.
- Use `PROJECT_ROOT/tasks` when `--project-root` is set.
- Use `./tasks` when `--project-root` is omitted.
- Create the task directory for commands that write to it.
- Read and write UTF-8 text files.

### Task Model

Each task must support these fields:

- `id`
- `title`
- `assignee`
- `priority`
- `story_points`
- `tags`
- `status`
- `created_at`
- `updated_at`
- `created_by`
- `branches`
- `prs`
- `ci_status`
- Markdown `body`

Unknown front matter fields should initially be treated as an error to match the
documented Python behavior. This can later be relaxed if compatibility with
hand-edited files becomes more important.

### Commands

Implement:

- `gitboard list`
- `gitboard task TASK_ID`
- `gitboard create BASE64_TITLE`
- `gitboard body TASK_ID BASE64_BODY`
- `gitboard assignee TASK_ID BASE64_ASSIGNEE`
- `gitboard branches TASK_ID BASE64_BRANCHES`
- `gitboard ci_status TASK_ID BASE64_CI_STATUS`
- `gitboard priority TASK_ID BASE64_PRIORITY`
- `gitboard prs TASK_ID BASE64_PRS`
- `gitboard tags TASK_ID BASE64_TAGS`
- `gitboard title TASK_ID BASE64_TITLE`
- `gitboard move TASK_ID BASE64_NEW_STATUS`
- `gitboard comment TASK_ID BASE64_MESSAGE`
- `gitboard publish`
- `gitboard sync`
- `gitboard batch BATCH_PATH`

All task update operations should be implemented as regular subcommands so their
syntax is consistent with `create`, `move`, `comment`, `publish`, and `sync`.

## Open Design Decisions

### Task ID Generation

The Python behavior uses `count(tasks/*.md) + 1`, which can reuse IDs after
deletions. The C++ implementation should fix this.

Proposed strategy:

1. Scan `tasks/*.md`.
2. Extract IDs matching `^TASK-([0-9]+)` from filenames.
3. Also parse front matter IDs from readable task files.
4. Use the largest numeric ID found plus one.
5. Format with at least three digits, preserving wider IDs if the repository
   already has them.

This prevents reuse after deletion and remains compatible with existing
`TASK-NNN_slug.md` files.

### Slug Generation

The Python slugifier only lowercases and replaces spaces. The C++ version should
produce safer filenames:

- Lowercase ASCII letters.
- Convert whitespace and punctuation runs to a single `-`.
- Keep digits.
- Trim leading and trailing `-`.
- Use `task` if the title contains no usable characters.

Unicode titles can remain in front matter and body. The initial filename slug can
be ASCII-only for portability across macOS, Linux, and Windows.

### Status Configuration

The repository contains the default workflow at `config/statuses.json` with the
documented statuses:

- `backlog`
- `todo`
- `in_progress`
- `review`
- `done`
- `released`
- `archived`

The binary should load this bundled config. A future extension can allow a
project-local override such as `.gitboard/statuses.json`.

## External Libraries

The initial macOS implementation is self-contained and uses only the C++17
standard library plus Bazel `rules_cc`. This keeps the first build reliable
without network-fetched parser/runtime dependencies.

Permissive external libraries remain reasonable future upgrades if the local
parsers grow too complex:

- `abseil-cpp`, Apache-2.0, for status/statusor, strings, and time helpers.
- `nlohmann/json`, MIT, for full JSON parsing and emission.
- `yaml-cpp`, MIT, for complete YAML front matter support.
- `CLI11`, BSD-3-Clause, for richer command-line parsing.
- `googletest`, BSD-3-Clause, for structured unit tests.

No Git library is required. `publish` and `sync` use the `git` executable
through a subprocess wrapper, matching the Python behavior and keeping
authentication/credential handling delegated to Git.

## Bazel Structure

Proposed files:

```text
MODULE.bazel
BUILD.bazel
src/
  BUILD.bazel
  main.cc
  app.cc
  app.h
  cli.cc
  cli.h
  commands/
    batch.cc
    batch.h
    create.cc
    create.h
    git.cc
    git.h
    list.cc
    list.h
    read_task.cc
    read_task.h
    update.cc
    update.h
  core/
    base64.cc
    base64.h
    filesystem.cc
    filesystem.h
    front_matter.cc
    front_matter.h
    status_config.cc
    status_config.h
    task.cc
    task.h
    task_store.cc
    task_store.h
    time.cc
    time.h
    user.cc
    user.h
config/
  statuses.json
tests/
  BUILD.bazel
  task_store_test.cc
  front_matter_test.cc
  status_config_test.cc
  command_test.cc
```

Use Bazel module dependencies through `bazel_dep` where available. For
dependencies without a suitable registry module, use `http_archive` in a
dedicated extension or `WORKSPACE.bazel` fallback if needed.

## Core Components

### `Task`

Owns strongly typed task data:

- Scalar strings for `id`, `title`, `assignee`, `priority`, `status`,
  `created_at`, `updated_at`, `created_by`, and `ci_status`.
- Numeric `story_points` constrained to `1`, `2`, `3`, `5`, `8`, `13`, `21`,
  or `100`, defaulting to `100`.
- `std::vector<std::string>` for `tags`, `branches`, and `prs`.
- String `body`.

It should provide:

- Load from parsed front matter plus body.
- Validate required fields.
- Save to YAML front matter plus Markdown body.
- Update `updated_at` on every save.
- Append comment in the documented format.

### `TaskStore`

Responsible for filesystem operations:

- Resolve task directory from project root.
- Create task directory for write commands.
- List task Markdown files.
- Find task file by ID using `TASK-NNN_*.md`.
- Generate the next ID.
- Create tasks with the standard body template.
- Save task files atomically where practical.

Atomic save plan:

1. Write to a temporary file in the task directory.
2. Flush and close it.
3. Rename over the target file.

On Windows, replacement semantics differ, so the Windows phase should verify and
adjust this behavior.

### `FrontMatter`

Responsible for:

- Splitting `---` front matter from the Markdown body.
- Parsing YAML with `yaml-cpp`.
- Emitting stable YAML.
- Converting YAML scalars and sequences into the `Task` model.

The emitter should prefer the documented scalar `ci_status: unknown` form, while
the loader should accept legacy list form and normalize it to a scalar.

### `StatusConfig`

Responsible for:

- Loading the default `config/statuses.json`.
- Sorting statuses by `order`.
- Implementing `can_transition(from, to)`:
  - Unknown source status allows movement.
  - Empty transition list allows movement to any status.
  - Otherwise destination must appear in the transition list.

Unknown destination statuses remain allowed when transition rules permit them,
matching the spec.

### `GitRunner`

Responsible for subprocess execution:

- Run `git status --porcelain -uall`.
- Run `git add` with changed task files only.
- Run `git commit -m MESSAGE`.
- Run `git rev-parse --is-inside-work-tree`.
- Run `git status --porcelain`.
- Run `git pull`.
- Run `git diff --name-only --diff-filter=U`.
- Run `git push`.

The macOS/Linux implementation can use `std::system` only as a temporary
fallback. Prefer a small subprocess abstraction using `popen` for captured
output and `fork`/`exec` or `posix_spawn` for argument-safe execution. Windows
support will need a `CreateProcess` implementation.

## Command Behavior Details

### `list`

- Read task files.
- Return JSON object mapping task IDs to `{ "file": "filename" }`.
- Prefer front matter IDs when files are parseable.
- Fall back to filename ID only for malformed files if doing so helps diagnose
  repository state; otherwise return an error.

### `task`

- Accept a single task ID.
- Print:

```text
TASK-001
BASE64_FILE_CONTENT
```

- Preserve complete file content in the Base64 payload.

### `create`

- Accept `BASE64_TITLE` and decode it before writing the task file.
- Generate ID using the improved max-ID strategy.
- Save default body:

```markdown
# Task title

## Description


## Checklist
- [ ] 

## Comments
```

### Field Updates

- Decode the incoming Base64 value.
- Load the task.
- Update the requested front matter field or body.
- For comma-separated list fields, split on commas, trim whitespace, and remove
  empty entries.
- Save the task and update `updated_at`.

### `move`

- Decode the new status.
- Load statuses.
- Validate transition with `can_transition`.
- Warn if destination is not a known status, but allow it when transition rules
  permit.
- Save the new status.

### `comment`

- Decode message.
- Generate UTC timestamp with a trailing `Z`.
- Read username from the OS.
- Insert immediately after `## Comments` when present.
- Append a new comments section otherwise.

### `publish`

- Ensure task directory exists.
- Use `git status --porcelain -uall`.
- Select changed `.md` files under the task directory.
- Stage only those files.
- Commit with `Publish tasks: TASK-001, TASK-002`.
- Do not push.

Care is needed when the task directory path is absolute but Git reports relative
paths. Normalize both paths relative to the Git work tree before filtering.

### `sync`

- Verify the current directory is inside a Git work tree.
- Refuse if `git status --porcelain` has any output.
- Run `git pull`.
- Check unresolved conflicts.
- Run `git push`.
- Print `Sync complete`.

### `batch`

- Load JSON from `BATCH_PATH`.
- Require each batch item to use `{"cmd": "...", "args": [...]}`.
- Execute commands in order against the same process context.
- Return one JSON result per attempted command with `cmd`, `ok`, `result`, and
  `error` fields.
- Base64 encode command output in `result` when a command produces output.
- Stop at the first failed command, include that failed command in the result
  list with `ok: false`, and return a non-zero process exit code.

## Testing Plan

### Unit Tests

- Front matter split and parse.
- YAML load/save round trips.
- Unknown front matter rejection.
- Legacy list-form `ci_status` normalization.
- Base64 encode/decode.
- Slug generation.
- Next task ID generation with gaps and deleted files.
- Status transition rules, including unknown source and empty transitions.
- Comment insertion with and without an existing comments section.

### Command Tests

Run commands against temporary directories:

- `create` writes expected filename, front matter, and body.
- `list` emits expected JSON.
- `task` returns Base64 file content.
- Field update commands decode and save values correctly.
- `move` enforces transitions.
- `batch` executes multiple updates in order.

### Git Tests

Use temporary Git repositories where available:

- `publish` stages and commits only task Markdown files.
- `publish` reports no unpublished task changes.
- `sync` refuses dirty work trees.

These tests can be enabled on macOS first and made conditional on `git` being
available in `PATH`.

## macOS First Milestones

1. Add Bazel module/build files and a minimal `gitboard` binary.
2. Add the default status configuration.
3. Implement `Task`, front matter parsing, Base64, time, and filesystem helpers.
4. Implement `TaskStore` and task lookup/create/save.
5. Implement `list`, `task`, `create`, update commands, `move`, and `comment`.
6. Add unit and command tests for local task operations.
7. Implement `batch`.
8. Implement `publish` and `sync` with a macOS subprocess runner.
9. Verify `bazel test //...` and manual CLI runs on macOS.

## Linux and Windows Follow-Up

### Linux

- Verify Bazel module dependency resolution.
- Run all tests.
- Confirm subprocess behavior and path normalization.
- Confirm username lookup and UTC timestamp behavior.

### Windows

- Add Windows-specific subprocess runner using `CreateProcess`.
- Review path handling for drive letters, backslashes, and Git porcelain output.
- Verify atomic save behavior.
- Ensure generated filenames avoid Windows-reserved characters and names.
- Run tests under Windows Bazel.

## Resolved Risk Decisions

- `create` takes `BASE64_TITLE`; the title is always decoded before use.
- `config/statuses.json` is part of the repository and is the initial workflow
  source.
- YAML compatibility is semantic, not byte-for-byte. Tests should verify parsed
  fields and body preservation rather than exact emitter formatting, except for
  externally specified CLI output.
- Subprocess execution is an explicit portability layer: use `posix_spawn` or an
  equivalent argument-safe POSIX implementation for macOS/Linux, and add a
  `CreateProcess` implementation for Windows.
- `batch` uses array arguments, stops at the first failed command, includes the
  failed command in the JSON result, and returns a non-zero process exit code.
- Desktop clients use native folder selection to register local Git
  repositories on macOS, Linux, and Windows. The wrapper obtains the selected
  path, while `gitboard-server` remains responsible for validation and
  persistence through `POST /api/repos`; browser-only use retains manual path
  entry.
- The Linux Flatpak grants `--filesystem=home` because Barnaby is a developer
  tool operating on local Git working trees. Native folder selection is for
  user convenience, not the current sandbox access boundary; narrower
  permissions may be evaluated later.

## Initial Acceptance Criteria

The macOS implementation is ready when:

- `bazel build //...` succeeds.
- `bazel test //...` succeeds.
- A user can create, list, read, update, move, comment on, and batch-update tasks
  in a temporary project.
- Task files remain compatible with the documented Markdown/YAML format.
- `publish` can commit changed task files without staging unrelated changes.
- `sync` performs the intended clean-worktree pull/conflict-check/push sequence.
