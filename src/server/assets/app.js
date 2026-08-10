(function () {
  "use strict";

  const repoIdPattern = /^[a-z0-9_]+$/;
  const tagPattern = /^[a-z]+$/;
  const pathValuePattern = /^[A-Za-z0-9._/-]+$/;
  const sidePanelStorageKey = "gitboard.sidePanelCollapsed";
  const taskPageSize = 25;
  const pipHistoryWindow = 10;
  const pipHistoryOverflow = 3;
  const pipMaxHistoryMessageChars = 3600;
  const pipPanelMinWidth = 320;
  const pipPanelMaxWidth = 640;
  const statusPalette = [
    "status-color-0",
    "status-color-1",
    "status-color-2",
    "status-color-3",
    "status-color-4",
    "status-color-5",
    "status-color-6",
    "status-color-7",
  ];

  const state = {
    view: "start",
    selectedRepoId: null,
    repos: {},
    loading: false,
    sidePanelCollapsed: loadSidePanelCollapsed(),
    message: "",
    messageKind: "",
    form: {
      id: "",
      path: "",
      selectingPath: false,
    },
    pendingDeleteRepoId: null,
    tasks: {
      repoId: null,
      mode: "list",
      team: [],
      assignees: [""],
      statuses: {},
      statusOrder: [],
      tasksById: {},
      taskOrder: [],
      loading: false,
      loaded: false,
      loadState: "idle",
      progress: "",
      error: "",
      lastLoadedAt: null,
      selectedTaskId: null,
      loadToken: 0,
      movingTaskIds: {},
      assigningTaskIds: {},
      publishing: false,
      publishStatus: defaultPublishStatus(),
      publishStatusStale: false,
      remoteStatus: defaultRemoteStatus(),
      remoteStatusStale: false,
      searchTextEnabled: false,
      appliedSearchTextEnabled: false,
      searchText: "",
      appliedSearchText: "",
      filters: defaultTaskFilters(),
      filtersEnabled: defaultTaskFilterEnabled(),
      appliedFilters: defaultTaskFilters(),
      appliedFiltersEnabled: defaultTaskFilterEnabled(),
      searchPanelOpen: false,
      editTaskId: null,
      paneMode: null,
      savingEdit: false,
    },
    taskCreate: defaultTaskForm(),
    taskEdit: null,
    ai: {
      enabled: false,
      provider: "openai-compatible",
      baseUrl: "",
      model: "",
      apiKey: "",
      apiKeyConfigured: false,
      timeoutSeconds: 30,
      retryAttempts: 5,
      maxOutputTokens: "",
      allowWrites: false,
      systemPrompt: "",
      secretStorageWarning: "",
      savedConfig: null,
      hasUnsavedChanges: false,
      loading: false,
      testing: false,
      saving: false,
      pipOpen: false,
      pipPanelWidth: 380,
      chatInput: "",
      chatMessages: [],
      contextMessages: [],
      historySummary: "",
      conversationId: "",
      sending: false,
      drafts: [],
      draftsHeight: 0,
      creatingDrafts: false,
      proposedActions: [],
      proposedActionsHeight: 0,
      applyingActions: false,
      fileAccess: {
        repoId: null,
        repoPath: "",
        folders: [],
        loading: false,
        saving: false,
        loaded: false,
      },
    },
  };

  const app = document.getElementById("app");
  const apiToken = loadApiToken();

  function loadSidePanelCollapsed() {
    try {
      return window.localStorage.getItem(sidePanelStorageKey) === "true";
    } catch (_) {
      return false;
    }
  }

  function loadApiToken() {
    const storageKey = "barnaby.apiToken";
    try {
      const params = new URLSearchParams(window.location.search);
      const token = params.get("token");
      if (token) {
        window.sessionStorage.setItem(storageKey, token);
        params.delete("token");
        const query = params.toString();
        const nextUrl =
          window.location.pathname + (query ? `?${query}` : "") + window.location.hash;
        window.history.replaceState(null, "", nextUrl);
        return token;
      }
      return window.sessionStorage.getItem(storageKey) || "";
    } catch (_) {
      return "";
    }
  }

  function apiHeaders(headers) {
    return {
      "Content-Type": "application/json",
      "X-Barnaby-Token": apiToken,
      ...(headers || {}),
    };
  }

  function normalizedAiProvider(value) {
    const provider = String(value || "").trim().toLowerCase();
    if (
      provider === "anthropic-compatible" ||
      provider === "anthropic" ||
      provider === "claude" ||
      provider === "anthropic-messages"
    ) {
      return "anthropic-compatible";
    }
    return "openai-compatible";
  }

  function aiSettingsSnapshot() {
    return {
      enabled: Boolean(state.ai.enabled),
      provider: normalizedAiProvider(state.ai.provider),
      baseUrl: state.ai.baseUrl.trim(),
      model: state.ai.model.trim(),
      timeoutSeconds: Number(state.ai.timeoutSeconds) || 30,
      retryAttempts:
        state.ai.retryAttempts === ""
          ? 5
          : Number.isFinite(Number(state.ai.retryAttempts))
            ? Number(state.ai.retryAttempts)
            : 5,
      maxOutputTokens: state.ai.maxOutputTokens
        ? Number(state.ai.maxOutputTokens)
        : 0,
      allowWrites: Boolean(state.ai.allowWrites),
      systemPrompt: state.ai.systemPrompt.trim(),
      apiKeyConfigured: Boolean(state.ai.apiKeyConfigured),
    };
  }

  function aiPersistedEnabled() {
    return Boolean(state.ai.savedConfig && state.ai.savedConfig.enabled);
  }

  function updateAiSettingsDirtyIndicators() {
    const dirty = Boolean(state.ai.hasUnsavedChanges);
    const form = document.querySelector("[data-ai-settings-form='true']");
    if (form) form.classList.toggle("dirty", dirty);
    const banner = document.querySelector("[data-ai-unsaved-banner='true']");
    if (banner) banner.classList.toggle("hidden", !dirty);
    const save = document.querySelector("[data-ai-save-button='true']");
    if (save && !state.ai.saving) {
      save.textContent = dirty ? "Save AI settings *" : "Save AI settings";
      save.classList.toggle("attention", dirty);
    }
    const test = document.querySelector("[data-ai-test-button='true']");
    if (test) {
      test.disabled = Boolean(state.ai.testing || state.ai.saving || dirty);
      test.title = dirty ? "Save AI settings before testing." : "";
    }
  }

  function markAiSettingsChanged(options = {}) {
    state.ai.hasUnsavedChanges = true;
    if (options.render) {
      render();
    } else {
      updateAiSettingsDirtyIndicators();
    }
  }

  function saveSidePanelCollapsed() {
    try {
      window.localStorage.setItem(
        sidePanelStorageKey,
        state.sidePanelCollapsed ? "true" : "false",
      );
    } catch (_) {
      // Ignore storage failures; collapsing is a local presentation preference.
    }
  }

  function h(tag, attrs, ...children) {
    const el = document.createElement(tag);
    for (const [key, value] of Object.entries(attrs || {})) {
      if (value === undefined || value === null) {
        continue;
      } else if (key === "class") {
        el.className = value;
      } else if (key === "disabled") {
        el.disabled = Boolean(value);
      } else if (key === "selected") {
        el.selected = Boolean(value);
      } else if (key === "checked") {
        el.checked = Boolean(value);
      } else if (key === "value") {
        el.value = value;
      } else if (key === "onclick") {
        el.addEventListener("click", value);
      } else if (key === "oninput") {
        el.addEventListener("input", value);
      } else if (key === "onpaste") {
        el.addEventListener("paste", value);
      } else if (key === "onchange") {
        el.addEventListener("change", value);
      } else if (key === "onkeydown") {
        el.addEventListener("keydown", value);
      } else if (key === "onsubmit") {
        el.addEventListener("submit", value);
      } else if (key === "onmousedown") {
        el.addEventListener("mousedown", value);
      } else if (key === "onmouseup") {
        el.addEventListener("mouseup", value);
      } else if (key === "style") {
        el.setAttribute("style", value);
      } else {
        el.setAttribute(key, value);
      }
    }
    for (const child of children.flat()) {
      if (child === null || child === undefined) continue;
      if (typeof child === "string") {
        el.appendChild(document.createTextNode(child));
      } else {
        el.appendChild(child);
      }
    }
    return el;
  }

  async function requestJson(path, options) {
    const response = await fetch(path, {
      ...options,
      headers: apiHeaders(options && options.headers),
    });
    let data = {};
    try {
      data = await response.json();
    } catch (_) {
      data = {};
    }
    if (!response.ok) {
      throw new Error(data.error || `request failed: ${response.status}`);
    }
    return data;
  }

  function decodeBase64(value) {
    const binary = window.atob(value || "");
    const bytes = new Uint8Array(binary.length);
    for (let i = 0; i < binary.length; i += 1) {
      bytes[i] = binary.charCodeAt(i);
    }
    return new TextDecoder().decode(bytes);
  }

  function encodeBase64(value) {
    const bytes = new TextEncoder().encode(value);
    let binary = "";
    for (const byte of bytes) binary += String.fromCharCode(byte);
    return window.btoa(binary);
  }

  function defaultBody(title) {
    const heading = title.trim() || "Task title";
    return `# ${heading}\n\n## Description\n\n\n## Checklist\n- [ ] \n`;
  }

  function defaultTaskForm() {
    return {
      title: "",
      assignee: "",
      priority: "medium",
      story_points: 100,
      status: "backlog",
      tags: [],
      branches: [],
      prs: [],
      body: "",
      bodyTouched: false,
      bodyMode: "edit",
      newComments: [],
      pendingComment: "",
      saving: false,
    };
  }

  function storyPointOptions() {
    return [1, 2, 3, 5, 8, 13, 21, 100];
  }

  function normalizeStoryPoints(value) {
    const number = Number(value);
    return storyPointOptions().includes(number) ? number : 100;
  }

  function defaultDraftForm() {
    const form = defaultTaskForm();
    form.selected = true;
    return form;
  }

  function defaultTaskFilters() {
    return {
      title: "",
      status: "",
      assignee: "",
      priority: "",
      tag: "",
      createdBy: "",
    };
  }

  function defaultTaskFilterEnabled() {
    return {
      title: false,
      status: false,
      assignee: false,
      priority: false,
      tag: false,
      createdBy: false,
    };
  }

  function defaultPublishStatus() {
    return {
      added: [],
      modified: [],
      deleted: [],
    };
  }

  function defaultRemoteStatus() {
    return {
      available: false,
      upstream: "",
      added: [],
      modified: [],
      deleted: [],
    };
  }

  function cloneTaskForm(form) {
    return {
      ...form,
      tags: form.tags.slice(),
      branches: form.branches.slice(),
      prs: form.prs.slice(),
      newComments: form.newComments.slice(),
    };
  }

  function bodyWithoutComments(body) {
    const lines = String(body || "").split(/\r?\n/);
    const index = lines.findIndex((line) => line.trim() === "## Comments");
    const kept = index >= 0 ? lines.slice(0, index) : lines;
    return kept.join("\n").replace(/\s+$/, "") + "\n";
  }

  function commentLines(body) {
    const lines = String(body || "").split(/\r?\n/);
    const index = lines.findIndex((line) => line.trim() === "## Comments");
    if (index < 0) return [];
    return lines.slice(index + 1).filter((line) => line.trim().length > 0);
  }

  function markdownPreview(markdown) {
    return renderPipMarkdown(markdown);
  }

  function safeMarkdownHref(value) {
    const href = String(value || "").trim();
    if (/^(https?:|mailto:)/i.test(href)) return href;
    if (href.startsWith("#")) return href;
    return "";
  }

  function inlineMarkdownNodes(text) {
    const source = String(text || "");
    const nodes = [];
    let index = 0;

    function pushText(end) {
      if (end > index) nodes.push(source.slice(index, end));
      index = end;
    }

    while (index < source.length) {
      if (source[index] === "`") {
        const end = source.indexOf("`", index + 1);
        if (end > index + 1) {
          nodes.push(h("code", {}, source.slice(index + 1, end)));
          index = end + 1;
          continue;
        }
      }
      if (source.startsWith("**", index)) {
        const end = source.indexOf("**", index + 2);
        if (end > index + 2) {
          nodes.push(h("strong", {}, inlineMarkdownNodes(source.slice(index + 2, end))));
          index = end + 2;
          continue;
        }
      }
      if (source[index] === "*") {
        const end = source.indexOf("*", index + 1);
        if (end > index + 1) {
          nodes.push(h("em", {}, inlineMarkdownNodes(source.slice(index + 1, end))));
          index = end + 1;
          continue;
        }
      }
      if (source[index] === "[") {
        const textEnd = source.indexOf("]", index + 1);
        const urlStart = textEnd >= 0 ? source.indexOf("(", textEnd) : -1;
        const urlEnd = urlStart === textEnd + 1 ? source.indexOf(")", urlStart + 1) : -1;
        if (textEnd > index + 1 && urlEnd > urlStart + 1) {
          const href = safeMarkdownHref(source.slice(urlStart + 1, urlEnd));
          if (href) {
            nodes.push(
              h(
                "a",
                { href, target: "_blank", rel: "noopener noreferrer" },
                inlineMarkdownNodes(source.slice(index + 1, textEnd)),
              ),
            );
            index = urlEnd + 1;
            continue;
          }
        }
      }
      pushText(index + 1);
    }
    return nodes;
  }

  function splitMarkdownTableRow(line) {
    let trimmed = String(line || "").trim();
    if (trimmed.startsWith("|")) trimmed = trimmed.slice(1);
    if (trimmed.endsWith("|")) trimmed = trimmed.slice(0, -1);
    return trimmed.split("|").map((cell) => cell.trim());
  }

  function isMarkdownTableRow(line) {
    const trimmed = String(line || "").trim();
    return (
      trimmed.startsWith("|") &&
      trimmed.endsWith("|") &&
      splitMarkdownTableRow(trimmed).length > 1
    );
  }

  function isMarkdownTableDivider(line) {
    const cells = splitMarkdownTableRow(line);
    return (
      cells.length > 1 &&
      cells.every((cell) => /^:?-{3,}:?$/.test(cell.replace(/\s+/g, "")))
    );
  }

  function isMarkdownTableStart(lines, index) {
    if (index + 1 >= lines.length || !isMarkdownTableRow(lines[index])) {
      return false;
    }
    return isMarkdownTableDivider(lines[index + 1]) || isMarkdownTableRow(lines[index + 1]);
  }

  function renderMarkdownTable(lines, start) {
    const header = splitMarkdownTableRow(lines[start]);
    let index = start + 1;
    if (index < lines.length && isMarkdownTableDivider(lines[index])) {
      ++index;
    }
    const rows = [];
    while (index < lines.length && isMarkdownTableRow(lines[index])) {
      rows.push(splitMarkdownTableRow(lines[index]));
      ++index;
    }
    return {
      node: h(
        "div",
        { class: "markdown-table-wrap" },
        h(
          "table",
          {},
          h(
            "thead",
            {},
            h(
              "tr",
              {},
              header.map((cell) => h("th", {}, inlineMarkdownNodes(cell))),
            ),
          ),
          h(
            "tbody",
            {},
            rows.map((row) =>
              h(
                "tr",
                {},
                header.map((_, cellIndex) =>
                  h("td", {}, inlineMarkdownNodes(row[cellIndex] || "")),
                ),
              ),
            ),
          ),
        ),
      ),
      next: index,
    };
  }

  function markdownListItemNodes(item) {
    const checkbox = /^\[( |x|X)?\]\s+(.*)$/.exec(String(item || ""));
    if (!checkbox) return inlineMarkdownNodes(item);
    const checked = checkbox[1] && checkbox[1].toLowerCase() === "x";
    return [checked ? "☑ " : "☐ ", ...inlineMarkdownNodes(checkbox[2])];
  }

  function renderPipMarkdown(markdown) {
    const lines = String(markdown || "").replace(/\r\n/g, "\n").split("\n");
    const nodes = [];
    let index = 0;

    while (index < lines.length) {
      const line = lines[index];
      const trimmed = line.trim();
      if (!trimmed) {
        ++index;
        continue;
      }

      if (trimmed.startsWith("```")) {
        const language = trimmed.slice(3).trim().split(/\s+/)[0] || "";
        const code = [];
        ++index;
        while (index < lines.length && !lines[index].trim().startsWith("```")) {
          code.push(lines[index]);
          ++index;
        }
        if (index < lines.length) ++index;
        nodes.push(
          h(
            "pre",
            {},
            h("code", { class: language ? `language-${language}` : "" }, code.join("\n")),
          ),
        );
        continue;
      }

      if (
        isMarkdownTableStart(lines, index)
      ) {
        const table = renderMarkdownTable(lines, index);
        nodes.push(table.node);
        index = table.next;
        continue;
      }

      const heading = /^(#{1,3})\s+(.+)$/.exec(trimmed);
      if (heading) {
        nodes.push(h(`h${heading[1].length}`, {}, inlineMarkdownNodes(heading[2])));
        ++index;
        continue;
      }

      if (/^>\s?/.test(trimmed)) {
        const quote = [];
        while (index < lines.length && /^>\s?/.test(lines[index].trim())) {
          quote.push(lines[index].trim().replace(/^>\s?/, ""));
          ++index;
        }
        nodes.push(h("blockquote", {}, renderPipMarkdown(quote.join("\n"))));
        continue;
      }

      if (/^[-*]\s+/.test(trimmed) || /^\d+\.\s+/.test(trimmed)) {
        const ordered = /^\d+\.\s+/.test(trimmed);
        const items = [];
        const pattern = ordered ? /^\d+\.\s+/ : /^[-*]\s+/;
        while (index < lines.length && pattern.test(lines[index].trim())) {
          items.push(lines[index].trim().replace(pattern, ""));
          ++index;
        }
        nodes.push(
          h(
            ordered ? "ol" : "ul",
            {},
            items.map((item) => h("li", {}, markdownListItemNodes(item))),
          ),
        );
        continue;
      }

      const paragraph = [trimmed];
      ++index;
      while (
        index < lines.length &&
        lines[index].trim() &&
        !lines[index].trim().startsWith("```") &&
        !/^(#{1,3})\s+/.test(lines[index].trim()) &&
        !/^>\s?/.test(lines[index].trim()) &&
        !/^[-*]\s+/.test(lines[index].trim()) &&
        !/^\d+\.\s+/.test(lines[index].trim()) &&
        !isMarkdownTableStart(lines, index)
      ) {
        paragraph.push(lines[index].trim());
        ++index;
      }
      nodes.push(h("p", {}, inlineMarkdownNodes(paragraph.join(" "))));
    }

    return nodes.length ? nodes : [h("p", {}, "")];
  }

  function parseCreatedTaskId(output) {
    const match = String(output || "").match(/^Created\s+(TASK-[A-Za-z0-9+-]+)\s+/);
    if (!match) throw new Error("create did not return a task id");
    return match[1];
  }

  function taskIdLabel(taskId) {
    const value = String(taskId || "");
    if (value.length <= 18) return value;
    return `${value.slice(0, 11)}...${value.slice(-6)}`;
  }

  async function runBatch(repoId, commands) {
    const response = await fetch("/batch", {
      method: "POST",
      headers: apiHeaders(),
      body: JSON.stringify({ repoId, batch: commands }),
    });
    let data = {};
    try {
      data = await response.json();
    } catch (_) {
      data = {};
    }
    if (!response.ok && !Array.isArray(data.batch)) {
      throw new Error(data.error || `request failed: ${response.status}`);
    }
    const results = data.batch || [];
    for (const result of results) {
      if (!result.ok) {
        const error = new Error(
          result.error ? decodeBase64(result.error) : `${result.cmd} failed`,
        );
        error.results = results.map((item) => ({
          cmd: item.cmd,
          ok: item.ok,
          output: item.result ? decodeBase64(item.result) : "",
          error: item.error ? decodeBase64(item.error) : "",
        }));
        throw error;
      }
    }
    return results.map((result) => ({
      cmd: result.cmd,
      output: result.result ? decodeBase64(result.result) : "",
    }));
  }

  function dbstatusCommand() {
    return { cmd: "dbstatus", args: [] };
  }

  function remotestatusCommand() {
    return { cmd: "remotestatus", args: [] };
  }

  function withDbstatus(commands) {
    return [...commands, dbstatusCommand()];
  }

  function repoLabel(id, path) {
    const parts = path.split(/[\\/]+/).filter(Boolean);
    const folder = parts.length ? parts[parts.length - 1] : path;
    return `${id} [${folder}]`;
  }

  function setMessage(message, kind) {
    state.message = message || "";
    state.messageKind = kind || "";
  }

  function setView(view) {
    state.view = view;
    if (view !== "settings") state.selectedRepoId = null;
    state.tasks.selectedTaskId = null;
    state.tasks.editTaskId = null;
    state.tasks.paneMode = null;
    state.taskEdit = null;
    setMessage("", "");
    render();
    if (view === "settings" && state.selectedRepoId) {
      loadRepoFileAccess(state.selectedRepoId);
    }
  }

  function resetTasks(repoId) {
    state.tasks.repoId = repoId;
    state.tasks.team = [];
    state.tasks.assignees = [""];
    state.tasks.statuses = {};
    state.tasks.statusOrder = [];
    state.tasks.tasksById = {};
    state.tasks.taskOrder = [];
    state.tasks.loading = false;
    state.tasks.loaded = false;
    state.tasks.loadState = "idle";
    state.tasks.progress = "";
    state.tasks.error = "";
    state.tasks.lastLoadedAt = null;
    state.tasks.selectedTaskId = null;
    state.tasks.movingTaskIds = {};
    state.tasks.assigningTaskIds = {};
    state.tasks.publishing = false;
    state.tasks.publishStatus = defaultPublishStatus();
    state.tasks.publishStatusStale = false;
    state.tasks.remoteStatus = defaultRemoteStatus();
    state.tasks.remoteStatusStale = false;
    state.tasks.searchTextEnabled = false;
    state.tasks.appliedSearchTextEnabled = false;
    state.tasks.searchText = "";
    state.tasks.appliedSearchText = "";
    state.tasks.filters = defaultTaskFilters();
    state.tasks.filtersEnabled = defaultTaskFilterEnabled();
    state.tasks.appliedFilters = defaultTaskFilters();
    state.tasks.appliedFiltersEnabled = defaultTaskFilterEnabled();
    state.tasks.searchPanelOpen = false;
    state.tasks.editTaskId = null;
    state.tasks.paneMode = null;
    state.tasks.savingEdit = false;
    state.taskEdit = null;
  }

  function resetPipConversation() {
    state.ai.chatInput = "";
    state.ai.chatMessages = [];
    state.ai.contextMessages = [];
    state.ai.historySummary = "";
    state.ai.conversationId = "";
    state.ai.sending = false;
    state.ai.drafts = [];
    state.ai.draftsHeight = 0;
    state.ai.creatingDrafts = false;
    state.ai.proposedActions = [];
    state.ai.proposedActionsHeight = 0;
    state.ai.applyingActions = false;
  }

  function resetRepoFileAccess(repoId) {
    state.ai.fileAccess = {
      repoId: repoId || null,
      repoPath: "",
      folders: [],
      loading: false,
      saving: false,
      loaded: false,
    };
  }

  function selectRepo(id) {
    const repoChanged = state.selectedRepoId !== id;
    state.view = "repo";
    state.selectedRepoId = id;
    state.tasks.mode = state.tasks.mode || "list";
    resetTasks(id);
    if (repoChanged) resetPipConversation();
    if (repoChanged) resetRepoFileAccess(id);
    setMessage("", "");
    render();
    loadTasks(id, { preserveExisting: false });
  }

  function parseJsonOutput(output, label) {
    try {
      return JSON.parse(output);
    } catch (_) {
      throw new Error(`${label} returned invalid JSON`);
    }
  }

  function normalizeStringArray(value) {
    return Array.isArray(value)
      ? value.filter((item) => typeof item === "string")
      : [];
  }

  function parseDbstatusOutput(output) {
    const parsed = parseJsonOutput(output, "dbstatus");
    return {
      added: normalizeStringArray(parsed.added),
      modified: normalizeStringArray(parsed.modified),
      deleted: normalizeStringArray(parsed.deleted),
    };
  }

  function applyDbstatusResult(result) {
    if (!result || result.cmd !== "dbstatus") return false;
    state.tasks.publishStatus = parseDbstatusOutput(result.output);
    state.tasks.publishStatusStale = false;
    return true;
  }

  function parseRemoteStatusOutput(output) {
    const parsed = parseJsonOutput(output, "remotestatus");
    return {
      available: parsed.available === true,
      upstream: typeof parsed.upstream === "string" ? parsed.upstream : "",
      added: normalizeStringArray(parsed.added),
      modified: normalizeStringArray(parsed.modified),
      deleted: normalizeStringArray(parsed.deleted),
    };
  }

  function applyRemoteStatusResult(result) {
    if (!result || result.cmd !== "remotestatus") return false;
    state.tasks.remoteStatus = parseRemoteStatusOutput(result.output);
    state.tasks.remoteStatusStale = false;
    return true;
  }

  function applyFinalDbstatus(results) {
    if (!results || !results.length) return false;
    return applyDbstatusResult(results[results.length - 1]);
  }

  function markPublishStatusStaleIfNeeded(err) {
    const results = Array.isArray(err && err.results) ? err.results : [];
    const last = results[results.length - 1];
    if (!last || last.cmd !== "dbstatus" || !last.ok) {
      state.tasks.publishStatusStale = true;
    }
  }

  function publishChangeCount() {
    const status = state.tasks.publishStatus || defaultPublishStatus();
    return status.added.length + status.modified.length + status.deleted.length;
  }

  function remoteChangeCount() {
    const status = state.tasks.remoteStatus || defaultRemoteStatus();
    return status.added.length + status.modified.length + status.deleted.length;
  }

  function hasRemoteChanges() {
    return remoteChangeCount() > 0;
  }

  function remoteTaskChanged(taskId) {
    const status = state.tasks.remoteStatus || defaultRemoteStatus();
    return status.modified.includes(taskId) || status.deleted.includes(taskId);
  }

  function remoteWarningMessage() {
    return "Tasks in repo have changed. Use Git or your Git client to sync your local repository.";
  }

  function hasPublishChanges() {
    return publishChangeCount() > 0;
  }

  function publishSummaryText() {
    const status = state.tasks.publishStatus || defaultPublishStatus();
    const parts = [];
    if (status.added.length) parts.push(`${status.added.length} added`);
    if (status.modified.length) parts.push(`${status.modified.length} modified`);
    if (status.deleted.length) parts.push(`${status.deleted.length} deleted`);
    return parts.length ? parts.join(" / ") : "";
  }

  function publishSummaryTitle() {
    const status = state.tasks.publishStatus || defaultPublishStatus();
    const sections = [];
    if (status.added.length) sections.push(`Added: ${status.added.join(", ")}`);
    if (status.modified.length) {
      sections.push(`Modified: ${status.modified.join(", ")}`);
    }
    if (status.deleted.length) sections.push(`Deleted: ${status.deleted.join(", ")}`);
    return sections.join("\n");
  }

  function taskPublishClass(taskId) {
    const status = state.tasks.publishStatus || defaultPublishStatus();
    if (status.added.includes(taskId)) return " publish-added";
    if (status.modified.includes(taskId)) return " publish-modified";
    return "";
  }

  function mutationsDisabled() {
    return Boolean(state.tasks.publishing || hasRemoteChanges());
  }

  function sortStatuses(statuses) {
    return Object.keys(statuses).sort((a, b) => {
      const left = statuses[a] && Number.isFinite(statuses[a].order)
        ? statuses[a].order
        : Number.MAX_SAFE_INTEGER;
      const right = statuses[b] && Number.isFinite(statuses[b].order)
        ? statuses[b].order
        : Number.MAX_SAFE_INTEGER;
      if (left !== right) return left - right;
      return a.localeCompare(b);
    });
  }

  function assigneeDisplay(value) {
    return value ? value : "unassigned";
  }

  function extractAssignees(team) {
    const seen = new Set([""]);
    const assignees = [""];
    const members = team && Array.isArray(team.team) ? team.team : [];
    for (const member of members) {
      if (!member || typeof member.alias !== "string" || !member.alias) continue;
      if (seen.has(member.alias)) continue;
      seen.add(member.alias);
      assignees.push(member.alias);
    }
    return assignees;
  }

  function assigneeOptionsForTask(task) {
    const current = task.assignee || "";
    if (!current || state.tasks.assignees.includes(current)) {
      return state.tasks.assignees;
    }
    return [current, ...state.tasks.assignees];
  }

  function formAssigneeOptions(form) {
    const current = form.assignee || "";
    if (!current || state.tasks.assignees.includes(current)) {
      return state.tasks.assignees;
    }
    return [current, ...state.tasks.assignees];
  }

  function defaultStatus() {
    if (state.tasks.statusOrder.includes("backlog")) return "backlog";
    return state.tasks.statusOrder[0] || "backlog";
  }

  function statusOptionsForForm(form) {
    const current = form.status || defaultStatus();
    if (!current || state.tasks.statusOrder.includes(current)) {
      return state.tasks.statusOrder;
    }
    return [current, ...state.tasks.statusOrder];
  }

  function validateListValue(kind, value) {
    const text = value.trim();
    if (!text) return `${kind} cannot be empty.`;
    if (text.includes(",")) return `${kind} cannot contain commas.`;
    if (kind === "tag" && !tagPattern.test(text)) {
      return "Tags must contain lowercase letters a-z only.";
    }
    if ((kind === "branch" || kind === "PR") && !validPathValue(text)) {
      return `${kind} must be a valid path-like value.`;
    }
    return "";
  }

  function validPathValue(value) {
    if (!pathValuePattern.test(value)) return false;
    if (value.startsWith("/") || value.endsWith("/")) return false;
    const parts = value.split("/");
    return parts.every((part) => part && part !== "." && part !== "..");
  }

  function addListValue(form, field, kind, value) {
    const text = value.trim();
    const error = validateListValue(kind, text);
    if (error) {
      setMessage(error, "error");
      render();
      return;
    }
    if (!form[field].includes(text)) form[field].push(text);
    setMessage("", "");
    const input = document.getElementById(`token-${field}`);
    if (input) input.value = "";
    render();
  }

  function removeListValue(form, field, value) {
    form[field] = form[field].filter((item) => item !== value);
    render();
  }

  function taskFormBody(form) {
    if (form.bodyTouched) return form.body;
    return defaultBody(form.title);
  }

  function formHasCreateInput(form) {
    return Boolean(
        form.title.trim() ||
        form.assignee ||
        form.priority !== "medium" ||
        form.story_points !== 100 ||
        form.status !== defaultStatus() ||
        form.tags.length ||
        form.branches.length ||
        form.prs.length ||
        form.bodyTouched ||
        form.newComments.length ||
        form.pendingComment.trim(),
    );
  }

  function createFormValid() {
    return (
      state.selectedRepoId &&
      !state.taskCreate.saving &&
      !mutationsDisabled() &&
      state.taskCreate.title.trim().length > 0
    );
  }

  function editFormDirty() {
    if (!state.taskEdit) return false;
    const original = state.taskEdit.original;
    const form = state.taskEdit.form;
    return (
      form.title !== original.title ||
      form.assignee !== original.assignee ||
      form.priority !== original.priority ||
      form.story_points !== original.story_points ||
      form.status !== original.status ||
      form.body !== original.body ||
      form.tags.join("\n") !== original.tags.join("\n") ||
      form.branches.join("\n") !== original.branches.join("\n") ||
      form.prs.join("\n") !== original.prs.join("\n") ||
      form.newComments.length > 0 ||
      form.pendingComment.trim().length > 0
    );
  }

  function formatTime(value) {
    if (!value) return "";
    return value.toLocaleTimeString([], { hour: "2-digit", minute: "2-digit" });
  }

  function activeSearchText() {
    return state.tasks.appliedSearchText.trim();
  }

  function splitFilterValues(value) {
    return String(value || "")
      .split(",")
      .map((item) => item.trim())
      .filter(Boolean);
  }

  function queryArgsFromState() {
    const args = [];
    if (state.tasks.appliedSearchTextEnabled) {
      const text = activeSearchText();
      if (text) args.push("--text", text);
      return args;
    }
    const filters = state.tasks.appliedFilters;
    const enabled = state.tasks.appliedFiltersEnabled;
    if (enabled.title) {
      for (const value of splitFilterValues(filters.title)) args.push("--title", value);
    }
    if (enabled.status) {
      for (const value of splitFilterValues(filters.status)) args.push("--status", value);
    }
    if (enabled.assignee) {
      for (const value of splitFilterValues(filters.assignee)) args.push("--assignee", value);
    }
    if (enabled.priority) {
      for (const value of splitFilterValues(filters.priority)) args.push("--priority", value);
    }
    if (enabled.tag) {
      for (const value of splitFilterValues(filters.tag)) args.push("--tag", value);
    }
    if (enabled.createdBy) {
      for (const value of splitFilterValues(filters.createdBy)) args.push("--created-by", value);
    }
    return args;
  }

  function hasActiveTaskQuery() {
    return queryArgsFromState().length > 0;
  }

  function hasDraftTaskQuery() {
    if (state.tasks.searchTextEnabled && state.tasks.searchText.trim()) return true;
    const filters = state.tasks.filters;
    return Object.keys(filters).some((key) =>
      state.tasks.filtersEnabled[key] && String(filters[key] || "").trim(),
    );
  }

  function taskIndexCommand() {
    const args = queryArgsFromState();
    return args.length
      ? { cmd: "query", args }
      : { cmd: "list", args: [] };
  }

  function taskIndexLabel() {
    return hasActiveTaskQuery() ? "query" : "list";
  }

  async function loadTasks(repoId, options) {
    const preserveExisting = Boolean(options && options.preserveExisting && state.tasks.loaded);
    const silent = Boolean(options && options.silent);
    const token = state.tasks.loadToken + 1;
    const searching = hasActiveTaskQuery();
    state.tasks.loadToken = token;
    state.tasks.loading = true;
    state.tasks.loadState = "statuses";
    state.tasks.error = "";
    state.tasks.progress = preserveExisting
      ? searching
        ? "Searching tasks..."
        : "Refreshing team, statuses, and task index..."
      : searching
        ? "Searching tasks..."
        : "Loading team, statuses, and task index...";
    if (!preserveExisting) {
      state.tasks.loaded = false;
      state.tasks.team = [];
      state.tasks.assignees = [""];
      state.tasks.statuses = {};
      state.tasks.statusOrder = [];
      state.tasks.tasksById = {};
      state.tasks.taskOrder = [];
    }
    render();

    try {
      const [teamResult, statusesResult, listResult, dbstatusResult, remoteStatusResult] = await runBatch(repoId, [
        { cmd: "team", args: [] },
        { cmd: "statuses", args: [] },
        taskIndexCommand(),
        dbstatusCommand(),
        remotestatusCommand(),
      ]);
      if (state.tasks.loadToken !== token) return;
      applyDbstatusResult(dbstatusResult);
      applyRemoteStatusResult(remoteStatusResult);

      const team = parseJsonOutput(teamResult.output, "team");
      const statuses = parseJsonOutput(statusesResult.output, "statuses");
      const list = parseJsonOutput(listResult.output, taskIndexLabel());
      const ids = Object.keys(list).sort((a, b) => a.localeCompare(b));
      const nextTasksById = {};

      if (!preserveExisting) {
        state.tasks.team = team.team || [];
        state.tasks.assignees = extractAssignees(team);
        state.tasks.statuses = statuses;
        state.tasks.statusOrder = sortStatuses(statuses);
        state.tasks.taskOrder = ids;
      }
      state.tasks.loadState = "details";
      state.tasks.progress = ids.length
        ? searching
          ? `Loading 0/${ids.length} matching tasks...`
          : `Loading 0/${ids.length} tasks...`
        : searching
          ? "No matching tasks."
          : "No tasks.";
      render();

      for (let start = 0; start < ids.length; start += taskPageSize) {
        const pageIds = ids.slice(start, start + taskPageSize);
        const results = await runBatch(
          repoId,
          pageIds.map((id) => ({ cmd: "task", args: [id, "--json"] })),
        );
        if (state.tasks.loadToken !== token) return;
        for (let i = 0; i < pageIds.length; i += 1) {
          nextTasksById[pageIds[i]] = parseJsonOutput(
            results[i].output,
            `task ${pageIds[i]}`,
          );
        }
        if (!preserveExisting) {
          for (const id of pageIds) state.tasks.tasksById[id] = nextTasksById[id];
        }
        const loaded = Math.min(start + pageIds.length, ids.length);
        state.tasks.progress = searching
          ? `Loading ${loaded}/${ids.length} matching tasks...`
          : `Loading ${loaded}/${ids.length} tasks...`;
        render();
      }

      state.tasks.team = team.team || [];
      state.tasks.assignees = extractAssignees(team);
      state.tasks.statuses = statuses;
      state.tasks.statusOrder = sortStatuses(statuses);
      state.tasks.taskOrder = ids;
      state.tasks.tasksById = nextTasksById;
      state.tasks.loading = false;
      state.tasks.loaded = true;
      state.tasks.loadState = ids.length ? "loaded" : "empty";
      state.tasks.progress = "";
      state.tasks.error = "";
      state.tasks.lastLoadedAt = new Date();
      if (hasRemoteChanges()) {
        setMessage(remoteWarningMessage(), "warning");
      } else if (silent) {
        if (state.messageKind === "warning") setMessage("", "");
      } else {
        setMessage(
          searching
            ? ids.length
              ? `${ids.length} matching tasks loaded.`
              : "No matching tasks found."
            : ids.length
              ? `${ids.length} tasks loaded.`
              : "No tasks found.",
          "ok",
        );
      }
    } catch (err) {
      if (state.tasks.loadToken !== token) return;
      markPublishStatusStaleIfNeeded(err);
      state.tasks.loading = false;
      state.tasks.loaded = preserveExisting;
      state.tasks.loadState = preserveExisting ? "loaded" : "failed";
      state.tasks.progress = "";
      state.tasks.error = err.message;
      setMessage(
        preserveExisting ? `Refresh failed: ${err.message}` : err.message,
        "error",
      );
    } finally {
      if (state.tasks.loadToken === token) render();
    }
  }

  async function loadConfig() {
    state.loading = true;
    render();
    try {
      const config = await requestJson("/api/config");
      state.repos = config.repositories || {};
      setMessage("", "");
    } catch (err) {
      setMessage(err.message, "error");
    } finally {
      state.loading = false;
      render();
    }
  }

  async function loadAiConfig() {
    state.ai.loading = true;
    render();
    try {
      const config = await requestJson("/api/ai/config");
      state.ai.enabled = Boolean(config.enabled);
      state.ai.provider = normalizedAiProvider(config.provider);
      state.ai.baseUrl = config.baseUrl || "";
      state.ai.model = config.model || "";
      state.ai.apiKey = "";
      state.ai.apiKeyConfigured = Boolean(config.apiKeyConfigured);
      state.ai.timeoutSeconds = Number(config.timeoutSeconds) || 30;
      state.ai.retryAttempts = Number.isFinite(Number(config.retryAttempts))
        ? Number(config.retryAttempts)
        : 5;
      state.ai.maxOutputTokens = config.maxOutputTokens
        ? String(config.maxOutputTokens)
        : "";
      state.ai.allowWrites = Boolean(config.allowWrites);
      state.ai.systemPrompt = config.systemPrompt || "";
      state.ai.secretStorageWarning = config.secretStorageWarning || "";
      state.ai.savedConfig = aiSettingsSnapshot();
      state.ai.hasUnsavedChanges = false;
    } catch (err) {
      setMessage(err.message, "error");
    } finally {
      state.ai.loading = false;
      render();
    }
  }

  async function saveAiConfig(options) {
    state.ai.saving = true;
    render();
    try {
      const body = {
        enabled: state.ai.enabled,
        provider: normalizedAiProvider(state.ai.provider),
        baseUrl: state.ai.baseUrl.trim(),
        model: state.ai.model.trim(),
        timeoutSeconds: Number(state.ai.timeoutSeconds) || 30,
        retryAttempts:
          state.ai.retryAttempts === ""
            ? 5
            : Number.isFinite(Number(state.ai.retryAttempts))
              ? Number(state.ai.retryAttempts)
              : 5,
        maxOutputTokens: state.ai.maxOutputTokens
          ? Number(state.ai.maxOutputTokens)
          : 0,
        allowWrites: state.ai.allowWrites,
        systemPrompt: state.ai.systemPrompt.trim(),
      };
      if (state.ai.apiKey.trim()) body.apiKey = state.ai.apiKey.trim();
      if (options && options.clearApiKey) body.clearApiKey = true;
      const config = await requestJson("/api/ai/config", {
        method: "PUT",
        body: JSON.stringify(body),
      });
      state.ai.apiKey = "";
      state.ai.apiKeyConfigured = Boolean(config.apiKeyConfigured);
      state.ai.hasUnsavedChanges = false;
      setMessage(options && options.clearApiKey ? "AI API key cleared." : "AI settings saved.", "ok");
      await loadAiConfig();
    } catch (err) {
      setMessage(err.message, "error");
    } finally {
      state.ai.saving = false;
      render();
    }
  }

  async function testAiConfig() {
    state.ai.testing = true;
    render();
    try {
      const result = await requestJson("/api/ai/test", { method: "POST" });
      setMessage(result.message || "AI configuration is valid.", "ok");
    } catch (err) {
      setMessage(err.message, "error");
    } finally {
      state.ai.testing = false;
      render();
    }
  }

  async function loadRepoFileAccess(repoId) {
    if (!repoId) return;
    state.ai.fileAccess.loading = true;
    state.ai.fileAccess.repoId = repoId;
    render();
    try {
      const data = await requestJson(
        `/api/repos/${encodeURIComponent(repoId)}/ai/file-access`,
      );
      state.ai.fileAccess.repoId = data.repoId || repoId;
      state.ai.fileAccess.repoPath = data.repoPath || "";
      state.ai.fileAccess.folders = Array.isArray(data.folders)
        ? data.folders.map((folder) => ({
            path: folder.path || "",
            access: folder.access || "forbidden",
          }))
        : [];
      state.ai.fileAccess.loaded = true;
    } catch (err) {
      setMessage(err.message, "error");
    } finally {
      state.ai.fileAccess.loading = false;
      render();
    }
  }

  async function saveRepoFileAccess() {
    const repoId = state.selectedRepoId;
    if (!repoId) return;
    state.ai.fileAccess.saving = true;
    render();
    try {
      const fileAccess = state.ai.fileAccess.folders.map((folder) => ({
        path: folder.path,
        access: folder.access || "forbidden",
      }));
      const data = await requestJson(
        `/api/repos/${encodeURIComponent(repoId)}/ai/file-access`,
        {
          method: "PUT",
          body: JSON.stringify({ fileAccess }),
        },
      );
      state.ai.fileAccess.repoId = data.repoId || repoId;
      state.ai.fileAccess.repoPath = data.repoPath || "";
      state.ai.fileAccess.folders = Array.isArray(data.folders)
        ? data.folders.map((folder) => ({
            path: folder.path || "",
            access: folder.access || "forbidden",
          }))
        : [];
      state.ai.fileAccess.loaded = true;
      setMessage("Agent Pip file access saved.", "ok");
    } catch (err) {
      setMessage(err.message, "error");
    } finally {
      state.ai.fileAccess.saving = false;
      render();
    }
  }

  function resetRepoFileAccessToRecommended() {
    const recommended = {
      "doc/": "full_access",
      "src/": "read_only",
      "tasks/": "full_access",
      "tests/": "read_only",
    };
    state.ai.fileAccess.folders = state.ai.fileAccess.folders.map((folder) => ({
      ...folder,
      access: recommended[folder.path] || "forbidden",
    }));
    render();
  }

  function setRepoFolderAccess(path, access) {
    state.ai.fileAccess.folders = state.ai.fileAccess.folders.map((folder) =>
      folder.path === path ? { ...folder, access } : folder,
    );
  }

  function sanitizeDraftList(value) {
    if (!Array.isArray(value)) return [];
    return value
      .map((draft) => {
        const form = defaultDraftForm();
        form.title = typeof draft.title === "string" ? draft.title.trim() : "";
        form.assignee =
          typeof draft.assignee === "string" ? draft.assignee.trim() : "";
        form.priority = ["high", "medium", "low"].includes(draft.priority)
          ? draft.priority
          : "medium";
        form.story_points = normalizeStoryPoints(draft.story_points);
        form.status =
          typeof draft.status === "string" && draft.status.trim()
            ? draft.status.trim()
            : defaultStatus();
        form.tags = Array.isArray(draft.tags)
          ? draft.tags
              .filter((tag) => typeof tag === "string" && tagPattern.test(tag))
              .slice(0, 12)
          : [];
        form.body =
          typeof draft.body === "string" && draft.body.trim()
            ? draft.body
            : defaultBody(form.title);
        form.bodyTouched = true;
        return form;
      })
      .filter((form) => form.title);
  }

  function mergePipDrafts(drafts) {
    const forms = sanitizeDraftList(drafts);
    if (!forms.length) return;
    if (!state.ai.drafts.length) {
      state.ai.draftsHeight = 0;
    }
    state.ai.drafts.push(...forms);
  }

  function sanitizePipActions(actions) {
    if (!Array.isArray(actions)) return [];
    return actions
      .map((action, index) => {
        if (!action || typeof action !== "object") return null;
        const type = String(action.type || "");
        if (!["update_task", "move_task", "comment_task", "add_team_member"].includes(type)) {
          return null;
        }
        const taskId = String(action.taskId || "").trim();
        if (type !== "add_team_member" && !/^TASK-[A-Za-z0-9+-]+$/.test(taskId)) return null;
        return {
          ...action,
          id: String(action.id || `action_${Date.now()}_${index}`),
          type,
          taskId,
          title: String(action.title || actionTypeLabel(type, taskId)),
          summary: String(action.summary || ""),
          selected: action.selected !== false,
        };
      })
      .filter(Boolean);
  }

  function mergePipActions(actions) {
    const clean = sanitizePipActions(actions);
    if (!clean.length) return;
    if (!state.ai.proposedActions.length) {
      state.ai.proposedActionsHeight = 0;
    }
    state.ai.proposedActions.push(...clean);
  }

  function addPipFeedbackMessage(content) {
    const message = { role: "assistant", content };
    state.ai.chatMessages.push(message);
    state.ai.contextMessages.push(pipContextMessage(message));
  }

  function pipContextMessage(message) {
    const content = String(message.content || "");
    return {
      role: message.role || "assistant",
      content:
        content.length > pipMaxHistoryMessageChars
          ? `${content.slice(0, pipMaxHistoryMessageChars)}\n\n[Message truncated for context.]`
          : content,
    };
  }

  function scrollPipMessagesToBottom() {
    window.setTimeout(() => {
      const messages = document.querySelector(".agent-messages");
      if (messages) messages.scrollTop = messages.scrollHeight;
    }, 0);
  }

  async function sendPipMessage() {
    const message = state.ai.chatInput.trim();
    if (!message || !state.selectedRepoId) return;
    const history = state.ai.contextMessages.slice(-pipHistoryWindow);
    state.ai.chatInput = "";
    const userMessage = { role: "user", content: message };
    state.ai.chatMessages.push(userMessage);
    state.ai.contextMessages.push(pipContextMessage(userMessage));
    state.ai.sending = true;
    render();
    scrollPipMessagesToBottom();
    try {
      const response = await fetch("/api/agent/chat", {
        method: "POST",
        headers: apiHeaders(),
        body: JSON.stringify({
          repoId: state.selectedRepoId,
          conversationId: state.ai.conversationId || undefined,
          history,
          historySummary: state.ai.historySummary || undefined,
          message,
          mode: "chat",
        }),
      });
      const result = await response.json();
      state.ai.conversationId = result.conversationId || state.ai.conversationId;
      if (Array.isArray(result.messages) && result.messages.length) {
        state.ai.chatMessages.push(...result.messages);
        state.ai.contextMessages.push(...result.messages.map(pipContextMessage));
        mergePipDrafts(result.drafts);
        mergePipActions(result.proposedActions);
        if (result.tasksChanged && state.selectedRepoId) {
          await loadTasks(state.selectedRepoId, { preserveExisting: true });
        }
        await summarizePipHistoryIfNeeded();
      } else if (!response.ok) {
        state.ai.chatMessages.push({
          role: "assistant",
          content: result.error || `request failed: ${response.status}`,
        });
      }
    } catch (err) {
      state.ai.chatMessages.push({ role: "assistant", content: err.message });
    } finally {
      state.ai.sending = false;
      render();
    }
  }

  async function summarizePipHistoryIfNeeded() {
    if (state.ai.contextMessages.length <= pipHistoryWindow + pipHistoryOverflow) {
      return;
    }
    const overflow = state.ai.contextMessages.slice(0, pipHistoryOverflow);
    const retained = state.ai.contextMessages.slice(pipHistoryOverflow);
    try {
      const result = await requestJson("/api/agent/summarize", {
        method: "POST",
        body: JSON.stringify({
          repoId: state.selectedRepoId,
          historySummary: state.ai.historySummary || undefined,
          messages: overflow,
        }),
      });
      state.ai.historySummary = result.historySummary || state.ai.historySummary;
      state.ai.contextMessages = retained;
    } catch (_) {
      // Keep raw context if summarization fails; future turns can retry.
    }
  }

  function togglePip() {
    state.ai.pipOpen = !state.ai.pipOpen;
    render();
  }

  function pipAvailable() {
    return aiPersistedEnabled() && Boolean(state.selectedRepoId);
  }

  function clampPipPanelWidth(width) {
    const viewportMax = Math.max(pipPanelMinWidth, window.innerWidth - 160);
    return Math.min(
      Math.max(width, pipPanelMinWidth),
      Math.min(pipPanelMaxWidth, viewportMax),
    );
  }

  function startPipPanelResize(event) {
    event.preventDefault();
    const startX = event.clientX;
    const startWidth = state.ai.pipPanelWidth;

    function onMove(moveEvent) {
      state.ai.pipPanelWidth = clampPipPanelWidth(
        startWidth + startX - moveEvent.clientX,
      );
      const panel = document.querySelector(".agent-dock");
      if (panel) panel.style.width = `${state.ai.pipPanelWidth}px`;
    }

    function onEnd() {
      window.removeEventListener("mousemove", onMove);
      window.removeEventListener("mouseup", onEnd);
    }

    window.addEventListener("mousemove", onMove);
    window.addEventListener("mouseup", onEnd);
  }

  function clearPipHistory() {
    state.ai.chatMessages = [];
    state.ai.contextMessages = [];
    state.ai.historySummary = "";
    state.ai.conversationId = "";
    render();
  }

  async function addRepo() {
    const id = state.form.id.trim();
    const path = state.form.path.trim();
    if (!repoIdPattern.test(id)) {
      setMessage("Repository id must match [a-z0-9_]+.", "error");
      render();
      return;
    }
    if (!path) {
      setMessage("Repository path is required.", "error");
      render();
      return;
    }

    state.loading = true;
    render();
    try {
      await requestJson("/api/repos", {
        method: "POST",
        body: JSON.stringify({ id, path }),
      });
      state.form.id = "";
      state.form.path = "";
      state.view = "start";
      await loadConfig();
      setMessage(`Repository ${id} added.`, "ok");
    } catch (err) {
      setMessage(err.message, "error");
    } finally {
      state.loading = false;
      render();
    }
  }

  async function selectRepoPath() {
    state.form.selectingPath = true;
    setMessage("", "");
    render();
    try {
      const result = await requestJson("/api/repos/select-path", {
        method: "POST",
        body: "{}",
      });
      if (result.cancelled) {
        setMessage("Repository path selection cancelled.", "");
        return;
      }
      state.form.path = result.path || "";
      setMessage("Repository path selected.", "ok");
    } catch (err) {
      setMessage(err.message, "error");
    } finally {
      state.form.selectingPath = false;
      render();
    }
  }

  function requestDeleteRepo(id) {
    state.pendingDeleteRepoId = id;
    setMessage("", "");
    render();
  }

  function cancelDeleteRepo() {
    state.pendingDeleteRepoId = null;
    render();
  }

  async function confirmDeleteRepo() {
    const id = state.pendingDeleteRepoId;
    if (!id) return;
    state.loading = true;
    render();
    try {
      await requestJson(`/api/repos/${encodeURIComponent(id)}`, {
        method: "DELETE",
      });
      state.pendingDeleteRepoId = null;
      if (state.selectedRepoId === id) {
        state.view = "start";
        state.selectedRepoId = null;
        resetTasks(null);
        resetPipConversation();
        resetRepoFileAccess(null);
      }
      await loadConfig();
      setMessage(`Repository ${id} deleted.`, "ok");
    } catch (err) {
      setMessage(err.message, "error");
    } finally {
      state.loading = false;
      render();
    }
  }

  function contextTitle() {
    if (state.view === "add") return "Add repository";
    if (state.view === "create-task") return "Create task";
    if (state.view === "settings") return "Settings";
    if (state.view === "task") return state.tasks.selectedTaskId || "Task";
    if (state.selectedRepoId) return state.selectedRepoId;
    return "Start";
  }

  function taskSummaryText() {
    if (state.view !== "repo" || !state.selectedRepoId || !state.tasks.loaded) {
      return "";
    }
    const count = state.tasks.taskOrder.length;
    const details = [`${count} ${count === 1 ? "task" : "tasks"}`];
    if (hasActiveTaskQuery()) {
      details.push(
        state.tasks.appliedSearchTextEnabled && activeSearchText()
          ? `Search "${activeSearchText()}"`
          : "Filtered",
      );
    }
    if (state.tasks.lastLoadedAt) {
      details.push(`Last loaded ${formatTime(state.tasks.lastLoadedAt)}`);
    }
    if (state.tasks.loading) {
      details.push(state.tasks.progress || "Refreshing...");
    }
    if (state.tasks.publishing) {
      details.push("Publishing...");
    } else if (hasRemoteChanges()) {
      details.push("Remote task changes available");
    } else if (state.tasks.publishStatusStale) {
      details.push("Publishing state may be stale");
    } else if (hasPublishChanges()) {
      details.push(publishSummaryText());
    }
    return details.join(" | ");
  }

  function addEnabled() {
    return (
      repoIdPattern.test(state.form.id.trim()) &&
      state.form.path.trim().length > 0 &&
      !state.loading &&
      !state.form.selectingPath
    );
  }

  function toggleSidePanel() {
    state.sidePanelCollapsed = !state.sidePanelCollapsed;
    saveSidePanelCollapsed();
    render();
  }

  function sidePanelToggleButton() {
    return h(
      "button",
      {
        class: "side-panel-icon-button",
        title: state.sidePanelCollapsed ? "Expand side panel" : "Collapse side panel",
        "aria-label": state.sidePanelCollapsed
          ? "Expand side panel"
          : "Collapse side panel",
        onclick: toggleSidePanel,
      },
      h("img", { src: "/static/app_icon_128x128.png", alt: "" }),
    );
  }

  function renderPublishButton() {
    return h(
      "button",
      {
        class: "publish-button",
        title: publishSummaryTitle(),
        disabled:
          state.tasks.loading ||
          state.tasks.publishing ||
          !state.tasks.loaded,
        onclick: publishTasks,
      },
      h("span", { class: "publish-label" }, state.tasks.publishing ? "Publishing" : "Publish"),
      h("span", { class: "publish-summary" }, publishSummaryText()),
    );
  }

  function runTaskSearch() {
    if (!state.selectedRepoId || state.tasks.loading || state.tasks.publishing) return;
    if (!prepareTaskPaneForSearch()) return;
    state.tasks.appliedSearchTextEnabled = state.tasks.searchTextEnabled;
    state.tasks.appliedSearchText = state.tasks.searchText.trim();
    state.tasks.appliedFilters = { ...state.tasks.filters };
    state.tasks.appliedFiltersEnabled = { ...state.tasks.filtersEnabled };
    loadTasks(state.selectedRepoId, { preserveExisting: true });
  }

  function clearTaskSearch() {
    const hadAppliedSearch = hasActiveTaskQuery();
    if (hadAppliedSearch && !prepareTaskPaneForSearch()) return;
    state.tasks.searchTextEnabled = false;
    state.tasks.appliedSearchTextEnabled = false;
    state.tasks.searchText = "";
    state.tasks.appliedSearchText = "";
    state.tasks.filters = defaultTaskFilters();
    state.tasks.filtersEnabled = defaultTaskFilterEnabled();
    state.tasks.appliedFilters = defaultTaskFilters();
    state.tasks.appliedFiltersEnabled = defaultTaskFilterEnabled();
    if (hadAppliedSearch && state.selectedRepoId) {
      loadTasks(state.selectedRepoId, { preserveExisting: true });
    } else {
      render();
    }
  }

  function closeTaskPaneWithoutConfirmation() {
    state.tasks.editTaskId = null;
    state.tasks.paneMode = null;
    state.taskEdit = null;
  }

  function prepareTaskPaneForSearch() {
    if (state.tasks.paneMode === "inspect") {
      closeTaskPaneWithoutConfirmation();
      return true;
    }
    if (state.tasks.paneMode === "edit") {
      if (!window.confirm("Close the task editor and run the search?")) {
        return false;
      }
      closeTaskPaneWithoutConfirmation();
    }
    return true;
  }

  function setTaskFilter(field, value) {
    state.tasks.filters[field] = value;
    state.tasks.filtersEnabled[field] = Boolean(String(value || "").trim());
    if (state.tasks.filtersEnabled[field]) {
      state.tasks.searchTextEnabled = false;
    }
  }

  function refreshSearchPanelVisualState() {
    const textField = document.querySelector(".full-text-search-field");
    if (textField) {
      textField.classList.toggle("active", state.tasks.searchTextEnabled);
      const textCheckbox = textField.querySelector("input[type='checkbox']");
      if (textCheckbox) textCheckbox.checked = state.tasks.searchTextEnabled;
    }
    document.querySelectorAll(".task-filter-field[data-field]").forEach((field) => {
      const key = field.getAttribute("data-field");
      const active = Boolean(key && state.tasks.filtersEnabled[key]);
      field.classList.toggle("active", active);
      const checkbox = field.querySelector("input[type='checkbox']");
      if (checkbox) checkbox.checked = active;
    });
  }

  function setTaskFilterEnabled(field, enabled) {
    state.tasks.filtersEnabled[field] = enabled;
    if (enabled) {
      state.tasks.searchTextEnabled = false;
    }
    render();
  }

  function setSearchTextEnabled(enabled) {
    state.tasks.searchTextEnabled = enabled;
    if (enabled) {
      state.tasks.filtersEnabled = defaultTaskFilterEnabled();
    }
    render();
  }

  function setFullTextSearchValue(value) {
    state.tasks.searchText = value;
    state.tasks.searchTextEnabled = Boolean(String(value || "").trim());
    if (state.tasks.searchTextEnabled) {
      state.tasks.filtersEnabled = defaultTaskFilterEnabled();
    }
    refreshSearchPanelVisualState();
  }

  function renderFilterInput(label, field, placeholder) {
    const active = Boolean(state.tasks.filtersEnabled[field]);
    return h(
      "label",
      { class: `task-filter-field ${active ? "active" : ""}`, "data-field": field },
      h(
        "span",
        { class: "task-filter-label" },
        h("input", {
          type: "checkbox",
          checked: active,
          onchange: (event) => setTaskFilterEnabled(field, event.target.checked),
        }),
        h("span", {}, label),
      ),
      h("input", {
        value: state.tasks.filters[field],
        placeholder,
        disabled: state.tasks.loading || state.tasks.publishing || !state.tasks.loaded,
        oninput: (event) => {
          setTaskFilter(field, event.target.value);
          refreshSearchPanelVisualState();
        },
        onkeydown: (event) => {
          if (event.key === "Enter") {
            event.preventDefault();
            runTaskSearch();
          }
        },
      }),
    );
  }

  function renderFilterSelect(label, field, options) {
    const active = Boolean(state.tasks.filtersEnabled[field]);
    return h(
      "label",
      { class: `task-filter-field ${active ? "active" : ""}`, "data-field": field },
      h(
        "span",
        { class: "task-filter-label" },
        h("input", {
          type: "checkbox",
          checked: active,
          onchange: (event) => setTaskFilterEnabled(field, event.target.checked),
        }),
        h("span", {}, label),
      ),
      h(
        "select",
        {
          value: state.tasks.filters[field],
          disabled: state.tasks.loading || state.tasks.publishing || !state.tasks.loaded,
          onchange: (event) => {
            setTaskFilter(field, event.target.value);
            refreshSearchPanelVisualState();
          },
        },
        options.map((option) =>
          h(
            "option",
            { value: option.value, selected: option.value === state.tasks.filters[field] },
            option.label,
          ),
        ),
      ),
    );
  }

  function renderFullTextSearchField() {
    return h(
      "label",
      {
        class: `task-filter-field full-text-search-field ${state.tasks.searchTextEnabled ? "active" : ""}`,
      },
      h(
        "span",
        { class: "task-filter-label" },
        h("input", {
          type: "checkbox",
          checked: state.tasks.searchTextEnabled,
          onchange: (event) => setSearchTextEnabled(event.target.checked),
        }),
        h("span", {}, "Full text search"),
      ),
      h("input", {
        class: "task-search-input",
        type: "search",
        value: state.tasks.searchText,
        placeholder: "Search tasks",
        disabled: state.tasks.loading || state.tasks.publishing || !state.tasks.loaded,
        oninput: (event) => {
          setFullTextSearchValue(event.target.value);
        },
        onpaste: (event) => {
          const text = event.clipboardData && event.clipboardData.getData("text");
          if (text) setFullTextSearchValue(text);
        },
        onkeydown: (event) => {
          if (event.key === "Enter") {
            event.preventDefault();
            runTaskSearch();
          }
        },
      }),
    );
  }

  function priorityFilterOptions() {
    return [
      { value: "", label: "Any" },
      { value: "high", label: "high" },
      { value: "medium", label: "medium" },
      { value: "low", label: "low" },
    ];
  }

  function statusFilterOptions() {
    return [
      { value: "", label: "Any" },
      ...state.tasks.statusOrder.map((status) => ({
        value: status,
        label: statusDisplay(status),
      })),
    ];
  }

  function assigneeFilterOptions() {
    return state.tasks.assignees.map((assignee) => ({
      value: assignee,
      label: assignee ? assigneeDisplay(assignee) : "Any",
    }));
  }

  function toggleSearchPanel() {
    if (state.tasks.searchPanelOpen) {
      state.tasks.searchPanelOpen = false;
      clearTaskSearch();
      return;
    }
    state.tasks.searchPanelOpen = true;
    render();
  }

  function renderSearchToggleButton() {
    return h(
      "button",
      {
        class: `search-toggle ${state.tasks.searchPanelOpen ? "active" : ""}`,
        title: state.tasks.searchPanelOpen ? "Hide search" : "Show search",
        "aria-label": state.tasks.searchPanelOpen ? "Hide search" : "Show search",
        disabled: state.tasks.loading || state.tasks.publishing || !state.tasks.loaded,
        onclick: toggleSearchPanel,
      },
      h("span", { class: "search-toggle-icon" }, "🔍"),
    );
  }

  function renderTaskSearchPanel() {
    if (!state.tasks.searchPanelOpen) return null;
    return h(
      "div",
      { class: "task-search-panel", role: "search" },
      renderFullTextSearchField(),
      h(
        "div",
        { class: "task-filter-grid" },
        renderFilterInput("Title", "title", "login"),
        renderFilterSelect("Status", "status", statusFilterOptions()),
        renderFilterSelect("Assignee", "assignee", assigneeFilterOptions()),
        renderFilterSelect("Priority", "priority", priorityFilterOptions()),
        renderFilterInput("Tag", "tag", "frontend,bug"),
        renderFilterInput("Created by", "createdBy", "alice"),
      ),
      h(
        "div",
        { class: "task-search-actions" },
        h(
          "button",
          {
            class: "command-button",
            disabled: state.tasks.loading || state.tasks.publishing || !state.tasks.loaded,
            onclick: runTaskSearch,
          },
          "Search",
        ),
        h(
          "button",
          {
            class: "command-button",
            disabled:
            state.tasks.loading ||
            state.tasks.publishing ||
            !state.tasks.loaded ||
            (!hasDraftTaskQuery() && !hasActiveTaskQuery()),
            onclick: clearTaskSearch,
          },
          "Clear",
        ),
      ),
    );
  }

  function renderContextActions() {
    if (state.view === "add") {
      return [
        h(
          "button",
          {
            class: "command-button primary",
            disabled: !addEnabled(),
            onclick: addRepo,
          },
          "Add",
        ),
        h(
          "button",
          { class: "command-button", onclick: () => setView("start") },
          "Back",
        ),
      ];
    }
    if (state.view === "create-task") {
      return [
        h(
          "button",
          {
            class: "command-button primary",
            disabled: !createFormValid(),
            onclick: createTask,
          },
          "Create",
        ),
        h(
          "button",
          { class: "command-button", onclick: closeCreateTask },
          "Back",
        ),
      ];
    }
    if (state.view === "start") {
      return [
        h(
          "button",
          { class: "command-button primary", onclick: () => setView("add") },
          "Add repository",
        ),
        h(
          "button",
          { class: "command-button", onclick: loadConfig, disabled: state.loading },
          "Refresh",
        ),
      ];
    }
    if (state.view === "repo") {
      if (state.tasks.paneMode === "edit") {
        return [];
      }
      const actions = [];
      if (hasPublishChanges()) {
        actions.push(renderPublishButton());
      }
      actions.push(renderSearchToggleButton());
      actions.push(
        h(
          "div",
          { class: "segmented-control", role: "group", "aria-label": "Task view mode" },
          h(
            "button",
            {
              class: state.tasks.mode === "list" ? "active" : "",
              onclick: () => {
                state.tasks.mode = "list";
                render();
              },
            },
            "List",
          ),
          h(
            "button",
            {
              class: state.tasks.mode === "board" ? "active" : "",
              onclick: () => {
                state.tasks.mode = "board";
                render();
              },
            },
            "Board",
          ),
        ),
      );
      actions.push(
        h(
          "button",
          {
            class: "command-button",
            onclick: openCreateTask,
            disabled: state.tasks.loading || !state.tasks.loaded || mutationsDisabled(),
          },
          "Create Task",
        ),
      );
      actions.push(
        h(
          "button",
          {
            class: hasRemoteChanges() ? "command-button remote-changes" : "command-button",
            onclick: () => refreshTasks(state.selectedRepoId),
            disabled: state.tasks.loading || state.tasks.publishing,
            title: hasRemoteChanges() ? remoteWarningMessage() : "Refresh",
          },
          "Refresh",
        ),
      );
      actions.push(
        h(
          "button",
          { class: "command-button", onclick: () => setView("start") },
          "Back",
        ),
      );
      return actions;
    }
    if (state.view === "task") {
      return [
        h(
          "button",
          {
            class: "command-button",
            onclick: () => {
              state.view = "repo";
              state.tasks.selectedTaskId = null;
              setMessage("", "");
              render();
            },
          },
          "Back",
        ),
      ];
    }
    return [
      h(
        "button",
        { class: "command-button", onclick: () => setView("start") },
        "Back",
      ),
    ];
  }

  function renderSidePanel() {
    const repoEntries = Object.entries(state.repos).sort(([a], [b]) =>
      a.localeCompare(b),
    );
    const collapsed = state.sidePanelCollapsed;

    return h(
      "aside",
      { class: `side-panel ${collapsed ? "collapsed" : ""}` },
      collapsed
        ? sidePanelToggleButton()
        : h(
            "div",
            { class: "side-panel-header" },
            h(
              "div",
              { class: "side-panel-brand" },
              h(
                "span",
                { class: "side-panel-brand-icon" },
                h("img", { src: "/static/app_icon_128x128.png", alt: "" }),
              ),
              h("h1", {}, "Barnaby"),
            ),
            h(
              "button",
              {
                class: "icon-button",
                title: "Collapse side panel",
                "aria-label": "Collapse side panel",
                onclick: toggleSidePanel,
              },
              "x",
            ),
          ),
      collapsed
        ? h(
            "div",
            { class: "side-panel-top" },
            h(
              "section",
              { class: "nav-section" },
              h(
                "button",
                {
                  class: `nav-item ${state.view === "start" ? "active" : ""}`,
                  title: "Start",
                  "aria-label": "Start",
                  onclick: () => setView("start"),
                },
                "⌂",
              ),
            ),
            h(
              "section",
              { class: "nav-section" },
              repoEntries.length
                ? repoEntries.map(([id, path], index) =>
                    h(
                      "button",
                      {
                        class: `nav-item ${
                          state.selectedRepoId === id ? "active" : ""
                        }`,
                        title: repoLabel(id, path),
                        "aria-label": repoLabel(id, path),
                        onclick: () => selectRepo(id),
                      },
                      String(index + 1),
                    ),
                  )
                : null,
            ),
            h(
              "section",
              { class: "nav-section" },
              h(
                "button",
                {
                  class: `nav-item ${state.view === "settings" ? "active" : ""}`,
                  title: "Settings",
                  "aria-label": "Settings",
                  onclick: () => setView("settings"),
                },
                "⚙",
              ),
            ),
          )
        : h(
            "div",
            { class: "side-panel-top" },
            h(
              "section",
              { class: "nav-section" },
              h("p", { class: "nav-title" }, "Navigation"),
              h(
                "button",
                {
                  class: `nav-item ${state.view === "start" ? "active" : ""}`,
                  onclick: () => setView("start"),
                },
                "Start",
              ),
            ),
            h(
              "section",
              { class: "nav-section" },
              h("p", { class: "nav-title" }, "Repositories"),
              repoEntries.length
                ? repoEntries.map(([id, path]) =>
                    h(
                      "button",
                      {
                        class: `nav-item ${
                          state.selectedRepoId === id ? "active" : ""
                        }`,
                        onclick: () => selectRepo(id),
                      },
                      repoLabel(id, path),
                    ),
                  )
                : h("p", { class: "nav-empty" }, "No repositories registered."),
            ),
            h(
              "section",
              { class: "nav-section" },
              h("p", { class: "nav-title" }, "System"),
              h(
                "button",
                {
                  class: `nav-item ${state.view === "settings" ? "active" : ""}`,
                  onclick: () => setView("settings"),
                },
                "Settings",
              ),
            ),
          ),
    );
  }

  function renderContextLine() {
    const contextMessage = state.message ||
      taskSummaryText() ||
      (state.tasks.loading && state.selectedRepoId ? state.tasks.progress : "") ||
      (state.loading ? "Working..." : "");
    const contextMessageKind = state.message ? state.messageKind : "";
    return h(
      "div",
      { class: "context-line" },
      h(
        "div",
        { class: "context-body" },
        h("h2", { class: "context-title", title: contextTitle() }, contextTitle()),
        contextMessage
          ? h(
              "p",
              {
                class: `context-message ${contextMessageKind}`,
                title: contextMessage,
              },
              contextMessage,
            )
          : null,
      ),
      h("div", { class: "context-actions" }, renderContextActions()),
      renderPipContextButton(),
    );
  }

  function renderPipContextButton() {
    if (!pipAvailable() || state.ai.pipOpen) return null;
    return h(
      "button",
      {
        class: "pip-context-button",
        title: "Open Agent Pip",
        "aria-label": "Open Agent Pip",
        onclick: togglePip,
      },
      h("img", { src: "/static/agent_pip.png", alt: "" }),
    );
  }

  function refreshContextLine() {
    const current = document.querySelector(".context-line");
    if (current) current.replaceWith(renderContextLine());
  }

  function refreshEditSaveButton() {
    const save = document.querySelector(".pane-action.save");
    if (!save) return;
    save.disabled = state.tasks.savingEdit || !editFormDirty() || mutationsDisabled();
  }

  function renderStartView() {
    const repoEntries = Object.entries(state.repos).sort(([a], [b]) =>
      a.localeCompare(b),
    );

    return h(
      "div",
      { class: "tile-grid" },
      repoEntries.map(([id, path]) =>
        h(
          "article",
          { class: "repo-tile" },
          h("h3", { class: "repo-id" }, id),
          h("p", { class: "repo-path" }, path),
          h(
            "div",
            { class: "tile-actions" },
            h(
              "button",
              {
                class: "command-button primary",
                onclick: () => selectRepo(id),
                disabled: state.loading,
              },
              "Open",
            ),
            h(
              "button",
              {
                class: "command-button danger",
                onclick: () => requestDeleteRepo(id),
                disabled: state.loading,
              },
              "Delete",
            ),
          ),
        ),
      ),
    );
  }

  function renderAddView() {
    const id = state.form.id;
    const idInvalid = id.length > 0 && !repoIdPattern.test(id);
    return h(
      "form",
      {
        class: "form",
        onsubmit: (event) => {
          event.preventDefault();
          if (addEnabled()) addRepo();
        },
      },
      h(
        "div",
        { class: "field" },
        h("label", { for: "repo-id" }, "Repository id"),
        h("input", {
          id: "repo-id",
          value: state.form.id,
          autocomplete: "off",
          placeholder: "work_repo",
          oninput: (event) => {
            state.form.id = event.target.value;
            if (state.form.id && !repoIdPattern.test(state.form.id)) {
              setMessage("Repository id must match [a-z0-9_]+.", "error");
            } else {
              setMessage("", "");
            }
            const hint = document.querySelector("#repo-id-hint");
            if (hint) {
              hint.className =
                state.form.id && !repoIdPattern.test(state.form.id)
                  ? "hint error"
                  : "hint";
            }
            refreshContextLine();
          },
        }),
        h(
          "p",
          { id: "repo-id-hint", class: `hint ${idInvalid ? "error" : ""}` },
          "[a-z0-9_]+",
        ),
      ),
      h(
        "div",
        { class: "field" },
        h("label", { for: "repo-path" }, "Repository path"),
        h(
          "div",
          { class: "path-input-row" },
          h("input", {
            id: "repo-path",
            value: state.form.path,
            autocomplete: "off",
            placeholder: "/Users/alex/projects/work-repo",
            disabled: state.form.selectingPath,
            oninput: (event) => {
              state.form.path = event.target.value;
              refreshContextLine();
            },
          }),
          h(
            "button",
            {
              type: "button",
              class: "command-button",
              onclick: selectRepoPath,
              disabled: state.loading || state.form.selectingPath,
            },
            state.form.selectingPath ? "Choosing..." : "Choose...",
          ),
        ),
        h(
          "p",
          { class: "hint" },
          "Choose or enter an absolute path. The server validates that it is a Git work tree.",
        ),
      ),
    );
  }

  function statusDisplay(status) {
    const definition = state.tasks.statuses[status];
    return (definition && definition.display) || status || "Undefined";
  }

  function statusIndex(status) {
    const index = state.tasks.statusOrder.indexOf(status);
    return index >= 0 ? index : state.tasks.statusOrder.length;
  }

  function statusClass(status) {
    return statusPalette[statusIndex(status) % statusPalette.length];
  }

  function orderedTasks() {
    const tasks = state.tasks.taskOrder
      .map((id) => state.tasks.tasksById[id])
      .filter(Boolean)
      .concat(
        (state.tasks.remoteStatus || defaultRemoteStatus()).added
          .filter((file) => file.endsWith(".md"))
          .map((file) => ({
            id: `remote:${file}`,
            title: file,
            status: "backlog",
            priority: "medium",
            story_points: 100,
            remoteAdded: true,
          })),
      )
      .sort((a, b) => {
        const statusDiff = statusIndex(a.status) - statusIndex(b.status);
        if (statusDiff !== 0) return statusDiff;
        return a.id.localeCompare(b.id);
      });
    return tasks;
  }

  function allowedDestinationStatuses(currentStatus) {
    const definition = state.tasks.statuses[currentStatus];
    if (!definition || !Array.isArray(definition.transitions) ||
        definition.transitions.length === 0) {
      return state.tasks.statusOrder.slice();
    }
    const allowed = definition.transitions.filter((status) =>
      state.tasks.statusOrder.includes(status),
    );
    if (!allowed.includes(currentStatus)) allowed.unshift(currentStatus);
    return allowed;
  }

  function refreshTasks(repoId, options) {
    if (!repoId || state.tasks.loading || state.tasks.publishing || !state.tasks.loaded) {
      return;
    }
    loadTasks(repoId, {
      preserveExisting: true,
      silent: Boolean(options && options.silent),
    });
  }

  async function publishTasks() {
    if (!state.selectedRepoId || !hasPublishChanges() || state.tasks.publishing) {
      return;
    }
    state.tasks.publishing = true;
    setMessage("Publishing...", "");
    render();
    try {
      const results = await runBatch(state.selectedRepoId, [
        { cmd: "publish", args: [] },
        { cmd: "sync", args: [] },
        dbstatusCommand(),
      ]);
      applyFinalDbstatus(results);
      if (hasPublishChanges()) {
        setMessage("Published, but unpublished changes remain.", "error");
      } else {
        setMessage("Published and synced.", "ok");
      }
    } catch (err) {
      markPublishStatusStaleIfNeeded(err);
      setMessage(err.message, "error");
    } finally {
      state.tasks.publishing = false;
      render();
    }
  }

  async function moveTask(taskId, nextStatus) {
    const task = state.tasks.tasksById[taskId];
    if (
      !task ||
      task.status === nextStatus ||
      state.tasks.movingTaskIds[taskId] ||
      mutationsDisabled()
    ) return;
    const previousStatus = task.status;
    state.tasks.movingTaskIds[taskId] = true;
    task.status = nextStatus;
    setMessage(`Moving ${taskId} to ${statusDisplay(nextStatus)}...`, "");
    render();
    try {
      const results = await runBatch(state.selectedRepoId, withDbstatus([
        { cmd: "move", args: [taskId, encodeBase64(nextStatus)] },
      ]));
      applyFinalDbstatus(results);
      setMessage(`${taskId} moved to ${statusDisplay(nextStatus)}.`, "ok");
    } catch (err) {
      markPublishStatusStaleIfNeeded(err);
      task.status = previousStatus;
      setMessage(err.message, "error");
    } finally {
      delete state.tasks.movingTaskIds[taskId];
      render();
    }
  }

  async function assignTask(taskId, nextAssignee) {
    const task = state.tasks.tasksById[taskId];
    if (
      !task ||
      task.assignee === nextAssignee ||
      state.tasks.assigningTaskIds[taskId] ||
      mutationsDisabled()
    ) {
      return;
    }
    const previousAssignee = task.assignee || "";
    state.tasks.assigningTaskIds[taskId] = true;
    task.assignee = nextAssignee;
    setMessage(`Assigning ${taskId} to ${assigneeDisplay(nextAssignee)}...`, "");
    render();
    try {
      const results = await runBatch(state.selectedRepoId, withDbstatus([
        { cmd: "assignee", args: [taskId, encodeBase64(nextAssignee)] },
      ]));
      applyFinalDbstatus(results);
      setMessage(`${taskId} assigned to ${assigneeDisplay(nextAssignee)}.`, "ok");
    } catch (err) {
      markPublishStatusStaleIfNeeded(err);
      task.assignee = previousAssignee;
      setMessage(err.message, "error");
    } finally {
      delete state.tasks.assigningTaskIds[taskId];
      render();
    }
  }

  function openCreateTask() {
    state.taskCreate = defaultTaskForm();
    state.taskCreate.status = defaultStatus();
    state.view = "create-task";
    state.tasks.editTaskId = null;
    state.taskEdit = null;
    setMessage("", "");
    render();
  }

  function closeCreateTask() {
    if (
      formHasCreateInput(state.taskCreate) &&
      !window.confirm("Discard the new task?")
    ) {
      return;
    }
    state.taskCreate = defaultTaskForm();
    state.view = "repo";
    setMessage("", "");
    render();
  }

  function taskFormFromTask(task) {
    return {
      title: task.title || "",
      assignee: task.assignee || "",
      priority: task.priority || "medium",
      story_points: normalizeStoryPoints(task.story_points),
      status: task.status || defaultStatus(),
      tags: Array.isArray(task.tags) ? task.tags.slice() : [],
      branches: Array.isArray(task.branches) ? task.branches.slice() : [],
      prs: Array.isArray(task.prs) ? task.prs.slice() : [],
      body: bodyWithoutComments(task.body || defaultBody(task.title || "")),
      bodyTouched: true,
      bodyMode: "edit",
      newComments: [],
      pendingComment: "",
      saving: false,
    };
  }

  function canReplaceTaskPane(taskId, mode) {
    if (state.tasks.editTaskId === taskId && state.tasks.paneMode === mode) {
      return false;
    }
    if (
      state.tasks.paneMode === "edit" &&
      editFormDirty() &&
      !window.confirm("Discard unsaved task changes?")
    ) {
      return false;
    }
    return true;
  }

  function openTaskInspect(taskId) {
    if (!canReplaceTaskPane(taskId, "inspect")) {
      return;
    }
    const task = state.tasks.tasksById[taskId];
    if (!task) return;
    const form = taskFormFromTask(task);
    state.view = "repo";
    state.tasks.editTaskId = taskId;
    state.tasks.paneMode = "inspect";
    state.taskEdit = {
      form,
      original: cloneTaskForm(form),
      comments: commentLines(task.body),
    };
    setMessage("", "");
    render();
  }

  function openTaskEdit(taskId) {
    if (!canReplaceTaskPane(taskId, "edit")) {
      return;
    }
    const task = state.tasks.tasksById[taskId];
    if (!task) return;
    const form = taskFormFromTask(task);
    state.view = "repo";
    state.tasks.editTaskId = taskId;
    state.tasks.paneMode = "edit";
    state.taskEdit = {
      form,
      original: cloneTaskForm(form),
      comments: commentLines(task.body),
    };
    setMessage("", "");
    render();
  }

  function closeTaskEdit() {
    if (
      state.tasks.paneMode === "edit" &&
      editFormDirty() &&
      !window.confirm("Discard unsaved task changes?")
    ) {
      return;
    }
    state.tasks.editTaskId = null;
    state.tasks.paneMode = null;
    state.taskEdit = null;
    setMessage("", "");
    render();
  }

  function buildCreateUpdateCommands(taskId, form) {
    const commands = [];
    if (form.assignee) commands.push({ cmd: "assignee", args: [taskId, encodeBase64(form.assignee)] });
    if (form.priority !== "medium") {
      commands.push({ cmd: "priority", args: [taskId, encodeBase64(form.priority)] });
    }
    if (form.story_points !== 100) {
      commands.push({ cmd: "points", args: [taskId, String(form.story_points)] });
    }
    commands.push({ cmd: "move", args: [taskId, encodeBase64(form.status || "backlog")] });
    if (form.tags.length) commands.push({ cmd: "tags", args: [taskId, encodeBase64(form.tags.join(","))] });
    if (form.branches.length) {
      commands.push({ cmd: "branches", args: [taskId, encodeBase64(form.branches.join(","))] });
    }
    if (form.prs.length) commands.push({ cmd: "prs", args: [taskId, encodeBase64(form.prs.join(","))] });
    const body = taskFormBody(form);
    if (body !== defaultBody(form.title)) {
      commands.push({ cmd: "body", args: [taskId, encodeBase64(body)] });
    }
    return commands;
  }

  async function createTask() {
    const form = state.taskCreate;
    if (!createFormValid()) return;
    form.saving = true;
    setMessage("Creating task...", "");
    render();
    try {
      const createResults = await runBatch(state.selectedRepoId, withDbstatus([
        { cmd: "create", args: [encodeBase64(form.title.trim())] },
      ]));
      applyFinalDbstatus(createResults);
      const createResult = createResults[0];
      const taskId = parseCreatedTaskId(createResult.output);
      const updates = buildCreateUpdateCommands(taskId, form);
      if (updates.length) {
        const updateResults = await runBatch(state.selectedRepoId, withDbstatus(updates));
        applyFinalDbstatus(updateResults);
      }
      state.taskCreate = defaultTaskForm();
      state.view = "repo";
      await loadTasks(state.selectedRepoId, { preserveExisting: false });
      setMessage(`${taskId} created.`, "ok");
    } catch (err) {
      markPublishStatusStaleIfNeeded(err);
      setMessage(err.message, "error");
    } finally {
      form.saving = false;
      render();
    }
  }

  function draftFormValid(form) {
    return Boolean(form && form.selected && form.title.trim());
  }

  async function createTaskFromForm(form) {
    const createResults = await runBatch(state.selectedRepoId, withDbstatus([
      { cmd: "create", args: [encodeBase64(form.title.trim())] },
    ]));
    applyFinalDbstatus(createResults);
    const taskId = parseCreatedTaskId(createResults[0].output);
    const updates = buildCreateUpdateCommands(taskId, form);
    if (updates.length) {
      const updateResults = await runBatch(state.selectedRepoId, withDbstatus(updates));
      applyFinalDbstatus(updateResults);
    }
    return taskId;
  }

  async function createSelectedPipDrafts() {
    const selected = state.ai.drafts.filter(draftFormValid);
    if (!selected.length || !state.selectedRepoId || state.ai.creatingDrafts) {
      return;
    }
    state.ai.creatingDrafts = true;
    setMessage(`Creating ${selected.length} draft task${selected.length === 1 ? "" : "s"}...`, "");
    render();
    const created = [];
    try {
      for (const draft of selected) {
        const taskId = await createTaskFromForm(draft);
        draft.createdTaskId = taskId;
        draft.selected = false;
        created.push(taskId);
      }
      await loadTasks(state.selectedRepoId, { preserveExisting: false });
      setMessage(`Created ${created.join(", ")}.`, "ok");
    } catch (err) {
      markPublishStatusStaleIfNeeded(err);
      setMessage(
        created.length
          ? `Created ${created.join(", ")} before failure: ${err.message}`
          : err.message,
        "error",
      );
    } finally {
      if (created.length) {
        state.ai.drafts = state.ai.drafts.filter((draft) => !draft.createdTaskId);
        if (!state.ai.drafts.length) state.ai.draftsHeight = 0;
      }
      state.ai.creatingDrafts = false;
      render();
    }
  }

  function discardPipDraft(index) {
    state.ai.drafts.splice(index, 1);
    if (!state.ai.drafts.length) state.ai.draftsHeight = 0;
    render();
  }

  function actionTypeLabel(type, taskId) {
    if (type === "update_task") return `Update ${taskId}`;
    if (type === "move_task") return `Move ${taskId}`;
    if (type === "comment_task") return `Comment on ${taskId}`;
    if (type === "add_team_member") return "Add team member";
    return "Apply action";
  }

  function actionDetailLines(action) {
    if (!action) return [];
    if (action.type === "update_task") {
      const fields = action.fields && typeof action.fields === "object" ? action.fields : {};
      return Object.keys(fields).map((field) => {
        const value = Array.isArray(fields[field]) ? fields[field].join(", ") : String(fields[field] || "");
        return `${field}: ${value}`;
      });
    }
    if (action.type === "move_task") return [`status: ${action.status || ""}`];
    if (action.type === "comment_task") return [String(action.message || "")];
    if (action.type === "add_team_member") {
      return [`alias: ${action.alias || ""}`, action.email ? `email: ${action.email}` : ""].filter(Boolean);
    }
    return [];
  }

  function actionFeedbackLabel(action) {
    if (!action) return "proposed action";
    if (action.type === "update_task") {
      const fields = action.fields && typeof action.fields === "object" ? Object.keys(action.fields) : [];
      return `${action.taskId} update${fields.length ? ` (${fields.join(", ")})` : ""}`;
    }
    if (action.type === "move_task") return `${action.taskId} move to ${action.status || "new status"}`;
    if (action.type === "comment_task") return `${action.taskId} comment`;
    if (action.type === "add_team_member") return `team member ${action.alias || "entry"}`;
    return action.title || "proposed action";
  }

  function actionFeedbackList(actions) {
    const labels = actions.map(actionFeedbackLabel);
    if (labels.length <= 3) return labels.join(", ");
    return `${labels.slice(0, 3).join(", ")} and ${labels.length - 3} more`;
  }

  async function applySelectedPipActions() {
    const selected = state.ai.proposedActions.filter((action) => action.selected);
    if (!selected.length || !state.selectedRepoId || state.ai.applyingActions) return;
    state.ai.applyingActions = true;
    setMessage(`Applying ${selected.length} Agent Pip action${selected.length === 1 ? "" : "s"}...`, "");
    render();
    try {
      const response = await fetch("/api/agent/actions/apply", {
        method: "POST",
        headers: apiHeaders(),
        body: JSON.stringify({ repoId: state.selectedRepoId, actions: selected }),
      });
      const result = await response.json();
      if (!response.ok) {
        throw new Error(result.error || `request failed: ${response.status}`);
      }
      state.ai.proposedActions = state.ai.proposedActions.filter((action) => !action.selected);
      if (!state.ai.proposedActions.length) state.ai.proposedActionsHeight = 0;
      await loadTasks(state.selectedRepoId, { preserveExisting: false });
      const applied = result.applied || selected.length;
      const label = actionFeedbackList(selected);
      addPipFeedbackMessage(
        `Applied your selected ${applied === 1 ? "action" : "actions"}: ${label}.`,
      );
      setMessage(`Applied ${applied} Agent Pip action${applied === 1 ? "" : "s"}.`, "ok");
    } catch (err) {
      state.tasks.publishStatusStale = true;
      state.ai.proposedActions = [];
      state.ai.proposedActionsHeight = 0;
      addPipFeedbackMessage(
        `🔴 Apply failed. Barnaby rejected the selected ${selected.length === 1 ? "action" : "actions"}: ${err.message}. No changes were applied.`,
      );
      setMessage(err.message, "error");
    } finally {
      state.ai.applyingActions = false;
      render();
      scrollPipMessagesToBottom();
    }
  }

  function discardPipAction(index) {
    const discarded = state.ai.proposedActions[index];
    state.ai.proposedActions.splice(index, 1);
    if (!state.ai.proposedActions.length) state.ai.proposedActionsHeight = 0;
    addPipFeedbackMessage(
      `Discarded the proposed action: ${actionFeedbackLabel(discarded)}. No changes were applied.`,
    );
    render();
  }

  function discardAllPipActions() {
    if (!state.ai.proposedActions.length) return;
    if (!window.confirm("Discard all Agent Pip proposed actions?")) return;
    const discarded = state.ai.proposedActions.slice();
    state.ai.proposedActions = [];
    state.ai.proposedActionsHeight = 0;
    addPipFeedbackMessage(
      `Discarded ${discarded.length} proposed ${discarded.length === 1 ? "action" : "actions"}: ${actionFeedbackList(discarded)}. No changes were applied.`,
    );
    render();
  }

  function discardAllPipDrafts() {
    if (!state.ai.drafts.length) return;
    if (!window.confirm("Discard all Agent Pip task drafts?")) return;
    state.ai.drafts = [];
    state.ai.draftsHeight = 0;
    render();
  }

  function buildEditCommands(taskId, form, original) {
    const commands = [];
    if (form.title !== original.title) {
      commands.push({ cmd: "title", args: [taskId, encodeBase64(form.title)] });
    }
    if (form.assignee !== original.assignee) {
      commands.push({ cmd: "assignee", args: [taskId, encodeBase64(form.assignee)] });
    }
    if (form.priority !== original.priority) {
      commands.push({ cmd: "priority", args: [taskId, encodeBase64(form.priority)] });
    }
    if (form.story_points !== original.story_points) {
      commands.push({ cmd: "points", args: [taskId, String(form.story_points)] });
    }
    if (form.status !== original.status) {
      commands.push({ cmd: "move", args: [taskId, encodeBase64(form.status)] });
    }
    if (form.tags.join("\n") !== original.tags.join("\n")) {
      commands.push({ cmd: "tags", args: [taskId, encodeBase64(form.tags.join(","))] });
    }
    if (form.branches.join("\n") !== original.branches.join("\n")) {
      commands.push({ cmd: "branches", args: [taskId, encodeBase64(form.branches.join(","))] });
    }
    if (form.prs.join("\n") !== original.prs.join("\n")) {
      commands.push({ cmd: "prs", args: [taskId, encodeBase64(form.prs.join(","))] });
    }
    if (form.body !== original.body) {
      commands.push({ cmd: "body", args: [taskId, encodeBase64(form.body)] });
    }
    const comments = form.pendingComment.trim()
      ? [...form.newComments, form.pendingComment.trim()]
      : form.newComments;
    for (const comment of comments) {
      commands.push({ cmd: "comment", args: [taskId, encodeBase64(comment)] });
    }
    return commands;
  }

  async function saveTaskEdit() {
    if (
      !state.taskEdit ||
      !state.tasks.editTaskId ||
      !editFormDirty() ||
      mutationsDisabled()
    ) return;
    const taskId = state.tasks.editTaskId;
    const form = state.taskEdit.form;
    const commands = buildEditCommands(taskId, form, state.taskEdit.original);
    if (!commands.length) return;
    state.tasks.savingEdit = true;
    setMessage(`Saving ${taskId}...`, "");
    render();
    try {
      const saveResults = await runBatch(state.selectedRepoId, withDbstatus(commands));
      applyFinalDbstatus(saveResults);
      const [taskResult] = await runBatch(state.selectedRepoId, [
        { cmd: "task", args: [taskId, "--json"] },
      ]);
      const task = parseJsonOutput(taskResult.output, `task ${taskId}`);
      state.tasks.tasksById[taskId] = task;
      const nextForm = taskFormFromTask(task);
      state.taskEdit = {
        form: nextForm,
        original: cloneTaskForm(nextForm),
        comments: commentLines(task.body),
      };
      setMessage(`${taskId} saved.`, "ok");
    } catch (err) {
      markPublishStatusStaleIfNeeded(err);
      setMessage(err.message, "error");
    } finally {
      state.tasks.savingEdit = false;
      render();
    }
  }

  function renderStatusChip(status) {
    return h(
      "span",
      { class: `status-chip ${statusClass(status)}` },
      statusDisplay(status),
    );
  }

  function priorityClass(priority) {
    if (priority === "low") return "low";
    if (priority === "high") return "high";
    return "medium";
  }

  function renderStoryPointsPill(task) {
    const points = normalizeStoryPoints(task.story_points);
    return h(
      "button",
      {
        class: "story-points-pill",
        type: "button",
        title: `Edit ${task.id} story points`,
        "aria-label": `Edit ${task.id} story points`,
        onclick: (event) => {
          event.stopPropagation();
          openTaskEdit(task.id);
        },
      },
      String(points),
    );
  }

  function openTask(taskId) {
    openTaskInspect(taskId);
  }

  function renderTaskTile(task, options) {
    const withChanger = options && options.withChanger;
    const remoteAdded = task.remoteAdded === true;
    const remoteChanged = !remoteAdded && remoteTaskChanged(task.id);
    const moving = Boolean(state.tasks.movingTaskIds[task.id]);
    const assigning = Boolean(state.tasks.assigningTaskIds[task.id]);
    const busy = moving || assigning;
    const locked = remoteAdded || remoteChanged;
    const controlsDisabled = remoteChanged || mutationsDisabled();
    return h(
      "article",
      {
        class: `task-tile ${busy ? "moving" : ""}${taskPublishClass(task.id)}${remoteChanged ? " remote-changed" : ""}${remoteAdded ? " remote-added" : ""}`,
        onclick: locked ? undefined : () => openTask(task.id),
      },
      h(
        "div",
        { class: `task-tile-header ${priorityClass(task.priority)}` },
        remoteAdded
          ? h("span", { class: "task-id" }, "")
          : h("span", { class: "task-id", title: task.id }, taskIdLabel(task.id)),
        remoteAdded
          ? null
          : h(
              "div",
              { class: "task-header-actions" },
              h(
                "button",
                {
                  class: "task-edit-button inspect",
                  title: `Inspect ${task.id}`,
                  "aria-label": `Inspect ${task.id}`,
                  disabled: remoteChanged,
                  onclick: (event) => {
                    event.stopPropagation();
                    openTaskInspect(task.id);
                  },
                },
                "👀",
              ),
              h(
                "button",
                {
                  class: "task-edit-button edit",
                  title: `Edit ${task.id}`,
                  "aria-label": `Edit ${task.id}`,
                  disabled: remoteChanged,
                  onclick: (event) => {
                    event.stopPropagation();
                    openTaskEdit(task.id);
                  },
                },
                "✎",
              ),
            ),
      ),
      h("h3", { class: "task-title", title: task.title || "(untitled)" }, task.title || "(untitled)"),
      h(
        "div",
        { class: "task-tile-actions" },
        withChanger && !remoteAdded
          ? h(
              "select",
              {
                class: "status-select",
                value: task.status,
                disabled: moving || controlsDisabled,
                onclick: (event) => event.stopPropagation(),
                onchange: (event) => {
                  event.stopPropagation();
                  moveTask(task.id, event.target.value);
                },
              },
              allowedDestinationStatuses(task.status).map((status) =>
                h(
                  "option",
                  { value: status, selected: status === task.status },
                  statusDisplay(status),
                ),
              ),
            )
          : renderStatusChip(task.status),
        remoteAdded ? null : renderStoryPointsPill(task),
      ),
      remoteAdded
        ? null
        : h(
            "div",
            { class: "task-controls" },
            h(
              "select",
              {
                class: "assignee-select",
                value: task.assignee || "",
                disabled: assigning || controlsDisabled,
                onclick: (event) => event.stopPropagation(),
                onchange: (event) => {
                  event.stopPropagation();
                  assignTask(task.id, event.target.value);
                },
              },
              assigneeOptionsForTask(task).map((assignee) =>
                h(
                  "option",
                  { value: assignee, selected: assignee === (task.assignee || "") },
                  assigneeDisplay(assignee),
                ),
              ),
            ),
            busy
              ? h(
                  "span",
                  { class: "task-busy-label" },
                  assigning ? "Assigning..." : "Moving...",
                )
              : null,
          ),
    );
  }

  function renderTaskLoading() {
    return h(
      "div",
      { class: "task-state-panel" },
      h("p", { class: "task-state-title" }, "Loading tasks"),
      h("p", { class: "task-state-message" }, state.tasks.progress || "Loading tasks..."),
    );
  }

  function renderTaskFailure() {
    return h(
      "div",
      { class: "task-state-panel error" },
      h("p", { class: "task-state-title" }, "Could not load tasks"),
      h("p", { class: "task-state-message" }, state.tasks.error || "Unknown error"),
      h("p", { class: "task-state-meta" }, `Repository: ${state.selectedRepoId}`),
      h(
        "button",
        {
          class: "command-button primary",
          onclick: () => loadTasks(state.selectedRepoId, { preserveExisting: false }),
          disabled: state.tasks.loading,
        },
        "Retry",
      ),
    );
  }

  function renderTaskViewHeader(tasks) {
    return state.tasks.error
      ? h(
          "div",
          { class: "task-view-header" },
          h("p", { class: "task-view-warning" }, state.tasks.error),
        )
      : null;
  }

  function renderTaskList(tasks) {
    return h(
      "div",
      { class: "task-list" },
      tasks.map((task) => renderTaskTile(task, { withChanger: true })),
    );
  }

  function boardColumns(tasks) {
    const byStatus = new Map();
    for (const status of state.tasks.statusOrder) byStatus.set(status, []);
    for (const task of tasks) {
      if (!byStatus.has(task.status)) byStatus.set(task.status, []);
      byStatus.get(task.status).push(task);
    }
    return Array.from(byStatus.entries()).filter(([status, items]) => {
      const definition = state.tasks.statuses[status];
      return items.length > 0 || !definition || definition.visible !== false;
    });
  }

  function renderTaskBoard(tasks) {
    return h(
      "div",
      { class: "task-board" },
      boardColumns(tasks).map(([status, items]) =>
        h(
          "section",
          { class: "board-column" },
          h(
            "div",
            { class: `board-column-header ${statusClass(status)}` },
            h("span", {}, statusDisplay(status)),
            h("span", { class: "board-count" }, String(items.length)),
          ),
          h(
            "div",
            { class: "board-column-body" },
            items.length
              ? items.map((task) => renderTaskTile(task, { withChanger: true }))
              : h("p", { class: "board-empty" }, "No tasks"),
          ),
        ),
      ),
    );
  }

  function renderTokenField(label, form, field, kind, placeholder) {
    const inputId = `token-${field}`;
    const submit = () => {
      const input = document.getElementById(inputId);
      const value = input ? input.value : "";
      addListValue(form, field, kind, value);
    };
    return h(
      "div",
      { class: "field" },
      h("label", {}, label),
      h(
        "div",
        { class: "token-box" },
        h("input", {
          id: inputId,
          class: "token-input",
          placeholder,
          onkeydown: (event) => {
            if (event.key === "Enter") {
              event.preventDefault();
              submit();
            }
          },
        }),
        h(
          "button",
          {
            type: "button",
            class: "token-submit",
            title: `Add ${kind}`,
            "aria-label": `Add ${kind}`,
            onclick: submit,
          },
          "↵",
        ),
        form[field].map((value) =>
          h(
            "span",
            { class: "token" },
            value,
            h(
              "button",
              {
                type: "button",
                class: "token-remove",
                title: `Remove ${value}`,
                onclick: () => removeListValue(form, field, value),
              },
              "x",
            ),
          ),
        ),
      ),
    );
  }

  function renderTaskForm(form, options) {
    const isCreate = options && options.isCreate;
    const bodyValue = isCreate ? taskFormBody(form) : form.body;
    return h(
      "form",
      {
        class: options && options.compact ? "task-form compact" : "task-form",
        onsubmit: (event) => event.preventDefault(),
      },
      h(
        "div",
        { class: "field" },
        h("label", { for: "task-title" }, "Title"),
        h("input", {
          id: "task-title",
          value: form.title,
          autocomplete: "off",
          oninput: (event) => {
            form.title = event.target.value;
            refreshContextLine();
            refreshEditSaveButton();
          },
        }),
      ),
      h(
        "div",
        { class: "task-form-grid" },
        h(
          "div",
          { class: "field" },
          h("label", { for: "task-assignee" }, "Assignee"),
          h(
            "select",
            {
              id: "task-assignee",
              value: form.assignee,
              onchange: (event) => {
                form.assignee = event.target.value;
                render();
              },
            },
            formAssigneeOptions(form).map((assignee) =>
              h(
                "option",
                { value: assignee, selected: assignee === form.assignee },
                assigneeDisplay(assignee),
              ),
            ),
          ),
        ),
        h(
          "div",
          { class: "field" },
          h("label", { for: "task-priority" }, "Priority"),
          h(
            "select",
            {
              id: "task-priority",
              value: form.priority,
              onchange: (event) => {
                form.priority = event.target.value;
                render();
              },
            },
            ["high", "medium", "low"].map((priority) =>
              h("option", { value: priority, selected: priority === form.priority }, priority),
            ),
          ),
        ),
        h(
          "div",
          { class: "field" },
          h("label", { for: "task-points" }, "Points"),
          h(
            "select",
            {
              id: "task-points",
              value: String(form.story_points),
              onchange: (event) => {
                form.story_points = normalizeStoryPoints(event.target.value);
                render();
              },
            },
            storyPointOptions().map((points) =>
              h("option", { value: String(points), selected: points === form.story_points }, String(points)),
            ),
          ),
        ),
        h(
          "div",
          { class: "field" },
          h("label", { for: "task-status" }, "Status"),
          h(
            "select",
            {
              id: "task-status",
              value: form.status,
              onchange: (event) => {
                form.status = event.target.value;
                render();
              },
            },
            statusOptionsForForm(form).map((status) =>
              h(
                "option",
                { value: status, selected: status === form.status },
                statusDisplay(status),
              ),
            ),
          ),
        ),
      ),
      renderTokenField("Tags", form, "tags", "tag", "frontend"),
      renderTokenField("Branches", form, "branches", "branch", "feature/task-edit"),
      renderTokenField("PRs", form, "prs", "PR", "pull/123"),
      h(
        "div",
        { class: "field" },
        h(
          "div",
          { class: "field-line" },
          h("label", { for: "task-body" }, "Body"),
          h(
            "div",
            { class: "segmented-control small" },
            h(
              "button",
              {
                type: "button",
                class: form.bodyMode === "edit" ? "active" : "",
                onclick: () => {
                  form.bodyMode = "edit";
                  render();
                },
              },
              "Edit",
            ),
            h(
              "button",
              {
                type: "button",
                class: form.bodyMode === "preview" ? "active" : "",
                onclick: () => {
                  form.bodyMode = "preview";
                  render();
                },
              },
              "Preview",
            ),
          ),
        ),
        form.bodyMode === "preview"
          ? h("div", { class: "markdown-preview" }, markdownPreview(bodyValue))
          : h("textarea", {
              id: "task-body",
              value: bodyValue,
              oninput: (event) => {
                form.body = event.target.value;
                form.bodyTouched = true;
                refreshContextLine();
                refreshEditSaveButton();
              },
            }),
      ),
      options && options.showComments
        ? renderCommentsEditor(form, options.existingComments)
        : null,
    );
  }

  function renderCommentsEditor(form, existingComments) {
    return h(
      "section",
      { class: "comments-editor" },
      h("div", { class: "comments-header" }, h("span", {}, "Comments")),
      existingComments && existingComments.length
        ? h(
            "div",
            { class: "comments-list" },
            existingComments.map((comment) =>
              h("p", { class: "comment-row readonly" }, comment),
            ),
          )
        : null,
      form.newComments.length
        ? h(
            "div",
            { class: "comments-list" },
            form.newComments.map((comment, index) =>
              h(
                "p",
                { class: "comment-row" },
                comment,
                h(
                  "button",
                  {
                    type: "button",
                    class: "comment-remove",
                    onclick: () => {
                      form.newComments.splice(index, 1);
                      render();
                    },
                  },
                  "x",
                ),
              ),
            ),
          )
        : null,
      h("textarea", {
        class: "comment-input",
        placeholder: "Add a comment",
        value: form.pendingComment,
        oninput: (event) => {
          form.pendingComment = event.target.value;
          refreshContextLine();
          refreshEditSaveButton();
        },
      }),
      h(
        "button",
        {
          type: "button",
          class: "command-button",
          onclick: () => {
            const comment = form.pendingComment.trim();
            if (!comment) return;
            form.newComments.push(comment);
            form.pendingComment = "";
            render();
          },
        },
        "+ Comment",
      ),
    );
  }

  function renderCreateTaskView() {
    return renderTaskForm(state.taskCreate, { isCreate: true });
  }

  function renderReadonlyList(values) {
    const items = Array.isArray(values) ? values : [];
    if (!items.length) return h("p", { class: "readonly-empty" }, "None");
    return h(
      "div",
      { class: "readonly-token-list" },
      items.map((value) => h("span", { class: "token readonly" }, value)),
    );
  }

  function renderReadonlyField(label, value) {
    return h(
      "div",
      { class: "readonly-field" },
      h("span", { class: "readonly-label" }, label),
      h("span", { class: "readonly-value" }, value || "None"),
    );
  }

  function renderInspectPane() {
    if (!state.taskEdit || !state.tasks.editTaskId) return null;
    const form = state.taskEdit.form;
    return h(
      "aside",
      { class: "task-edit-pane" },
      h(
        "div",
        { class: "task-edit-pane-header" },
        h("h3", {}, state.tasks.editTaskId),
        h(
          "div",
          { class: "task-edit-pane-actions" },
          h(
            "button",
            {
              class: "icon-button pane-action edit",
              title: "Edit task",
              "aria-label": "Edit task",
              onclick: () => openTaskEdit(state.tasks.editTaskId),
            },
            "✎",
          ),
          h(
            "button",
            {
              class: "icon-button pane-action close",
              title: "Close task inspector",
              "aria-label": "Close task inspector",
              onclick: closeTaskEdit,
            },
            "×",
          ),
        ),
      ),
      h(
        "div",
        { class: "inspect-pane-content" },
        renderReadonlyField("Title", form.title || "(untitled)"),
        h(
          "div",
          { class: "readonly-grid" },
          renderReadonlyField("Assignee", assigneeDisplay(form.assignee)),
          renderReadonlyField("Priority", form.priority),
          renderReadonlyField("Points", String(form.story_points)),
          renderReadonlyField("Status", statusDisplay(form.status)),
        ),
        h("div", { class: "readonly-field" }, h("span", { class: "readonly-label" }, "Tags"), renderReadonlyList(form.tags)),
        h("div", { class: "readonly-field" }, h("span", { class: "readonly-label" }, "Branches"), renderReadonlyList(form.branches)),
        h("div", { class: "readonly-field" }, h("span", { class: "readonly-label" }, "PRs"), renderReadonlyList(form.prs)),
        h(
          "div",
          { class: "readonly-field" },
          h("span", { class: "readonly-label" }, "Body"),
          h("div", { class: "markdown-preview" }, markdownPreview(form.body)),
        ),
        state.taskEdit.comments.length
          ? h(
              "section",
              { class: "comments-editor readonly" },
              h("div", { class: "comments-header" }, h("span", {}, "Comments")),
              h(
                "div",
                { class: "comments-list" },
                state.taskEdit.comments.map((comment) =>
                  h("p", { class: "comment-row readonly" }, comment),
                ),
              ),
            )
          : null,
      ),
    );
  }

  function renderEditPane() {
    if (!state.taskEdit || !state.tasks.editTaskId) return null;
    if (state.tasks.paneMode === "inspect") return renderInspectPane();
    return h(
      "aside",
      { class: "task-edit-pane" },
      h(
        "div",
        { class: "task-edit-pane-header" },
        h("h3", {}, state.tasks.editTaskId),
        h(
          "div",
          { class: "task-edit-pane-actions" },
          h(
            "button",
            {
              class: "icon-button pane-action save",
              title: "Save task",
              "aria-label": "Save task",
              disabled: state.tasks.savingEdit || !editFormDirty() || mutationsDisabled(),
              onclick: saveTaskEdit,
            },
            "✓",
          ),
          h(
            "button",
            {
              class: "icon-button pane-action close",
              title: "Close task editor",
              "aria-label": "Close task editor",
              disabled: state.tasks.savingEdit,
              onclick: closeTaskEdit,
            },
            "×",
          ),
        ),
      ),
      renderTaskForm(state.taskEdit.form, {
        compact: true,
        showComments: true,
        existingComments: state.taskEdit.comments,
      }),
    );
  }

  function renderRepoView() {
    const path = state.repos[state.selectedRepoId];
    let body = null;
    if (!path) {
      body = h("p", { class: "empty-state" }, "Repository not found.");
      return h("div", { class: "repo-surface" }, body);
    }
    if (state.tasks.loading && !state.tasks.loaded) {
      return h("div", { class: "repo-surface" }, renderTaskLoading());
    }
    if (state.tasks.loadState === "failed" && !state.tasks.loaded) {
      return h("div", { class: "repo-surface" }, renderTaskFailure());
    }

    const tasks = orderedTasks();
    if (!state.tasks.loaded && !tasks.length) {
      body = h(
        "div",
        { class: "task-loading" },
        h("p", {}, "Tasks have not been loaded yet."),
        h(
          "button",
          {
            class: "command-button primary",
            onclick: () => loadTasks(state.selectedRepoId, { preserveExisting: false }),
          },
          "Load tasks",
        ),
      );
      return h("div", { class: "repo-surface" }, body);
    }
    if (!tasks.length) {
      body = h(
        "div",
        { class: "tasks-view" },
        renderTaskSearchPanel(),
        h(
          "div",
          { class: "task-state-panel" },
          h("p", { class: "task-state-title" }, "No tasks"),
          h("p", { class: "task-state-message" }, "No tasks found in this repository."),
          h(
            "button",
            { class: "command-button primary", onclick: openCreateTask },
            "New task",
          ),
        ),
        renderEditPane(),
      );
      return h("div", { class: "repo-surface" }, body);
    }
    body = h(
      "div",
      { class: state.tasks.editTaskId ? "tasks-view with-edit-pane" : "tasks-view" },
      renderTaskSearchPanel(),
      h(
        "div",
        { class: "tasks-workspace" },
        renderTaskViewHeader(tasks),
        state.tasks.mode === "board" ? renderTaskBoard(tasks) : renderTaskList(tasks),
      ),
      renderEditPane(),
    );
    return h("div", { class: "repo-surface" }, body);
  }

  function renderAgentPip() {
    if (!pipAvailable() || !state.ai.pipOpen) return null;
    const reviewingOutput =
      state.ai.drafts.length > 0 || state.ai.proposedActions.length > 0;
    return h(
      "aside",
      {
        class: "agent-dock",
        style: `width: ${clampPipPanelWidth(state.ai.pipPanelWidth)}px`,
      },
      h("div", {
        class: "agent-resizer",
        title: "Resize Agent Pip",
        onmousedown: startPipPanelResize,
      }),
      h(
        "div",
        { class: "agent-panel" },
        h(
          "div",
          { class: "agent-header" },
          h(
            "div",
            { class: "agent-title" },
            h(
              "span",
              { class: "agent-title-icon" },
              h("img", { src: "/static/agent_pip.png", alt: "" }),
            ),
            h("strong", {}, "Agent Pip"),
          ),
          h(
            "div",
            { class: "agent-header-actions" },
            h(
              "button",
              {
                class: "icon-button",
                title: "Clear chat history",
                disabled: state.ai.sending || !state.ai.chatMessages.length,
                onclick: clearPipHistory,
              },
              "⌫",
            ),
            h(
              "button",
              { class: "icon-button", title: "Close Pip", onclick: togglePip },
              "x",
            ),
          ),
        ),
        h(
          "div",
          { class: "agent-messages" },
          renderPipMessageNodes(),
        ),
        renderPipDraftReview(),
        renderPipActionReview(),
        reviewingOutput
          ? null
          : h(
              "form",
              {
                class: "agent-form",
                onsubmit: (event) => {
                  event.preventDefault();
                  sendPipMessage();
                },
              },
              h("textarea", {
                rows: "3",
                value: state.ai.chatInput,
                placeholder: "Message Pip...",
                disabled: state.ai.sending,
                oninput: (event) => {
                  state.ai.chatInput = event.target.value;
                  const send = event.target.form.querySelector(".agent-send");
                  if (send) {
                    send.disabled = state.ai.sending || !state.ai.chatInput.trim();
                  }
                },
              }),
              h(
                "button",
                {
                  class: "command-button primary agent-send",
                  disabled: state.ai.sending || !state.ai.chatInput.trim(),
                },
                state.ai.sending ? "Waiting for response..." : "Send",
              ),
            ),
      ),
    );
  }

  function renderPipMessageNodes() {
    const nodes = state.ai.chatMessages.length
      ? state.ai.chatMessages.map((message) =>
          h(
            "div",
            { class: `agent-message ${message.role || "assistant"}` },
            renderPipMarkdown(message.content || ""),
          ),
        )
      : [
          h(
            "div",
            { class: "agent-message assistant" },
            renderPipMarkdown("Ask about tasks in this repository."),
          ),
        ];
    if (state.ai.sending) nodes.push(renderPipWaitingMessage());
    return nodes;
  }

  function renderPipWaitingMessage() {
    return h(
      "div",
      { class: "agent-message assistant agent-message-waiting", "aria-label": "Waiting for response" },
      h("div", { class: "agent-waiting-block" }),
    );
  }

  function reviewAreaMaxHeight() {
    const panel = document.querySelector(".agent-panel");
    const panelHeight = panel ? panel.clientHeight : Math.max(360, window.innerHeight - 80);
    return Math.max(180, Math.floor(panelHeight * 0.75));
  }

  function reviewAreaInitialHeight(count, itemHeight) {
    const estimated = 112 + count * itemHeight;
    return Math.min(Math.max(180, estimated), reviewAreaMaxHeight());
  }

  function reviewAreaHeightState(kind) {
    return kind === "drafts" ? "draftsHeight" : "proposedActionsHeight";
  }

  function reviewAreaCount(kind) {
    return kind === "drafts" ? state.ai.drafts.length : state.ai.proposedActions.length;
  }

  function reviewAreaItemHeight(kind) {
    return kind === "drafts" ? 260 : 126;
  }

  function reviewAreaStyle(kind) {
    const stateKey = reviewAreaHeightState(kind);
    if (!state.ai[stateKey]) {
      state.ai[stateKey] = reviewAreaInitialHeight(
        reviewAreaCount(kind),
        reviewAreaItemHeight(kind),
      );
    }
    const maxHeight = reviewAreaMaxHeight();
    const height = Math.min(state.ai[stateKey], maxHeight);
    state.ai[stateKey] = height;
    return `height: ${height}px; max-height: ${maxHeight}px;`;
  }

  function startReviewAreaResize(kind, event) {
    event.preventDefault();
    const stateKey = reviewAreaHeightState(kind);
    const startY = event.clientY;
    const startHeight = state.ai[stateKey] || event.currentTarget.parentElement.offsetHeight;

    function onMove(moveEvent) {
      const nextHeight = Math.min(
        Math.max(180, startHeight + startY - moveEvent.clientY),
        reviewAreaMaxHeight(),
      );
      state.ai[stateKey] = nextHeight;
      const panel = document.querySelector(`.agent-review-area.${kind}`);
      if (panel) panel.style.height = `${nextHeight}px`;
    }

    function onEnd() {
      window.removeEventListener("mousemove", onMove);
      window.removeEventListener("mouseup", onEnd);
    }

    window.addEventListener("mousemove", onMove);
    window.addEventListener("mouseup", onEnd);
  }

  function renderPipActionReview() {
    if (!state.ai.proposedActions.length) return null;
    const selectedCount = state.ai.proposedActions.filter((action) => action.selected).length;
    return h(
      "section",
      {
        class: "agent-drafts agent-review-area actions",
        style: reviewAreaStyle("actions"),
      },
      h("div", {
        class: "agent-review-resize-handle",
        title: "Resize Proposed Actions",
        onmousedown: (event) => startReviewAreaResize("actions", event),
      }),
      h(
        "div",
        { class: "agent-drafts-header" },
        h("strong", {}, "Proposed actions"),
        h(
          "button",
          {
            type: "button",
            class: "icon-button",
            title: "Discard all actions",
            disabled: state.ai.applyingActions,
            onclick: discardAllPipActions,
          },
          "×",
        ),
      ),
      state.ai.proposedActions.map((action, index) =>
        h(
          "article",
          { class: `agent-draft ${action.selected ? "selected" : ""}` },
          h(
            "label",
            { class: "agent-draft-select" },
            h("input", {
              type: "checkbox",
              checked: action.selected,
              disabled: state.ai.applyingActions,
              onchange: (event) => {
                action.selected = event.target.checked;
                render();
              },
            }),
            "Apply",
          ),
          h("strong", {}, action.title || actionTypeLabel(action.type, action.taskId)),
          action.summary ? h("p", { class: "agent-action-summary" }, action.summary) : null,
          h(
            "div",
            { class: "agent-action-details" },
            actionDetailLines(action).map((line) => h("p", {}, line)),
          ),
          h(
            "div",
            { class: "agent-draft-actions" },
            h(
              "button",
              {
                type: "button",
                class: "command-button danger",
                disabled: state.ai.applyingActions,
                onclick: () => discardPipAction(index),
              },
              "Discard",
            ),
          ),
        ),
      ),
      h(
        "div",
        { class: "agent-drafts-actions" },
        h(
          "button",
          {
            type: "button",
            class: "command-button primary",
            disabled: state.ai.applyingActions || selectedCount === 0,
            onclick: applySelectedPipActions,
          },
          state.ai.applyingActions
            ? "Applying..."
            : `Apply selected (${selectedCount})`,
        ),
      ),
    );
  }

  function renderPipDraftReview() {
    if (!state.ai.drafts.length) return null;
    const selectedCount = state.ai.drafts.filter(draftFormValid).length;
    return h(
      "section",
      {
        class: "agent-drafts agent-review-area drafts",
        style: reviewAreaStyle("drafts"),
      },
      h("div", {
        class: "agent-review-resize-handle",
        title: "Resize Task Drafts",
        onmousedown: (event) => startReviewAreaResize("drafts", event),
      }),
      h(
        "div",
        { class: "agent-drafts-header" },
        h("strong", {}, "Task drafts"),
        h(
          "button",
          {
            type: "button",
            class: "icon-button",
            title: "Discard all drafts",
            disabled: state.ai.creatingDrafts,
            onclick: discardAllPipDrafts,
          },
          "×",
        ),
      ),
      state.ai.drafts.map((draft, index) =>
        h(
          "article",
          { class: `agent-draft ${draft.selected ? "selected" : ""}` },
          h(
            "label",
            { class: "agent-draft-select" },
            h("input", {
              type: "checkbox",
              checked: draft.selected,
              disabled: state.ai.creatingDrafts,
              onchange: (event) => {
                draft.selected = event.target.checked;
                render();
              },
            }),
            "Create",
          ),
          h(
            "div",
            { class: "field compact" },
            h("label", {}, "Title"),
            h("input", {
              value: draft.title,
              disabled: state.ai.creatingDrafts,
              oninput: (event) => {
                draft.title = event.target.value;
              },
            }),
          ),
          h(
            "div",
            { class: "agent-draft-grid" },
            h(
              "div",
              { class: "field compact" },
              h("label", {}, "Assignee"),
              h(
                "select",
                {
                  value: draft.assignee,
                  disabled: state.ai.creatingDrafts,
                  onchange: (event) => {
                    draft.assignee = event.target.value;
                    render();
                  },
                },
                formAssigneeOptions(draft).map((assignee) =>
                  h("option", { value: assignee, selected: assignee === draft.assignee }, assigneeDisplay(assignee)),
                ),
              ),
            ),
            h(
              "div",
              { class: "field compact" },
              h("label", {}, "Priority"),
              h(
                "select",
                {
                  value: draft.priority,
                  disabled: state.ai.creatingDrafts,
                  onchange: (event) => {
                    draft.priority = event.target.value;
                    render();
                  },
                },
                ["high", "medium", "low"].map((priority) =>
                  h("option", { value: priority, selected: priority === draft.priority }, priority),
                ),
              ),
            ),
            h(
              "div",
              { class: "field compact" },
              h("label", {}, "Points"),
              h(
                "select",
                {
                  value: String(draft.story_points),
                  disabled: state.ai.creatingDrafts,
                  onchange: (event) => {
                    draft.story_points = normalizeStoryPoints(event.target.value);
                    render();
                  },
                },
                storyPointOptions().map((points) =>
                  h("option", { value: String(points), selected: points === draft.story_points }, String(points)),
                ),
              ),
            ),
            h(
              "div",
              { class: "field compact" },
              h("label", {}, "Status"),
              h(
                "select",
                {
                  value: draft.status,
                  disabled: state.ai.creatingDrafts,
                  onchange: (event) => {
                    draft.status = event.target.value;
                    render();
                  },
                },
                statusOptionsForForm(draft).map((status) =>
                  h("option", { value: status, selected: status === draft.status }, statusDisplay(status)),
                ),
              ),
            ),
          ),
          h(
            "div",
            { class: "field compact" },
            h("label", {}, "Tags"),
            h("input", {
              value: draft.tags.join(","),
              placeholder: "frontend,release",
              disabled: state.ai.creatingDrafts,
              oninput: (event) => {
                draft.tags = event.target.value
                  .split(",")
                  .map((tag) => tag.trim())
                  .filter((tag) => tagPattern.test(tag));
              },
            }),
          ),
          h(
            "div",
            { class: "field compact" },
            h("label", {}, "Body"),
            h("textarea", {
              value: taskFormBody(draft),
              disabled: state.ai.creatingDrafts,
              oninput: (event) => {
                draft.body = event.target.value;
                draft.bodyTouched = true;
              },
            }),
          ),
          h(
            "div",
            { class: "agent-draft-actions" },
            h(
              "button",
              {
                type: "button",
                class: "command-button danger",
                disabled: state.ai.creatingDrafts,
                onclick: () => discardPipDraft(index),
              },
              "Discard",
            ),
          ),
        ),
      ),
      h(
        "div",
        { class: "agent-drafts-actions" },
        h(
          "button",
          {
            type: "button",
            class: "command-button primary",
            disabled: state.ai.creatingDrafts || selectedCount === 0,
            onclick: createSelectedPipDrafts,
          },
          state.ai.creatingDrafts
            ? "Creating..."
            : `Create selected (${selectedCount})`,
        ),
      ),
    );
  }

  function renderTaskView() {
    const task = state.tasks.tasksById[state.tasks.selectedTaskId];
    if (!task) return h("p", { class: "empty-state" }, "Task not found.");
    return h(
      "div",
      { class: "task-detail-stub" },
      h(
        "div",
        { class: "task-detail-header" },
        h("span", { class: "task-id", title: task.id }, taskIdLabel(task.id)),
        renderStatusChip(task.status),
      ),
      h("h3", { class: "task-title large" }, task.title || "(untitled)"),
      h("p", { class: "task-meta" }, "Task editing is not implemented yet."),
    );
  }

  function renderRepoFileAccessSettings() {
    if (!state.selectedRepoId) {
      return h(
        "div",
        { class: "form" },
        h("h3", { class: "form-title" }, "Agent Pip file access"),
        h(
          "p",
          { class: "empty-state" },
          "Open a repository before editing Agent Pip file access.",
        ),
      );
    }

    const access = state.ai.fileAccess;
    const rows = access.folders.slice().sort((a, b) => a.path.localeCompare(b.path));
    return h(
      "div",
      { class: "form" },
      h("h3", { class: "form-title" }, "Agent Pip file access"),
      h(
        "div",
        { class: "settings-list" },
        h(
          "div",
          { class: "settings-row" },
          h("div", { class: "settings-key" }, "Repository"),
          h(
            "div",
            { class: "settings-value" },
            repoLabel(state.selectedRepoId, state.repos[state.selectedRepoId] || access.repoPath),
          ),
        ),
      ),
      access.loading
        ? h("p", { class: "empty-state" }, "Loading file access policy...")
        : rows.length
          ? h(
              "div",
              { class: "file-access-table" },
              h(
                "div",
                { class: "file-access-head" },
                h("div", {}, "Folder"),
                h("div", {}, "Access"),
              ),
              rows.map((folder) =>
                h(
                  "div",
                  { class: "file-access-row" },
                  h("div", { class: "file-access-path" }, folder.path),
                  h(
                    "select",
                    {
                      value: folder.access,
                      disabled: access.saving,
                      onchange: (event) => {
                        setRepoFolderAccess(folder.path, event.target.value);
                        render();
                      },
                    },
                    h(
                      "option",
                      {
                        value: "forbidden",
                        selected: folder.access === "forbidden",
                      },
                      "forbidden",
                    ),
                    h(
                      "option",
                      {
                        value: "read_only",
                        selected: folder.access === "read_only",
                      },
                      "read-only",
                    ),
                    h(
                      "option",
                      {
                        value: "full_access",
                        selected: folder.access === "full_access",
                      },
                      "full access",
                    ),
                  ),
                ),
              ),
            )
          : h("p", { class: "empty-state" }, "No top-level folders found."),
      h(
        "p",
        { class: "hint" },
        "Each top-level folder policy applies automatically to all files and subfolders below it.",
      ),
      h(
        "div",
        { class: "form-actions" },
        h(
          "button",
          {
            type: "button",
            class: "command-button primary",
            disabled: access.loading || access.saving,
            onclick: saveRepoFileAccess,
          },
          access.saving ? "Saving..." : "Save file access",
        ),
        h(
          "button",
          {
            type: "button",
            class: "command-button",
            disabled: access.loading || access.saving,
            onclick: resetRepoFileAccessToRecommended,
          },
          "Recommended policy",
        ),
        h(
          "button",
          {
            type: "button",
            class: "command-button",
            disabled: access.loading || access.saving,
            onclick: () => loadRepoFileAccess(state.selectedRepoId),
          },
          "Reload",
        ),
      ),
    );
  }

  function renderSettingsView() {
    const aiDirty = Boolean(state.ai.hasUnsavedChanges);
    return h(
      "div",
      { class: "settings-stack" },
      h(
        "div",
        { class: "settings-list" },
        h(
          "div",
          { class: "settings-row" },
          h("div", { class: "settings-key" }, "Server URL"),
          h("div", { class: "settings-value" }, window.location.origin),
        ),
        h(
          "div",
          { class: "settings-row" },
          h("div", { class: "settings-key" }, "API"),
          h("div", { class: "settings-value" }, "GitBoard Server API 0.4.2"),
        ),
      ),
      h(
        "form",
        {
          class: `form ai-settings-form ${aiDirty ? "dirty" : ""}`,
          "data-ai-settings-form": "true",
          autocomplete: "off",
          onsubmit: (event) => {
            event.preventDefault();
            saveAiConfig();
          },
        },
        h("h3", { class: "form-title" }, "Agent Pip"),
        h(
          "div",
          {
            class: `settings-unsaved ${aiDirty ? "" : "hidden"}`,
            "data-ai-unsaved-banner": "true",
          },
          "Unsaved AI settings. Save changes before testing or using Agent Pip.",
        ),
        h(
          "label",
          { class: "check-field" },
          h("input", {
            type: "checkbox",
            checked: state.ai.enabled,
            onchange: (event) => {
              state.ai.enabled = event.target.checked;
              markAiSettingsChanged({ render: true });
            },
          }),
          "Enable AI assistant",
        ),
        h(
          "div",
          { class: "field" },
          h("label", { for: "ai-provider" }, "Provider"),
          h(
            "div",
            {
              id: "ai-provider",
              class: "segmented-control",
              role: "group",
              "aria-label": "AI provider",
            },
            h(
              "button",
              {
                type: "button",
                class:
                  normalizedAiProvider(state.ai.provider) === "openai-compatible"
                    ? "active"
                    : "",
                onclick: () => {
                  state.ai.provider = "openai-compatible";
                  markAiSettingsChanged({ render: true });
                },
              },
              "OpenAI-compatible",
            ),
            h(
              "button",
              {
                type: "button",
                class:
                  normalizedAiProvider(state.ai.provider) === "anthropic-compatible"
                    ? "active"
                    : "",
                onclick: () => {
                  state.ai.provider = "anthropic-compatible";
                  markAiSettingsChanged({ render: true });
                },
              },
              "Anthropic-compatible",
            ),
          ),
        ),
        h(
          "div",
          { class: "field" },
          h("label", { for: "ai-base-url" }, "Endpoint URL"),
          h("input", {
            id: "ai-base-url",
            name: "barnaby-ai-base-url",
            autocomplete: "off",
            value: state.ai.baseUrl,
            placeholder: "https://api.example.com/v1",
            oninput: (event) => {
              state.ai.baseUrl = event.target.value;
              markAiSettingsChanged();
            },
          }),
        ),
        h(
          "div",
          { class: "field" },
          h("label", { for: "ai-model" }, "Model"),
          h("input", {
            id: "ai-model",
            name: "barnaby-ai-model",
            autocomplete: "off",
            value: state.ai.model,
            placeholder: "model-or-deployment-name",
            oninput: (event) => {
              state.ai.model = event.target.value;
              markAiSettingsChanged();
            },
          }),
        ),
        h(
          "div",
          { class: "field" },
          h("label", { for: "ai-api-key" }, "API key"),
          h("input", {
            id: "ai-api-key",
            name: "barnaby-ai-api-key",
            type: "password",
            autocomplete: "new-password",
            "data-lpignore": "true",
            "data-1p-ignore": "true",
            value: state.ai.apiKey,
            placeholder: state.ai.apiKeyConfigured ? "Saved key configured" : "Paste API key",
            oninput: (event) => {
              state.ai.apiKey = event.target.value;
              markAiSettingsChanged();
            },
          }),
          h(
            "p",
            { class: "hint" },
            state.ai.apiKeyConfigured
              ? "A key is saved. It will not be shown here."
              : "No key is saved.",
          ),
        ),
        h(
          "div",
          { class: "settings-grid" },
          h(
            "div",
            { class: "field" },
            h("label", { for: "ai-timeout" }, "Timeout seconds"),
            h("input", {
              id: "ai-timeout",
              type: "number",
              min: "1",
              max: "3600",
              value: String(state.ai.timeoutSeconds),
              oninput: (event) => {
                state.ai.timeoutSeconds = event.target.value;
                markAiSettingsChanged();
              },
            }),
          ),
          h(
            "div",
            { class: "field" },
            h("label", { for: "ai-retry-attempts" }, "Retry attempts"),
            h("input", {
              id: "ai-retry-attempts",
              type: "number",
              min: "0",
              max: "20",
              value: String(state.ai.retryAttempts),
              oninput: (event) => {
                state.ai.retryAttempts = event.target.value;
                markAiSettingsChanged();
              },
            }),
          ),
          h(
            "div",
            { class: "field" },
            h("label", { for: "ai-max-output" }, "Max output tokens"),
            h("input", {
              id: "ai-max-output",
              type: "number",
              min: "0",
              value: state.ai.maxOutputTokens,
              placeholder: "Default",
              oninput: (event) => {
                state.ai.maxOutputTokens = event.target.value;
                markAiSettingsChanged();
              },
            }),
          ),
        ),
        h(
          "label",
          { class: "check-field" },
          h("input", {
            type: "checkbox",
            checked: state.ai.allowWrites,
            onchange: (event) => {
              state.ai.allowWrites = event.target.checked;
              markAiSettingsChanged({ render: true });
            },
          }),
          "Allow proposed write actions",
        ),
        h(
          "div",
          { class: "field" },
          h("label", { for: "ai-system-prompt" }, "System prompt"),
          h("textarea", {
            id: "ai-system-prompt",
            rows: "10",
            value: state.ai.systemPrompt,
            oninput: (event) => {
              state.ai.systemPrompt = event.target.value;
              markAiSettingsChanged();
            },
          }),
        ),
        state.ai.secretStorageWarning
          ? h("p", { class: "hint warning" }, state.ai.secretStorageWarning)
          : null,
        h(
          "div",
          { class: "form-actions" },
          h(
            "button",
            {
              class: "command-button primary",
              "data-ai-save-button": "true",
              disabled: state.ai.saving,
            },
            state.ai.saving
              ? "Saving..."
              : aiDirty
                ? "Save AI settings *"
                : "Save AI settings",
          ),
          h(
            "button",
            {
              type: "button",
              class: "command-button",
              "data-ai-test-button": "true",
              title: aiDirty ? "Save AI settings before testing." : "",
              disabled: state.ai.testing || state.ai.saving || aiDirty,
              onclick: testAiConfig,
            },
            state.ai.testing ? "Testing..." : "Test",
          ),
          h(
            "button",
            {
              type: "button",
              class: "command-button danger",
              disabled: state.ai.saving || !state.ai.apiKeyConfigured,
              onclick: () => saveAiConfig({ clearApiKey: true }),
            },
            "Clear key",
          ),
        ),
      ),
      renderRepoFileAccessSettings(),
    );
  }

  function renderContent() {
    if (state.view === "add") return renderAddView();
    if (state.view === "create-task") return renderCreateTaskView();
    if (state.view === "settings") return renderSettingsView();
    if (state.view === "repo") return renderRepoView();
    if (state.view === "task") return renderTaskView();
    return renderStartView();
  }

  function renderDeleteRepoDialog() {
    const id = state.pendingDeleteRepoId;
    if (!id) return null;
    const path = state.repos[id] || "";
    return h(
      "div",
      { class: "modal-backdrop", role: "presentation" },
      h(
        "section",
        {
          class: "confirm-dialog",
          role: "dialog",
          "aria-modal": "true",
          "aria-labelledby": "delete-repo-title",
        },
        h("h2", { id: "delete-repo-title" }, "Delete repository?"),
        h("p", {}, `Remove ${repoLabel(id, path)} from Barnaby.`),
        h(
          "div",
          { class: "confirm-actions" },
          h(
            "button",
            {
              class: "command-button",
              onclick: cancelDeleteRepo,
              disabled: state.loading,
            },
            "Cancel",
          ),
          h(
            "button",
            {
              class: "command-button danger",
              onclick: confirmDeleteRepo,
              disabled: state.loading,
            },
            state.loading ? "Deleting..." : "Delete",
          ),
        ),
      ),
    );
  }

  function render() {
    app.className = `app-shell ${state.sidePanelCollapsed ? "side-collapsed" : ""} ${
      state.ai.pipOpen && pipAvailable() ? "pip-open" : ""
    }`;
    const deleteRepoDialog = renderDeleteRepoDialog();
    app.replaceChildren(
      renderSidePanel(),
      h(
        "main",
        { class: "main-panel" },
        renderContextLine(),
        h(
          "div",
          { class: "main-workspace" },
          h("section", { class: "content" }, renderContent()),
          renderAgentPip(),
        ),
      ),
      ...(deleteRepoDialog ? [deleteRepoDialog] : []),
    );
    if (state.ai.pipOpen) {
      window.requestAnimationFrame(() => {
        const messages = document.querySelector(".agent-messages");
        if (messages) messages.scrollTop = messages.scrollHeight;
      });
    }
  }

  window.addEventListener("focus", () => {
    if (state.view === "repo" && state.selectedRepoId && state.tasks.loaded) {
      refreshTasks(state.selectedRepoId, { silent: true });
    }
  });

  window.setInterval(() => {
    if (state.view === "repo" && state.selectedRepoId && state.tasks.loaded) {
      refreshTasks(state.selectedRepoId, { silent: true });
    }
  }, 60000);

  window.addEventListener("beforeunload", (event) => {
    if (!state.ai.hasUnsavedChanges) return;
    event.preventDefault();
    event.returnValue = "";
  });

  render();
  loadConfig();
  loadAiConfig();
})();
