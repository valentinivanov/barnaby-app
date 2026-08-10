#include "src/server/engine.h"

#ifdef _WIN32
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "src/core/base64.h"
#include "src/core/error.h"
#include "src/core/filesystem.h"
#include "src/core/json.h"
#include "src/core/strings.h"
#include "src/core/user.h"
#include "src/server/ai_agent.h"
#include "src/server/config.h"
#include "src/server/http.h"
#include "src/server/process.h"

namespace fs = std::filesystem;

namespace gitboard::server {

constexpr std::uintmax_t kMaxAgentFileReadBytes = 512 * 1024;
constexpr int kMaxAgentFileRangeLines = 200;
constexpr std::size_t kMaxAgentFileRangeLineChars = 4000;
constexpr std::size_t kMaxAgentSearchResults = 100;

std::string file_revision(const fs::path& path) {
  std::error_code ec;
  if (!fs::exists(path, ec) || ec) return "";
  std::uintmax_t size = fs::file_size(path, ec);
  if (ec) size = 0;
  auto write_time = fs::last_write_time(path, ec);
  if (ec) return std::to_string(size) + ":0";
  return std::to_string(size) + ":" +
         std::to_string(
             static_cast<long long>(write_time.time_since_epoch().count()));
}

std::string normalize_agent_file_path(std::string path) {
  path = gitboard::trim(path);
  while (!path.empty() && path.front() == '/') path.erase(path.begin());
  fs::path normalized = fs::path(path).lexically_normal();
  std::string out = normalized.generic_string();
  if (out == ".") out.clear();
  if (out.empty() || out.find('\0') != std::string::npos) {
    throw error("invalid project file path");
  }
  for (const auto& part : fs::path(out)) {
    std::string token = part.string();
    if (token.empty() || token == "." || token == "..") {
      throw error("invalid project file path");
    }
    if (token == ".git") {
      throw error("project file path cannot access .git");
    }
  }
  return out;
}

std::string access_for_agent_path(const repo_ai_config& repo_ai,
                                  const std::string& relative_path) {
  std::string path = relative_path;
  if (path.empty()) return "forbidden";
  std::size_t slash = path.find('/');
  std::string top = slash == std::string::npos ? path + "/" : path.substr(0, slash + 1);
  for (const auto& rule : repo_ai.file_access) {
    if (rule.path == top) return rule.access;
  }
  return "forbidden";
}

void require_agent_file_access(const repo_ai_config& repo_ai,
                               const std::string& relative_path,
                               bool write) {
  std::string access = access_for_agent_path(repo_ai, relative_path);
  if (write) {
    if (access != "full_access") throw error("project file path is not writable");
  } else if (access != "read_only" && access != "full_access") {
    throw error("project file path is not readable");
  }
}

fs::path resolve_agent_file_path(const fs::path& repo_path,
                                 const std::string& relative_path,
                                 bool allow_missing_leaf) {
  fs::path current = fs::absolute(repo_path).lexically_normal();
  fs::path rel(relative_path);
  for (auto it = rel.begin(); it != rel.end(); ++it) {
    std::string part = it->string();
    if (part.empty()) continue;
    if (part == "." || part == "..") {
      throw error("invalid project file path");
    }
    current /= part;
    std::error_code ec;
    bool is_leaf = std::next(it) == rel.end();
    if (allow_missing_leaf && is_leaf && !fs::exists(current, ec)) {
      continue;
    }
    fs::file_status status = fs::symlink_status(current, ec);
    if (ec) throw error("project file path does not exist");
    if (fs::is_symlink(status)) {
      throw error("project file path cannot include symlinks");
    }
  }
  return current.lexically_normal();
}

json_value require_object_body(const http_request& request) {
  json_value value = json_parser(request.body).parse();
  if (value.type != json_value::k_object) throw error("request body must be an object");
  return value;
}
class temp_file {
 public:
  explicit temp_file(const fs::path& directory) {
    fs::create_directories(directory);
#ifdef _WIN32
    for (int i = 0; i < 100; ++i) {
      fs::path candidate =
          directory / (L"gitboard-server-batch-" + std::to_wstring(_getpid()) +
                       L"." + std::to_wstring(GetTickCount64()) + L"." +
                       std::to_wstring(i) + L".json");
      if (!fs::exists(candidate)) {
        path_ = candidate;
        return;
      }
    }
    throw error("failed to create batch temp file");
#else
    std::string pattern = (directory / "gitboard-server-batch-XXXXXX.json").string();
    std::vector<char> buffer(pattern.begin(), pattern.end());
    buffer.push_back('\0');
    int fd = mkstemps(buffer.data(), 5);
    if (fd < 0) {
      throw error(std::string("failed to create batch temp file: ") +
                  std::strerror(errno));
    }
    close(fd);
    path_ = buffer.data();
#endif
  }

  ~temp_file() {
    if (!path_.empty()) {
      std::error_code ignored;
      fs::remove(path_, ignored);
    }
  }

  const fs::path& path() const { return path_; }

  temp_file(const temp_file&) = delete;
  temp_file& operator=(const temp_file&) = delete;

 private:
  fs::path path_;
};
std::string batch_command_json(const std::string& cmd,
                               const std::vector<std::string>& args) {
  std::string out = "{\"cmd\":" + json_quote(cmd) + ",\"args\":[";
  bool first = true;
  for (const auto& arg : args) {
    if (!first) out += ",";
    first = false;
    out += json_quote(arg);
  }
  out += "]}";
  return out;
}

process_result run_gitboard_batch_raw(const fs::path& gitboard_path,
                                      const fs::path& repo_path,
                                      const std::string& batch_json) {
  temp_file temp(config_dir() / "batch");
  write_file_atomic(temp.path(), "{\"batch\":" + batch_json + "}\n");
  return run_process(gitboard_path,
                     {"--project-root", repo_path.string(), "batch",
                      temp.path().string()});
}

std::vector<std::string> run_readonly_batch_outputs(
    const fs::path& gitboard_path,
    const fs::path& repo_path,
    const std::vector<std::string>& command_jsons) {
  std::string batch = "[";
  bool first_command = true;
  for (const auto& command : command_jsons) {
    if (!first_command) batch += ",";
    first_command = false;
    batch += command;
  }
  batch += "]";

  process_result result = run_gitboard_batch_raw(gitboard_path, repo_path, batch);
  json_value root = json_parser(result.output).parse();
  if (root.type != json_value::k_object) throw error("batch response root is not an object");
  auto batch_it = root.object.find("batch");
  if (batch_it == root.object.end() || batch_it->second.type != json_value::k_array) {
    throw error("batch response did not include a batch array");
  }
  std::vector<std::string> outputs;
  for (const json_value& item : batch_it->second.array) {
    if (item.type != json_value::k_object) throw error("invalid batch result entry");
    auto ok_it = item.object.find("ok");
    bool ok = ok_it != item.object.end() && ok_it->second.type == json_value::k_bool &&
              ok_it->second.boolean;
    if (!ok) {
      std::string encoded = json_string_member(item, "error");
      throw error(encoded.empty() ? "read-only tool command failed"
                                  : gitboard::base64_decode(encoded));
    }
    outputs.push_back(gitboard::base64_decode(json_string_member(item, "result")));
  }
  return outputs;
}

std::string require_task_id_arg(const json_value& args) {
  std::string task_id = gitboard::trim(json_string_member(args, "taskId"));
  if (task_id.empty()) throw error("taskId is required");
  if (!gitboard::starts_with(task_id, "TASK-") || task_id.size() <= 5) {
    throw error("invalid task id");
  }
  for (char c : std::string_view(task_id).substr(5)) {
    const auto uc = static_cast<unsigned char>(c);
    if (!std::isalnum(uc) && c != '+' && c != '-') {
      throw error("invalid task id");
    }
  }
  return task_id;
}

std::string json_string_array_to_csv(const json_value& value,
                                     const std::string& field) {
  if (value.type != json_value::k_array) {
    throw error(field + " must be an array");
  }
  std::ostringstream out;
  bool first = true;
  for (const json_value& item : value.array) {
    if (item.type != json_value::k_string) {
      throw error(field + " entries must be strings");
    }
    std::string token = gitboard::trim(item.string);
    if (token.empty()) continue;
    if (!first) out << ",";
    first = false;
    out << token;
  }
  return out.str();
}

std::string encoded_update_command(const std::string& field,
                                   const std::string& task_id,
                                   const std::string& value) {
  return batch_command_json(field, {task_id, gitboard::base64_encode(value)});
}

std::string points_update_command(const std::string& task_id,
                                  const std::string& value) {
  return batch_command_json("points", {task_id, value});
}

void require_known_assignee(const fs::path& repo_path,
                            const std::string& assignee);
void require_known_task_field(const std::string& field) {
  static const std::set<std::string> fields = {
      "title", "body", "assignee", "priority", "story_points", "status",
      "ci_status", "tags", "branches", "prs"};
  if (!fields.count(field)) throw error("unknown task field: " + field);
}

std::string task_points_value(const json_value& value) {
  std::string raw;
  if (value.type == json_value::k_number) raw = value.number;
  else if (value.type == json_value::k_string) raw = value.string;
  else throw error("story_points must be a number");
  int points = 0;
  try {
    std::size_t used = 0;
    points = std::stoi(raw, &used);
    if (used != raw.size()) throw std::invalid_argument("trailing characters");
  } catch (const std::exception&) {
    throw error("story_points must be a number");
  }
  if (points == 1 || points == 2 || points == 3 || points == 5 ||
      points == 8 || points == 13 || points == 21 || points == 100) {
    return std::to_string(points);
  }
  throw error("story_points must be one of 1, 2, 3, 5, 8, 13, 21, 100");
}

bool task_exists_in_repo(const fs::path& repo_path, const std::string& task_id) {
  fs::path tasks_dir = repo_path / "tasks";
  std::error_code ec;
  if (!fs::is_directory(tasks_dir, ec) || ec) return false;
  const std::string id_line = "id: " + task_id;
  for (const auto& entry : fs::directory_iterator(tasks_dir, ec)) {
    if (ec) return false;
    if (!entry.is_regular_file(ec) || ec) {
      ec.clear();
      continue;
    }
    if (entry.path().extension() != ".md") continue;
    std::string filename = entry.path().filename().string();
    if (filename == task_id + ".md" || gitboard::starts_with(filename, task_id + "_")) {
      return true;
    }
    try {
      std::string content = read_file(entry.path());
      if (content.find(id_line) != std::string::npos) return true;
    } catch (const std::exception&) {
      continue;
    }
  }
  return false;
}

void require_existing_task(const fs::path& repo_path, const std::string& task_id) {
  if (!task_exists_in_repo(repo_path, task_id)) {
    throw error("task id not found: " + task_id);
  }
}


static std::string truncate_text(std::string value, std::size_t max_size) {
  if (value.size() > max_size) value.resize(max_size);
  return value;
}

std::string action_title(const std::string& type, const std::string& task_id) {
  if (type == "update_task") return "Update " + task_id;
  if (type == "move_task") return "Move " + task_id;
  if (type == "comment_task") return "Comment on " + task_id;
  if (type == "add_team_member") return "Add team member";
  return "Apply action";
}

std::string proposed_move_action_json(const std::string& task_id,
                                      const std::string& status,
                                      std::size_t index) {
  return "{\"id\":" + json_quote("action_" + std::to_string(index + 1)) +
         ",\"type\":\"move_task\",\"taskId\":" + json_quote(task_id) +
         ",\"title\":" + json_quote(action_title("move_task", task_id)) +
         ",\"summary\":" + json_quote("Move to " + status) +
         ",\"selected\":true,\"status\":" + json_quote(status) + "}";
}

std::string proposed_action_from_tool_call(const agent_tool_call& call,
                                           const json_value& args,
                                           const fs::path& repo_path,
                                           std::size_t index) {
  std::string id = "action_" + std::to_string(index + 1);
  if (call.name == "update_task") {
    std::string task_id = require_task_id_arg(args);
    require_existing_task(repo_path, task_id);
    auto fields_it = args.object.find("fields");
    if (fields_it == args.object.end() ||
        fields_it->second.type != json_value::k_object) {
      throw error("fields object is required");
    }
    if (fields_it->second.object.empty()) throw error("no task fields to update");
    std::ostringstream summary;
    bool first = true;
    for (const auto& [field, value] : fields_it->second.object) {
      require_known_task_field(field);
      if (field == "story_points") {
        task_points_value(value);
      } else if (field == "title" || field == "body" || field == "assignee" ||
          field == "priority" || field == "status" || field == "ci_status") {
        if (value.type != json_value::k_string) throw error(field + " must be a string");
        if (field == "assignee") {
          require_known_assignee(repo_path, gitboard::trim(value.string));
        }
      } else if (value.type != json_value::k_array) {
        throw error(field + " must be an array");
      }
      if (!first) summary << ", ";
      first = false;
      summary << field;
    }
    return "{\"id\":" + json_quote(id) +
           ",\"type\":\"update_task\",\"taskId\":" + json_quote(task_id) +
           ",\"title\":" + json_quote(action_title(call.name, task_id)) +
           ",\"summary\":" + json_quote("Update " + summary.str()) +
           ",\"selected\":true,\"fields\":" + stringify_json(fields_it->second) + "}";
  }
  if (call.name == "move_task") {
    std::string task_id = require_task_id_arg(args);
    require_existing_task(repo_path, task_id);
    std::string status = gitboard::trim(json_string_member(args, "status"));
    if (status.empty()) throw error("status is required");
    return proposed_move_action_json(task_id, status, index);
  }
  if (call.name == "comment_task") {
    std::string task_id = require_task_id_arg(args);
    require_existing_task(repo_path, task_id);
    std::string message = gitboard::trim(json_string_member(args, "message"));
    if (message.empty()) throw error("message is required");
    return "{\"id\":" + json_quote(id) +
           ",\"type\":\"comment_task\",\"taskId\":" + json_quote(task_id) +
           ",\"title\":" + json_quote(action_title(call.name, task_id)) +
           ",\"summary\":" + json_quote(truncate_text(message, 180)) +
           ",\"selected\":true,\"message\":" + json_quote(message) + "}";
  }
  if (call.name == "add_team_member") {
    std::string alias = gitboard::trim(json_string_member(args, "alias"));
    std::string email = gitboard::trim(json_string_member(args, "email"));
    if (alias.empty()) throw error("alias is required");
    return "{\"id\":" + json_quote(id) +
           ",\"type\":\"add_team_member\",\"title\":" +
           json_quote(action_title(call.name, "")) +
           ",\"summary\":" + json_quote("Add " + alias) +
           ",\"selected\":true,\"alias\":" + json_quote(alias) +
           ",\"email\":" + json_quote(email) + "}";
  }
  throw error("unknown mutating tool: " + call.name);
}

std::vector<std::string> run_agent_task_batch(
    const fs::path& gitboard_path,
    const fs::path& repo_path,
    std::vector<std::string> commands) {
  commands.push_back(batch_command_json("dbstatus", {}));
  return run_readonly_batch_outputs(gitboard_path, repo_path, commands);
}

fs::path team_json_path(const fs::path& repo_path) {
  return repo_path / "tasks" / "team.json";
}

json_value load_team_json(const fs::path& repo_path) {
  fs::path path = team_json_path(repo_path);
  if (!fs::exists(path)) {
    return json_parser("{\"team\":[]}").parse();
  }
  json_value team = json_parser(read_file(path)).parse();
  if (team.type != json_value::k_object) throw error("team.json root must be an object");
  auto team_it = team.object.find("team");
  if (team_it == team.object.end()) {
    team.object["team"] = json_parser("[]").parse();
  } else if (team_it->second.type != json_value::k_array) {
    throw error("team.json team must be an array");
  }
  return team;
}

std::set<std::string> team_aliases(const json_value& team) {
  std::set<std::string> aliases;
  auto team_it = team.object.find("team");
  if (team_it == team.object.end() || team_it->second.type != json_value::k_array) {
    return aliases;
  }
  for (const json_value& member : team_it->second.array) {
    std::string alias = gitboard::trim(json_string_member(member, "alias"));
    if (!alias.empty()) aliases.insert(alias);
  }
  return aliases;
}

void require_known_assignee(const fs::path& repo_path,
                            const std::string& assignee) {
  if (assignee.empty()) return;
  std::set<std::string> aliases = team_aliases(load_team_json(repo_path));
  if (!aliases.count(assignee)) {
    throw error("assignee is not in team list: " + assignee);
  }
}

std::string team_json_response(const fs::path& repo_path) {
  return "{\"ok\":true,\"team\":" + stringify_json(load_team_json(repo_path)) + "}";
}

std::string add_team_member(const fs::path& repo_path,
                            const std::string& alias,
                            const std::string& email) {
  std::string normalized_alias = gitboard::trim(alias);
  if (normalized_alias.empty()) throw error("alias is required");
  if (normalized_alias.size() > 80) throw error("alias is too long");
  if (normalized_alias.find(',') != std::string::npos ||
      normalized_alias.find('\n') != std::string::npos ||
      normalized_alias.find('\r') != std::string::npos) {
    throw error("alias contains unsupported characters");
  }

  json_value team = load_team_json(repo_path);
  auto& members = team.object["team"].array;
  for (const json_value& member : members) {
    if (gitboard::trim(json_string_member(member, "alias")) == normalized_alias) {
      return "{\"ok\":true,\"added\":false,\"team\":" + stringify_json(team) + "}";
    }
  }

  json_value member;
  member.type = json_value::k_object;
  json_value alias_value;
  alias_value.type = json_value::k_string;
  alias_value.string = normalized_alias;
  member.object["alias"] = alias_value;
  std::string normalized_email = gitboard::trim(email);
  if (!normalized_email.empty()) {
    json_value email_value;
    email_value.type = json_value::k_string;
    email_value.string = normalized_email;
    member.object["email"] = email_value;
  }
  members.push_back(member);

  fs::path path = team_json_path(repo_path);
  fs::create_directories(path.parent_path());
  write_file_atomic(path, stringify_json(team) + "\n");
  return "{\"ok\":true,\"added\":true,\"team\":" + stringify_json(team) + "}";
}

std::vector<std::string> extract_task_ids(std::string_view text) {
  std::vector<std::string> ids;
  std::set<std::string> seen;
  for (std::size_t i = 0; i + 5 < text.size(); ++i) {
    if (text.substr(i, 5) != "TASK-") continue;
    std::size_t j = i + 5;
    while (j < text.size() &&
           std::isdigit(static_cast<unsigned char>(text[j]))) {
      ++j;
    }
    if (j == i + 5) continue;
    std::string id(text.substr(i, j - i));
    if (seen.insert(id).second) ids.push_back(id);
  }
  return ids;
}

std::string task_summary_json(const json_value& task, bool include_body) {
  if (task.type != json_value::k_object) return "{}";
  auto field = [&](const std::string& name) -> std::string {
    auto it = task.object.find(name);
    if (it == task.object.end()) return "";
    if (it->second.type == json_value::k_string) return it->second.string;
    return "";
  };
  auto value = [&](const std::string& name) -> std::string {
    auto it = task.object.find(name);
    if (it == task.object.end()) return "null";
    return stringify_json(it->second);
  };

  std::ostringstream out;
  out << "{";
  out << "\"id\":" << json_quote(field("id"));
  out << ",\"title\":" << json_quote(field("title"));
  out << ",\"status\":" << json_quote(field("status"));
  out << ",\"assignee\":" << json_quote(field("assignee"));
  out << ",\"priority\":" << json_quote(field("priority"));
  out << ",\"story_points\":" << value("story_points");
  out << ",\"tags\":" << value("tags");
  out << ",\"updated_at\":" << json_quote(field("updated_at"));
  if (include_body) {
    std::string body = field("body");
    constexpr std::size_t kMaxBodyChars = 2000;
    if (body.size() > kMaxBodyChars) body = body.substr(0, kMaxBodyChars) + "\n[truncated]";
    out << ",\"body\":" << json_quote(body);
  }
  out << "}";
  return out.str();
}

std::string build_agent_tool_context(const fs::path& gitboard_path,
                                     const fs::path& repo_path,
                                     const std::string& repo_id,
                                     const repo_ai_config& repo_ai,
                                     const std::string& user_message) {
  std::vector<std::string> base = run_readonly_batch_outputs(
      gitboard_path, repo_path,
      {batch_command_json("team", {}),
       batch_command_json("statuses", {}),
       batch_command_json("dbstatus", {}),
       batch_command_json("list", {})});

  json_value team = json_parser(base[0]).parse();
  json_value statuses = json_parser(base[1]).parse();
  json_value publish_status = json_parser(base[2]).parse();
  json_value task_index = json_parser(base[3]).parse();
  if (task_index.type != json_value::k_object) throw error("list returned invalid JSON");

  std::vector<std::string> all_ids;
  for (const auto& [id, _] : task_index.object) all_ids.push_back(id);
  std::vector<std::string> exact_ids = extract_task_ids(user_message);
  std::set<std::string> exact_set(exact_ids.begin(), exact_ids.end());

  std::vector<std::string> detail_ids;
  constexpr std::size_t kMaxSummaryTasks = 25;
  for (const auto& id : all_ids) {
    if (detail_ids.size() >= kMaxSummaryTasks) break;
    detail_ids.push_back(id);
  }
  for (const auto& id : exact_ids) {
    if (task_index.object.find(id) != task_index.object.end() &&
        std::find(detail_ids.begin(), detail_ids.end(), id) == detail_ids.end()) {
      detail_ids.push_back(id);
    }
  }

  std::vector<std::string> detail_commands;
  for (const auto& id : detail_ids) {
    detail_commands.push_back(batch_command_json("task", {id, "--json"}));
  }
  std::vector<std::string> detail_outputs;
  if (!detail_commands.empty()) {
    detail_outputs = run_readonly_batch_outputs(gitboard_path, repo_path,
                                                detail_commands);
  }

  std::ostringstream tasks;
  tasks << "[";
  for (std::size_t i = 0; i < detail_outputs.size(); ++i) {
    if (i > 0) tasks << ",";
    json_value task = json_parser(detail_outputs[i]).parse();
    tasks << task_summary_json(task, exact_set.count(detail_ids[i]) > 0);
  }
  tasks << "]";

  std::ostringstream allowed_files;
  allowed_files << "[";
  bool first_file_rule = true;
  for (const auto& rule : repo_ai.file_access) {
    if (rule.access == "forbidden") continue;
    if (!first_file_rule) allowed_files << ",";
    first_file_rule = false;
    allowed_files << "{\"path\":" << json_quote(rule.path)
                  << ",\"access\":" << json_quote(rule.access) << "}";
  }
  allowed_files << "]";

  std::ostringstream out;
  out << "{";
  out << "\"repoId\":" << json_quote(repo_id) << ",";
  out << "\"currentUser\":" << json_quote(gitboard::current_user()) << ",";
  out << "\"availableTools\":[\"get_team\",\"get_statuses\",\"get_publish_status\","
         "\"list_tasks\",\"get_task\",\"list_allowed_paths\","
         "\"read_project_file\",\"read_project_file_range\","
         "\"search_project_files\","
         "\"write_project_file\",\"list_team\",\"add_team_member\","
         "\"update_task\",\"move_task\","
         "\"comment_task\"],";
  out << "\"projectFileAccess\":{\"default\":\"forbidden\","
      << "\"allowedPaths\":" << allowed_files.str() << "},";
  out << "\"taskCount\":" << all_ids.size() << ",";
  out << "\"taskSampleLimit\":" << kMaxSummaryTasks << ",";
  out << "\"team\":" << stringify_json(team) << ",";
  out << "\"statuses\":" << stringify_json(statuses) << ",";
  out << "\"publishStatus\":" << stringify_json(publish_status) << ",";
  out << "\"tasks\":" << tasks.str();
  out << "}";
  return out.str();
}

struct agent_file_request {
  std::string repo_id;
  std::string path;
  std::string query;
  std::string content;
  std::string expected_revision;
  int start_line = 0;
  int line_count = 0;
};

agent_file_request parse_agent_file_request(const http_request& request) {
  json_value body = require_object_body(request);
  agent_file_request parsed;
  auto repo_it = body.object.find("repoId");
  if (repo_it == body.object.end() || repo_it->second.type != json_value::k_string) {
    throw error("repoId is required");
  }
  parsed.repo_id = repo_it->second.string;
  if (!valid_repo_id(parsed.repo_id)) throw error("invalid repository id");
  auto path_it = body.object.find("path");
  if (path_it != body.object.end()) {
    if (path_it->second.type != json_value::k_string) {
      throw error("path must be a string");
    }
    parsed.path = normalize_agent_file_path(path_it->second.string);
  }
  auto query_it = body.object.find("query");
  if (query_it != body.object.end()) {
    if (query_it->second.type != json_value::k_string) {
      throw error("query must be a string");
    }
    parsed.query = gitboard::trim(query_it->second.string);
  }
  auto content_it = body.object.find("content");
  if (content_it != body.object.end()) {
    if (content_it->second.type != json_value::k_string) {
      throw error("content must be a string");
    }
    parsed.content = content_it->second.string;
  }
  auto rev_it = body.object.find("expectedRevision");
  if (rev_it != body.object.end()) {
    if (rev_it->second.type != json_value::k_string) {
      throw error("expectedRevision must be a string");
    }
    parsed.expected_revision = rev_it->second.string;
  }
  parsed.start_line = json_int_member(body, "startLine");
  parsed.line_count = json_int_member(body, "lineCount");
  return parsed;
}

std::pair<fs::path, repo_ai_config> repo_and_ai_for_agent_file(
    const server_config& config, const std::string& repo_id) {
  auto repo = config.repositories.find(repo_id);
  if (repo == config.repositories.end()) {
    throw error("repository id not found");
  }
  auto ai = config.repository_ai.find(repo_id);
  return {repo->second, ai == config.repository_ai.end() ? repo_ai_config{} : ai->second};
}

void require_valid_file_range(int start_line, int line_count) {
  if (start_line < 1) throw error("startLine must be 1 or greater");
  if (line_count < 1) throw error("lineCount must be 1 or greater");
  if (line_count > kMaxAgentFileRangeLines) {
    throw error("lineCount is too large");
  }
}

std::string project_file_range_json(const std::string& relative_path,
                                    const fs::path& path,
                                    int start_line,
                                    int line_count) {
  require_valid_file_range(start_line, line_count);
  std::ifstream in(path);
  if (!in) throw error("failed to read project file");

  int end_line = start_line + line_count - 1;
  int current_line = 0;
  int returned = 0;
  bool first = true;
  bool has_more = false;
  std::ostringstream lines;
  std::string line;
  while (std::getline(in, line)) {
    ++current_line;
    if (current_line < start_line) continue;
    if (current_line > end_line) {
      has_more = true;
      break;
    }
    bool truncated = false;
    if (line.size() > kMaxAgentFileRangeLineChars) {
      line.resize(kMaxAgentFileRangeLineChars);
      truncated = true;
    }
    if (!first) lines << ",";
    first = false;
    ++returned;
    lines << "{\"line\":" << current_line
          << ",\"text\":" << json_quote(line)
          << ",\"truncated\":" << (truncated ? "true" : "false") << "}";
  }

  return "{\"ok\":true,\"path\":" + json_quote(relative_path) +
         ",\"revision\":" + json_quote(file_revision(path)) +
         ",\"startLine\":" + std::to_string(start_line) +
         ",\"requestedLineCount\":" + std::to_string(line_count) +
         ",\"returnedLineCount\":" + std::to_string(returned) +
         ",\"hasMore\":" + std::string(has_more ? "true" : "false") +
         ",\"lines\":[" + lines.str() + "]}";
}

http_response handle_agent_file_list(const http_request& request) {
  try {
    agent_file_request parsed = parse_agent_file_request(request);
    server_config config = load_config();
    auto [repo_path, repo_ai] = repo_and_ai_for_agent_file(config, parsed.repo_id);
    require_agent_file_access(repo_ai, parsed.path, false);
    fs::path directory = resolve_agent_file_path(repo_path, parsed.path, false);
    if (!fs::is_directory(directory)) {
      return json_response(400, error_json("path is not a directory"));
    }
    std::ostringstream out;
    out << "{\n  \"ok\": true,\n  \"path\": " << json_quote(parsed.path)
        << ",\n  \"entries\": [";
    bool first = true;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(directory, ec)) {
      if (ec) break;
      fs::file_status status = entry.symlink_status(ec);
      if (ec || fs::is_symlink(status)) {
        ec.clear();
        continue;
      }
      bool is_dir = fs::is_directory(status);
      bool is_file = fs::is_regular_file(status);
      if (!is_dir && !is_file) continue;
      if (!first) out << ",";
      first = false;
      out << "\n    {\"name\": " << json_quote(entry.path().filename().string())
          << ", \"type\": " << json_quote(is_dir ? "directory" : "file") << "}";
    }
    if (!first) out << "\n  ";
    out << "]\n}\n";
    return json_response(200, out.str());
  } catch (const std::exception& ex) {
    return json_response(400, error_json(ex.what()));
  }
}

http_response handle_agent_file_read(const http_request& request) {
  try {
    agent_file_request parsed = parse_agent_file_request(request);
    server_config config = load_config();
    auto [repo_path, repo_ai] = repo_and_ai_for_agent_file(config, parsed.repo_id);
    require_agent_file_access(repo_ai, parsed.path, false);
    fs::path path = resolve_agent_file_path(repo_path, parsed.path, false);
    if (!fs::is_regular_file(path)) return json_response(400, error_json("path is not a file"));
    std::error_code ec;
    std::uintmax_t size = fs::file_size(path, ec);
    if (ec || size > kMaxAgentFileReadBytes) {
      return json_response(400, error_json("file is too large to read"));
    }
    std::string content = read_file(path);
    return json_response(200,
                         "{\n  \"ok\": true,\n  \"path\": " +
                             json_quote(parsed.path) + ",\n  \"revision\": " +
                             json_quote(file_revision(path)) + ",\n  \"content\": " +
                             json_quote(content) + "\n}\n");
  } catch (const std::exception& ex) {
    return json_response(400, error_json(ex.what()));
  }
}

http_response handle_agent_file_read_range(const http_request& request) {
  try {
    agent_file_request parsed = parse_agent_file_request(request);
    server_config config = load_config();
    auto [repo_path, repo_ai] = repo_and_ai_for_agent_file(config, parsed.repo_id);
    require_agent_file_access(repo_ai, parsed.path, false);
    fs::path path = resolve_agent_file_path(repo_path, parsed.path, false);
    if (!fs::is_regular_file(path)) return json_response(400, error_json("path is not a file"));
    return json_response(200,
                         project_file_range_json(parsed.path, path,
                                                 parsed.start_line,
                                                 parsed.line_count) +
                             "\n");
  } catch (const std::exception& ex) {
    return json_response(400, error_json(ex.what()));
  }
}

void search_agent_file(const fs::path& root, const fs::path& file,
                       const std::string& relative_prefix,
                       const std::string& query, std::ostringstream& out,
                       std::size_t& count, bool& first) {
  if (count >= kMaxAgentSearchResults) return;
  std::error_code ec;
  if (!fs::is_regular_file(file, ec) || ec) return;
  std::uintmax_t size = fs::file_size(file, ec);
  if (ec || size > kMaxAgentFileReadBytes) return;
  std::ifstream in(file);
  if (!in) return;
  std::string line;
  std::size_t line_number = 0;
  while (std::getline(in, line) && count < kMaxAgentSearchResults) {
    ++line_number;
    if (line.find(query) == std::string::npos) continue;
    fs::path rel = fs::relative(file, root, ec);
    std::string rel_path =
        ec ? (relative_prefix + file.filename().generic_string()) : rel.generic_string();
    if (!first) out << ",";
    first = false;
    ++count;
    if (line.size() > 500) line.resize(500);
    out << "\n    {\"path\": " << json_quote(rel_path)
        << ", \"line\": " << line_number << ", \"text\": " << json_quote(line)
        << "}";
  }
}

http_response handle_agent_file_search(const http_request& request) {
  try {
    agent_file_request parsed = parse_agent_file_request(request);
    if (parsed.query.empty()) return json_response(400, error_json("query is required"));
    server_config config = load_config();
    auto [repo_path, repo_ai] = repo_and_ai_for_agent_file(config, parsed.repo_id);
    require_agent_file_access(repo_ai, parsed.path, false);
    fs::path start = resolve_agent_file_path(repo_path, parsed.path, false);
    std::ostringstream out;
    out << "{\n  \"ok\": true,\n  \"matches\": [";
    bool first = true;
    std::size_t count = 0;
    std::error_code ec;
    if (fs::is_regular_file(start, ec) && !ec) {
      search_agent_file(repo_path, start, "", parsed.query, out, count, first);
    } else if (fs::is_directory(start, ec) && !ec) {
      for (fs::recursive_directory_iterator it(start, fs::directory_options::skip_permission_denied, ec), end;
           it != end && !ec && count < kMaxAgentSearchResults; it.increment(ec)) {
        fs::file_status status = it->symlink_status(ec);
        if (ec) {
          ec.clear();
          continue;
        }
        if (fs::is_symlink(status)) {
          if (fs::is_directory(status)) it.disable_recursion_pending();
          continue;
        }
        search_agent_file(repo_path, it->path(), "", parsed.query, out, count, first);
      }
    } else {
      return json_response(400, error_json("path is not searchable"));
    }
    if (!first) out << "\n  ";
    out << "],\n  \"truncated\": "
        << (count >= kMaxAgentSearchResults ? "true" : "false") << "\n}\n";
    return json_response(200, out.str());
  } catch (const std::exception& ex) {
    return json_response(400, error_json(ex.what()));
  }
}

http_response handle_agent_file_write(const http_request& request) {
  try {
    agent_file_request parsed = parse_agent_file_request(request);
    server_config config = load_config();
    auto [repo_path, repo_ai] = repo_and_ai_for_agent_file(config, parsed.repo_id);
    require_agent_file_access(repo_ai, parsed.path, true);
    if (parsed.content.size() > kMaxAgentFileReadBytes) {
      return json_response(400, error_json("content is too large to write"));
    }
    fs::path path = resolve_agent_file_path(repo_path, parsed.path, true);
    std::error_code ec;
    if (fs::exists(path, ec) && !fs::is_regular_file(path, ec)) {
      return json_response(400, error_json("path is not a file"));
    }
    std::string revision = file_revision(path);
    if (!parsed.expected_revision.empty() &&
        parsed.expected_revision != revision) {
      return json_response(409, error_json("file revision changed"));
    }
    write_file_atomic(path, parsed.content);
    return json_response(200,
                         "{\n  \"ok\": true,\n  \"path\": " +
                             json_quote(parsed.path) + ",\n  \"revision\": " +
                             json_quote(file_revision(path)) + "\n}\n");
  } catch (const std::exception& ex) {
    return json_response(400, error_json(ex.what()));
  }
}

http_response handle_agent_file_allowed(const http_request& request) {
  try {
    agent_file_request parsed = parse_agent_file_request(request);
    server_config config = load_config();
    auto [repo_path, repo_ai] = repo_and_ai_for_agent_file(config, parsed.repo_id);
    (void)repo_path;
    std::ostringstream out;
    out << "{\n  \"ok\": true,\n  \"paths\": [";
    bool first = true;
    for (const auto& rule : repo_ai.file_access) {
      if (rule.access == "forbidden") continue;
      if (!first) out << ",";
      first = false;
      out << "\n    {\"path\": " << json_quote(rule.path)
          << ", \"access\": " << json_quote(rule.access) << "}";
    }
    if (!first) out << "\n  ";
    out << "]\n}\n";
    return json_response(200, out.str());
  } catch (const std::exception& ex) {
    return json_response(400, error_json(ex.what()));
  }
}

std::string execute_agent_tool(const agent_tool_call& call,
                               const fs::path& gitboard_path,
                               const std::string& repo_id,
                               const fs::path& repo_path,
                               const repo_ai_config& repo_ai) {
  try {
    json_value args;
    if (call.arguments.empty()) {
      args.type = json_value::k_object;
    } else {
      args = json_parser(call.arguments).parse();
      if (args.type != json_value::k_object) {
        throw error("tool arguments must be an object");
      }
    }

    if (call.name == "list_tasks") {
      std::vector<std::string> query_args;
      auto add_filter = [&](const std::string& member, const std::string& option) {
        std::string value = gitboard::trim(json_string_member(args, member));
        if (!value.empty()) {
          query_args.push_back(option);
          query_args.push_back(value);
        }
      };
      add_filter("text", "--text");
      add_filter("status", "--status");
      add_filter("assignee", "--assignee");
      add_filter("priority", "--priority");
      add_filter("tag", "--tag");

      std::vector<std::string> outputs = run_readonly_batch_outputs(
          gitboard_path, repo_path,
          {query_args.empty() ? batch_command_json("list", {})
                              : batch_command_json("query", query_args)});
      json_value index = json_parser(outputs[0]).parse();
      if (index.type != json_value::k_object) throw error("task index is invalid");
      std::vector<std::string> ids;
      for (const auto& [id, _] : index.object) {
        if (ids.size() >= 50) break;
        ids.push_back(id);
      }
      std::vector<std::string> detail_commands;
      for (const auto& id : ids) {
        detail_commands.push_back(batch_command_json("task", {id, "--json"}));
      }
      std::vector<std::string> detail_outputs;
      if (!detail_commands.empty()) {
        detail_outputs =
            run_readonly_batch_outputs(gitboard_path, repo_path, detail_commands);
      }
      std::ostringstream out;
      out << "{\"ok\":true,\"tasks\":[";
      for (std::size_t i = 0; i < detail_outputs.size(); ++i) {
        if (i > 0) out << ",";
        out << task_summary_json(json_parser(detail_outputs[i]).parse(), false);
      }
      out << "],\"truncated\":" << (index.object.size() > ids.size() ? "true" : "false")
          << "}";
      return out.str();
    }

    if (call.name == "list_team") {
      return team_json_response(repo_path);
    }

    if (call.name == "add_team_member") {
      std::string alias = json_string_member(args, "alias");
      std::string email = json_string_member(args, "email");
      return add_team_member(repo_path, alias, email);
    }

    if (call.name == "get_task") {
      std::string task_id = require_task_id_arg(args);
      std::vector<std::string> outputs = run_readonly_batch_outputs(
          gitboard_path, repo_path,
          {batch_command_json("task", {task_id, "--json"})});
      return "{\"ok\":true,\"task\":" +
             task_summary_json(json_parser(outputs[0]).parse(), true) + "}";
    }

    if (call.name == "update_task") {
      std::string task_id = require_task_id_arg(args);
      auto fields_it = args.object.find("fields");
      if (fields_it == args.object.end() ||
          fields_it->second.type != json_value::k_object) {
        throw error("fields object is required");
      }
      std::vector<std::string> commands;
      const std::set<std::string> string_fields = {
          "title", "body", "assignee", "priority", "ci_status"};
      for (const auto& [field, value] : fields_it->second.object) {
        if (field == "status") {
          if (value.type != json_value::k_string) throw error("status must be a string");
          commands.push_back(
              encoded_update_command("move", task_id, gitboard::trim(value.string)));
        } else if (string_fields.count(field)) {
          if (value.type != json_value::k_string) {
            throw error(field + " must be a string");
          }
          if (field == "assignee") {
            require_known_assignee(repo_path, gitboard::trim(value.string));
          }
          commands.push_back(encoded_update_command(field, task_id, value.string));
        } else if (field == "story_points") {
          commands.push_back(points_update_command(task_id, task_points_value(value)));
        } else if (field == "tags" || field == "branches" || field == "prs") {
          commands.push_back(encoded_update_command(
              field, task_id, json_string_array_to_csv(value, field)));
        } else {
          throw error("unknown task field: " + field);
        }
      }
      if (commands.empty()) throw error("no task fields to update");
      commands.push_back(batch_command_json("task", {task_id, "--json"}));
      std::vector<std::string> outputs =
          run_agent_task_batch(gitboard_path, repo_path, std::move(commands));
      return "{\"ok\":true,\"task\":" +
             task_summary_json(json_parser(outputs[outputs.size() - 2]).parse(), true) +
             ",\"dbstatus\":" + outputs.back() + "}";
    }

    if (call.name == "move_task") {
      std::string task_id = require_task_id_arg(args);
      std::string status = gitboard::trim(json_string_member(args, "status"));
      if (status.empty()) throw error("status is required");
      std::vector<std::string> outputs = run_agent_task_batch(
          gitboard_path, repo_path,
          {encoded_update_command("move", task_id, status),
           batch_command_json("task", {task_id, "--json"})});
      return "{\"ok\":true,\"task\":" +
             task_summary_json(json_parser(outputs[outputs.size() - 2]).parse(), true) +
             ",\"dbstatus\":" + outputs.back() + "}";
    }

    if (call.name == "comment_task") {
      std::string task_id = require_task_id_arg(args);
      std::string message = gitboard::trim(json_string_member(args, "message"));
      if (message.empty()) throw error("message is required");
      std::vector<std::string> outputs = run_agent_task_batch(
          gitboard_path, repo_path,
          {encoded_update_command("comment", task_id, message),
           batch_command_json("task", {task_id, "--json"})});
      return "{\"ok\":true,\"task\":" +
             task_summary_json(json_parser(outputs[outputs.size() - 2]).parse(), true) +
             ",\"dbstatus\":" + outputs.back() + "}";
    }

    if (call.name == "list_allowed_paths") {
      std::ostringstream out;
      out << "{\"ok\":true,\"repoId\":" << json_quote(repo_id) << ",\"paths\":[";
      bool first = true;
      for (const auto& rule : repo_ai.file_access) {
        if (rule.access == "forbidden") continue;
        if (!first) out << ",";
        first = false;
        out << "{\"path\":" << json_quote(rule.path)
            << ",\"access\":" << json_quote(rule.access) << "}";
      }
      out << "]}";
      return out.str();
    }

    std::string relative_path = normalize_agent_file_path(json_string_member(args, "path"));
    if (call.name == "list_project_files") {
      require_agent_file_access(repo_ai, relative_path, false);
      fs::path directory = resolve_agent_file_path(repo_path, relative_path, false);
      if (!fs::is_directory(directory)) throw error("path is not a directory");
      std::ostringstream out;
      out << "{\"ok\":true,\"path\":" << json_quote(relative_path)
          << ",\"entries\":[";
      bool first = true;
      std::error_code ec;
      for (const auto& entry : fs::directory_iterator(directory, ec)) {
        if (ec) break;
        fs::file_status status = entry.symlink_status(ec);
        if (ec || fs::is_symlink(status)) {
          ec.clear();
          continue;
        }
        bool is_dir = fs::is_directory(status);
        bool is_file = fs::is_regular_file(status);
        if (!is_dir && !is_file) continue;
        if (!first) out << ",";
        first = false;
        out << "{\"name\":" << json_quote(entry.path().filename().string())
            << ",\"type\":" << json_quote(is_dir ? "directory" : "file")
            << "}";
      }
      out << "]}";
      return out.str();
    }

    if (call.name == "read_project_file") {
      require_agent_file_access(repo_ai, relative_path, false);
      fs::path path = resolve_agent_file_path(repo_path, relative_path, false);
      if (!fs::is_regular_file(path)) throw error("path is not a file");
      std::error_code ec;
      std::uintmax_t size = fs::file_size(path, ec);
      if (ec || size > kMaxAgentFileReadBytes) {
        throw error("file is too large to read");
      }
      return "{\"ok\":true,\"path\":" + json_quote(relative_path) +
             ",\"revision\":" + json_quote(file_revision(path)) +
             ",\"content\":" + json_quote(read_file(path)) + "}";
    }

    if (call.name == "read_project_file_range") {
      int start_line = json_int_member(args, "startLine");
      int line_count = json_int_member(args, "lineCount");
      require_agent_file_access(repo_ai, relative_path, false);
      fs::path path = resolve_agent_file_path(repo_path, relative_path, false);
      if (!fs::is_regular_file(path)) throw error("path is not a file");
      return project_file_range_json(relative_path, path, start_line, line_count);
    }

    if (call.name == "search_project_files") {
      std::string query = gitboard::trim(json_string_member(args, "query"));
      if (query.empty()) throw error("query is required");
      require_agent_file_access(repo_ai, relative_path, false);
      fs::path start = resolve_agent_file_path(repo_path, relative_path, false);
      std::ostringstream out;
      out << "{\"ok\":true,\"matches\":[";
      bool first = true;
      std::size_t count = 0;
      std::error_code ec;
      if (fs::is_regular_file(start, ec) && !ec) {
        search_agent_file(repo_path, start, "", query, out, count, first);
      } else if (fs::is_directory(start, ec) && !ec) {
        for (fs::recursive_directory_iterator it(
                 start, fs::directory_options::skip_permission_denied, ec),
             end;
             it != end && !ec && count < kMaxAgentSearchResults; it.increment(ec)) {
          fs::file_status status = it->symlink_status(ec);
          if (ec) {
            ec.clear();
            continue;
          }
          if (fs::is_symlink(status)) {
            if (fs::is_directory(status)) it.disable_recursion_pending();
            continue;
          }
          search_agent_file(repo_path, it->path(), "", query, out, count, first);
        }
      } else {
        throw error("path is not searchable");
      }
      out << "],\"truncated\":"
          << (count >= kMaxAgentSearchResults ? "true" : "false") << "}";
      return out.str();
    }

    throw error("unknown tool: " + call.name);
  } catch (const std::exception& ex) {
    return "{\"ok\":false,\"error\":" + json_quote(ex.what()) + "}";
  }
}

void append_update_action_commands(const fs::path& repo_path,
                                   const json_value& fields,
                                   const std::string& task_id,
                                   std::vector<std::string>& commands) {
  if (fields.type != json_value::k_object) throw error("fields must be an object");
  const std::set<std::string> string_fields = {
      "title", "body", "assignee", "priority", "ci_status"};
  for (const auto& [field, value] : fields.object) {
    if (field == "status") {
      if (value.type != json_value::k_string) throw error("status must be a string");
      commands.push_back(
          encoded_update_command("move", task_id, gitboard::trim(value.string)));
    } else if (string_fields.count(field)) {
      if (value.type != json_value::k_string) {
        throw error(field + " must be a string");
      }
      if (field == "assignee") {
        require_known_assignee(repo_path, gitboard::trim(value.string));
      }
      commands.push_back(encoded_update_command(field, task_id, value.string));
    } else if (field == "story_points") {
      commands.push_back(points_update_command(task_id, task_points_value(value)));
    } else if (field == "tags" || field == "branches" || field == "prs") {
      commands.push_back(encoded_update_command(
          field, task_id, json_string_array_to_csv(value, field)));
    } else {
      throw error("unknown task field: " + field);
    }
  }
}

http_response handle_agent_apply_actions(const http_request& request,
                                         const fs::path& gitboard_path) {
  json_value body = require_object_body(request);
  auto repo_it = body.object.find("repoId");
  auto actions_it = body.object.find("actions");
  if (repo_it == body.object.end() || repo_it->second.type != json_value::k_string) {
    return json_response(400, error_json("repoId is required"));
  }
  if (actions_it == body.object.end() ||
      actions_it->second.type != json_value::k_array) {
    return json_response(400, error_json("actions are required"));
  }
  std::string repo_id = repo_it->second.string;
  if (!valid_repo_id(repo_id)) {
    return json_response(400, error_json("invalid repository id"));
  }
  server_config config = load_config();
  auto repo = config.repositories.find(repo_id);
  if (repo == config.repositories.end()) {
    return json_response(404, error_json("repository id not found"));
  }

  std::vector<std::string> commands;
  std::vector<std::size_t> task_output_indices;
  std::size_t applied = 0;
  try {
    for (const json_value& action : actions_it->second.array) {
      if (action.type != json_value::k_object) throw error("action must be an object");
      if (!json_bool_member_or_default(action, "selected", true)) continue;
      std::string type = json_string_member(action, "type");
      if (type == "add_team_member") {
        add_team_member(repo->second, json_string_member(action, "alias"),
                        json_string_member(action, "email"));
        ++applied;
        continue;
      }

      std::string task_id = require_task_id_arg(action);
      require_existing_task(repo->second, task_id);
      std::size_t before = commands.size();
      if (type == "update_task") {
        auto fields_it = action.object.find("fields");
        if (fields_it == action.object.end()) throw error("fields object is required");
        append_update_action_commands(repo->second, fields_it->second, task_id, commands);
      } else if (type == "move_task") {
        std::string status = gitboard::trim(json_string_member(action, "status"));
        if (status.empty()) throw error("status is required");
        commands.push_back(encoded_update_command("move", task_id, status));
      } else if (type == "comment_task") {
        std::string message = gitboard::trim(json_string_member(action, "message"));
        if (message.empty()) throw error("message is required");
        commands.push_back(encoded_update_command("comment", task_id, message));
      } else {
        throw error("unknown action type: " + type);
      }
      if (commands.size() == before) throw error("action did not include changes");
      task_output_indices.push_back(commands.size());
      commands.push_back(batch_command_json("task", {task_id, "--json"}));
      ++applied;
    }
    if (applied == 0) {
      return json_response(400, error_json("no selected actions"));
    }
    std::vector<std::string> outputs;
    if (!commands.empty()) {
      outputs = run_agent_task_batch(gitboard_path, repo->second, std::move(commands));
    }
    std::ostringstream out;
    out << "{\n  \"ok\": true,\n  \"applied\": " << applied
        << ",\n  \"tasks\": [";
    bool first = true;
    for (std::size_t index : task_output_indices) {
      if (index >= outputs.size()) continue;
      if (!first) out << ",";
      first = false;
      out << task_summary_json(json_parser(outputs[index]).parse(), true);
    }
    out << "],\n  \"dbstatus\": ";
    if (!outputs.empty()) {
      out << outputs.back();
    } else {
      out << "{}";
    }
    out << "\n}\n";
    return json_response(200, out.str());
  } catch (const std::exception& ex) {
    return json_response(400, error_json(ex.what()));
  }
}
http_response handle_batch(const http_request& request, const fs::path& gitboard_path) {
  json_value body = require_object_body(request);
  auto repo_it = body.object.find("repoId");
  auto batch_it = body.object.find("batch");
  if (repo_it == body.object.end() || repo_it->second.type != json_value::k_string) {
    return json_response(400, error_json("repoId is required"));
  }
  if (batch_it == body.object.end() || batch_it->second.type != json_value::k_array) {
    return json_response(400, error_json("batch is required"));
  }
  std::string repo_id = repo_it->second.string;
  if (!valid_repo_id(repo_id)) {
    return json_response(400, error_json("invalid repository id"));
  }
  server_config config = load_config();
  auto repo = config.repositories.find(repo_id);
  if (repo == config.repositories.end()) {
    return json_response(404, error_json("repository id not found"));
  }

  temp_file temp(config_dir() / "batch");
  write_file_atomic(temp.path(),
                    "{\"batch\":" + stringify_json(batch_it->second) + "}\n");
  process_result result =
      run_process(gitboard_path,
                  {"--project-root", repo->second.string(), "batch",
                   temp.path().string()});

  try {
    json_parser(result.output).parse();
  } catch (const std::exception&) {
    if (result.exit_code == 0) {
      return json_response(500, error_json("gitboard returned invalid JSON"));
    }
    return json_response(500, error_json(result.output.empty()
                                             ? "gitboard command failed"
                                             : result.output));
  }
  return json_response(result.exit_code == 0 ? 200 : 400, result.output);
}

}  // namespace gitboard::server
