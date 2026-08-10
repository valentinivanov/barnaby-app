# GitBoard Server Specification

`gitboard-server` is a separate long-running executable that runs on the user's
machine. It provides a local HTTP API and static web UI for the existing
`gitboard` command-line application.

The server is intentionally an adapter around the CLI. Task behavior, task file
format, command validation, and command output remain owned by `gitboard`.

## Goals

- Serve the GitBoard web client from the local machine.
- Provide an HTTP endpoint that forwards batch command requests to the
  `gitboard` CLI.
- Store and manage a local mapping from user-defined repository ids to Git
  repository paths.
- Serialize request handling so only one API request can mutate configuration
  or invoke `gitboard` at a time.
- Enforce a single running `gitboard-server` instance per user configuration.
- Provide an OpenAPI schema for the REST API so web client code can be
  generated or validated from the contract.

## Non-Goals

- Reimplement GitBoard task operations in the server.
- Expose the server as a network service for other machines by default.
- Provide multi-user authentication or remote collaboration.
- Store request-specific task context such as the repository selected by the
  web client.

## Process Model

`gitboard-server` is a persistent process. It starts an HTTP listener, serves
requests until terminated, prints the web UI URL on startup, and opens that URL
in the user's default browser.

Only one `gitboard-server` instance may run for the same user configuration at
a time. On startup, the server must acquire an inter-process lock in the
configuration directory before opening the listening socket. If another server
instance already holds the lock, startup must fail with a clear error message.
The implementation should use a platform file lock or another OS-backed
inter-process mutex; a plain PID file without locking is not sufficient.

By default the server listens on `127.0.0.1:8080`. The port can be overridden:

```text
gitboard-server --port 12345
```

Passing `--port 0` directs the operating system to choose an available loopback
port. Desktop wrappers should use this mode instead of probing and releasing a
candidate port before launching the server.

On startup it prints:

```text
GitBoard server listening at http://127.0.0.1:8080/
GITBOARD_SERVER_URL=http://127.0.0.1:8080/
```

The `GITBOARD_SERVER_URL` line is a machine-readable startup contract. When
`--port 0` is used, it includes the actual port selected by the operating
system.

By default the server also opens that URL in the user's default browser. Browser
launch can be disabled for scripts, tests, and non-interactive sessions:

```text
gitboard-server --no-open-browser
```

The configuration directory can be overridden for tests and development runs:

```text
gitboard-server --config-dir /tmp/gitboard-server-dev
```

## Request Serialization

The server must not process API requests in parallel. It must serialize all
requests that read or write configuration or invoke `gitboard`.

This requirement exists because the current CLI and repository file operations
are not specified as safe for concurrent execution. In particular:

- only one `/batch` request may run at a time,
- configuration reads and writes must not interleave with each other,
- configuration changes must not interleave with `/batch` repository
  resolution, and
- the server must never launch multiple `gitboard` child processes
  concurrently.

Static asset requests may be served independently, but API request handling must
pass through a single request queue or a process-wide mutex. Requests waiting for
the lock should remain pending until the current API request completes, unless a
future timeout policy is explicitly added.

## Static Web Content

The server serves bundled static assets for the web client.

- `GET /` returns the main HTML page.
- `GET /index.html` returns the main HTML page.
- `GET /static/**` returns bundled static assets such as JavaScript, CSS,
  images, and fonts.

Static content must be bundled with the server executable or build outputs. The
source files live under `src/server/assets`. A pre-build step enumerates that folder
and generates `assets.cc` with byte arrays and a `find_asset()` implementation.
Each embedded asset should include its URL path, content type, byte content, and
content length.

HTML files are served from the server root. For example,
`src/server/assets/index.html` is available as `/` and `/index.html`, while
`src/server/assets/about.html` is available as `/about.html`. Non-HTML files are
served under `/static/` using their relative path, for example
`src/server/assets/app.css` is available as `/static/app.css`.

The implementation must not depend on the current working directory to locate
the web client files.

## Repository Configuration

The server stores a mapping from user-defined repository ids to local Git
repository paths. Repository ids are strings chosen by the user and must match
`[a-z0-9_]+`: lowercase ASCII letters, digits, and underscores only. Repository
paths are absolute filesystem paths.

The server does not store an active repository id. Each request that operates on
a repository must include the repository id explicitly.

### Configuration Storage

Configuration is stored in a per-user application data directory:

- macOS: use the platform application support directory when available.
- Linux: use the XDG config directory when available.
- Windows: use the platform application data directory when available.
- Fallback: `~/.gitboard-server`.

The configuration file should be JSON. A recommended shape is:

```json
{
  "repositories": {
    "work": "/Users/alex/projects/work-repo",
    "personal": "/Users/alex/projects/personal-repo"
  }
}
```

Configuration writes must be atomic: write to a temporary file in the same
directory, then rename it over the existing configuration file.

### Repository Validation

When adding a repository, the server should validate that:

- the id is non-empty,
- the id contains only lowercase ASCII letters, digits, and underscores,
- the path exists,
- the path is a directory,
- the path is a Git repository or is inside a Git work tree.

If validation fails, the server returns an error and does not update the
configuration.

### Desktop Repository Selection Decision

The server offers native folder selection through `POST /api/repos/select-path`
when registering a repository. The endpoint returns either a canonical absolute
path that has already passed repository-path validation, or a cancellation
response if the user closes the dialog.

After a path is selected, the web UI sends that path to `POST /api/repos` with
the user-chosen repository id. The add endpoint remains authoritative for
validating the id, revalidating the path, and saving the repository mapping.
Browser-only clients may continue to submit a manually entered path through the
same add endpoint.

For sandboxed Linux distribution, Barnaby currently grants home-directory
filesystem access to support reliable use of local Git working trees and
bundled Git operations across launches. Native folder selection improves the
registration experience but is not the current sandbox permission boundary.
This policy may be tightened in a future release.

## HTTP API

The HTTP API is REST-style. New endpoints should model resources and state
transitions with HTTP methods rather than command names in the URL. For example,
use `POST /api/repos` instead of `/config/add`.

All API responses use `application/json`.

Successful responses use HTTP `2xx`. Failed requests use a suitable `4xx` or
`5xx` status with this shape:

```json
{
  "ok": false,
  "error": "human-readable error message"
}
```

### `GET /api/config`

Returns all configured repositories.

Response:

```json
{
  "ok": true,
  "repositories": {
    "work": "/Users/alex/projects/work-repo",
    "personal": "/Users/alex/projects/personal-repo"
  }
}
```

### `POST /api/repos`

Adds a repository mapping.

Request:

```json
{
  "id": "work",
  "path": "/Users/alex/projects/work-repo"
}
```

Response:

```json
{
  "ok": true
}
```

If the id already exists, return HTTP `409`.

### `POST /api/repos/select-path`

Opens a native folder selection dialog from the server process and validates
the selected directory as a Git repository or a directory inside a Git work
tree.

Selected response:

```json
{
  "ok": true,
  "cancelled": false,
  "path": "/Users/alex/projects/work-repo"
}
```

Cancelled response:

```json
{
  "ok": true,
  "cancelled": true
}
```

If the selected directory is inaccessible or is not inside a Git work tree,
return HTTP `400` and do not save configuration.

### `DELETE /api/repos/{id}`

Removes a repository mapping.

Response:

```json
{
  "ok": true
}
```

If the id does not exist, return HTTP `404`.

### `POST /batch`

Runs a GitBoard CLI batch request for a configured repository.

Request:

```json
{
  "repoId": "work",
  "batch": [
    {"cmd": "list", "args": []},
    {"cmd": "task", "args": ["TASK-001"]}
  ]
}
```

`repoId` is required. The server must not infer repository context from server
state.

The `batch` field must match the CLI batch command format described in
`doc/spec.md`: each entry has a command name and an argument array. Arguments
that are Base64-encoded for the CLI remain Base64-encoded in the request.

The workflow status configuration is also requested through `/batch` by sending
the `statuses` command:

```json
{
  "repoId": "work",
  "batch": [
    {"cmd": "statuses", "args": []}
  ]
}
```

The command result is Base64-encoded in the batch response, like other CLI
command output. After decoding, the result is the raw statuses JSON returned by
`gitboard statuses`.

Implementation flow:

1. Resolve the repository path from `repoId`.
2. Validate that the repository id exists.
3. Write the request's batch payload to a temporary JSON file:

   ```json
   {
     "batch": [
       {"cmd": "list", "args": []}
     ]
   }
   ```

4. Invoke:

   ```text
   gitboard --project-root REPO_PATH batch TEMP_BATCH_FILE
   ```

5. Wait for the process to exit.
6. Delete the temporary batch file.
7. Return the CLI's JSON batch result to the HTTP client.

If `gitboard` exits successfully, return HTTP `200` and the parsed CLI JSON
response.

If `gitboard` exits with an error but returns a valid batch JSON error payload,
return HTTP `400` and that parsed JSON payload.

If the server cannot invoke `gitboard`, cannot parse the CLI output, or hits an
unexpected internal error, return HTTP `500`.

## OpenAPI Schema

The implementation must provide an OpenAPI 3.x schema describing all HTTP API
endpoints, request bodies, response bodies, path parameters, and error
responses.

The schema should be committed as:

```text
doc/server_openapi.yaml
```

The schema is the client-facing API contract. When the API changes, update the
schema in the same change.

## CLI Discovery

The server needs a path to the `gitboard` executable. Supported options:

- `--gitboard-path PATH` explicitly sets the executable path.
- If omitted, the server searches next to its own executable.
- If still not found, the server searches `PATH`.

If no usable executable is found, startup fails with a clear error message.

## Security

The server must bind to loopback by default (`127.0.0.1` or `localhost`) and
must not listen on all interfaces unless an explicit command-line option is
added later.

The server must treat HTTP request bodies as untrusted input:

- Reject invalid JSON.
- Reject unknown or malformed repository ids.
- Do not execute shell-constructed commands. Launch `gitboard` with structured
  subprocess APIs and an argument vector.
- Use temporary files created with safe platform APIs.
