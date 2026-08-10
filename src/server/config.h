#ifndef GITBOARD_SERVER_CONFIG_H_
#define GITBOARD_SERVER_CONFIG_H_

#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "src/core/json.h"

namespace gitboard::server {

struct ai_config {
  bool enabled = false;
  std::string provider = "openai-compatible";
  std::string base_url;
  std::string model;
  std::string api_key_ref;
  int timeout_seconds = 30;
  int retry_attempts = 5;
  int max_output_tokens = 0;
  bool allow_writes = false;
  std::string system_prompt;
};

struct file_access_rule {
  std::string path;
  std::string access = "forbidden";
};

struct repo_ai_config {
  std::vector<file_access_rule> file_access;
};

struct server_config {
  std::map<std::string, std::filesystem::path> repositories;
  std::map<std::string, repo_ai_config> repository_ai;
  ai_config ai;
};

std::filesystem::path config_dir();
void set_config_dir_for_process(std::filesystem::path path);
std::filesystem::path config_path();
bool valid_repo_id(const std::string& id);
server_config load_config();
void save_config(const server_config& config);
std::filesystem::path validate_repository_path(const std::filesystem::path& path);
void validate_repository(const std::string& id, const std::filesystem::path& path);
std::string config_response_json(const server_config& config);
bool valid_file_access_policy(const std::string& access);
repo_ai_config parse_repo_ai_config_update(const gitboard::json_value& body,
                                           const repo_ai_config& existing);
std::string repo_file_access_response_json(
    const std::string& repo_id, const std::filesystem::path& repo_path,
    const repo_ai_config& config);
std::string default_ai_system_prompt();
std::string ai_api_key_ref(const ai_config& config);
bool ai_api_key_configured(const ai_config& config);
void save_ai_api_key(const std::string& api_key, const ai_config& config);
std::string load_ai_api_key(const ai_config& config);
void clear_ai_api_key(const ai_config& config);
std::string ai_config_response_json(const ai_config& config);
ai_config parse_ai_config_update(const gitboard::json_value& body,
                                 const ai_config& existing);

}  // namespace gitboard::server

#endif  // GITBOARD_SERVER_CONFIG_H_
