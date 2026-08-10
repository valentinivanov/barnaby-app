#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "src/core/json.h"
#include "src/core/strings.h"
#include "src/server/config.h"

namespace fs = std::filesystem;

namespace {

struct failure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

void expect(bool condition, const std::string& message) {
  if (!condition) throw failure(message);
}

std::string legacy_default_ai_system_prompt() {
  return
      "You are Agent Pip, Barnaby's repository task assistant.\n"
      "You are an experienced scrum master and project manager. Users are "
      "encouraged to ask you for advice about tasks in the current repository "
      "or broader project situations.\n"
      "\n"
      "Use Barnaby task context as the source of truth for repository-specific "
      "claims. For general software engineering, planning, agile, and process "
      "questions, you may use your general knowledge. Clearly distinguish "
      "repository-specific facts from general guidance.\n"
      "\n"
      "You cannot modify tasks, publish, sync, run shell commands, or access "
      "arbitrary files directly. You may use only the task tools and "
      "policy-limited project file tools exposed by Barnaby. Do not claim to "
      "have read or changed tasks or files unless that information appears in "
      "the provided context or a tool result. Answer in plain text. If the "
      "context is insufficient for a repository-specific answer, say what is "
      "missing.";
}

int process_id() {
#ifdef _WIN32
  return static_cast<int>(GetCurrentProcessId());
#else
  return static_cast<int>(getpid());
#endif
}

void test_default_ai_prompt_supports_code_review() {
  std::string prompt = gitboard::server::default_ai_system_prompt();
  expect(prompt.find("code reviewer") != std::string::npos,
         "default AI prompt should include code reviewer role");
  expect(prompt.find("code review requests") != std::string::npos,
         "default AI prompt should include code review guidance");

  gitboard::json_value update =
      gitboard::json_parser("{\"systemPrompt\":\"" +
                            gitboard::json_escape(legacy_default_ai_system_prompt()) +
                            "\"}")
          .parse();
  gitboard::server::ai_config migrated =
      gitboard::server::parse_ai_config_update(update, gitboard::server::ai_config{});
  expect(migrated.system_prompt == prompt,
         "legacy default prompt should migrate to current default prompt");

  gitboard::json_value custom_update =
      gitboard::json_parser("{\"systemPrompt\":\"Custom developer prompt\"}").parse();
  gitboard::server::ai_config custom =
      gitboard::server::parse_ai_config_update(custom_update,
                                               gitboard::server::ai_config{});
  expect(custom.system_prompt == "Custom developer prompt",
         "custom AI prompt should be preserved");
}

void test_ai_config_round_trip_redacts_key() {
  fs::path cwd = fs::temp_directory_path() /
                 ("gitboard-server-ai-config-" + std::to_string(process_id()));
  fs::remove_all(cwd);
  fs::create_directories(cwd);
  gitboard::server::set_config_dir_for_process(cwd);

  gitboard::server::server_config config;
  config.ai.enabled = true;
  config.ai.base_url = "https://api.example.test/v1";
  config.ai.model = "model-a";
  config.ai.timeout_seconds = 45;
  config.ai.max_output_tokens = 2048;
  config.ai.allow_writes = true;
  config.ai.system_prompt = "Custom Pip prompt";
  config.ai.api_key_ref = gitboard::server::ai_api_key_ref(config.ai);
  gitboard::server::save_config(config);

  gitboard::server::server_config loaded = gitboard::server::load_config();
  expect(loaded.ai.enabled, "AI enabled should persist");
  expect(loaded.ai.base_url == "https://api.example.test/v1",
         "baseUrl should persist");
  expect(loaded.ai.model == "model-a", "model should persist");
  expect(loaded.ai.api_key_ref == gitboard::server::ai_api_key_ref(loaded.ai),
         "AI key reference should persist");
  expect(loaded.ai.system_prompt == "Custom Pip prompt",
         "AI system prompt should persist");

  std::string response = gitboard::server::ai_config_response_json(loaded.ai);
  gitboard::json_value parsed = gitboard::json_parser(response).parse();
  expect(parsed.object.find("apiKey") == parsed.object.end(),
         "AI config response must not expose an API key value");
  expect(parsed.object["apiKeyRef"].string ==
             gitboard::server::ai_api_key_ref(loaded.ai),
         "AI config response should report the secure key reference");
#if defined(__APPLE__)
  expect(parsed.object["secretStorage"].string == "macos-keychain",
         "AI config response should report secure storage");
  expect(parsed.object["apiKeyRef"].string.find("macos-keychain:") == 0,
         "AI key reference should use macOS Keychain");
#elif defined(__linux__)
  expect(parsed.object["secretStorage"].string == "linux-secret-service",
         "AI config response should report Linux secure storage");
  expect(parsed.object["apiKeyRef"].string.find("linux-secret-service:") == 0,
         "AI key reference should use Linux Secret Service");
#elif defined(_WIN32)
  expect(parsed.object["secretStorage"].string == "windows-credential-manager",
         "AI config response should report Windows secure storage");
  expect(parsed.object["apiKeyRef"].string.find("windows-credential-manager:") == 0,
         "AI key reference should use Windows Credential Manager");
#else
  expect(parsed.object["secretStorage"].string == "unsupported",
         "AI config response should report unsupported secure storage");
#endif
  expect(parsed.object["systemPrompt"].string == "Custom Pip prompt",
         "AI config response should include the system prompt");
  fs::remove_all(cwd);
}

void test_repo_file_access_round_trip() {
  fs::path cwd = fs::temp_directory_path() /
                 ("gitboard-server-file-access-" + std::to_string(process_id()));
  fs::remove_all(cwd);
  fs::create_directories(cwd);
  gitboard::server::set_config_dir_for_process(cwd);

  gitboard::server::server_config config;
  config.repositories["work"] = "/tmp/work";
  config.repository_ai["work"].file_access = {
      {"doc/", "full_access"},
      {"src/", "read_only"},
      {"build/", "forbidden"},
  };
  gitboard::server::save_config(config);

  gitboard::server::server_config loaded = gitboard::server::load_config();
  expect(loaded.repository_ai["work"].file_access.size() == 3,
         "file access rules should persist");
  expect(loaded.repository_ai["work"].file_access[0].path == "doc/",
         "file access path should persist");
  expect(loaded.repository_ai["work"].file_access[0].access == "full_access",
         "file access policy should persist");
  expect(loaded.repository_ai["work"].file_access[1].access == "read_only",
         "read-only policy should persist");

  gitboard::json_value update =
      gitboard::json_parser(
          "{\"fileAccess\":[{\"path\":\"tests\",\"access\":\"read_only\"}]}")
          .parse();
  gitboard::server::repo_ai_config parsed =
      gitboard::server::parse_repo_ai_config_update(
          update, gitboard::server::repo_ai_config{});
  expect(parsed.file_access.size() == 1,
         "file access update should parse one rule");
  expect(parsed.file_access[0].path == "tests/",
         "file access update should normalize folder path");
  expect(parsed.file_access[0].access == "read_only",
         "file access update should parse access");

  fs::remove_all(cwd);
}

void test_repo_file_access_defaults_include_tasks() {
  fs::path repo = fs::temp_directory_path() /
                  ("gitboard-server-file-access-defaults-" +
                   std::to_string(process_id()));
  fs::remove_all(repo);
  fs::create_directories(repo / "src");
  fs::create_directories(repo / "tasks");

  std::string response = gitboard::server::repo_file_access_response_json(
      "work", repo, gitboard::server::repo_ai_config{});
  gitboard::json_value parsed = gitboard::json_parser(response).parse();
  auto folders_it = parsed.object.find("folders");
  expect(folders_it != parsed.object.end() &&
             folders_it->second.type == gitboard::json_value::k_array,
         "file access response should include folders");

  std::string src_access;
  std::string tasks_access;
  for (const gitboard::json_value& folder : folders_it->second.array) {
    if (folder.type != gitboard::json_value::k_object) continue;
    std::string path = gitboard::json_string_member(folder, "path");
    if (path == "src/") src_access = gitboard::json_string_member(folder, "access");
    if (path == "tasks/") {
      tasks_access = gitboard::json_string_member(folder, "access");
    }
  }
  expect(src_access == "forbidden",
         "ordinary folders should still default to forbidden");
  expect(tasks_access == "full_access",
         "tasks folder should default to full file access");

  gitboard::server::repo_ai_config explicit_config;
  explicit_config.file_access = {{"tasks/", "forbidden"}};
  std::string explicit_response = gitboard::server::repo_file_access_response_json(
      "work", repo, explicit_config);
  gitboard::json_value explicit_parsed =
      gitboard::json_parser(explicit_response).parse();
  std::string explicit_tasks_access;
  for (const gitboard::json_value& folder :
       explicit_parsed.object["folders"].array) {
    if (gitboard::json_string_member(folder, "path") == "tasks/") {
      explicit_tasks_access = gitboard::json_string_member(folder, "access");
    }
  }
  expect(explicit_tasks_access == "forbidden",
         "explicit tasks folder access should be preserved");
  fs::remove_all(repo);
}

}  // namespace

int main() {
  try {
    test_default_ai_prompt_supports_code_review();
    test_ai_config_round_trip_redacts_key();
    test_repo_file_access_round_trip();
    test_repo_file_access_defaults_include_tasks();
  } catch (const std::exception& ex) {
    std::cerr << "server_ai_config_test: " << ex.what() << "\n";
    return 1;
  }
  std::cout << "server AI config tests passed\n";
  return 0;
}
