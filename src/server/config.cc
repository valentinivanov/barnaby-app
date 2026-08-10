#include "src/server/config.h"

#include <cstdlib>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <vector>

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#endif
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincred.h>
#endif
#if defined(__linux__)
#include <libsecret/secret.h>
#endif

#include "src/core/error.h"
#include "src/core/filesystem.h"
#include "src/core/json.h"
#include "src/core/strings.h"
#include "src/server/process.h"

namespace fs = std::filesystem;

namespace gitboard::server {
namespace {

constexpr int kMaxAiTimeoutSeconds = 3600;
constexpr int kMaxAiRetryAttempts = 20;

fs::path&
config_dir_override()
{
  static fs::path path;
  return path;
}

fs::path
home_dir()
{
  const char* home = std::getenv("HOME");
  if (!home || std::string(home).empty())
    return ".";
  return home;
}

std::string
default_ai_api_key_ref()
{
#if defined(__APPLE__)
  return "macos-keychain";
#elif defined(__linux__)
  return "linux-secret-service";
#elif defined(_WIN32)
  return "windows-credential-manager";
#else
  return "unsupported";
#endif
}

constexpr std::string_view kLegacyDefaultAiSystemPrompt =
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

constexpr std::string_view kDefaultAiSystemPrompt =
  "You are Agent Pip, Barnaby's repository task assistant.\n"
  "You are an experienced software developer, code reviewer, scrum master, "
  "and project manager. Users may ask you for help with task management, "
  "project planning, implementation analysis, and code review in the "
  "current repository or broader project situations.\n"
  "\n"
  "Use Barnaby task context as the source of truth for repository-specific "
  "claims. For repository code review requests, inspect allowed project "
  "files with Barnaby file tools when needed, focus on correctness, "
  "regressions, maintainability, security, and missing tests, and cite "
  "concrete files or symbols when the information comes from repository "
  "state. For general software engineering, planning, agile, and process "
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

std::string
quote_json(std::string_view s)
{
  return "\"" + gitboard::json_escape(s) + "\"";
}

std::string
normalized_policy_path(std::string path)
{
  path = gitboard::trim(path);
  while (!path.empty() && path.front() == '/')
    path.erase(path.begin());
  fs::path normalized = fs::path(path).lexically_normal();
  std::string out = normalized.generic_string();
  if (out == ".")
    out.clear();
  if (!out.empty() && out.back() != '/')
    out.push_back('/');
  return out;
}

bool
valid_policy_path(const std::string& path)
{
  if (path.empty() || path.front() == '/')
    return false;
  if (path == ".git/" || gitboard::starts_with(path, ".git/"))
    return false;
  std::string checked = path;
  while (!checked.empty() && checked.back() == '/')
    checked.pop_back();
  if (checked.empty())
    return false;
  fs::path parsed(checked);
  for (const auto& part : parsed) {
    std::string token = part.string();
    if (token.empty() || token == "." || token == "..")
      return false;
  }
  return true;
}

std::vector<file_access_rule>
parse_file_access_rules(const json_value& value)
{
  if (value.type != json_value::k_array) {
    throw error("fileAccess must be an array");
  }
  std::vector<file_access_rule> rules;
  std::set<std::string> seen;
  for (const json_value& item : value.array) {
    if (item.type != json_value::k_object) {
      throw error("fileAccess entries must be objects");
    }
    auto path_it = item.object.find("path");
    auto access_it = item.object.find("access");
    if (path_it == item.object.end() ||
        path_it->second.type != json_value::k_string ||
        access_it == item.object.end() ||
        access_it->second.type != json_value::k_string) {
      throw error("fileAccess entries require path and access strings");
    }
    std::string path = normalized_policy_path(path_it->second.string);
    if (!valid_policy_path(path)) {
      throw error("invalid file access path: " + path_it->second.string);
    }
    std::string access = gitboard::trim(access_it->second.string);
    if (!valid_file_access_policy(access)) {
      throw error("invalid file access policy: " + access);
    }
    if (!seen.insert(path).second) {
      throw error("duplicate file access path: " + path);
    }
    rules.push_back(file_access_rule{ path, access });
  }
  return rules;
}

void
write_file_access_rules(std::ostream& out,
                        const std::vector<file_access_rule>& rules,
                        std::string_view indent)
{
  out << "[";
  bool first = true;
  for (const auto& rule : rules) {
    if (!first)
      out << ",";
    first = false;
    out << "\n"
        << indent << "  {\"path\": " << quote_json(rule.path)
        << ", \"access\": " << quote_json(rule.access) << "}";
  }
  if (!rules.empty())
    out << "\n" << indent;
  out << "]";
}

std::string
default_file_access_for_path(const std::string& path)
{
  if (path == "tasks/")
    return "full_access";
  return "forbidden";
}

#if defined(__APPLE__) || defined(__linux__) || defined(_WIN32)

std::map<std::string, std::string>&
ai_api_key_cache()
{
  static std::map<std::string, std::string> cache;
  return cache;
}

std::mutex&
ai_api_key_cache_mutex()
{
  static std::mutex mutex;
  return mutex;
}

std::optional<std::string>
cached_ai_api_key(const std::string& ref)
{
  std::lock_guard<std::mutex> lock(ai_api_key_cache_mutex());
  auto it = ai_api_key_cache().find(ref);
  if (it == ai_api_key_cache().end())
    return std::nullopt;
  return it->second;
}

void
cache_ai_api_key(const std::string& ref, const std::string& api_key)
{
  std::lock_guard<std::mutex> lock(ai_api_key_cache_mutex());
  ai_api_key_cache()[ref] = api_key;
}

void
erase_cached_ai_api_key(const std::string& ref)
{
  std::lock_guard<std::mutex> lock(ai_api_key_cache_mutex());
  ai_api_key_cache().erase(ref);
}

#endif

#if defined(_WIN32)

constexpr const char* kWindowsCredentialManagerRefPrefix =
  "windows-credential-manager:";

std::string
windows_credential_account_from_ref(const std::string& ref)
{
  if (gitboard::starts_with(ref, kWindowsCredentialManagerRefPrefix)) {
    return ref.substr(std::string(kWindowsCredentialManagerRefPrefix).size());
  }
  return ref;
}

std::wstring
widen(std::string_view value)
{
  if (value.empty())
    return L"";
  int size = MultiByteToWideChar(CP_UTF8,
                                 0,
                                 value.data(),
                                 static_cast<int>(value.size()),
                                 nullptr,
                                 0);
  if (size <= 0)
    throw error("failed to convert UTF-8 text to Windows string");
  std::wstring out(static_cast<std::size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8,
                      0,
                      value.data(),
                      static_cast<int>(value.size()),
                      out.data(),
                      size);
  return out;
}

std::wstring
windows_credential_target_name(const std::string& ref)
{
  return L"dev.gitboard.barnaby.ai:" +
         widen(windows_credential_account_from_ref(ref));
}

std::string
windows_credential_error_message(DWORD code, std::string_view action)
{
  std::ostringstream out;
  out << "failed to " << action
      << " AI API key in Windows Credential Manager: " << code;
  return out.str();
}

#endif

#if defined(__APPLE__)

constexpr const char* kAiKeychainService = "dev.gitboard.barnaby.ai";
constexpr const char* kAiKeychainRefPrefix = "macos-keychain:";
constexpr OSStatus kNoDefaultKeychainStatus = -60006;
constexpr OSStatus kNoSuchKeychainStatus = -25307;
constexpr OSStatus kInteractionNotAllowedStatus = -25308;

class cf_ref
{
public:
  explicit cf_ref(CFTypeRef value = nullptr)
    : value_(value)
  {}
  ~cf_ref()
  {
    if (value_)
      CFRelease(value_);
  }

  cf_ref(const cf_ref&) = delete;
  cf_ref& operator=(const cf_ref&) = delete;

  CFTypeRef get() const { return value_; }

private:
  CFTypeRef value_ = nullptr;
};

CFStringRef
make_cf_string(std::string_view value)
{
  return CFStringCreateWithBytes(kCFAllocatorDefault,
                                 reinterpret_cast<const UInt8*>(value.data()),
                                 static_cast<CFIndex>(value.size()),
                                 kCFStringEncodingUTF8,
                                 false);
}

void
add_dictionary_value(CFMutableDictionaryRef dictionary,
                     const void* key,
                     CFTypeRef value)
{
  CFDictionarySetValue(dictionary, key, value);
}

std::string
ai_keychain_account_from_ref(const std::string& ref)
{
  if (gitboard::starts_with(ref, kAiKeychainRefPrefix)) {
    return ref.substr(std::string(kAiKeychainRefPrefix).size());
  }
  return ref;
}

cf_ref
ai_keychain_base_query(const std::string& ref)
{
  CFMutableDictionaryRef query =
    CFDictionaryCreateMutable(kCFAllocatorDefault,
                              0,
                              &kCFTypeDictionaryKeyCallBacks,
                              &kCFTypeDictionaryValueCallBacks);
  if (!query)
    throw error("failed to allocate Keychain query");
  cf_ref service(make_cf_string(kAiKeychainService));
  cf_ref account(make_cf_string(ai_keychain_account_from_ref(ref)));
  add_dictionary_value(query, kSecClass, kSecClassGenericPassword);
  add_dictionary_value(query, kSecAttrService, service.get());
  add_dictionary_value(query, kSecAttrAccount, account.get());
  return cf_ref(query);
}

std::string
keychain_status_message(OSStatus status, std::string_view action)
{
  std::ostringstream out;
  out << "failed to " << action << " AI API key in macOS Keychain: " << status;
  return out.str();
}

bool
macos_keychain_available()
{
  SecKeychainRef keychain = nullptr;
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
  OSStatus status = SecKeychainCopyDefault(&keychain);
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
  if (status == errSecSuccess) {
    if (keychain)
      CFRelease(keychain);
    return true;
  }
  if (status == kNoDefaultKeychainStatus || status == kNoSuchKeychainStatus ||
      status == kInteractionNotAllowedStatus) {
    return false;
  }
  throw error(keychain_status_message(status, "access"));
}

#endif

#if defined(__linux__)

constexpr const char* kLinuxSecretServiceRefPrefix = "linux-secret-service:";

const SecretSchema*
linux_ai_secret_schema()
{
  static const SecretSchema schema = [] {
    SecretSchema value{};
    value.name = "dev.gitboard.barnaby.ai";
    value.flags = SECRET_SCHEMA_NONE;
    value.attributes[0].name = "account";
    value.attributes[0].type = SECRET_SCHEMA_ATTRIBUTE_STRING;
    return value;
  }();
  return &schema;
}

std::string
linux_secret_account_from_ref(const std::string& ref)
{
  if (gitboard::starts_with(ref, kLinuxSecretServiceRefPrefix)) {
    return ref.substr(std::string(kLinuxSecretServiceRefPrefix).size());
  }
  return ref;
}

std::string
linux_secret_error_message(GError* error, std::string_view action)
{
  std::ostringstream out;
  out << "failed to " << action << " AI API key in Linux Secret Service";
  if (error && error->message)
    out << ": " << error->message;
  return out.str();
}

bool
linux_secret_service_available()
{
  GError* error = nullptr;
  SecretService* service =
    secret_service_get_sync(SECRET_SERVICE_NONE, nullptr, &error);
  if (error) {
    g_error_free(error);
    return false;
  }
  if (!service)
    return false;
  g_object_unref(service);
  return true;
}

#endif

} // namespace

fs::path
config_dir()
{
  if (!config_dir_override().empty())
    return config_dir_override();
#if defined(__APPLE__)
  return home_dir() / "Library" / "Application Support" / "gitboard-server";
#elif defined(_WIN32)
  const char* appdata = std::getenv("APPDATA");
  if (appdata && std::string(appdata).size() > 0) {
    return fs::path(appdata) / "gitboard-server";
  }
  return home_dir() / ".gitboard-server";
#else
  const char* xdg = std::getenv("XDG_CONFIG_HOME");
  if (xdg && std::string(xdg).size() > 0) {
    return fs::path(xdg) / "gitboard-server";
  }
  return home_dir() / ".config" / "gitboard-server";
#endif
}

void
set_config_dir_for_process(fs::path path)
{
  config_dir_override() = fs::absolute(std::move(path)).lexically_normal();
}

fs::path
config_path()
{
  return config_dir() / "config.json";
}

std::string
default_ai_system_prompt()
{
  return std::string(kDefaultAiSystemPrompt);
}

std::string
normalized_ai_system_prompt(std::string prompt)
{
  prompt = gitboard::trim(prompt);
  if (prompt.empty() || prompt == kLegacyDefaultAiSystemPrompt) {
    return default_ai_system_prompt();
  }
  return prompt;
}

bool
valid_repo_id(const std::string& id)
{
  if (id.empty())
    return false;
  for (unsigned char c : id) {
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) {
      return false;
    }
  }
  return true;
}

server_config
load_config()
{
  server_config config;
  fs::path path = config_path();
  if (!fs::exists(path))
    return config;
  json_value root = json_parser(read_file(path)).parse();
  if (root.type != json_value::k_object)
    throw error("invalid config root");
  auto repos_it = root.object.find("repositories");
  if (repos_it != root.object.end() &&
      repos_it->second.type != json_value::k_object) {
    throw error("invalid config repositories");
  }
  if (repos_it != root.object.end()) {
    for (const auto& [id, value] : repos_it->second.object) {
      if (!valid_repo_id(id))
        throw error("invalid repository id in config: " + id);
      if (value.type == json_value::k_string) {
        config.repositories[id] = value.string;
      } else if (value.type == json_value::k_object) {
        auto path_it = value.object.find("path");
        if (path_it == value.object.end() ||
            path_it->second.type != json_value::k_string) {
          throw error("invalid repository path in config: " + id);
        }
        config.repositories[id] = path_it->second.string;
        auto ai_it = value.object.find("ai");
        if (ai_it != value.object.end()) {
          config.repository_ai[id] = parse_repo_ai_config_update(
            ai_it->second, config.repository_ai[id]);
        }
      } else {
        throw error("invalid repository path in config: " + id);
      }
    }
  }
  auto repo_ai_it = root.object.find("repositoryAi");
  if (repo_ai_it != root.object.end()) {
    if (repo_ai_it->second.type != json_value::k_object) {
      throw error("invalid config repositoryAi");
    }
    for (const auto& [id, value] : repo_ai_it->second.object) {
      if (!valid_repo_id(id)) {
        throw error("invalid repository id in repositoryAi config: " + id);
      }
      config.repository_ai[id] =
        parse_repo_ai_config_update(value, config.repository_ai[id]);
    }
  }
  auto ai_it = root.object.find("ai");
  if (ai_it != root.object.end()) {
    if (ai_it->second.type != json_value::k_object) {
      throw error("invalid config ai");
    }
    config.ai = parse_ai_config_update(ai_it->second, config.ai);
  }
  return config;
}

void
save_config(const server_config& config)
{
  std::ostringstream out;
  out << "{\n  \"repositories\": {";
  bool first = true;
  for (const auto& [id, path] : config.repositories) {
    if (!first)
      out << ",";
    first = false;
    out << "\n    " << quote_json(id) << ": " << quote_json(path.string());
  }
  if (!config.repositories.empty())
    out << "\n  ";
  out << "},\n";
  out << "  \"repositoryAi\": {";
  first = true;
  for (const auto& [id, repo_ai] : config.repository_ai) {
    if (!config.repositories.count(id))
      continue;
    if (!first)
      out << ",";
    first = false;
    out << "\n    " << quote_json(id) << ": {\n";
    out << "      \"fileAccess\": ";
    write_file_access_rules(out, repo_ai.file_access, "      ");
    out << "\n    }";
  }
  if (!first)
    out << "\n  ";
  out << "},\n";
  out << "  \"ai\": {\n";
  out << "    \"enabled\": " << (config.ai.enabled ? "true" : "false") << ",\n";
  out << "    \"provider\": " << quote_json(config.ai.provider) << ",\n";
  out << "    \"baseUrl\": " << quote_json(config.ai.base_url) << ",\n";
  out << "    \"model\": " << quote_json(config.ai.model) << ",\n";
  out << "    \"apiKeyRef\": " << quote_json(config.ai.api_key_ref) << ",\n";
  out << "    \"timeoutSeconds\": " << config.ai.timeout_seconds << ",\n";
  out << "    \"retryAttempts\": " << config.ai.retry_attempts << ",\n";
  out << "    \"maxOutputTokens\": " << config.ai.max_output_tokens << ",\n";
  out << "    \"allowWrites\": " << (config.ai.allow_writes ? "true" : "false")
      << ",\n";
  out << "    \"systemPrompt\": "
      << quote_json(normalized_ai_system_prompt(config.ai.system_prompt))
      << "\n";
  out << "  }\n}\n";
  write_file_atomic(config_path(), out.str());
}

fs::path
validate_repository_path(const fs::path& path)
{
  fs::path absolute = fs::absolute(path).lexically_normal();
  std::error_code ec;
  if (!fs::exists(absolute, ec)) {
    if (ec)
      throw error("repository path is inaccessible: " + ec.message());
    throw error("repository path does not exist");
  }
  if (!fs::is_directory(absolute, ec)) {
    if (ec)
      throw error("repository path is inaccessible: " + ec.message());
    throw error("repository path is not a directory");
  }
  fs::path canonical = fs::canonical(absolute, ec);
  if (ec)
    throw error("repository path is inaccessible: " + ec.message());
  process_result result = run_process(
    "git", { "-C", canonical.string(), "rev-parse", "--is-inside-work-tree" });
  if (result.exit_code != 0 ||
      gitboard::trim(result.output).find("true") == std::string::npos) {
    throw error("repository path is not inside a Git work tree");
  }
  return canonical;
}

void
validate_repository(const std::string& id, const fs::path& path)
{
  if (!valid_repo_id(id)) {
    throw error("repository id must match [a-z0-9_]+");
  }
  (void)validate_repository_path(path);
}

std::string
config_response_json(const server_config& config)
{
  std::ostringstream out;
  out << "{\n  \"ok\": true,\n  \"repositories\": {";
  bool first = true;
  for (const auto& [id, path] : config.repositories) {
    if (!first)
      out << ",";
    first = false;
    out << "\n    " << quote_json(id) << ": " << quote_json(path.string());
  }
  if (!config.repositories.empty())
    out << "\n  ";
  out << "}\n}\n";
  return out.str();
}

bool
valid_file_access_policy(const std::string& access)
{
  return access == "forbidden" || access == "read_only" ||
         access == "full_access";
}

repo_ai_config
parse_repo_ai_config_update(const json_value& body,
                            const repo_ai_config& existing)
{
  if (body.type != json_value::k_object) {
    throw error("repository AI config body must be an object");
  }
  repo_ai_config config = existing;
  for (const auto& [key, value] : body.object) {
    if (key == "fileAccess" || key == "folders") {
      config.file_access = parse_file_access_rules(value);
    }
  }
  return config;
}

std::string
repo_file_access_response_json(const std::string& repo_id,
                               const fs::path& repo_path,
                               const repo_ai_config& config)
{
  std::map<std::string, std::string> configured;
  for (const auto& rule : config.file_access) {
    configured[rule.path] = rule.access;
  }

  std::ostringstream out;
  out << "{\n";
  out << "  \"ok\": true,\n";
  out << "  \"repoId\": " << quote_json(repo_id) << ",\n";
  out << "  \"repoPath\": " << quote_json(repo_path.string()) << ",\n";
  out << "  \"folders\": [";
  bool first = true;
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(repo_path, ec)) {
    if (ec)
      break;
    if (!entry.is_directory(ec) || ec) {
      ec.clear();
      continue;
    }
    std::string name = entry.path().filename().string();
    if (name.empty())
      continue;
    if (name == ".git")
      continue;
    std::string path = name + "/";
    if (!first)
      out << ",";
    first = false;
    auto access_it = configured.find(path);
    std::string access =
      access_it == configured.end() ? default_file_access_for_path(path)
                                    : access_it->second;
    out << "\n    {\"path\": " << quote_json(path)
        << ", \"access\": " << quote_json(access) << "}";
  }
  if (!first)
    out << "\n  ";
  out << "],\n";
  out << "  \"fileAccess\": ";
  write_file_access_rules(out, config.file_access, "  ");
  out << "\n}\n";
  return out.str();
}

std::string
ai_api_key_ref(const ai_config& config)
{
  std::string base_url = gitboard::trim(config.base_url);
  std::string model = gitboard::trim(config.model);
#if defined(__APPLE__)
  if (!base_url.empty() && !model.empty()) {
    return std::string("macos-keychain:") + model + "@" + base_url;
  }
  if (!base_url.empty())
    return std::string("macos-keychain:") + base_url;
#elif defined(__linux__)
  if (!base_url.empty() && !model.empty()) {
    return std::string(kLinuxSecretServiceRefPrefix) + model + "@" + base_url;
  }
  if (!base_url.empty())
    return std::string(kLinuxSecretServiceRefPrefix) + base_url;
#elif defined(_WIN32)
  if (!base_url.empty() && !model.empty()) {
    return std::string(kWindowsCredentialManagerRefPrefix) + model + "@" +
           base_url;
  }
  if (!base_url.empty())
    return std::string(kWindowsCredentialManagerRefPrefix) + base_url;
#else
  (void)config;
#endif
  return default_ai_api_key_ref();
}

bool
ai_api_key_configured(const ai_config& config)
{
#if defined(__APPLE__)
  std::string ref =
    config.api_key_ref.empty() ? ai_api_key_ref(config) : config.api_key_ref;
  if (!gitboard::starts_with(ref, "macos-keychain:"))
    return false;
  if (cached_ai_api_key(ref))
    return true;
  if (!macos_keychain_available())
    return false;
  cf_ref query = ai_keychain_base_query(ref);
  CFMutableDictionaryRef dictionary =
    static_cast<CFMutableDictionaryRef>(const_cast<void*>(query.get()));
  add_dictionary_value(dictionary, kSecMatchLimit, kSecMatchLimitOne);
  OSStatus status = SecItemCopyMatching(dictionary, nullptr);
  if (status == errSecSuccess)
    return true;
  if (status == errSecItemNotFound)
    return false;
  throw error(keychain_status_message(status, "read"));
#elif defined(__linux__)
  std::string ref =
    config.api_key_ref.empty() ? ai_api_key_ref(config) : config.api_key_ref;
  if (!gitboard::starts_with(ref, kLinuxSecretServiceRefPrefix))
    return false;
  if (cached_ai_api_key(ref))
    return true;
  if (!linux_secret_service_available())
    return false;
  GError* error = nullptr;
  gchar* secret =
    secret_password_lookup_sync(linux_ai_secret_schema(),
                                nullptr,
                                &error,
                                "account",
                                linux_secret_account_from_ref(ref).c_str(),
                                nullptr);
  if (error) {
    std::string message = linux_secret_error_message(error, "read");
    g_error_free(error);
    throw gitboard::error(message);
  }
  if (!secret)
    return false;
  secret_password_free(secret);
  return true;
#elif defined(_WIN32)
  std::string ref =
    config.api_key_ref.empty() ? ai_api_key_ref(config) : config.api_key_ref;
  if (!gitboard::starts_with(ref, kWindowsCredentialManagerRefPrefix))
    return false;
  if (cached_ai_api_key(ref))
    return true;
  PCREDENTIALW credential = nullptr;
  if (CredReadW(windows_credential_target_name(ref).c_str(),
                CRED_TYPE_GENERIC,
                0,
                &credential)) {
    CredFree(credential);
    return true;
  }
  DWORD code = GetLastError();
  if (code == ERROR_NOT_FOUND)
    return false;
  throw error(windows_credential_error_message(code, "read"));
#else
  (void)config;
  return false;
#endif
}

void
save_ai_api_key(const std::string& api_key, const ai_config& config)
{
#if defined(__APPLE__)
  std::string ref = ai_api_key_ref(config);
  if (!gitboard::starts_with(ref, "macos-keychain:")) {
    throw error(
      "AI endpoint URL and model are required before saving an API key");
  }
  if (!macos_keychain_available()) {
    throw error(
      "macOS default Keychain is not available for secure AI API key storage");
  }
  cf_ref query = ai_keychain_base_query(ref);
  cf_ref secret(CFDataCreate(kCFAllocatorDefault,
                             reinterpret_cast<const UInt8*>(api_key.data()),
                             static_cast<CFIndex>(api_key.size())));
  if (!secret.get())
    throw error("failed to allocate Keychain secret data");

  CFMutableDictionaryRef add_query =
    static_cast<CFMutableDictionaryRef>(const_cast<void*>(query.get()));
  add_dictionary_value(add_query, kSecValueData, secret.get());
  OSStatus status = SecItemAdd(add_query, nullptr);
  if (status == errSecDuplicateItem) {
    cf_ref update_query = ai_keychain_base_query(ref);
    CFMutableDictionaryRef attrs =
      CFDictionaryCreateMutable(kCFAllocatorDefault,
                                0,
                                &kCFTypeDictionaryKeyCallBacks,
                                &kCFTypeDictionaryValueCallBacks);
    if (!attrs)
      throw error("failed to allocate Keychain update attributes");
    cf_ref attrs_ref(attrs);
    add_dictionary_value(attrs, kSecValueData, secret.get());
    status =
      SecItemUpdate(static_cast<CFDictionaryRef>(update_query.get()), attrs);
  }
  if (status != errSecSuccess) {
    throw error(keychain_status_message(status, "save"));
  }
  cache_ai_api_key(ref, api_key);
#elif defined(__linux__)
  std::string ref = ai_api_key_ref(config);
  if (!gitboard::starts_with(ref, kLinuxSecretServiceRefPrefix)) {
    throw error(
      "AI endpoint URL and model are required before saving an API key");
  }
  if (!linux_secret_service_available()) {
    throw error(
      "Linux Secret Service is not available for secure AI API key storage");
  }
  GError* error = nullptr;
  gboolean saved =
    secret_password_store_sync(linux_ai_secret_schema(),
                               SECRET_COLLECTION_DEFAULT,
                               "Barnaby AI API key",
                               api_key.c_str(),
                               nullptr,
                               &error,
                               "account",
                               linux_secret_account_from_ref(ref).c_str(),
                               nullptr);
  if (!saved || error) {
    std::string message = linux_secret_error_message(error, "save");
    if (error)
      g_error_free(error);
    throw gitboard::error(message);
  }
  cache_ai_api_key(ref, api_key);
#elif defined(_WIN32)
  std::string ref = ai_api_key_ref(config);
  if (!gitboard::starts_with(ref, kWindowsCredentialManagerRefPrefix)) {
    throw error(
      "AI endpoint URL and model are required before saving an API key");
  }
  std::wstring target = windows_credential_target_name(ref);
  std::wstring user = L"Barnaby AI API key";
  std::vector<unsigned char> secret(api_key.begin(), api_key.end());
  CREDENTIALW credential{};
  credential.Type = CRED_TYPE_GENERIC;
  credential.TargetName = target.data();
  credential.UserName = user.data();
  credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
  credential.CredentialBlobSize = static_cast<DWORD>(secret.size());
  credential.CredentialBlob = secret.empty() ? nullptr : secret.data();
  if (!CredWriteW(&credential, 0)) {
    throw error(windows_credential_error_message(GetLastError(), "save"));
  }
  cache_ai_api_key(ref, api_key);
#else
  (void)api_key;
  (void)config;
  throw error(
    "secure AI API key storage is only implemented on macOS, Linux, and Windows");
#endif
}

std::string
load_ai_api_key(const ai_config& config)
{
#if defined(__APPLE__)
  std::string ref =
    config.api_key_ref.empty() ? ai_api_key_ref(config) : config.api_key_ref;
  if (!gitboard::starts_with(ref, "macos-keychain:")) {
    throw error("AI API key is not stored in macOS Keychain");
  }
  if (auto cached = cached_ai_api_key(ref))
    return *cached;
  if (!macos_keychain_available()) {
    throw error(
      "macOS default Keychain is not available for AI API key access");
  }
  cf_ref query = ai_keychain_base_query(ref);
  CFMutableDictionaryRef dictionary =
    static_cast<CFMutableDictionaryRef>(const_cast<void*>(query.get()));
  add_dictionary_value(dictionary, kSecMatchLimit, kSecMatchLimitOne);
  add_dictionary_value(dictionary, kSecReturnData, kCFBooleanTrue);
  CFTypeRef result = nullptr;
  OSStatus status = SecItemCopyMatching(dictionary, &result);
  if (status == errSecItemNotFound)
    throw error("AI API key is not configured");
  if (status != errSecSuccess)
    throw error(keychain_status_message(status, "read"));
  cf_ref result_ref(result);
  CFDataRef data = static_cast<CFDataRef>(result);
  const UInt8* bytes = CFDataGetBytePtr(data);
  CFIndex length = CFDataGetLength(data);
  std::string api_key(reinterpret_cast<const char*>(bytes),
                      static_cast<std::size_t>(length));
  cache_ai_api_key(ref, api_key);
  return api_key;
#elif defined(__linux__)
  std::string ref =
    config.api_key_ref.empty() ? ai_api_key_ref(config) : config.api_key_ref;
  if (!gitboard::starts_with(ref, kLinuxSecretServiceRefPrefix)) {
    throw error("AI API key is not stored in Linux Secret Service");
  }
  if (auto cached = cached_ai_api_key(ref))
    return *cached;
  if (!linux_secret_service_available()) {
    throw error(
      "Linux Secret Service is not available for AI API key access");
  }
  GError* error = nullptr;
  gchar* secret =
    secret_password_lookup_sync(linux_ai_secret_schema(),
                                nullptr,
                                &error,
                                "account",
                                linux_secret_account_from_ref(ref).c_str(),
                                nullptr);
  if (error) {
    std::string message = linux_secret_error_message(error, "read");
    g_error_free(error);
    throw gitboard::error(message);
  }
  if (!secret)
    throw gitboard::error("AI API key is not configured");
  std::string api_key(secret);
  secret_password_free(secret);
  cache_ai_api_key(ref, api_key);
  return api_key;
#elif defined(_WIN32)
  std::string ref =
    config.api_key_ref.empty() ? ai_api_key_ref(config) : config.api_key_ref;
  if (!gitboard::starts_with(ref, kWindowsCredentialManagerRefPrefix)) {
    throw error("AI API key is not stored in Windows Credential Manager");
  }
  if (auto cached = cached_ai_api_key(ref))
    return *cached;
  PCREDENTIALW credential = nullptr;
  if (!CredReadW(windows_credential_target_name(ref).c_str(),
                 CRED_TYPE_GENERIC,
                 0,
                 &credential)) {
    DWORD code = GetLastError();
    if (code == ERROR_NOT_FOUND)
      throw error("AI API key is not configured");
    throw error(windows_credential_error_message(code, "read"));
  }
  std::string api_key(
    reinterpret_cast<const char*>(credential->CredentialBlob),
    static_cast<std::size_t>(credential->CredentialBlobSize));
  CredFree(credential);
  cache_ai_api_key(ref, api_key);
  return api_key;
#else
  (void)config;
  throw error(
    "secure AI API key storage is only implemented on macOS, Linux, and Windows");
#endif
}

void
clear_ai_api_key(const ai_config& config)
{
#if defined(__APPLE__)
  std::string ref =
    config.api_key_ref.empty() ? ai_api_key_ref(config) : config.api_key_ref;
  if (!gitboard::starts_with(ref, "macos-keychain:"))
    return;
  erase_cached_ai_api_key(ref);
  if (!macos_keychain_available())
    return;
  cf_ref query = ai_keychain_base_query(ref);
  OSStatus status = SecItemDelete(static_cast<CFDictionaryRef>(query.get()));
  if (status != errSecSuccess && status != errSecItemNotFound) {
    throw error(keychain_status_message(status, "clear"));
  }
#elif defined(__linux__)
  std::string ref =
    config.api_key_ref.empty() ? ai_api_key_ref(config) : config.api_key_ref;
  if (!gitboard::starts_with(ref, kLinuxSecretServiceRefPrefix))
    return;
  erase_cached_ai_api_key(ref);
  if (!linux_secret_service_available())
    return;
  GError* error = nullptr;
  secret_password_clear_sync(linux_ai_secret_schema(),
                             nullptr,
                             &error,
                             "account",
                             linux_secret_account_from_ref(ref).c_str(),
                             nullptr);
  if (error) {
    std::string message = linux_secret_error_message(error, "clear");
    g_error_free(error);
    throw gitboard::error(message);
  }
#elif defined(_WIN32)
  std::string ref =
    config.api_key_ref.empty() ? ai_api_key_ref(config) : config.api_key_ref;
  if (!gitboard::starts_with(ref, kWindowsCredentialManagerRefPrefix))
    return;
  erase_cached_ai_api_key(ref);
  if (!CredDeleteW(windows_credential_target_name(ref).c_str(),
                   CRED_TYPE_GENERIC,
                   0)) {
    DWORD code = GetLastError();
    if (code != ERROR_NOT_FOUND) {
      throw error(windows_credential_error_message(code, "clear"));
    }
  }
#else
  (void)config;
#endif
}

std::string
ai_config_response_json(const ai_config& config)
{
  std::ostringstream out;
  out << "{\n";
  out << "  \"ok\": true,\n";
  out << "  \"enabled\": " << (config.enabled ? "true" : "false") << ",\n";
  out << "  \"provider\": " << quote_json(config.provider) << ",\n";
  out << "  \"baseUrl\": " << quote_json(config.base_url) << ",\n";
  out << "  \"model\": " << quote_json(config.model) << ",\n";
  out << "  \"apiKeyRef\": "
      << quote_json(config.api_key_ref.empty() ? ai_api_key_ref(config)
                                               : config.api_key_ref)
      << ",\n";
  out << "  \"timeoutSeconds\": " << config.timeout_seconds << ",\n";
  out << "  \"retryAttempts\": " << config.retry_attempts << ",\n";
  out << "  \"maxOutputTokens\": " << config.max_output_tokens << ",\n";
  out << "  \"allowWrites\": " << (config.allow_writes ? "true" : "false")
      << ",\n";
  out << "  \"systemPrompt\": "
      << quote_json(normalized_ai_system_prompt(config.system_prompt)) << ",\n";
  bool key_configured =
    !config.api_key_ref.empty() && ai_api_key_configured(config);
  out << "  \"apiKeyConfigured\": " << (key_configured ? "true" : "false")
      << ",\n";
  out << "  \"secretStorage\": " << quote_json(default_ai_api_key_ref())
      << ",\n";
  out << "  \"secretStorageWarning\": ";
#if defined(__APPLE__)
  out << "null\n";
#elif defined(__linux__)
  out << "null\n";
#elif defined(_WIN32)
  out << "null\n";
#else
  out << quote_json(
    "Secure API key storage is only implemented on macOS, Linux, and Windows.")
      << "\n";
#endif
  out << "}\n";
  return out.str();
}

ai_config
parse_ai_config_update(const json_value& body, const ai_config& existing)
{
  if (body.type != json_value::k_object)
    throw error("AI config body must be an object");
  ai_config config = existing;
  for (const auto& [key, value] : body.object) {
    if (key == "enabled") {
      if (value.type != json_value::k_bool)
        throw error("enabled must be a boolean");
      config.enabled = value.boolean;
    } else if (key == "provider") {
      if (value.type != json_value::k_string)
        throw error("provider must be a string");
      config.provider = gitboard::trim(value.string).empty()
                          ? "openai-compatible"
                          : gitboard::trim(value.string);
    } else if (key == "baseUrl") {
      if (value.type != json_value::k_string)
        throw error("baseUrl must be a string");
      config.base_url = gitboard::trim(value.string);
    } else if (key == "model") {
      if (value.type != json_value::k_string)
        throw error("model must be a string");
      config.model = gitboard::trim(value.string);
    } else if (key == "apiKeyRef") {
      if (value.type != json_value::k_string)
        throw error("apiKeyRef must be a string");
      config.api_key_ref = gitboard::trim(value.string).empty()
                             ? ai_api_key_ref(config)
                             : gitboard::trim(value.string);
    } else if (key == "timeoutSeconds") {
      if (value.type != json_value::k_number)
        throw error("timeoutSeconds must be a number");
      config.timeout_seconds = std::stoi(value.number);
    } else if (key == "retryAttempts") {
      if (value.type != json_value::k_number)
        throw error("retryAttempts must be a number");
      config.retry_attempts = std::stoi(value.number);
    } else if (key == "maxOutputTokens") {
      if (value.type != json_value::k_number)
        throw error("maxOutputTokens must be a number");
      config.max_output_tokens = std::stoi(value.number);
    } else if (key == "allowWrites") {
      if (value.type != json_value::k_bool)
        throw error("allowWrites must be a boolean");
      config.allow_writes = value.boolean;
    } else if (key == "systemPrompt") {
      if (value.type != json_value::k_string)
        throw error("systemPrompt must be a string");
      config.system_prompt = normalized_ai_system_prompt(value.string);
    }
  }
  if (config.timeout_seconds < 1 ||
      config.timeout_seconds > kMaxAiTimeoutSeconds) {
    throw error("timeoutSeconds must be between 1 and 3600");
  }
  if (config.retry_attempts < 0 ||
      config.retry_attempts > kMaxAiRetryAttempts) {
    throw error("retryAttempts must be between 0 and 20");
  }
  if (config.max_output_tokens < 0 || config.max_output_tokens > 200000) {
    throw error("maxOutputTokens must be between 0 and 200000");
  }
  if (config.enabled) {
    if (config.base_url.empty())
      throw error("baseUrl is required when AI is enabled");
    if (config.model.empty())
      throw error("model is required when AI is enabled");
  }
  if (config.api_key_ref.empty() || config.api_key_ref == "local-file" ||
      config.api_key_ref == "macos-keychain" ||
      config.api_key_ref == "linux-secret-service" ||
      config.api_key_ref == "windows-credential-manager" ||
      config.api_key_ref == "unsupported") {
    config.api_key_ref = ai_api_key_ref(config);
  }
  config.system_prompt = normalized_ai_system_prompt(config.system_prompt);
  if (config.system_prompt.size() > 20000) {
    throw error("systemPrompt must be 20000 characters or fewer");
  }
  return config;
}

} // namespace gitboard::server
