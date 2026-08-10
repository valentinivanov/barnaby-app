#include "src/core/task.h"

#include <cctype>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

#include "src/core/error.h"
#include "src/core/strings.h"
#include "src/core/time.h"

namespace gitboard {
namespace {

struct parsed_front_matter {
  std::map<std::string, std::string> scalars;
  std::map<std::string, std::vector<std::string>> lists;
  std::string body;
};

std::string unquote_yaml(std::string value) {
  value = trim(std::move(value));
  if (value.size() >= 2 && value.front() == '\'' && value.back() == '\'') {
    value = value.substr(1, value.size() - 2);
    std::string out;
    out.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
      if (value[i] == '\'' && i + 1 < value.size() && value[i + 1] == '\'') {
        out += '\'';
        ++i;
      } else {
        out += value[i];
      }
    }
    return out;
  }
  if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
    value = value.substr(1, value.size() - 2);
  }
  return value;
}

std::string quote_yaml(const std::string& value) {
  if (value.empty()) return "''";
  bool plain = true;
  for (unsigned char c : value) {
    if (!(std::isalnum(c) || c == '_' || c == '-' || c == '+' || c == '.' ||
          c == '/' || c == ':' || c == '@')) {
      plain = false;
      break;
    }
  }
  if (plain && value != "true" && value != "false" && value != "null") {
    return value;
  }
  std::string out = "'";
  for (char c : value) {
    if (c == '\'') out += "''";
    else out += c;
  }
  out += "'";
  return out;
}

parsed_front_matter parse_front_matter(const std::string& content) {
  std::size_t yaml_start = 0;
  if (starts_with(content, "---\r\n")) {
    yaml_start = 5;
  } else if (starts_with(content, "---\n")) {
    yaml_start = 4;
  } else if (content == "---" || content == "---\r") {
    yaml_start = content.size();
  } else {
    throw error("task file is missing YAML front matter");
  }
  const auto end = content.find("\n---", yaml_start);
  if (end == std::string::npos) {
    throw error("task file has unterminated YAML front matter");
  }
  std::string yaml = content.substr(yaml_start, end - yaml_start);
  std::string body = content.substr(end + 4);
  if (starts_with(body, "\r\n")) body.erase(0, 2);
  else if (starts_with(body, "\n")) body.erase(0, 1);
  if (starts_with(body, "\r\n")) body.erase(0, 2);
  else if (starts_with(body, "\n")) body.erase(0, 1);

  parsed_front_matter parsed;
  parsed.body = body;
  std::string current_list_key;
  for (std::string line : split(yaml, '\n')) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (trim(line).empty()) continue;
    std::string trimmed = trim(line);
    if (starts_with(trimmed, "- ")) {
      if (current_list_key.empty()) throw error("YAML list item without key");
      parsed.lists[current_list_key].push_back(unquote_yaml(trimmed.substr(2)));
      continue;
    }
    current_list_key.clear();
    auto colon = line.find(':');
    if (colon == std::string::npos) throw error("invalid YAML line: " + line);
    std::string key = trim(line.substr(0, colon));
    std::string value = trim(line.substr(colon + 1));
    if (value.empty()) {
      current_list_key = key;
      parsed.lists[key] = {};
    } else if (value == "[]") {
      parsed.lists[key] = {};
    } else if (value.size() >= 2 && value.front() == '[' &&
               value.back() == ']') {
      std::string inner = value.substr(1, value.size() - 2);
      std::vector<std::string> items;
      if (!trim(inner).empty()) {
        for (auto part : split(inner, ',')) items.push_back(unquote_yaml(part));
      }
      parsed.lists[key] = items;
    } else {
      parsed.scalars[key] = unquote_yaml(value);
    }
  }
  return parsed;
}

std::vector<std::string> get_list(const parsed_front_matter& parsed,
                                  const std::string& key) {
  auto it = parsed.lists.find(key);
  if (it != parsed.lists.end()) return it->second;
  auto scalar = parsed.scalars.find(key);
  if (scalar != parsed.scalars.end() && !scalar->second.empty()) {
    return {scalar->second};
  }
  return {};
}

std::string get_scalar(const parsed_front_matter& parsed, const std::string& key,
                       std::string fallback = "") {
  auto scalar = parsed.scalars.find(key);
  if (scalar != parsed.scalars.end()) return scalar->second;
  auto list = parsed.lists.find(key);
  if (list != parsed.lists.end() && !list->second.empty()) return list->second[0];
  return fallback;
}

void emit_list(std::ostringstream& out, const std::string& key,
               const std::vector<std::string>& values) {
  out << key << ":";
  if (values.empty()) {
    out << " []\n";
    return;
  }
  out << "\n";
  for (const auto& value : values) out << "- " << quote_yaml(value) << "\n";
}

const std::set<std::string>& known_front_matter_fields() {
  static const std::set<std::string> known = {
      "id",       "title",      "assignee", "priority", "story_points",
      "tags",     "status",     "created_at", "updated_at", "created_by",
      "branches", "prs",        "ci_status"};
  return known;
}

void emit_extra_front_matter(std::ostringstream& out, const task& current_task) {
  const auto& known = known_front_matter_fields();
  for (const auto& [key, value] : current_task.extra_scalars) {
    if (!known.count(key)) out << key << ": " << quote_yaml(value) << "\n";
  }
  for (const auto& [key, values] : current_task.extra_lists) {
    if (!known.count(key)) emit_list(out, key, values);
  }
}

bool is_allowed_story_points(int value) {
  static const std::set<int> allowed = {1, 2, 3, 5, 8, 13, 21, 100};
  return allowed.count(value) != 0;
}

int parse_story_points(const parsed_front_matter& parsed) {
  const std::string raw = get_scalar(parsed, "story_points", "100");
  int value = 0;
  try {
    std::size_t used = 0;
    value = std::stoi(raw, &used);
    if (used != raw.size()) throw std::invalid_argument("trailing characters");
  } catch (const std::exception&) {
    throw error("story_points must be a number");
  }
  // Story points are intentionally constrained to planning values understood by
  // the UI and Agent Pip, with 100 reserved for "unestimated".
  if (!is_allowed_story_points(value)) {
    throw error("story_points must be one of 1, 2, 3, 5, 8, 13, 21, 100");
  }
  return value;
}

}  // namespace

task task_from_content(const std::string& content) {
  parsed_front_matter parsed = parse_front_matter(content);

  task current_task;
  current_task.id = get_scalar(parsed, "id");
  current_task.title = get_scalar(parsed, "title");
  if (current_task.id.empty()) throw error("task is missing id");
  if (current_task.title.empty()) throw error("task is missing title");
  current_task.assignee = get_scalar(parsed, "assignee");
  current_task.priority = get_scalar(parsed, "priority", "medium");
  current_task.story_points = parse_story_points(parsed);
  current_task.tags = get_list(parsed, "tags");
  current_task.status = get_scalar(parsed, "status", "todo");
  current_task.created_at = get_scalar(parsed, "created_at");
  current_task.updated_at = get_scalar(parsed, "updated_at");
  current_task.created_by = get_scalar(parsed, "created_by");
  current_task.branches = get_list(parsed, "branches");
  current_task.prs = get_list(parsed, "prs");
  current_task.ci_status = get_scalar(parsed, "ci_status", "unknown");
  current_task.body = parsed.body;
  const auto& known = known_front_matter_fields();
  for (const auto& [key, value] : parsed.scalars) {
    if (!known.count(key)) current_task.extra_scalars[key] = value;
  }
  for (const auto& [key, values] : parsed.lists) {
    if (!known.count(key)) current_task.extra_lists[key] = values;
  }
  return current_task;
}

std::string task_to_content(const task& current_task) {
  const std::string updated_at = current_utc_iso();
  std::ostringstream out;
  out << "---\n";
  out << "assignee: " << quote_yaml(current_task.assignee) << "\n";
  emit_list(out, "branches", current_task.branches);
  out << "ci_status: " << quote_yaml(current_task.ci_status) << "\n";
  out << "created_at: " << quote_yaml(current_task.created_at) << "\n";
  out << "created_by: " << quote_yaml(current_task.created_by) << "\n";
  out << "id: " << quote_yaml(current_task.id) << "\n";
  out << "priority: " << quote_yaml(current_task.priority) << "\n";
  emit_list(out, "prs", current_task.prs);
  out << "status: " << quote_yaml(current_task.status) << "\n";
  out << "story_points: " << current_task.story_points << "\n";
  emit_list(out, "tags", current_task.tags);
  out << "title: " << quote_yaml(current_task.title) << "\n";
  out << "updated_at: " << quote_yaml(updated_at) << "\n";
  emit_extra_front_matter(out, current_task);
  out << "---\n\n";
  out << current_task.body;
  if (!current_task.body.empty() && current_task.body.back() != '\n') out << "\n";
  return out.str();
}

std::string default_body(const std::string& title) {
  return "# " + title +
         "\n\n## Description\n\n\n## Checklist\n- [ ] \n\n## Comments\n";
}

std::string extract_filename_id(const std::filesystem::path& path) {
  const std::string name = path.filename().string();
  const auto underscore = name.find('_');
  if (underscore == std::string::npos) return "";
  return name.substr(0, underscore);
}

}  // namespace gitboard
