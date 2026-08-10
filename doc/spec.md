# GitBoard Application Behavior

GitBoard is a local, Git-backed task board. Tasks are Markdown files under a
`tasks/` directory, with YAML front matter for task metadata and a Markdown body
for human-readable content.

This document describes the behavior implemented by the Python code under
`gitboard/`, including CLI commands, TUI operations, shared data model behavior,
and differences between the command-line and TUI interfaces.

## Entry Point

Global option:

```text
--project-root PATH
```

When provided, GitBoard uses `PATH/tasks` as the task directory. Relative paths
are resolved from the current working directory. When omitted or empty, GitBoard
uses `tasks` relative to the current working directory.

The task directory is created automatically by operations that need it. When
GitBoard creates the task directory for the first time, it also copies the
bundled workflow status configuration to `tasks/statuses.json` so the repository
can override future bundled defaults, and creates `tasks/team.json` with the
default Agent Pip team member.

## Storage Model

### Task Directory

The task directory is represented by `gitboard.utils.TASKS_DIR`.

Default path:

```text
tasks/
```

With `--project-root /repo/example`:

```text
/repo/example/tasks/
```

Task lookup uses glob patterns against this directory. Commands generally look
for files named:

```text
TASK-<base64-guid>_*.md
```

### Task Files

Expected task filename:

```text
TASK-<base64-guid>_slugified-title.md
```

The filename is used for lookup, but the task ID and title displayed to the user
come from front matter.

Task front matter is passed directly into the `Task` dataclass. Unknown front
matter fields will make `Task.load()` fail unless the dataclass is extended.

Task fields:

- `id`: task identifier, for example `TASK-TWE8xG+sQ++7xv4ChVJvEg`
- `title`: task title
- `assignee`: assignee string, default `""`
- `priority`: priority string, default `"medium"`
- `story_points`: numeric estimate, one of `1`, `2`, `3`, `5`, `8`, `13`, `21`, or `100`; default `100`
- `tags`: list of strings, default `[]`
- `status`: workflow status key, default `"todo"`
- `created_at`: creation timestamp string, default current UTC ISO timestamp
- `updated_at`: update timestamp string, rewritten on every save
- `created_by`: creator string, default `""`
- `branches`: list of strings, default `[]`
- `prs`: list of strings, default `[]`
- `ci_status`: intended to be a string with default `"unknown"`
- `body`: Markdown body, not written as front matter

Current implementation detail: `ci_status` has a trailing comma in the dataclass
definition, so the class-level default is a one-item tuple. Created tasks
usually avoid this by not explicitly depending on the class default.

### Saving

Replaces `updated_at` with Python's `datetime.utcnow().isoformat()`.
Every save rewrites the full task file and may normalize YAML formatting.

### New Task Body

See task_format.md on details.
New tasks are created with this body shape:

```markdown
# Task title

## Description


## Checklist
- [ ] 

## Comments
```

### Comments

Format:

```markdown
- [2026-05-08T12:34:56.000000Z] username: comment text
```

Behavior:

1. Split the body into lines.
2. Generate a UTC timestamp with `datetime.utcnow().isoformat() + "Z"`.
3. Read the comment author from `git config user.name` for the selected
   repository. This uses the repository-local value when present and Git's
   global fallback otherwise. If Git does not return a user name, fall back to
   the local OS username.
4. If a line exactly matching `## Comments` after trimming exists, insert the
   new comment immediately after that header.
5. If no comments section exists, append a new `## Comments` section at the end.

Newest comments appear first when inserted into an existing comments section.

## Workflow Statuses

Statuses are loaded from `tasks/statuses.json` when present, otherwise from the
bundled `config/statuses.json`.

The loaded `STATUSES` mapping is used by board rendering, TUI status buttons,
and transition validation.

`can_transition(from_status, to_status)`:

1. If `from_status` is not defined, return `True`.
2. Read `STATUSES[from_status]["transitions"]`.
3. If the transitions list is empty, return `True`.
4. Otherwise return whether `to_status` is in the transitions list.

An empty transitions list means unrestricted movement, not no movement.
See statuses_config.md for more details.

The status configuration is also available through `gitboard statuses`, which
prints the loaded JSON configuration without dropping UI metadata such as
`display`, `order`, or `visible`.

## CLI Commands

### `gitboard team`
Signature:

```text
gitboard team
```

Prints the current repository's team members as JSON.

The command takes no arguments. It loads and returns the JSON document from
`tasks/team.json`.

If `tasks/team.json` does not exist, the command returns:

```json
{"team":[]}
```

When GitBoard initializes a missing task directory, it creates this default team
file:

```json
{"team":[{"alias":"agent_pip","email":"pip@barnaby"}]}
```

The file has this format:

```json
{
  "team": [
    {"alias": "moris", "email": "moris@domain.com"},
    {"alias": "alice", "email": "alice@domain.com"}
  ]
}
```

Each team member has:

- `alias`: short user-facing assignee value stored in task front matter,
- `email`: email address for display or future integrations.

Task assignment stores only the selected `alias` in the task's `assignee`
front matter field. The team file is the source for the list of known assignees
shown by clients.

Current behavior does not validate, normalize, or deduplicate team entries. The
command returns the JSON as-is when the file exists. Team editing and validation
will be added later.

### `gitboard statuses`
Signature:

```text
gitboard statuses
```

Prints the loaded workflow status configuration as JSON.

The command takes no arguments. It returns the same JSON document that GitBoard
loads from `tasks/statuses.json` when present, from `config/statuses.json`
otherwise, or from the built-in fallback configuration if no configuration file
is found. Clients should use this command when they need the status list,
display names, ordering, visibility flags, or transition rules.

### `gitboard list`
Signature:

```text
gitboard list
```
Prints a JSON-encoded map of tasks ids and file names based on the current tasks folder. The format is as follows:
```json
{
    "task_id_0": {
        "file": "file name",
    },
    "task_id_1": {
        "file": "file name",
    }
}
```

Current behavior is strict: if any task file cannot be parsed, `list` fails and
prints an error. A future tolerant task-loading mode may add a separate command
or option that enumerates task ids and filenames without parsing every task
file, so clients can show valid tasks even when one task file is malformed.

### `gitboard query`

Signature:

```text
gitboard query [OPTIONS]
```

Purpose: enumerate tasks matching optional text search and structured filters.

`query` returns the same JSON shape as `list`:

```json
{
    "TASK-TWE8xG+sQ++7xv4ChVJvEg": {
        "file": "TASK-TWE8xG+sQ++7xv4ChVJvEg_example.md"
    },
    "TASK-bVqQOaRcRp+oy4DPG4NnSw": {
        "file": "TASK-bVqQOaRcRp+oy4DPG4NnSw_other.md"
    }
}
```

Options:

```text
--text TEXT
--id TASK_ID
--title TITLE
--status STATUS
--assignee ASSIGNEE
--priority PRIORITY
--tag TAG
--branch BRANCH
--pr PR
--ci-status CI_STATUS
--created-by USER
```

Behavior:

1. Enumerate task Markdown files from the task directory, as `list` does.
2. If `--text` is present, read each complete task file as plain text and
   perform a case-insensitive literal substring search over the whole file,
   including YAML front matter, body, and comments.
3. If structured filters are present, load and parse each remaining task file
   and compare the requested fields.
4. Return only task ids and filenames, using the same output format as `list`.
5. Search and filters operate on the working tree, so unpublished task changes
   are included.

No regex mode is planned for the initial implementation.

Filter semantics:

- Different filter fields are combined with AND.
- Repeated values for the same field are combined with OR.
- Scalar fields match by exact value unless otherwise specified.
- List fields match when the task field contains the requested value.
- `--title` uses case-insensitive literal substring matching against the task
  title.

Examples:

```text
gitboard query --text login
gitboard query --status todo --status in_progress --assignee alice
gitboard query --tag frontend --priority high
```

### `gitboard task`
Signature:

```text
gitboard task TASK_ID [--frontmatter|--json]
```

Prints the requested task.

If the output format is omitted, `--frontmatter` is used for backwards
compatibility.

`--frontmatter` prints the existing two-line format:

```text
task id value
base64 encoded task file content
```

The Base64 payload is the complete Markdown task file, including YAML front
matter and body.

`--json` prints structured task data:

```json
{
  "id": "TASK-TWE8xG+sQ++7xv4ChVJvEg",
  "title": "Example task",
  "assignee": "alice",
  "priority": "medium",
  "tags": ["ui"],
  "status": "todo",
  "created_at": "2026-05-13T10:00:00",
  "updated_at": "2026-05-13T10:00:00",
  "created_by": "",
  "branches": [],
  "prs": [],
  "ci_status": "unknown",
  "body": "# Example task\n\n## Description\n"
}
```

Clients that need task metadata should prefer `--json` so they do not need to
parse YAML front matter in the browser.

### `gitboard create`

Purpose: create a new task file.

Signature:

```text
gitboard create BASE64_TITLE
```
The title is BASE64 encoded in the command line and decoded before it is written
to the task file.

Behavior:

1. Ensure the task directory exists.
2. Generate a globally unique task ID independent of the current repo state.
   The ID format is `TASK-` plus the 16 raw bytes of a UUID v4 encoded as
   unpadded base64, with `/` mapped to `-` for filename safety.
3. Decode the title from BASE64.
4. Slugify the title by lowercasing it, replacing whitespace and punctuation
   runs with hyphens, trimming leading/trailing hyphens, and using `task` if no
   filename-safe characters remain.
5. Create filename `TASK-<base64-guid>_slug.md`.
6. Initialize `tags`, `branches`, and `prs` as empty lists.
7. Create a `Task` with default status `todo` and the standard body template.
8. Save the task file.
9. Print a created message.

Important details:

- Task IDs must not be reused after files are deleted and must not depend on the
  largest task ID visible in the current checkout. New IDs are UUID v4 values
  encoded compactly from raw bytes instead of rendered as hexadecimal UUID
  strings. Legacy numeric IDs remain readable.
- Slugs should be ASCII-only for cross-platform filename safety. The original
  Unicode title remains in front matter and Markdown body.

### `gitboard body`

Purpose: update body of the task file.

Signature:
```text
gitboard body TASK_ID "BASE64 encoded new body content"
```
The new body content is BASE64 encoded in the command line. But it's presented as plain text in the file.

### `gitboard assignee`
Signature:
```text
gitboard assignee TASK_ID "BASE64 encoded assignee"
```
Updates the assignee field in the frontmatter header.
The new content is BASE64 encoded in the command line. But it's presented as plain text in the file.

### `gitboard branches`
Signature:
```text
gitboard branches TASK_ID "BASE64 encoded branches"
```
Updates the branches field in the frontmatter header.
The new content is a comma separated list of branch names BASE64 encoded in the command line. But it's presented as plain text in the file in a form of YAML array.

### `gitboard ci_status`
Signature:
```text
gitboard ci_status TASK_ID "BASE64 encoded ci_status"
```
Updates the ci_status field in the frontmatter header.
The new content is BASE64 encoded in the command line. But it's presented as plain text in the file.

### `gitboard priority`
Signature:
```text
gitboard priority TASK_ID "BASE64 encoded priority"
```
Updates the priority field in the frontmatter header.
The new content is BASE64 encoded in the command line. But it's presented as plain text in the file.

### `gitboard points`
Signature:
```text
gitboard points TASK_ID NEW_VALUE
```
Updates the `story_points` field in the frontmatter header. `NEW_VALUE` is a plain numeric value and must be one of `1`, `2`, `3`, `5`, `8`, `13`, `21`, or `100`.

### `gitboard prs`
Signature:
```text
gitboard prs TASK_ID "BASE64 encoded pull request list"
```
Updates the prs field in the frontmatter header.
The new content is a comma separated list of pull requests names BASE64 encoded in the command line. But it's presented as plain text in the file in a form of YAML array.

### `gitboard tags`
Signature:
```text
gitboard tags TASK_ID "BASE64 encoded tags list"
```
Updates the tags field in the frontmatter header.
The new content is a comma separated list of tag names BASE64 encoded in the command line. But it's presented as plain text in the file in a form of YAML array.

### `gitboard title`
Signature:
```text
gitboard title TASK_ID "BASE64 encoded title"
```
Updates the title field in the frontmatter header.
The new content is BASE64 encoded in the command line. But it's presented as plain text in the file.

### `gitboard move`

Purpose: change a task's status.

Signature:

```text
gitboard move TASK_ID NEW_STATUS
```
New status is BASE64 encoded in the command line but is presented in a plain text in the file.

Important details:

- Unknown destination statuses are allowed if transition rules permit them.
- If the source status is unknown, movement is always allowed.
- If the source status has an empty transition list, movement is always allowed.

### `gitboard comment`

Purpose: add a comment to a task.

Signature:

```text
gitboard comment TASK_ID MESSAGE
```
New message is BASE64 encoded in the command line but is presented in a plain text in the file.

### `gitboard publish`

Purpose: commit changed task database files to Git.

Signature:

```text
gitboard publish
```

Behavior:

1. Ensure the task directory exists.
2. Run `git status --porcelain -uall`.
3. Parse changed files whose path starts with the task directory path and ends
   with `.md` or `.json`.
4. If no changed task database files are found, print
   `No unpublished task changes`.
5. For Markdown files, extract task IDs from changed filenames by taking the
   part before the first underscore. JSON files are committed but are not
   included in the task ID list.
6. Build commit message:

   ```text
   Publish tasks: TASK-TWE8xG+sQ++7xv4ChVJvEg, TASK-bVqQOaRcRp+oy4DPG4NnSw
   ```

7. Run `git add` for the changed task database files.
8. Run `git commit -m COMMIT_MESSAGE`.
9. Print the number of task database files and the commit message.

Git failures are caught, printed, and converted to Typer exit status `1`.

Important details:

- Only `.md` and `.json` files under the task directory are staged.
- Non-task changes are left unstaged.
- The command does not push; use `sync` for pushing.
- Task ID extraction is filename-based, not front-matter-based.

### `gitboard sync`

Purpose: pull remote changes and push local commits.

Signature:

```text
gitboard sync
```

Intended behavior:

1. Verify the current directory is inside a Git work tree.
2. Refuse to continue if `git status --porcelain -uall` reports uncommitted
   `.md` or `.json` task database changes under the task directory.
3. Run `git pull`.
4. Check for unresolved merge conflicts with
   `git diff --name-only --diff-filter=U`.
5. If conflicts exist, print them and exit with status `1`.
6. Run `git push`.
7. Print `Sync complete`.

Current implementation details:

- `ensure_git_repo()` uses `subprocess.run(..., check=True)`. If the directory
  is not a Git repository, the subprocess exception is not converted into a
  friendly Typer error.
- `git_pull()` and `git_push()` use `check=True`, so failures raise
  `CalledProcessError` before the later `returncode` checks can run.
- `get_conflicted_files()` calls `run_git()`, which returns a string, then
  tries to read `result.returncode` and `result.stdout`. After a successful
  pull, this can raise `AttributeError` instead of returning conflicts.
- Non-task changes in the work tree do not block `sync`; Git itself may still
  reject `pull` if those changes conflict with incoming remote changes.

To reimplement the intended behavior, keep the operation sequence above but
return a subprocess result object from the conflict check or parse the string
directly.

### `gitboard dbstatus`

Purpose: report unpublished task database changes.

Signature:

```text
gitboard dbstatus
```

Behavior:

1. Ensure the task directory exists.
2. Run `git status --porcelain -uall`.
3. Parse added, deleted, and modified files whose paths are inside the task
   directory and end with `.md` or `.json`.
4. Return a JSON object with separate arrays for each change type:
   `added`, `modified`, and `deleted`.
5. If no matching changed files are found, return empty arrays:

```json
{
    "added": [],
    "modified": [],
    "deleted": []
}
```

6. For added or modified Markdown task files, extract the task ID from task
   front matter.
7. For deleted Markdown task files, extract the task ID from the deleted file
   name by taking the part before the first underscore.
8. For JSON files, use the file name as the identifier. Any changed `.json`
   file under the task directory is included.
9. Print the changed identifiers grouped by change type:

```json
{
    "added": ["TASK-TWE8xG+sQ++7xv4ChVJvEg"],
    "modified": ["TASK-bVqQOaRcRp+oy4DPG4NnSw", "team.json"],
    "deleted": ["TASK-vc7rrAr3T5+JjR1HVkA9ng"]
}
```

### `gitboard remotestatus`

Purpose: fetch remote refs and report task database changes that exist upstream
without pulling them into the working tree.

Signature:

```text
gitboard remotestatus
```

Behavior:

1. Verify the current directory is inside a Git work tree.
2. Resolve the current branch upstream with `@{u}`.
3. If no upstream is configured, return `available: false` and empty change
   arrays.
4. Run `git fetch --quiet`.
5. Compare `HEAD..@{u}` with `git diff --name-status --find-renames` limited to
   the task directory.
6. Return JSON with:
   - `available`: whether an upstream comparison was available,
   - `upstream`: the upstream ref name when known,
   - `added`: remote-added task Markdown file names,
   - `modified`: local task ids whose files changed upstream,
   - `deleted`: local task ids whose files were deleted upstream.

The command must not run `git pull` or change local task files.

## CLI Commands batching
It's possible to batch CLI commands. Instead of calling them one by one its possible to use the batch command.

### `gitboard batch`
Signature:

```text
gitboard batch batch_path
```
Batch_path specifies the path to the JSON-encoded batch file. The JSON file consists of an array of commands:
```json
{
    "batch":[
        {"cmd": "", "args": []},
        ...
    ]
}
```
Each command has the command name from the possible commands list. `args` is an
array of command arguments in the same order they would appear on the command
line, excluding the `gitboard` executable name and command name. BASE64-encoded
arguments remain BASE64 encoded in the batch file.

Example:

```json
{
    "batch": [
        {"cmd": "create", "args": ["V3JpdGUgdGhlIENMSSB0b29s"]},
        {"cmd": "comment", "args": ["TASK-TWE8xG+sQ++7xv4ChVJvEg", "UmVhZHkgZm9yIHJldmlldw=="]}
    ]
}
```

Commands are executed in order. Execution stops at the first failed command.

The return value is a JSON array with the similar structure:
```json
{
    "batch":[
        {"cmd": "", "ok": true, "result": "", "error": ""},
        ...
    ]
}
```
For each attempted command there will be an entry in the results. The `result`
field is empty if there was no output, or a BASE64 encoded string if the command
produced output. If a command fails, its result entry has `ok: false`, an empty
`result`, and a BASE64 encoded `error` message. Commands after the first failure
are not executed and do not appear in the result list.
