---
assignee: valentyn
branches: []
ci_status: unknown
created_at: 2026-06-17T11:59:09.476840Z
created_by: ''
id: TASK-015
priority: medium
prs: []
status: todo
story_points: 5
tags: []
title: 'Add git and GitHub integrations'
updated_at: 2026-08-07T15:32:15.504126Z
---

# Summary
- Add Git history and GitHub PR integration capabilities to Agent Pip.

# Description
Agent Pip should be equipped with read‑only access to the git history of the project, enabling it to:
- Trace the commit history of any repository‑relative file.
- Retrieve file contents at specific historical points.
- Compare different versions to describe changes introduced by commits.

Additionally, Agent Pip should be able to interact with GitHub pull requests, allowing it to:
- Enumerate open pull requests.
- Read PR details, including changed files, comments, and review discussions.
- Post comments and reviews on PRs.
- Generate tasks based on insights derived from PR content.

# Acceptance Criteria
- [] Implement a Barnaby tool `get_git_history(file_path)` that returns recent commits for a given file path, read‑only.
- [] Implement a Barnaby tool (e.g., `get_pr_info`) that returns PR metadata, comments, and review changes.
- [] Both tools must enforce read‑only access; no writes or branch creation are permitted.
- [] Expose these tools within Agent Pip’s API and provide documentation.
- [] Add unit and integration tests covering file existence, commit limits, and PR enumeration.

# Subtasks
- [] Design API signatures and response formats for the git history and PR tools.
- [] Implement backend logic using `git log` (or equivalent) for history retrieval.
- [] Integrate with the GitHub REST API (or GitHub CLI) for PR enumeration and details.
- [] Write unit and integration tests.
- [] Update documentation and add inline comments.

# Documentation
- Update relevant design documents to describe the new tools and their usage.
- Add inline documentation and comments explaining implementation details.

# Tests
- Unit tests for file existence, commit retrieval limits, and error handling.
- Integration tests that exercise the tools against a sample repository.

# Dependencies
- May depend on existing Barnaby utility libraries for git interaction and HTTP client functionality.
