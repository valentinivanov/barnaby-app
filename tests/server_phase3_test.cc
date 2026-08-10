#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "src/server/process.h"

namespace fs = std::filesystem;

namespace {

struct failure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

void expect(bool condition, const std::string& message) {
  if (!condition) throw failure(message);
}

void test_run_process_checks_cwd() {
#ifdef _WIN32
  const int process_id = static_cast<int>(GetCurrentProcessId());
  const fs::path executable = "cmd.exe";
  const std::vector<std::string> args = {"/c", "cd"};
#else
  const int process_id = static_cast<int>(getpid());
  const fs::path executable = "/bin/pwd";
  const std::vector<std::string> args;
#endif
  fs::path cwd = fs::temp_directory_path() /
                 ("gitboard-server-phase3-" + std::to_string(process_id));
  fs::remove_all(cwd);
  fs::create_directories(cwd);

  gitboard::server::process_result ok =
      gitboard::server::run_process(executable, args, cwd);
  expect(ok.exit_code == 0, "pwd should succeed with valid cwd");
  expect(ok.output.find(cwd.string()) != std::string::npos,
         "process should run in requested cwd");

  gitboard::server::process_result failed =
      gitboard::server::run_process(executable, args, cwd / "missing");
  expect(failed.exit_code != 0, "process should fail with invalid cwd");
  expect(failed.output.find("chdir failed") != std::string::npos,
         "invalid cwd failure should explain chdir failure");

  fs::remove_all(cwd);
}

}  // namespace

int main() {
  try {
    test_run_process_checks_cwd();
  } catch (const std::exception& ex) {
    std::cerr << "server_phase3_test: " << ex.what() << "\n";
    return 1;
  }
  std::cout << "server phase 3 tests passed\n";
  return 0;
}
