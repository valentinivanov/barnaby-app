# GitBoard Task File Format

GitBoard task files are Markdown files stored in the `tasks/` directory. Each file combines YAML front matter for machine-readable task metadata with a Markdown body for human-readable task details, checklist items, and comments.

## File Name

Task files use this naming pattern:

```text
TASK-<base64-guid>_slugified-title.md
```

Example:

```text
TASK-TWE8xG+sQ++7xv4ChVJvEg_implement-session-chat-api.md
```

The task creation command generates a UUID v4 independent of the current repo
state, encodes its 16 raw bytes as unpadded base64, maps `/` to `-` for filename
safety, and appends a slugified title. Existing lookup commands find tasks with
this glob pattern:

```text
TASK-TWE8xG+sQ++7xv4ChVJvEg_*.md
```

The filename is not the source of truth for task metadata. The `id` and `title` fields inside the front matter are loaded into the task model.

## Overall Structure

Each task file starts with YAML front matter delimited by `---`, followed by Markdown content:

```markdown
---
assignee: alice
branches: []
ci_status: unknown
created_at: '2025-12-26T21:13:37.942252'
created_by: ''
id: TASK-TWE8xG+sQ++7xv4ChVJvEg
priority: low
prs: []
status: review
tags:
- session
- core
title: Implement session chat API
updated_at: '2026-01-24T22:25:27.503914'
---

# Implement session chat API

## Description

Task description goes here.

## Checklist
- [ ] First item
- [x] Completed item

## Comments
- [2026-05-05T16:05:27.303790Z] username: Comment text
```

## Front Matter Fields

The Python model in `gitboard/task_model.py` loads YAML front matter with `frontmatter.load()` and passes all metadata keys into the `Task` dataclass. Unknown metadata keys are not accepted by `Task.load()` because they are passed directly as constructor arguments.

### `id`

Required string task identifier, such as `TASK-TWE8xG+sQ++7xv4ChVJvEg`.

Used by:

- `status` command: displayed in the task table.
- `board` command and TUI: displayed on task cards and detail views.
- `comment`, `move`, and `update` commands: task files are found by the user-provided ID in the filename, then the loaded `id` is used for messages.
- `publish` command: commit messages are based on task IDs extracted from filenames.

Recommended format is `TASK-` plus 22 base64 characters encoded from raw UUID
bytes. Older short IDs such as `TASK-004` remain valid.

### `title`

Required string task title.

Used by:

- task creation: also used to generate the Markdown heading and filename slug.
- `status` command: displayed in the task table.
- `board` command and TUI: displayed in task cards, lists, and detail views.

The Markdown `#` heading usually matches this field, but the code does not enforce that.

### `assignee`

String identifying the assigned user. Empty string means no assignee. Existing sample files also use `unassigned`.

Used by:

- `status` command: displayed in the task table.
- TUI task list and detail view: empty values are shown as `-`.
- task creation: accepted as an optional input.

### `priority`

String priority label. Default is `medium`.

Used by:

- `status` command: displayed in the task table.
- TUI task list and detail view.
- task creation: accepted as an optional input.

The code does not validate allowed priority values. Existing files use `low`, `medium`, and `high`.

### `story_points`

Numeric task estimate. Default is `100`, which represents an unestimated task.
Allowed values are `1`, `2`, `3`, `5`, `8`, `13`, `21`, and `100`.

Use a plain YAML number:

```yaml
story_points: 100
```

### `tags`

List of strings describing task categories or labels.

Used by:

- task creation: comma-separated input is converted into a YAML list.
- TUI detail view: displayed as a comma-separated list.
- TUI metadata editor: if the original YAML value is a list, it is edited as a comma-separated string and saved back as a list.

Use an empty list when there are no tags:

```yaml
tags: []
```

### `status`

String workflow state. Default is `todo`.

Used by:

- `status` command: displayed in the task table.
- `board` command and TUI: tasks are grouped by status.
- `move` command and TUI move action: updated through `Task.set_status()`, then saved.

Known statuses come from `config/statuses.json`:

- `backlog`
- `todo`
- `in_progress`
- `review`
- `done`
- `released`
- `archived`

The board command shows tasks with unknown statuses in an `Undefined` column. The TUI only builds status tabs for known statuses, so tasks grouped under unknown statuses are not reachable through normal status tabs.

Status transitions are enforced by `can_transition()` when moving a task. A status with an empty `transitions` list allows movement to any status. A task already in an undefined status is also allowed to move out to any status.

### `created_at`

String creation timestamp. Generated with `datetime.utcnow().isoformat()`.

Used by:

- TUI detail view: displayed as the created timestamp.

The current code treats it as a string and does not parse or validate it. Existing files use ISO-like timestamps without a timezone suffix.

### `updated_at`

String last-updated timestamp. `Task.save()` replaces this value with the current UTC `datetime.utcnow().isoformat()` every time the task is saved.

Used by:

- TUI detail view: displayed as the updated timestamp.

Manual edits to this field will be overwritten by the next model save.

### `created_by`

String identifying the creator. Empty string means unknown or unset.

Used by:

- TUI detail view: displayed as created by, with empty values shown as `-`.

Task creation currently leaves this empty.

### `branches`

List of strings for related Git branch names.

Used by:

- TUI metadata editor: displayed and editable because it is a front matter field.

No current command otherwise reads branch values. Use an empty list when there are no branches:

```yaml
branches: []
```

### `prs`

List of strings for related pull request references.

Used by:

- TUI metadata editor: displayed and editable because it is a front matter field.

No current command otherwise reads PR values. Use an empty list when there are no PRs:

```yaml
prs: []
```

### `ci_status`

CI state for the task. The dataclass default is intended to be `unknown`, and current sample files contain both scalar and list forms:

```yaml
ci_status: unknown
```

or:

```yaml
ci_status:
- unknown
```

No current command displays or validates this field, except the TUI metadata editor. Prefer the scalar string form for consistency with the dataclass annotation.

## Markdown Body

The task body is everything after the closing front matter delimiter. It is stored as `Task.body`.

New tasks are created with this body shape:

```markdown
# Task title

## Description


## Checklist
- [ ] 

## Comments
```

### Heading

The first Markdown heading is normally the task title:

```markdown
# Implement login API
```

The code does not require this heading to match the front matter `title`. The TUI detail view prepends its own metadata heading and then renders the body below it.

### `## Description`

Marks the beginning of the editable description area in the TUI.

The TUI extracts the editable body content starting at the `## Description` line and ending before `## Comments`. This means the description editor includes both the description section and the checklist section.

If `## Description` is missing, the TUI treats the body from the beginning up to `## Comments` as editable content.

### `## Checklist`

Contains Markdown task list items:

```markdown
- [ ] Open item
- [x] Completed item
```

The current Python code does not parse checklist items. They are plain Markdown content edited as part of the TUI description area.

### `## Comments`

Marks the comments section. Comments are handled specially by the CLI and TUI.

The `comment` command and TUI comment action insert new comments immediately after the `## Comments` header. Newest comments therefore appear first.

Comment format:

```markdown
- [timestampZ] username: message
```

Example:

```markdown
- [2026-05-05T16:05:27.303790Z] vivanov: some comment
```

The timestamp is generated with `datetime.utcnow().isoformat() + "Z"`. The username comes from `getpass.getuser()`.

If `## Comments` is missing, `append_comment()` appends a new comments section to the end of the body.

The TUI shows non-empty lines after `## Comments` in the comments panel. It does not parse the timestamp or username.

## Saving Behavior

All saves through `Task.save()` rewrite the file using `python-frontmatter` and update `updated_at`.

The saved front matter contains every dataclass field except `body`. This means standard task files should keep metadata aligned with the `Task` dataclass fields:

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

Because metadata is passed directly into the dataclass, adding arbitrary extra front matter fields will currently make `Task.load()` fail unless the model is updated to accept them.
