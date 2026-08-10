#include "src/core/status_config.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>

#include "src/core/error.h"
#include "src/core/filesystem.h"
#include "src/core/json.h"

namespace gitboard {

bool status_config::known(const std::string& status) const {
  return transitions.find(status) != transitions.end();
}

bool status_config::can_transition(const std::string& from,
                                   const std::string& to) const {
  auto it = transitions.find(from);
  if (it == transitions.end()) return true;
  if (it->second.empty()) return true;
  return std::find(it->second.begin(), it->second.end(), to) !=
         it->second.end();
}

std::string load_status_config_json(const std::filesystem::path& tasks_dir) {
  std::vector<std::filesystem::path> candidates;
  if (!tasks_dir.empty()) {
    candidates.push_back(tasks_dir / "statuses.json");
  }
  candidates.insert(candidates.end(), {
      std::filesystem::current_path() / "config/statuses.json",
      std::filesystem::current_path() / "../config/statuses.json",
  });
  const char* runfiles = std::getenv("RUNFILES_DIR");
  if (runfiles) {
    candidates.push_back(std::filesystem::path(runfiles) /
                         "_main/config/statuses.json");
    candidates.push_back(std::filesystem::path(runfiles) /
                         "gitboard_cpp/config/statuses.json");
  }
  std::string content;
  for (const auto& path : candidates) {
    if (std::filesystem::exists(path)) {
      return read_file(path);
    }
  }
  return R"({
  "backlog": {
    "order": 0,
    "display": "Backlog",
    "transitions": []
  },
  "todo": {
    "order": 1,
    "display": "To Do",
    "transitions": []
  },
  "in_progress": {
    "order": 2,
    "display": "In Progress",
    "transitions": []
  },
  "review": {
    "order": 3,
    "display": "Review",
    "transitions": []
  },
  "done": {
    "order": 4,
    "display": "Done",
    "transitions": []
  },
  "released": {
    "order": 5,
    "display": "Released",
    "transitions": []
  },
  "archived": {
    "order": 6,
    "display": "Archived",
    "transitions": [],
    "visible": false
  }
})";
}

std::string load_default_status_config_json() {
  return load_status_config_json({});
}

status_config load_status_config(const std::filesystem::path& tasks_dir) {
  const std::string content = load_status_config_json(tasks_dir);
  json_value root = json_parser(content).parse();
  if (root.type != json_value::k_object) {
    throw error("status config must be a JSON object");
  }
  status_config cfg;
  for (const auto& [key, value] : root.object) {
    if (value.type != json_value::k_object) {
      throw error("status config entry must be an object: " + key);
    }
    auto transitions = value.object.find("transitions");
    if (transitions == value.object.end() ||
        transitions->second.type != json_value::k_array) {
      throw error("status config entry requires transitions array: " + key);
    }
    std::vector<std::string> values;
    for (const json_value& item : transitions->second.array) {
      if (item.type != json_value::k_string) {
        throw error("status config transitions must be strings: " + key);
      }
      values.push_back(item.string);
    }
    cfg.transitions[key] = values;
  }
  return cfg;
}

}  // namespace gitboard
