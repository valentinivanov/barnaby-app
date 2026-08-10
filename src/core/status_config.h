#ifndef GITBOARD_CORE_STATUS_CONFIG_H_
#define GITBOARD_CORE_STATUS_CONFIG_H_

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace gitboard {

struct status_config {
  std::map<std::string, std::vector<std::string>> transitions;

  bool known(const std::string& status) const;
  bool can_transition(const std::string& from, const std::string& to) const;
};

status_config load_status_config(const std::filesystem::path& tasks_dir = {});
std::string load_status_config_json(const std::filesystem::path& tasks_dir = {});
std::string load_default_status_config_json();

}  // namespace gitboard

#endif  // GITBOARD_CORE_STATUS_CONFIG_H_
