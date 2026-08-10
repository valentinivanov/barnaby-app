#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "src/core/error.h"
#include "src/core/filesystem.h"
#include "src/core/json.h"
#include "src/core/status_config.h"
#include "src/core/subprocess.h"

namespace fs = std::filesystem;

namespace {

struct failure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

void expect(bool condition, const std::string& message) {
  if (!condition) throw failure(message);
}

void expect_eq(const std::string& actual, const std::string& expected,
               const std::string& message) {
  if (actual != expected) {
    throw failure(message + ": expected '" + expected + "', got '" + actual +
                  "'");
  }
}

int process_id() {
#ifdef _WIN32
  return static_cast<int>(GetCurrentProcessId());
#else
  return static_cast<int>(getpid());
#endif
}

void test_subprocess_argv_and_exit_code() {
#ifdef _WIN32
  gitboard::process_output echo =
      gitboard::run_capture({"cmd.exe", "/c", "echo hello; exit 7"});
  const std::string expected_echo = "hello; exit 7\r\n";

  gitboard::process_output failed =
      gitboard::run_capture({"cmd.exe", "/c", "exit /b 7"});
#else
  gitboard::process_output echo =
      gitboard::run_capture({"/bin/echo", "hello; exit 7"});
  const std::string expected_echo = "hello; exit 7\n";

  gitboard::process_output failed =
      gitboard::run_capture({"/bin/sh", "-c", "exit 7"});
#endif
  expect(echo.exit_code == 0, "echo should exit cleanly");
  expect_eq(echo.stdout_text, expected_echo,
            "subprocess should pass metacharacters as argv data");

  expect(failed.exit_code == 7, "subprocess should decode real exit status");
}

void test_json_unicode_escapes() {
  gitboard::json_value value =
      gitboard::json_parser(R"({"plain":"A","snowman":"\u2603","face":"\uD83D\uDE00"})")
          .parse();
  expect(value.type == gitboard::json_value::k_object,
         "unicode JSON should parse as object");
  expect_eq(value.object.at("plain").string, "A", "plain string");
  expect_eq(value.object.at("snowman").string, "\xE2\x98\x83",
            "BMP unicode escape");
  expect_eq(value.object.at("face").string, "\xF0\x9F\x98\x80",
            "surrogate pair unicode escape");

  bool rejected = false;
  try {
    gitboard::json_parser(R"("\uD83D")").parse();
  } catch (const gitboard::error&) {
    rejected = true;
  }
  expect(rejected, "unpaired high surrogate should be rejected");
}

void test_json_depth_limit() {
  std::string input(300, '[');
  input.append(300, ']');
  bool rejected = false;
  try {
    gitboard::json_parser(input).parse();
  } catch (const gitboard::error&) {
    rejected = true;
  }
  expect(rejected, "deep JSON should be rejected");
}

void test_json_helpers() {
  expect_eq(gitboard::json_quote("line\n\"x\""), "\"line\\n\\\"x\\\"\"",
            "json_quote should escape strings");

  gitboard::json_value value =
      gitboard::json_parser(
          R"({"name":"Barnaby","count":3,"selected":false,"tags":["one"," two ","",123],"nested":{"ok":true}})")
          .parse();
  expect_eq(gitboard::json_string_member(value, "name"), "Barnaby",
            "json_string_member should return string fields");
  expect(gitboard::json_int_member(value, "count") == 3,
         "json_int_member should return integer fields");
  expect(!gitboard::json_bool_member_or_default(value, "selected", true),
         "json_bool_member_or_default should return boolean fields");
  expect_eq(gitboard::json_array_string_member(value, "tags"),
            "[\"one\",\"two\"]",
            "json_array_string_member should trim valid strings");
  expect_eq(gitboard::stringify_json(value),
            "{\"count\":3,\"name\":\"Barnaby\",\"nested\":{\"ok\":true},\"selected\":false,\"tags\":[\"one\",\" two \",\"\",123]}",
            "stringify_json should serialize parsed JSON");

  auto extracted = gitboard::extract_json_object_from_text(
      "reply\n```json\n{\"content\":\"ok\"}\n```\n");
  expect(extracted.has_value(), "extract_json_object_from_text should parse fenced JSON");
  expect_eq(gitboard::json_string_member(*extracted, "content"), "ok",
            "extracted object should contain fields");
}

void test_atomic_write() {
  fs::path dir = fs::temp_directory_path() /
                 ("gitboard-core-phase1-" + std::to_string(process_id()));
  fs::remove_all(dir);
  fs::create_directories(dir);
  fs::path path = dir / "data.txt";
  gitboard::write_file_atomic(path, "first");
  expect_eq(gitboard::read_file(path), "first", "initial atomic write");
  gitboard::write_file_atomic(path, "second");
  expect_eq(gitboard::read_file(path), "second", "replacement atomic write");
  for (const auto& entry : fs::directory_iterator(dir)) {
    expect(entry.path().filename().string().find(".tmp.") == std::string::npos,
           "atomic write should not leave temp files");
  }
  fs::remove_all(dir);
}

void test_status_config() {
  gitboard::status_config cfg = gitboard::load_status_config();
  expect(cfg.known("todo"), "default status config should include todo");
  expect(cfg.can_transition("todo", "in_progress"),
         "default status config should allow expected workflow movement");
  expect(cfg.can_transition("todo", "done"),
         "default status config should allow unrestricted movement");

  fs::path dir = fs::temp_directory_path() /
                 ("gitboard-status-config-" + std::to_string(process_id()));
  fs::remove_all(dir);
  fs::create_directories(dir);
  gitboard::write_file_atomic(
      dir / "statuses.json",
      R"({"todo":{"transitions":["blocked"]},"blocked":{"transitions":[]}})");
  gitboard::status_config local_cfg = gitboard::load_status_config(dir);
  expect(local_cfg.known("blocked"),
         "task-local status config should be loaded from tasks directory");
  expect(local_cfg.can_transition("todo", "blocked"),
         "task-local status transitions should be used");
  fs::remove_all(dir);
}

}  // namespace

int main() {
  try {
    test_subprocess_argv_and_exit_code();
    test_json_unicode_escapes();
    test_json_depth_limit();
    test_json_helpers();
    test_atomic_write();
    test_status_config();
  } catch (const std::exception& ex) {
    std::cerr << "core_phase1_test: " << ex.what() << "\n";
    return 1;
  }
  std::cout << "core phase 1 tests passed\n";
  return 0;
}
