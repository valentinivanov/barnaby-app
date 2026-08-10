# GitBoard Web UI Specification

The GitBoard server serves the Web UI as embedded static resources. The UI uses
the server's REST API to manage registered repositories and to run GitBoard
batch commands.

This document describes the first small implementation scope. The UI should be
easy to extend gradually as the server API and GitBoard workflows grow.

## Technology Choice

Use **Preact with TypeScript**, bundled into static JavaScript and CSS files.

Rationale:

- Preact is small enough for an embedded local web app.
- It provides a React-like component model without committing the project to a
  large frontend runtime.
- TypeScript gives useful structure for API request and response types,
  especially once `doc/server_openapi.yaml` is used to generate or validate
  client models.
- The app can be built into plain static assets under `src/server/assets`, then
  embedded by the existing Bazel asset generation step.
- The UI is interactive and stateful, but not graphics-heavy; Preact is a good
  fit for this level of richness.

Avoid heavy UI frameworks and graphics libraries for the initial version. The
visual language should be text-first, with Unicode symbols where useful and no
dependency on image icons.

## Visual Direction

The UI should feel like a terminal-style control surface adapted for the web:
compact, text-oriented, fast, and predictable. It should avoid decorative
graphics and should not require images or icon sets.

Use:

- plain text labels,
- monospace or terminal-adjacent typography where it improves scanning,
- restrained color for status and errors,
- Unicode symbols for simple actions when they remain clear,
- keyboard-friendly controls where practical.

## Layout

The main page is a single-page application with two persistent regions:

- a left side panel,
- a main canvas.

The side panel should take roughly 20% of the width on desktop. This value
should be easy to adjust. On narrow screens, it may collapse into a top-level
navigation panel in a later iteration; mobile layout is not part of the first
scope.

## Side Panel

The side panel contains:

- `Start`
- registered repositories
- `Settings`

Each registered repository is shown as:

```text
repo_id [folder-name]
```

`repo_id` is the configured repository id. `folder-name` is the final path
component of the configured repository path.

Selecting an item changes the main canvas content without reloading the page.

The side panel is collapsible.

In expanded mode, it shows the full `Barnaby` title and the full navigation
labels described above.

The collapse/expand control lives in the contextual top line, not inside the
side panel. The side panel state should persist across page refreshes using
`localStorage`.

In collapsed mode:

- the brand is shown as an uppercase `B` using a CSS-only style that differs
  from the normal text face, with subtle decoration such as weight, spacing, or
  border treatment,
- `Start` is shown as `⌂` (`U+2302`) with tooltip `Start`,
- each registered repository is shown as its 1-based index in the sorted
  repository list,
- each repository item has a tooltip with the full `repo_id [folder-name]`
  label,
- `Settings` is shown as `⚙` (`U+2699`) with tooltip `Settings`.

The collapsed panel should keep the same item order and selection state as the
expanded panel. Expanding or collapsing the panel must not change the selected
view or reload the page.

## Main Canvas

The main canvas displays the view selected in the side panel.

It also contains a contextual top line for:

- view-specific actions,
- short status messages,
- validation errors,
- server errors.

Contextual actions should appear directly after the title and status text, not
at the far right of the top line. The actions should be visually grouped in a
subtle framed toolbar area with a slightly different background.

Errors should be shown in a clear error color and should not replace the entire
page.

## Start View

The `Start` view shows a tile grid of registered repositories.

Each repository tile shows:

- repository id,
- full repository path,
- a delete action.

Task counts are intentionally omitted from the first version. They can be added
later by calling `/batch` with the repository id and a `list` command.

Adding a repository is done from the contextual top line. The Start view should
not include a large `+` tile.

Deleting a repository requires confirmation. After confirmation, the UI calls:

```text
DELETE /api/repos/{id}
```

On success, the repository list and Start view refresh.

## Add Repository View

The Add Repository view lets the user register a new repository.

Fields:

- repository id,
- selected repository path,
- repository path as a text-field fallback when no native picker bridge is
  available.

### Repository Selection Decision

The Add Repository view provides a `Choose...` action for repository
registration. Selecting a local repository is a core desktop workflow, not a
Linux-specific sandbox workaround.

The embedded web UI calls `POST /api/repos/select-path` when this action is
available. The server opens the native folder dialog, validates the selected
directory, and returns a canonical absolute path to the Add Repository view.
The UI then submits that path through the normal `POST /api/repos` API with the
repository id. The add endpoint remains responsible for validation and
persistence.

For the Linux Flatpak, native folder selection is a usability feature; the
current sandbox policy grants access to the user's home directory so Barnaby
and its bundled Git executable can reopen and modify selected repositories.

When the UI is served in an ordinary external browser, or a wrapper does not
support native folder selection, the repository path text field remains
available. The server validates that the submitted path exists and points to a
Git repository or a directory inside a Git work tree.

Actions in the contextual top line:

- `Add`
- `Back`

The `Add` action is disabled until:

- the repository id is present,
- the repository id matches `[a-z0-9_]+`,
- the repository path is present.

If the repository id is invalid, show an error in the contextual top line.

When the user activates `Add`, the UI calls:

```text
POST /api/repos
```

with:

```json
{
  "id": "work",
  "path": "/Users/alex/projects/work-repo"
}
```

On success, the UI returns to the Start view and refreshes the repository list.
On failure, the server error is shown in the contextual top line.

## Settings View

The first Settings view is read-only unless the server API is extended.

It should show:

- server URL,
- configuration path, if exposed by the API in the future,
- API/schema version, if exposed by the API in the future.

The earlier idea of controlling the port or restarting the server from the UI is
out of scope for the initial implementation because the current server API does
not provide those controls.

## Tasks View

The Tasks View presents tasks for the selected repository. The repository can be
selected from the side panel or from the Start view by clicking a repository
tile.

The UI uses the `/batch` API to:

- load known assignees with `team`,
- load workflow statuses and allowed transitions with `statuses`,
- enumerate tasks with `list`,
- request task details with `task TASK_ID --json`,
- apply changes through the existing CLI commands.

Selecting a repository starts the task loading flow. The first request should be
a batch containing `team`, `statuses`, and `list`.

The `team` command result is Base64-encoded in the batch response. After
decoding, it contains the repository team configuration from `tasks/team.json`.
The UI should use `team.team[].alias` values as the available assignees. Task
assignment stores the selected alias in the task's `assignee` field.

The assignee selector should always include an `unassigned` option. Selecting
that option stores an empty string in the task's `assignee` field, but the UI
displays it as `unassigned`.

The `statuses` command result is Base64-encoded in the batch response. After
decoding, it contains the status configuration used by the CLI, including
display names, ordering, visibility flags, and transition rules. The UI should
load statuses once when the repository is opened, then reuse the local status
model until the selected repository changes or the page is refreshed.

Status colors are local UI presentation. The statuses configuration does not
define colors; the UI should assign colors from a fixed palette by status order.

The `list` command returns the task ids and filenames. The UI should then load
task details with batched `task TASK_ID --json` commands. To keep each request
bounded, load details in pages of at most 25 `task` commands per batch.

### Search And Filtering

The Tasks View should support task search and structured filtering without a
permanent index.

The default repository load should continue to use `list`. When the user enters
a search pattern or applies any filter, the UI should use `query` instead of
`list` to enumerate matching task ids and filenames, then load details for the
matching ids with batched `task TASK_ID --json` commands.

The search box should map to:

```text
gitboard query --text TEXT
```

Search and filter controls should live in a collapsible panel between the
contextual top line and the task list or board. The panel should span the main
content width, excluding the left side panel. The contextual top line should
show a search toggle immediately before the List/Board segmented control. The
toggle label is only `🔍` (`U+1F50D`). When the panel is open, the toggle uses
the same active green background as the selected List/Board segment; when the
panel is closed, it returns to the default toolbar button background.

Closing the search panel clears the search/filter draft state and resets the
task results to the unfiltered `list` view.

Text search is case-insensitive literal substring search over the whole task
file. The UI should not expose regex search in the initial implementation.

The first filter set should map to the fixed fields supported by `query`:

- title,
- status,
- assignee,
- priority,
- tag,
- created by.

Full text search and field-based search are mutually exclusive. The full text
field appears on a distinct background beside the field filters and has a
checkbox label `Full text search`. Entering full text automatically checks it
and clears all field-filter checkboxes. Checking full text manually also clears
all field-filter checkboxes.

Each field filter has a small checkbox showing whether that field participates
in the query. Editing a field automatically checks it and unchecks full text
search. Clearing search resets all checkboxes. Users may also check or uncheck
fields manually. When a checkbox is active, that label uses white text on the
same green active background used elsewhere in the toolbar.

The Search and Clear buttons should be grouped at the right edge of the search
panel.

Issuing a new search while an Inspect Task pane is open should close the
inspector automatically. Issuing a new search while an Edit Task pane is open
should show a confirmation. If the user confirms, close the editor and run the
search; if the user cancels, keep the editor open and do not run the search.

The UI should combine filters using the command semantics:

- different fields are ANDed,
- repeated values for the same field are ORed.

For the initial UI, known-value filters such as status, assignee, and priority
may be single-select controls. Free-form filters may use comma-separated values
to produce repeated `query` options for the same field.

When search or filters are active, the loading states should use search-specific
copy such as `Searching tasks...` and `12 matching tasks loaded`. Empty search
results should show a neutral empty state, not a loading error.

Search and filtering should operate on the repository working tree, so
unpublished task edits can appear in results before they are published.

The client should keep a local task model while the view is open. When a task is
edited, the UI should update persistent state through the server first, then
refresh or patch the local model after the server succeeds. The server and CLI
remain the source of truth, but mode switching, search, and filtering should use
the local model.

### Task Publishing

The Tasks View should track unpublished task database changes with
`gitboard dbstatus`.

The UI should append `dbstatus` as the final command in batches that can change
or refresh repository state:

- the initial repository load batch containing `team`, `statuses`, and `list`,
- task edit operation batches,
- status move batches,
- create task batches,
- manual refresh batches,
- publish action batches.

Do not append `dbstatus` to every task-detail page during initial loading. The
first load batch is enough to establish publishing state while the task details
continue loading.

When a batch response is received, the client should decode the final
`dbstatus` result and store it in local publishing state:

```json
{
  "added": ["TASK-001"],
  "modified": ["TASK-002", "team.json"],
  "deleted": ["TASK-003"]
}
```

The Tasks View should show a `Publish` action in the contextual top line next to
the selected repository name when any `added`, `modified`, or `deleted` entry is
present. The action should be visually split into two clear parts:

- a primary label, `Publish`,
- a compact change summary, for example `2 changed` or `1 added / 1 deleted`.

The change summary should expose a compact tooltip or popover listing the
changed task ids and JSON files grouped by change type.

Task tiles whose task ids appear in `added` should be highlighted with a subtle
green frame. Task tiles whose task ids appear in `modified` should be
highlighted with a subtle amber frame. Deleted task ids do not have tiles after
refresh, so the UI should report them in the publish summary and tooltip only.

Activating `Publish` should run a batch containing:

```json
{
  "batch": [
    {"cmd": "publish", "args": []},
    {"cmd": "sync", "args": []},
    {"cmd": "dbstatus", "args": []}
  ]
}
```

Because publish and sync may take noticeable time, the Tasks View should enter a
single publishing state while the batch request is in flight. During publishing:

- disable the `Publish` action,
- show progress text in the contextual top line, such as `Publishing...`,
- keep the task list visible,
- prevent conflicting task mutations until the batch completes.

On success, the UI should decode the final `dbstatus` result and update the
publishing state. If all arrays are empty, hide the publish action and remove
task tile highlights. On failure, keep the previous publishing state visible and
show the CLI or server error in the contextual top line.

If a batch fails before `dbstatus` runs, the UI should keep the previous
publishing state but mark it as stale in the contextual top line, because the
local publishing state may no longer reflect the repository.

When the browser window regains focus while a repository is selected, the UI
should refresh publishing state by running a small batch containing only
`dbstatus`. This catches task database changes made outside the Web UI.

### Remote Task Changes

The Tasks View should also check for upstream task changes without pulling them
by running `gitboard remotestatus`.

The initial repository load and manual refresh batches should include
`remotestatus` after `dbstatus`. While a repository is open, the UI should
periodically run `remotestatus` and should also run it when the window regains
focus.

When upstream task changes are reported, the UI should:

- show a yellow status message saying `Tasks in repo have changed. Use Git or
  your Git client to sync your local repository.`,
- add a red outline to the Refresh button,
- make locally loaded cards whose task ids are modified or deleted upstream
  semitransparent and non-interactive,
- show remote-added task Markdown files as semitransparent Backlog placeholder
  cards using the file name as the visible text and no task id in the colored
  header.

Barnaby should not run `git pull` automatically as part of this check.

### Task Loading States

The Tasks View should show loading progress in the view body, not only in the
contextual top line.

The loading flow should have distinct states:

- loading workflow statuses,
- loading the task index,
- loading task details, with progress such as `25/80 tasks loaded`,
- loaded with tasks,
- loaded with no tasks,
- failed.

When loading fails, the view body should show a compact failure panel with:

- the error message,
- the repository id,
- a `Retry` action that restarts the full loading flow,
- a `Back` action through the contextual top line.

The empty state should be different from the failure state. A repository with no
tasks should show a neutral empty message, not an error.

The UI may continue to show already loaded local tasks while a manual refresh is
running. If the refresh succeeds, replace the local model. If it fails, keep the
previous local model and show the refresh error in the contextual top line.

### Task Loading Errors

Initial implementation may fail the whole Tasks View if any required loading
step fails. This includes `statuses`, `list`, invalid JSON, or any `task
TASK_ID --json` request.

A later tolerant loading mode should isolate invalid task files. In that mode,
the UI should show successfully loaded tasks and a warning listing task ids that
failed to load. This requires CLI support for enumerating task ids without
parsing every task file, or a batch mode that can continue after per-task
failures.

### Task Action States

Task-changing actions should have local pending states.

For status changes in Board Mode:

- disable only the changed task tile's status changer while the `move` command
  is running,
- keep the rest of the view interactive,
- optimistically update the local task status only if rollback behavior is
  implemented,
- roll back the local task status and show the CLI error if the command fails,
- show a short success or failure message in the contextual top line.

The Tasks View should also expose a clear `Refresh` action and, after a
successful load, may show the last loaded time in the contextual top line or a
small view header.

### Task List Modes

The Tasks View should support two modes.

Task tiles in both modes should include an assignee changer control. It should
use the same interaction pattern as the status changer control: a compact
selector on the tile. The option list comes from the `team` command loaded
during initial repository loading and refreshed by the `Refresh` action, plus
the built-in `unassigned` option.

When the user selects a new assignee, the UI calls the `assignee` command
through `/batch`, passing the selected alias as a Base64-encoded argument. On
success, the local task model is updated. On failure, the UI rolls back the
local assignee value and shows the CLI error in the contextual top line.

#### List Mode

List Mode shows tasks as tiles. Each tile contains:

- task id,
- title,
- assignee,
- status.

The layout should be similar to the repository tiles in the Start view. Clicking
a task tile opens the Inspect Task View. Status names should use color-coded
backgrounds so users can recognize task state at a glance.

The task id occupies the top line. Inspect, edit, and status controls are shown
under the task title and aligned to the left.

#### Board Mode

Board Mode groups task tiles into columns by status. Columns should be ordered
by the status configuration. Statuses with `visible: false` should be hidden by
default unless they contain tasks.

Each column has a header with the status display name and a color-coded
background. Each tile contains:

- task id,
- title,
- assignee,
- status changer control.

The task id occupies the top line. Inspect, edit, and status controls are shown
under the task title and aligned to the left.

Columns should be wide enough to show task tiles without horizontal scrolling
inside each column. If the full board does not fit, the main canvas may scroll
horizontally or vertically.

Clicking a task tile opens the Inspect Task View.

Each task tile should include a status changer control. Activating it should
show the valid destination statuses from the status configuration. After the
user selects a new status, the UI calls the `move` command through `/batch`; on
success, the local task model is updated and the task moves to the matching
status column.

### Mode Switcher

The context line should contain a compact mode switcher for `List` and `Board`.
A segmented control is preferable to radio buttons because it fits the
application toolbar style and keeps the control compact.

## Create Task View

Create Task is a separate screen opened from a `Create Task` action in the
contextual top line. The action should be available in the Tasks View regardless
of whether the current display mode is List or Board. The view uses a regular
form layout.

The form exposes:

- `title`: string, required,
- `assignee`: selector based on `team`, default `unassigned`,
- `priority`: selector with `high`, `medium`, and `low`, default `medium`,
- `story_points`: selector with `1`, `2`, `3`, `5`, `8`, `13`, `21`, and
  `100`, default `100`,
- `status`: selector based on statuses, default `backlog`,
- `tags`: list of strings,
- `branches`: list of strings,
- `prs`: list of strings,
- `body`: multiline Markdown text containing the full task body except the
  comments section.

List-of-string fields should render as token containers. Each token has a
slightly tinted background and wraps naturally at the end of the line. Each
token has a red `x` button at the end to remove it. The container contains an
inline text field followed by a framed arrow action such as `↵` (`U+21B5`) to
add the typed value. Pressing Enter in the text field performs the same action.

Token values must not contain commas because the current CLI stores these
fields as comma-separated lists. Tags must contain lowercase letters `a-z`
only. Branch and PR values must be valid path-like strings from a formatting
point of view: no empty path segments, no leading slash, no `.` or `..`
segments, no commas, and only path-safe characters.

The body field should support Markdown entry. It should have an
`Edit`/`Preview` switch. `Edit` shows a multiline text control. `Preview` shows
the rendered Markdown in a read-only area.

Create Task does not include comment editing. Comments can be appended later
from the Edit Task pane.

The contextual top line shows `Create` and `Back`.

`Back` returns to the previous screen. If the form has unsaved input, the UI
should ask for confirmation before leaving.

`Create` is disabled until the required fields are valid. When activated, it:

1. Calls `create BASE64_TITLE`.
2. Parses the created task id from the command output.
3. Builds a batch of update commands for every non-default field:
   `assignee`, `priority`, `move`, `tags`, `branches`, `prs`, and `body`.
4. Executes the update batch.
5. On success, returns to the task list and refreshes it.
6. On failure, stays on the Create Task screen and shows the error.

This flow is intentionally non-atomic in the first version. If the initial
`create` command succeeds but a later update command fails, the newly created
task remains in the repository with whatever fields were already saved. The UI
should keep the user on the Create Task screen and show the error so the user
can refresh and edit the partial task if needed.

The UI should always move the newly created task to `backlog` after creation.
The CLI may create tasks with a different default status, but the Create Task
form owns the UI default.

The body sent through the `body` command should preserve the standard task body
shape without embedding comments: title heading, `## Description`, body
Markdown, and `## Checklist`.

## Edit Task View

Edit Task uses the same field set and controls as Create Task, but it is shown
as a side pane instead of a separate full-screen view.

Each task tile should have an edit button using `✎` (`U+270E`) inside a small
frame. Activating it opens the edit pane for that task.

When the edit pane is open, the main layout changes:

- the task list or board remains on the left,
- the right third of the main canvas is occupied by the edit pane,
- the pane top is flush with the task list or board content,
- the pane is pre-filled with the selected task's current data.

The contextual top line does not show Tasks View actions while the edit pane is
open. This applies whether Edit Task was opened directly from a task tile or
from Inspect Task View. Task count and last-loaded metadata should appear in
the contextual top line next to the repository id instead of above the task
list or board.

The edit pane header contains:

- the task id,
- a framed save button using `✓` (`U+2713`),
- a framed close button using `×` (`U+00D7`).

`Save` is disabled when there are no changes. When fields change, the pane-local
save button becomes enabled. Activating `Save` sends the changed fields through
`/batch`, disables the button while the request is running, updates the local
task model on success, and leaves the pane open.

`Close` closes the edit pane. If the pane contains unsaved changes, the UI asks
for confirmation before closing.

If the user opens another task while the edit pane has unsaved changes, the UI
asks for confirmation. If confirmed, the pane is repopulated with the newly
selected task data.

Existing comments are read-only in the first version. The edit pane may show
them for context, but it should only allow appending new comments. Editing or
deleting existing comments is out of scope because the CLI does not support it.

If saving fails, keep the pane open, preserve the user's edits, and show the
error in the contextual top line or inside the pane.

The edit pane should send only changed fields. Field-to-command mapping:

- title: `title TASK_ID BASE64_TITLE`,
- assignee: `assignee TASK_ID BASE64_ALIAS`,
- priority: `priority TASK_ID BASE64_PRIORITY`,
- status: `move TASK_ID BASE64_STATUS`,
- tags: `tags TASK_ID BASE64_COMMA_SEPARATED_TAGS`,
- branches: `branches TASK_ID BASE64_COMMA_SEPARATED_BRANCHES`,
- prs: `prs TASK_ID BASE64_COMMA_SEPARATED_PRS`,
- body without comments: `body TASK_ID BASE64_BODY`,
- new comments: one `comment TASK_ID BASE64_COMMENT` command per comment.

## Inspect Task View

Each task tile should have an inspect button next to the edit button. Use `👀`
(`U+1F440`) for the inspect button.

Activating the inspect button opens the task in a side pane using the same
placement and sizing as the Edit Task View. Inspect Task View contains the same
task data as Edit Task View, but all fields are read-only and no inline editing
controls are shown.

The body field is always shown in Markdown preview mode. It should render the
task body without the comments section, matching the body scope used by Edit
Task View.

Existing comments are read-only. Inspect Task View does not provide a comment
input because comments are a task mutation and belong in Edit Task View.

The inspect pane header contains:

- the task id,
- a framed edit button using `✎` (`U+270E`),
- a framed close button using `×` (`U+00D7`).

Activating the edit button converts the pane into the full Edit Task View for
the same task. Activating the close button closes the pane.

The contextual top line keeps the normal Tasks View actions visible while
Inspect Task View is open.

Opening Inspect Task View while Edit Task View has unsaved changes should use
the same confirmation behavior as opening another task for editing. If the user
confirms, the dirty edit state is discarded and the inspect pane opens. If the
user cancels, the current edit pane remains unchanged.

## Asset Integration

Web UI source files live under `src/server/assets`.

The build should produce static files such as:

- `index.html`,
- `app.js`,
- `app.css`,
- optional future images or fonts.

The existing Bazel asset generation step embeds those files into the
`gitboard-server` executable. HTML files are served from the server root and
non-HTML files are served under `/static/`.
