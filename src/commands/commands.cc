#include "src/commands/commands.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <iostream>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>

#include "src/core/base64.h"
#include "src/core/error.h"
#include "src/core/filesystem.h"
#include "src/core/json.h"
#include "src/core/status_config.h"
#include "src/core/strings.h"
#include "src/core/subprocess.h"
#include "src/core/task.h"
#include "src/core/time.h"
#include "src/core/user.h"

namespace gitboard {
namespace fs = std::filesystem;
namespace {

std::string guid_task_id() {
  std::array<unsigned char, 16> bytes{};
  std::random_device random_device;
  std::uniform_int_distribution<int> octet(0, 255);
  for (unsigned char& byte : bytes) {
    byte = static_cast<unsigned char>(octet(random_device));
  }
  bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0F) | 0x40);
  bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3F) | 0x80);

  std::string raw;
  raw.reserve(bytes.size());
  for (unsigned char byte : bytes) raw.push_back(static_cast<char>(byte));

  std::string encoded = base64_encode(raw);
  while (!encoded.empty() && encoded.back() == '=') encoded.pop_back();
  for (char& c : encoded) {
    if (c == '/') c = '-';
  }
  return "TASK-" + encoded;
}

enum class git_change_type {
  k_added,
  k_modified,
  k_deleted,
};

struct git_status_entry {
  git_change_type type;
  std::string path;
};

struct query_filters {
  std::string text;
  std::vector<std::string> ids;
  std::vector<std::string> titles;
  std::vector<std::string> statuses;
  std::vector<std::string> assignees;
  std::vector<std::string> priorities;
  std::vector<std::string> tags;
  std::vector<std::string> branches;
  std::vector<std::string> prs;
  std::vector<std::string> ci_statuses;
  std::vector<std::string> created_by;
};

bool is_allowed_story_points(int value) {
  static const std::set<int> allowed = {1, 2, 3, 5, 8, 13, 21, 100};
  return allowed.count(value) != 0;
}

int parse_story_points_arg(const std::string& raw) {
  int value = 0;
  try {
    std::size_t used = 0;
    value = std::stoi(raw, &used);
    if (used != raw.size()) throw std::invalid_argument("trailing characters");
  } catch (const std::exception&) {
    throw error("points value must be a number");
  }
  // Keep CLI writes inside the same constrained planning scale as task loading.
  if (!is_allowed_story_points(value)) {
    throw error("points value must be one of 1, 2, 3, 5, 8, 13, 21, 100");
  }
  return value;
}

bool contains_case_insensitive(const std::string& haystack,
                               const std::string& needle) {
  if (needle.empty()) return true;
  const std::string lowered_needle = lower_ascii(needle);
  return std::search(
             haystack.begin(), haystack.end(), lowered_needle.begin(),
             lowered_needle.end(), [](char haystack_char, char needle_char) {
               return static_cast<char>(std::tolower(static_cast<unsigned char>(
                          haystack_char))) == needle_char;
             }) != haystack.end();
}

bool matches_any_exact(const std::string& value,
                       const std::vector<std::string>& filters) {
  if (filters.empty()) return true;
  for (const auto& filter : filters) {
    if (value == filter) return true;
  }
  return false;
}

bool matches_any_substring_case_insensitive(
    const std::string& value,
    const std::vector<std::string>& filters) {
  if (filters.empty()) return true;
  for (const auto& filter : filters) {
    if (contains_case_insensitive(value, filter)) return true;
  }
  return false;
}

bool contains_any_exact(const std::vector<std::string>& values,
                        const std::vector<std::string>& filters) {
  if (filters.empty()) return true;
  for (const auto& filter : filters) {
    for (const auto& value : values) {
      if (value == filter) return true;
    }
  }
  return false;
}

void ensure_task_database_initialized(const task_store& store) {
  std::error_code ec;
  const bool already_exists = fs::exists(store.tasks_dir, ec);
  if (ec) {
    throw error("task directory is inaccessible: " + ec.message());
  }
  fs::create_directories(store.tasks_dir);
  if (!already_exists) {
    const fs::path statuses_path = store.tasks_dir / "statuses.json";
    if (!fs::exists(statuses_path)) {
      std::string content = load_default_status_config_json();
      if (content.empty() || content.back() != '\n') content.push_back('\n');
      write_file_atomic(statuses_path, content);
    }
    const fs::path team_path = store.tasks_dir / "team.json";
    if (!fs::exists(team_path)) {
      write_file_atomic(
          team_path,
          "{\"team\":[{\"alias\":\"agent_pip\",\"email\":\"pip@barnaby\"}]}\n");
    }
  }
}

std::string task_index_json(const std::vector<std::pair<std::string, fs::path>>& tasks) {
  std::ostringstream out;
  out << "{\n";
  bool first = true;
  for (const auto& [id, path] : tasks) {
    if (!first) out << ",\n";
    first = false;
    out << "  \"" << json_escape(id) << "\": {\"file\": \""
        << json_escape(path.filename().string()) << "\"}";
  }
  out << "\n}\n";
  return out.str();
}

std::string cmd_list(context& ctx, const std::vector<std::string>& args) {
  if (!args.empty()) throw error("list takes no arguments");
  std::vector<std::pair<std::string, fs::path>> tasks;
  for (const auto& path : ctx.store.task_files()) {
    task current_task = task_from_content(read_file(path));
    tasks.push_back({current_task.id, path});
  }
  return task_index_json(tasks);
}

query_filters parse_query_filters(const std::vector<std::string>& args) {
  query_filters filters;
  for (std::size_t i = 0; i < args.size(); ++i) {
    auto require_value = [&](const std::string& option) -> std::string {
      if (++i >= args.size()) throw error("query " + option + " requires a value");
      return args[i];
    };
    if (args[i] == "--text") {
      filters.text = require_value(args[i]);
    } else if (args[i] == "--id") {
      filters.ids.push_back(require_value(args[i]));
    } else if (args[i] == "--title") {
      filters.titles.push_back(require_value(args[i]));
    } else if (args[i] == "--status") {
      filters.statuses.push_back(require_value(args[i]));
    } else if (args[i] == "--assignee") {
      filters.assignees.push_back(require_value(args[i]));
    } else if (args[i] == "--priority") {
      filters.priorities.push_back(require_value(args[i]));
    } else if (args[i] == "--tag") {
      filters.tags.push_back(require_value(args[i]));
    } else if (args[i] == "--branch") {
      filters.branches.push_back(require_value(args[i]));
    } else if (args[i] == "--pr") {
      filters.prs.push_back(require_value(args[i]));
    } else if (args[i] == "--ci-status") {
      filters.ci_statuses.push_back(require_value(args[i]));
    } else if (args[i] == "--created-by") {
      filters.created_by.push_back(require_value(args[i]));
    } else {
      throw error("unknown query option: " + args[i]);
    }
  }
  return filters;
}

bool task_matches_filters(const task& current_task,
                          const query_filters& filters) {
  return matches_any_exact(current_task.id, filters.ids) &&
         matches_any_substring_case_insensitive(current_task.title,
                                                filters.titles) &&
         matches_any_exact(current_task.status, filters.statuses) &&
         matches_any_exact(current_task.assignee, filters.assignees) &&
         matches_any_exact(current_task.priority, filters.priorities) &&
         contains_any_exact(current_task.tags, filters.tags) &&
         contains_any_exact(current_task.branches, filters.branches) &&
         contains_any_exact(current_task.prs, filters.prs) &&
         matches_any_exact(current_task.ci_status, filters.ci_statuses) &&
         matches_any_exact(current_task.created_by, filters.created_by);
}

std::string cmd_query(context& ctx, const std::vector<std::string>& args) {
  const query_filters filters = parse_query_filters(args);
  std::vector<std::pair<std::string, fs::path>> tasks;
  for (const auto& path : ctx.store.task_files()) {
    std::string content = read_file(path);
    if (!filters.text.empty() &&
        !contains_case_insensitive(content, filters.text)) {
      continue;
    }
    task current_task = task_from_content(content);
    if (!task_matches_filters(current_task, filters)) continue;
    tasks.push_back({current_task.id, path});
  }
  return task_index_json(tasks);
}

std::string cmd_team(context& ctx, const std::vector<std::string>& args) {
  if (!args.empty()) throw error("team takes no arguments");
  fs::path path = ctx.store.tasks_dir / "team.json";
  if (!fs::exists(path)) return "{\"team\":[]}\n";
  std::string content = read_file(path);
  if (content.empty() || content.back() != '\n') content.push_back('\n');
  return content;
}

std::string json_string_array(const std::vector<std::string>& values) {
  std::ostringstream out;
  out << "[";
  bool first = true;
  for (const auto& value : values) {
    if (!first) out << ", ";
    first = false;
    out << "\"" << json_escape(value) << "\"";
  }
  out << "]";
  return out.str();
}

std::string task_json(const task& current_task) {
  std::ostringstream out;
  out << "{\n"
      << "  \"id\": \"" << json_escape(current_task.id) << "\",\n"
      << "  \"title\": \"" << json_escape(current_task.title) << "\",\n"
      << "  \"assignee\": \"" << json_escape(current_task.assignee) << "\",\n"
      << "  \"priority\": \"" << json_escape(current_task.priority) << "\",\n"
      << "  \"story_points\": " << current_task.story_points << ",\n"
      << "  \"tags\": " << json_string_array(current_task.tags) << ",\n"
      << "  \"status\": \"" << json_escape(current_task.status) << "\",\n"
      << "  \"created_at\": \"" << json_escape(current_task.created_at) << "\",\n"
      << "  \"updated_at\": \"" << json_escape(current_task.updated_at) << "\",\n"
      << "  \"created_by\": \"" << json_escape(current_task.created_by) << "\",\n"
      << "  \"branches\": " << json_string_array(current_task.branches) << ",\n"
      << "  \"prs\": " << json_string_array(current_task.prs) << ",\n"
      << "  \"ci_status\": \"" << json_escape(current_task.ci_status) << "\",\n"
      << "  \"body\": \"" << json_escape(current_task.body) << "\"\n"
      << "}\n";
  return out.str();
}

std::string cmd_task(context& ctx, const std::vector<std::string>& args) {
  if (args.empty() || args.size() > 2) {
    throw error("task requires TASK_ID [--frontmatter|--json]");
  }
  fs::path path = ctx.store.find_task_path(args[0]);
  std::string mode = args.size() == 2 ? args[1] : "--frontmatter";
  if (mode == "--frontmatter") {
    return args[0] + "\n" + base64_encode(read_file(path)) + "\n";
  }
  if (mode == "--json") {
    return task_json(task_from_content(read_file(path)));
  }
  throw error("unknown task output format: " + mode);
}

std::string cmd_create(context& ctx, const std::vector<std::string>& args) {
  if (args.size() != 1) throw error("create requires BASE64_TITLE");
  ensure_task_database_initialized(ctx.store);
  const std::string title = base64_decode(args[0]);
  if (title.empty()) throw error("task title cannot be empty");
  std::string id;
  fs::path path;
  const std::string slug = slugify(title);
  for (int attempt = 0; attempt < 100; ++attempt) {
    id = guid_task_id();
    path = ctx.store.tasks_dir / (id + "_" + slug + ".md");
    bool id_exists = false;
    for (const auto& existing_path : ctx.store.task_files()) {
      if (starts_with(existing_path.filename().string(), id + "_")) {
        id_exists = true;
        break;
      }
    }
    if (!id_exists) break;
    if (attempt == 99) throw error("failed to generate unique task id");
  }
  task new_task;
  new_task.id = id;
  new_task.title = title;
  new_task.priority = "medium";
  new_task.story_points = 100;
  new_task.status = "todo";
  new_task.created_at = current_utc_iso();
  new_task.updated_at = new_task.created_at;
  new_task.ci_status = "unknown";
  new_task.body = default_body(title);
  ctx.store.save(path, new_task);
  return "Created " + new_task.id + " " + path.filename().string() + "\n";
}

std::string cmd_update(context& ctx, const std::string& field,
                       const std::vector<std::string>& args) {
  if (args.size() != 2) throw error(field + " requires TASK_ID and BASE64_VALUE");
  fs::path path = ctx.store.find_task_path(args[0]);
  task current_task = task_from_content(read_file(path));
  const std::string value = base64_decode(args[1]);
  if (field == "body") current_task.body = value;
  else if (field == "assignee") current_task.assignee = value;
  else if (field == "branches") current_task.branches = split_list(value);
  else if (field == "ci_status") current_task.ci_status = value;
  else if (field == "priority") current_task.priority = value;
  else if (field == "prs") current_task.prs = split_list(value);
  else if (field == "tags") current_task.tags = split_list(value);
  else if (field == "title") current_task.title = value;
  else throw error("unknown update command: " + field);
  ctx.store.save(path, current_task);
  return "";
}

std::string cmd_points(context& ctx, const std::vector<std::string>& args) {
  if (args.size() != 2) throw error("points requires TASK_ID and NEW_VALUE");
  fs::path path = ctx.store.find_task_path(args[0]);
  task current_task = task_from_content(read_file(path));
  current_task.story_points = parse_story_points_arg(args[1]);
  ctx.store.save(path, current_task);
  return "";
}

std::string cmd_move(context& ctx, const std::vector<std::string>& args) {
  if (args.size() != 2) throw error("move requires TASK_ID and BASE64_NEW_STATUS");
  fs::path path = ctx.store.find_task_path(args[0]);
  task current_task = task_from_content(read_file(path));
  const std::string new_status = base64_decode(args[1]);
  status_config statuses = load_status_config(ctx.store.tasks_dir);
  if (!statuses.known(current_task.status)) {
    std::cerr << "Warning: current status is not defined: " << current_task.status
              << "\n";
  }
  if (!statuses.known(new_status)) {
    throw error("destination status is not defined: " + new_status);
  }
  if (!statuses.can_transition(current_task.status, new_status)) {
    throw error("invalid transition from " + current_task.status + " to " +
                new_status);
  }
  current_task.status = new_status;
  ctx.store.save(path, current_task);
  return "";
}

std::string comment_author(const context& ctx) {
  process_output output =
      run_capture({"git", "-C", ctx.store.project_root.string(), "config",
                   "user.name"});
  std::string name = output.stdout_text;
  name = trim(name);
  if (output.exit_code == 0 && !name.empty()) return name;
  return current_user();
}

std::string cmd_comment(context& ctx, const std::vector<std::string>& args) {
  if (args.size() != 2) throw error("comment requires TASK_ID and BASE64_MESSAGE");
  fs::path path = ctx.store.find_task_path(args[0]);
  task current_task = task_from_content(read_file(path));
  const std::string message = base64_decode(args[1]);
  const std::string comment =
      "- [" + current_utc_iso() + "] " + comment_author(ctx) + ": " + message;
  std::vector<std::string> lines = split(current_task.body, '\n');
  bool inserted = false;
  for (auto it = lines.begin(); it != lines.end(); ++it) {
    if (trim(*it) == "## Comments") {
      lines.insert(std::next(it), comment);
      inserted = true;
      break;
    }
  }
  std::ostringstream body;
  if (!inserted) {
    body << current_task.body;
    if (!current_task.body.empty() && current_task.body.back() != '\n') {
      body << "\n";
    }
    body << "\n## Comments\n" << comment << "\n";
  } else {
    for (const auto& line : lines) body << line << "\n";
  }
  current_task.body = body.str();
  ctx.store.save(path, current_task);
  return "";
}

fs::path git_root(const context& ctx) {
  process_output output =
      run_capture({"git", "-C", ctx.store.project_root.string(), "rev-parse",
                   "--show-toplevel"});
  if (output.exit_code != 0) throw error("not inside a Git work tree");
  return fs::weakly_canonical(fs::path(trim(output.stdout_text)));
}

bool is_under_directory(const fs::path& path, const fs::path& directory) {
  fs::path rel = path.lexically_relative(directory);
  if (rel.empty()) return false;
  auto first = rel.begin();
  return first != rel.end() && *first != "..";
}

std::string porcelain_path(std::string path) {
  auto arrow = path.find(" -> ");
  if (arrow != std::string::npos) path = path.substr(arrow + 4);
  if (path.size() >= 2 && path.front() == '"' && path.back() == '"') {
    path = path.substr(1, path.size() - 2);
  }
  return path;
}

std::optional<git_status_entry> parse_git_status_entry(const std::string& line) {
  if (line.size() < 4) return std::nullopt;
  const char index_status = line[0];
  const char worktree_status = line[1];
  git_change_type type = git_change_type::k_modified;
  if (index_status == '?' && worktree_status == '?') {
    type = git_change_type::k_added;
  } else if (index_status == 'D' || worktree_status == 'D') {
    type = git_change_type::k_deleted;
  } else if (index_status == 'A' || worktree_status == 'A') {
    type = git_change_type::k_added;
  }
  return git_status_entry{type, porcelain_path(line.substr(3))};
}

std::optional<git_status_entry> parse_git_name_status_entry(
    const std::string& line) {
  std::vector<std::string> parts = split(line, '\t');
  if (parts.size() < 2 || parts[0].empty()) return std::nullopt;
  const char status = parts[0][0];
  git_change_type type = git_change_type::k_modified;
  if (status == 'A') type = git_change_type::k_added;
  else if (status == 'D') type = git_change_type::k_deleted;
  const std::string path = status == 'R' && parts.size() >= 3 ? parts[2] : parts[1];
  return git_status_entry{type, porcelain_path(path)};
}

bool is_task_database_path(const fs::path& path, const fs::path& tasks_dir) {
  if (path.extension() != ".md" && path.extension() != ".json") return false;
  return is_under_directory(path, tasks_dir);
}

std::string task_database_id(git_change_type type, const fs::path& abs) {
  if (abs.extension() == ".json") return abs.filename().string();
  if (abs.extension() != ".md") return "";
  if (type == git_change_type::k_deleted || !fs::exists(abs)) {
    return extract_filename_id(abs);
  }
  return task_from_content(read_file(abs)).id;
}

std::string cmd_publish(context& ctx, const std::vector<std::string>& args) {
  if (!args.empty()) throw error("publish takes no arguments");
  ensure_task_database_initialized(ctx.store);
  fs::path root = git_root(ctx);
  process_output git_status =
      run_capture({"git", "-C", root.string(), "status", "--porcelain",
                   "-uall"});
  if (git_status.exit_code != 0) {
    throw error(git_status.stderr_text.empty() ? git_status.stdout_text
                                               : git_status.stderr_text);
  }
  std::string status = git_status.stdout_text;
  std::vector<std::string> changed;
  std::set<std::string> ids;
  fs::path tasks_abs = fs::weakly_canonical(ctx.store.tasks_dir);
  for (const auto& line : split(status, '\n')) {
    auto entry = parse_git_status_entry(line);
    if (!entry) continue;
    std::string path = entry->path;
    fs::path abs = fs::weakly_canonical(root / path);
    if (is_task_database_path(abs, tasks_abs)) {
      changed.push_back(path);
      if (abs.extension() == ".md") {
        std::string id = extract_filename_id(abs);
        if (!id.empty()) ids.insert(id);
      }
    }
  }
  if (changed.empty()) return "No unpublished task changes\n";
  std::vector<std::string> add_args = {"git", "-C", root.string(), "add"};
  for (const auto& path : changed) add_args.push_back(path);
  run_checked(add_args);
  std::ostringstream msg;
  msg << "Publish tasks: ";
  bool first = true;
  for (const auto& id : ids) {
    if (!first) msg << ", ";
    first = false;
    msg << id;
  }
  run_checked({"git", "-C", root.string(), "commit", "-m", msg.str()});
  std::ostringstream out;
  out << "Published " << changed.size() << " task database files\n"
      << msg.str() << "\n";
  return out.str();
}

void append_dbstatus_id(std::vector<std::string>& ids,
                        git_change_type type,
                        const fs::path& abs) {
  std::string id = task_database_id(type, abs);
  if (!id.empty()) ids.push_back(id);
}

std::string cmd_dbstatus(context& ctx, const std::vector<std::string>& args) {
  if (!args.empty()) throw error("dbstatus takes no arguments");
  ensure_task_database_initialized(ctx.store);
  fs::path root = git_root(ctx);
  process_output git_status =
      run_capture({"git", "-C", root.string(), "status", "--porcelain",
                   "-uall"});
  if (git_status.exit_code != 0) {
    throw error(git_status.stderr_text.empty() ? git_status.stdout_text
                                               : git_status.stderr_text);
  }
  std::string status = git_status.stdout_text;

  std::vector<std::string> added;
  std::vector<std::string> modified;
  std::vector<std::string> deleted;
  fs::path tasks_abs = fs::weakly_canonical(ctx.store.tasks_dir);
  for (const auto& line : split(status, '\n')) {
    auto entry = parse_git_status_entry(line);
    if (!entry) continue;
    fs::path abs = fs::weakly_canonical(root / entry->path);
    if (!is_task_database_path(abs, tasks_abs)) continue;

    if (entry->type == git_change_type::k_added) {
      append_dbstatus_id(added, entry->type, abs);
    } else if (entry->type == git_change_type::k_deleted) {
      append_dbstatus_id(deleted, entry->type, abs);
    } else {
      append_dbstatus_id(modified, entry->type, abs);
    }
  }

  std::ostringstream out;
  out << "{\n"
      << "  \"added\": " << json_string_array(added) << ",\n"
      << "  \"modified\": " << json_string_array(modified) << ",\n"
      << "  \"deleted\": " << json_string_array(deleted) << "\n"
      << "}\n";
  return out.str();
}

std::string cmd_remotestatus(context& ctx, const std::vector<std::string>& args) {
  if (!args.empty()) throw error("remotestatus takes no arguments");
  ensure_task_database_initialized(ctx.store);
  fs::path root = git_root(ctx);
  process_output upstream_output =
      run_capture({"git", "-C", root.string(), "rev-parse", "--abbrev-ref",
                   "--symbolic-full-name", "@{u}"});
  if (upstream_output.exit_code != 0) {
    return "{\n  \"available\": false,\n  \"upstream\": \"\",\n"
           "  \"added\": [],\n  \"modified\": [],\n  \"deleted\": []\n}\n";
  }
  const std::string upstream = trim(upstream_output.stdout_text);
  process_output fetch_output =
      run_capture({"git", "-C", root.string(), "fetch", "--quiet"});
  if (fetch_output.exit_code != 0) {
    return "{\n  \"available\": false,\n  \"upstream\": \"" +
           json_escape(upstream) +
           "\",\n  \"added\": [],\n  \"modified\": [],\n  \"deleted\": []\n}\n";
  }

  fs::path tasks_abs = fs::weakly_canonical(ctx.store.tasks_dir);
  fs::path tasks_rel = fs::relative(tasks_abs, root);
  process_output diff_output =
      run_capture({"git", "-C", root.string(), "diff", "--name-status",
                   "--find-renames", "HEAD.." + upstream, "--",
                   tasks_rel.generic_string()});
  if (diff_output.exit_code != 0) {
    return "{\n  \"available\": false,\n  \"upstream\": \"" +
           json_escape(upstream) +
           "\",\n  \"added\": [],\n  \"modified\": [],\n  \"deleted\": []\n}\n";
  }

  std::vector<std::string> added;
  std::vector<std::string> modified;
  std::vector<std::string> deleted;
  for (const auto& line : split(diff_output.stdout_text, '\n')) {
    auto entry = parse_git_name_status_entry(line);
    if (!entry) continue;
    fs::path abs = fs::weakly_canonical(root / entry->path);
    if (!is_task_database_path(abs, tasks_abs)) continue;
    if (entry->type == git_change_type::k_added) {
      if (abs.extension() == ".md") added.push_back(abs.filename().string());
    } else if (entry->type == git_change_type::k_deleted) {
      std::string id = task_database_id(entry->type, abs);
      if (id.empty()) continue;
      deleted.push_back(id);
    } else {
      std::string id = task_database_id(entry->type, abs);
      if (id.empty()) continue;
      modified.push_back(id);
    }
  }

  std::ostringstream out;
  out << "{\n"
      << "  \"available\": true,\n"
      << "  \"upstream\": \"" << json_escape(upstream) << "\",\n"
      << "  \"added\": " << json_string_array(added) << ",\n"
      << "  \"modified\": " << json_string_array(modified) << ",\n"
      << "  \"deleted\": " << json_string_array(deleted) << "\n"
      << "}\n";
  return out.str();
}

std::string cmd_sync(context& ctx, const std::vector<std::string>& args) {
  if (!args.empty()) throw error("sync takes no arguments");
  fs::path root = git_root(ctx);
  process_output git_status =
      run_capture({"git", "-C", root.string(), "status", "--porcelain",
                   "-uall"});
  if (git_status.exit_code != 0) {
    throw error(git_status.stderr_text.empty() ? git_status.stdout_text
                                               : git_status.stderr_text);
  }
  std::string status = git_status.stdout_text;
  fs::path tasks_abs = fs::weakly_canonical(ctx.store.tasks_dir);
  for (const auto& line : split(status, '\n')) {
    auto entry = parse_git_status_entry(line);
    if (!entry) continue;
    fs::path abs = fs::weakly_canonical(root / entry->path);
    if (is_task_database_path(abs, tasks_abs)) {
      throw error("refusing to sync with uncommitted task database changes");
    }
  }
  run_checked({"git", "-C", root.string(), "pull"});
  process_output conflict_output =
      run_capture({"git", "-C", root.string(), "diff", "--name-only",
                   "--diff-filter=U"});
  if (conflict_output.exit_code != 0) {
    throw error(conflict_output.stderr_text.empty() ? conflict_output.stdout_text
                                                    : conflict_output.stderr_text);
  }
  std::string conflicts = conflict_output.stdout_text;
  if (!trim(conflicts).empty()) {
    throw error("unresolved merge conflicts:\n" + conflicts);
  }
  run_checked({"git", "-C", root.string(), "push"});
  return "Sync complete\n";
}

std::string cmd_statuses(context& ctx, const std::vector<std::string>& args) {
  if (!args.empty()) throw error("statuses takes no arguments");
  std::string content = load_status_config_json(ctx.store.tasks_dir);
  if (content.empty() || content.back() != '\n') content.push_back('\n');
  return content;
}

std::string cmd_batch(context& ctx, const std::vector<std::string>& args) {
  if (args.size() != 1) throw error("batch requires BATCH_PATH");
  json_value root = json_parser(read_file(args[0])).parse();
  auto batch_it = root.object.find("batch");
  if (root.type != json_value::k_object || batch_it == root.object.end() ||
      batch_it->second.type != json_value::k_array) {
    throw error("batch file must contain a batch array");
  }
  std::ostringstream out;
  out << "{\n  \"batch\": [\n";
  bool first = true;
  bool failed = false;
  for (const json_value& item : batch_it->second.array) {
    if (failed) break;
    if (item.type != json_value::k_object || !item.object.count("cmd") ||
        !item.object.count("args") ||
        item.object.at("cmd").type != json_value::k_string ||
        item.object.at("args").type != json_value::k_array) {
      throw error("invalid batch command entry");
    }
    std::string cmd = item.object.at("cmd").string;
    std::vector<std::string> cmd_args;
    for (const json_value& arg : item.object.at("args").array) {
      if (arg.type != json_value::k_string) throw error("batch args must be strings");
      cmd_args.push_back(arg.string);
    }
    bool ok = true;
    std::string result;
    std::string error_message;
    try {
      result = execute(ctx, cmd, cmd_args);
    } catch (const std::exception& ex) {
      ok = false;
      failed = true;
      error_message = ex.what();
    }
    if (!first) out << ",\n";
    first = false;
    out << "    {\"cmd\": \"" << json_escape(cmd) << "\", \"ok\": "
        << (ok ? "true" : "false") << ", \"result\": \""
        << (result.empty() ? "" : json_escape(base64_encode(result)))
        << "\", \"error\": \""
        << (error_message.empty() ? "" : json_escape(base64_encode(error_message)))
        << "\"}";
  }
  out << "\n  ]\n}\n";
  if (failed) throw error(out.str());
  return out.str();
}

}  // namespace

std::string execute(context& ctx, const std::string& cmd,
                    const std::vector<std::string>& args) {
  if (cmd == "team") return cmd_team(ctx, args);
  if (cmd == "list") return cmd_list(ctx, args);
  if (cmd == "query") return cmd_query(ctx, args);
  if (cmd == "task") return cmd_task(ctx, args);
  if (cmd == "create") return cmd_create(ctx, args);
  if (cmd == "body" || cmd == "assignee" || cmd == "branches" ||
      cmd == "ci_status" || cmd == "priority" || cmd == "prs" ||
      cmd == "tags" || cmd == "title") {
    return cmd_update(ctx, cmd, args);
  }
  if (cmd == "points") return cmd_points(ctx, args);
  if (cmd == "move") return cmd_move(ctx, args);
  if (cmd == "comment") return cmd_comment(ctx, args);
  if (cmd == "publish") return cmd_publish(ctx, args);
  if (cmd == "dbstatus") return cmd_dbstatus(ctx, args);
  if (cmd == "remotestatus") return cmd_remotestatus(ctx, args);
  if (cmd == "sync") return cmd_sync(ctx, args);
  if (cmd == "statuses") return cmd_statuses(ctx, args);
  if (cmd == "batch") return cmd_batch(ctx, args);
  throw error("unknown command: " + cmd);
}

}  // namespace gitboard
