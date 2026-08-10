#include "src/server/app.h"

#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shobjidl.h>
#endif

#include "src/core/error.h"
#include "src/core/json.h"
#include "src/core/strings.h"
#include "src/server/ai_agent.h"
#include "src/server/assets.h"
#include "src/server/config.h"
#include "src/server/engine.h"
#include "src/server/http.h"
#include "src/server/process.h"

namespace fs = std::filesystem;

namespace gitboard::server {

std::mutex& api_request_mutex() {
  static std::mutex mutex;
  return mutex;
}

std::optional<std::string> request_header(const http_request& request,
                                          const std::string& name) {
  auto it = request.headers.find(gitboard::lower_ascii(name));
  if (it == request.headers.end()) return std::nullopt;
  return it->second;
}

bool is_mutating_method(const std::string& method) {
  return method == "POST" || method == "PUT" || method == "PATCH" ||
         method == "DELETE";
}

bool is_json_content_type(std::string value) {
  value = gitboard::lower_ascii(gitboard::trim(value));
  return value == "application/json" ||
         gitboard::starts_with(value, "application/json;");
}

bool is_loopback_origin(std::string origin) {
  origin = gitboard::lower_ascii(gitboard::trim(origin));
  return gitboard::starts_with(origin, "http://127.0.0.1:") ||
         gitboard::starts_with(origin, "http://localhost:") ||
         gitboard::starts_with(origin, "http://[::1]:");
}

std::optional<http_response> reject_unsafe_api_request(
    const http_request& request, const std::string& api_token) {
  auto token = request_header(request, "x-barnaby-token");
  if (!token || *token != api_token) {
    return json_response(403, error_json("missing or invalid API token"));
  }

  if (!is_mutating_method(request.method)) return std::nullopt;

  auto content_type = request_header(request, "content-type");
  if (!content_type || !is_json_content_type(*content_type)) {
    return json_response(415, error_json("API requests must use application/json"));
  }

  if (auto origin = request_header(request, "origin")) {
    if (!is_loopback_origin(*origin)) {
      return json_response(403, error_json("cross-origin API request rejected"));
    }
  }

  if (auto fetch_site = request_header(request, "sec-fetch-site")) {
    std::string site = gitboard::lower_ascii(gitboard::trim(*fetch_site));
    if (site == "cross-site") {
      return json_response(403, error_json("cross-site API request rejected"));
    }
  }

  return std::nullopt;
}

std::string percent_decode(std::string_view input) {
  std::string out;
  for (std::size_t i = 0; i < input.size(); ++i) {
    if (input[i] == '%' && i + 2 < input.size()) {
      auto hex = input.substr(i + 1, 2);
      char* end = nullptr;
      std::string token(hex);
      long value = std::strtol(token.c_str(), &end, 16);
      if (end && *end == '\0') {
        out.push_back(static_cast<char>(value));
        i += 2;
        continue;
      }
    }
    out.push_back(input[i]);
  }
  return out;
}

std::string selected_directory_json(const fs::path& path) {
  return "{\n  \"ok\": true,\n  \"cancelled\": false,\n  \"path\": " +
         json_quote(path.string()) + "\n}\n";
}

std::string cancelled_directory_json() {
  return "{\n  \"ok\": true,\n  \"cancelled\": true\n}\n";
}

std::string trim_process_output(std::string output) {
  while (!output.empty() &&
         (output.back() == '\n' || output.back() == '\r')) {
    output.pop_back();
  }
  return output;
}

#if defined(_WIN32)
template <typename T>
class com_ptr {
 public:
  com_ptr() = default;
  ~com_ptr() {
    if (ptr_) ptr_->Release();
  }

  com_ptr(const com_ptr&) = delete;
  com_ptr& operator=(const com_ptr&) = delete;

  T** put() { return &ptr_; }
  T* get() const { return ptr_; }
  T* operator->() const { return ptr_; }

 private:
  T* ptr_ = nullptr;
};

class scoped_com {
 public:
  scoped_com() {
    HRESULT hr =
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (hr == RPC_E_CHANGED_MODE) return;
    if (FAILED(hr)) throw error("could not initialize Windows folder dialog");
    initialized_ = true;
  }

  ~scoped_com() {
    if (initialized_) CoUninitialize();
  }

  scoped_com(const scoped_com&) = delete;
  scoped_com& operator=(const scoped_com&) = delete;

 private:
  bool initialized_ = false;
};
#endif

std::optional<fs::path> choose_directory_with_native_dialog() {
#if defined(__APPLE__)
  process_result result = run_process(
      "osascript",
      {"-e",
       "POSIX path of (choose folder with prompt "
       "\"Choose a Git repository folder for Barnaby\")"});
  if (result.exit_code != 0) {
    std::string output = gitboard::lower_ascii(result.output);
    if (output.find("user canceled") != std::string::npos ||
        output.find("cancelled") != std::string::npos) {
      return std::nullopt;
    }
    throw error(result.output.empty()
                    ? "folder selection dialog failed"
                    : gitboard::trim(result.output));
  }
  std::string path = trim_process_output(result.output);
  if (path.empty()) return std::nullopt;
  return fs::path(path);
#elif defined(_WIN32)
  scoped_com com;
  com_ptr<IFileOpenDialog> dialog;
  HRESULT hr =
      CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                       IID_PPV_ARGS(dialog.put()));
  if (FAILED(hr)) throw error("could not create Windows folder dialog");

  DWORD options = 0;
  hr = dialog->GetOptions(&options);
  if (FAILED(hr)) throw error("could not read Windows folder dialog options");
  hr = dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM |
                          FOS_PATHMUSTEXIST);
  if (FAILED(hr)) throw error("could not configure Windows folder dialog");
  dialog->SetTitle(L"Choose a Git repository folder for Barnaby");

  hr = dialog->Show(nullptr);
  if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return std::nullopt;
  if (FAILED(hr)) throw error("Windows folder selection dialog failed");

  com_ptr<IShellItem> item;
  hr = dialog->GetResult(item.put());
  if (FAILED(hr)) throw error("could not read Windows folder selection");

  PWSTR selected_path = nullptr;
  hr = item->GetDisplayName(SIGDN_FILESYSPATH, &selected_path);
  if (FAILED(hr) || !selected_path) {
    throw error("could not read selected Windows folder path");
  }

  std::wstring selected_text(selected_path);
  CoTaskMemFree(selected_path);
  return fs::path(selected_text);
#else
  std::vector<std::pair<std::string, std::vector<std::string>>> pickers = {
      {"zenity",
       {"--file-selection", "--directory", "--title",
        "Choose a Git repository folder for Barnaby"}},
      {"kdialog", {"--getexistingdirectory", ".", "Choose a Git repository folder for Barnaby"}},
  };
  std::string last_error;
  for (const auto& [executable, args] : pickers) {
    process_result result = run_process(executable, args);
    std::string path = trim_process_output(result.output);
    if (result.exit_code == 0 && !path.empty()) {
      return fs::path(path);
    }
    if (result.exit_code == 1 && path.empty()) {
      return std::nullopt;
    }
    if (!gitboard::trim(result.output).empty()) {
      last_error = gitboard::trim(result.output);
    }
  }
  throw error(last_error.empty()
                  ? "no supported folder selection dialog is available"
                  : last_error);
#endif
}

fs::path find_gitboard_path(const fs::path& explicit_path, char* argv0) {
  if (!explicit_path.empty()) return explicit_path;
  fs::path self = fs::absolute(argv0).parent_path();
  fs::path sibling = self / "gitboard";
  if (fs::exists(sibling)) return sibling;
  return "gitboard";
}

http_response handle_add_repo(const http_request& request) {
  json_value body = require_object_body(request);
  auto id_it = body.object.find("id");
  auto path_it = body.object.find("path");
  if (id_it == body.object.end() || path_it == body.object.end() ||
      id_it->second.type != json_value::k_string ||
      path_it->second.type != json_value::k_string) {
    return json_response(400, error_json("id and path are required strings"));
  }
  const std::string id = id_it->second.string;
  if (!valid_repo_id(id)) {
    return json_response(400, error_json("repository id must match [a-z0-9_]+"));
  }
  const fs::path repo_path = validate_repository_path(path_it->second.string);

  server_config config = load_config();
  if (config.repositories.count(id)) {
    return json_response(409, error_json("repository id already exists"));
  }
  config.repositories[id] = repo_path;
  save_config(config);
  return json_response(201, "{\n  \"ok\": true\n}\n");
}

http_response handle_select_repo_path() {
  try {
    std::optional<fs::path> selected = choose_directory_with_native_dialog();
    if (!selected) {
      return json_response(200, cancelled_directory_json());
    }
    fs::path repo_path = validate_repository_path(*selected);
    return json_response(200, selected_directory_json(repo_path));
  } catch (const std::exception& ex) {
    return json_response(400, error_json(ex.what()));
  }
}

http_response handle_delete_repo(const http_request& request) {
  constexpr std::string_view prefix = "/api/repos/";
  std::string id = percent_decode(std::string_view(request.path).substr(prefix.size()));
  if (!valid_repo_id(id)) return json_response(400, error_json("invalid repository id"));
  server_config config = load_config();
  if (!config.repositories.erase(id)) {
    return json_response(404, error_json("repository id not found"));
  }
  config.repository_ai.erase(id);
  save_config(config);
  return json_response(200, "{\n  \"ok\": true\n}\n");
}

std::optional<std::string> repo_id_from_file_access_path(const std::string& path) {
  constexpr std::string_view prefix = "/api/repos/";
  constexpr std::string_view suffix = "/ai/file-access";
  if (!gitboard::starts_with(path, prefix)) return std::nullopt;
  if (path.size() < prefix.size() + suffix.size()) return std::nullopt;
  if (path.compare(path.size() - suffix.size(), suffix.size(), suffix) != 0) {
    return std::nullopt;
  }
  std::string encoded =
      path.substr(prefix.size(), path.size() - prefix.size() - suffix.size());
  return percent_decode(encoded);
}

http_response handle_get_repo_file_access(const http_request& request) {
  auto id = repo_id_from_file_access_path(request.path);
  if (!id || !valid_repo_id(*id)) {
    return json_response(400, error_json("invalid repository id"));
  }
  server_config config = load_config();
  auto repo = config.repositories.find(*id);
  if (repo == config.repositories.end()) {
    return json_response(404, error_json("repository id not found"));
  }
  return json_response(
      200, repo_file_access_response_json(*id, repo->second, config.repository_ai[*id]));
}

http_response handle_put_repo_file_access(const http_request& request) {
  auto id = repo_id_from_file_access_path(request.path);
  if (!id || !valid_repo_id(*id)) {
    return json_response(400, error_json("invalid repository id"));
  }
  json_value body = require_object_body(request);
  server_config config = load_config();
  auto repo = config.repositories.find(*id);
  if (repo == config.repositories.end()) {
    return json_response(404, error_json("repository id not found"));
  }
  try {
    config.repository_ai[*id] =
        parse_repo_ai_config_update(body, config.repository_ai[*id]);
  } catch (const std::exception& ex) {
    return json_response(400, error_json(ex.what()));
  }
  save_config(config);
  return json_response(
      200, repo_file_access_response_json(*id, repo->second, config.repository_ai[*id]));
}
http_response handle_api_request(const http_request& request,
                                 const fs::path& gitboard_path) {
  if (request.path == "/api/config" && request.method == "GET") {
    return json_response(200, config_response_json(load_config()));
  }
  if (request.path == "/api/ai/config" && request.method == "GET") {
    return handle_get_ai_config();
  }
  if (request.path == "/api/ai/config" && request.method == "PUT") {
    return handle_put_ai_config(request);
  }
  if (request.path == "/api/ai/test" && request.method == "POST") {
    return handle_ai_test();
  }
  if (request.path == "/api/agent/chat" && request.method == "POST") {
    return handle_agent_chat(request, gitboard_path);
  }
  if (request.path == "/api/agent/actions/apply" && request.method == "POST") {
    return handle_agent_apply_actions(request, gitboard_path);
  }
  if (request.path == "/api/agent/summarize" && request.method == "POST") {
    return handle_agent_summarize(request);
  }
  if (request.path == "/api/agent/files/allowed" && request.method == "POST") {
    return handle_agent_file_allowed(request);
  }
  if (request.path == "/api/agent/files/list" && request.method == "POST") {
    return handle_agent_file_list(request);
  }
  if (request.path == "/api/agent/files/read" && request.method == "POST") {
    return handle_agent_file_read(request);
  }
  if (request.path == "/api/agent/files/read-range" && request.method == "POST") {
    return handle_agent_file_read_range(request);
  }
  if (request.path == "/api/agent/files/search" && request.method == "POST") {
    return handle_agent_file_search(request);
  }
  if (request.path == "/api/agent/files/write" && request.method == "POST") {
    return handle_agent_file_write(request);
  }
  if (request.path == "/api/repos" && request.method == "POST") {
    return handle_add_repo(request);
  }
  if (request.path == "/api/repos/select-path" && request.method == "POST") {
    return handle_select_repo_path();
  }
  if (repo_id_from_file_access_path(request.path) && request.method == "GET") {
    return handle_get_repo_file_access(request);
  }
  if (repo_id_from_file_access_path(request.path) && request.method == "PUT") {
    return handle_put_repo_file_access(request);
  }
  if (gitboard::starts_with(request.path, "/api/repos/") &&
      request.method == "DELETE") {
    return handle_delete_repo(request);
  }
  if (request.path == "/batch" && request.method == "POST") {
    return handle_batch(request, gitboard_path);
  }
  return json_response(404, error_json("not found"));
}

bool is_api_request_path(const std::string& path) {
  return path == "/batch" || gitboard::starts_with(path, "/api/");
}

http_response handle_request(const http_request& request,
                             const fs::path& gitboard_path,
                             const std::string& api_token) {
  if (const asset* item = find_asset(request.path)) {
    if (request.method != "GET") return json_response(405, error_json("method not allowed"));
    return http_response{200, std::string(item->content_type),
                         std::string(reinterpret_cast<const char*>(item->content),
                                     item->content_length)};
  }

  if (is_api_request_path(request.path)) {
    if (auto rejected = reject_unsafe_api_request(request, api_token)) {
      return *rejected;
    }
    std::lock_guard<std::mutex> lock(api_request_mutex());
    return handle_api_request(request, gitboard_path);
  }

  return json_response(404, error_json("not found"));
}


}  // namespace gitboard::server
