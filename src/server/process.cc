#include "src/server/process.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <array>
#include <cerrno>
#include <cstring>
#include <iostream>

#include "src/core/error.h"

namespace gitboard::server {
namespace {

#ifdef _WIN32
std::wstring widen(const std::string& value) {
  if (value.empty()) return L"";
  int size = MultiByteToWideChar(CP_UTF8, 0, value.data(),
                                 static_cast<int>(value.size()), nullptr, 0);
  std::wstring out(static_cast<std::size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                      out.data(), size);
  return out;
}

std::wstring quote_arg(const std::wstring& arg) {
  if (arg.empty()) return L"\"\"";
  bool needs_quotes = arg.find_first_of(L" \t\n\v\"") != std::wstring::npos;
  if (!needs_quotes) return arg;

  std::wstring out = L"\"";
  std::size_t backslashes = 0;
  for (wchar_t c : arg) {
    if (c == L'\\') {
      ++backslashes;
    } else if (c == L'"') {
      out.append(backslashes * 2 + 1, L'\\');
      out.push_back(c);
      backslashes = 0;
    } else {
      out.append(backslashes, L'\\');
      backslashes = 0;
      out.push_back(c);
    }
  }
  out.append(backslashes * 2, L'\\');
  out.push_back(L'"');
  return out;
}

std::wstring command_line_for(const std::filesystem::path& executable,
                              const std::vector<std::string>& args) {
  std::wstring line = quote_arg(executable.wstring());
  for (const auto& arg : args) {
    line.push_back(L' ');
    line += quote_arg(widen(arg));
  }
  return line;
}
#endif

}  // namespace

process_result run_process(const std::filesystem::path& executable,
                           const std::vector<std::string>& args,
                           const std::filesystem::path& cwd) {
#ifdef _WIN32
  if (!cwd.empty() && !std::filesystem::is_directory(cwd)) {
    process_result result;
    result.exit_code = 127;
    result.output = "chdir failed: " + cwd.string() + "\n";
    return result;
  }

  SECURITY_ATTRIBUTES security{};
  security.nLength = sizeof(security);
  security.bInheritHandle = TRUE;

  HANDLE read_pipe = nullptr;
  HANDLE write_pipe = nullptr;
  if (!CreatePipe(&read_pipe, &write_pipe, &security, 0)) {
    throw gitboard::error("CreatePipe failed");
  }
  SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdOutput = write_pipe;
  startup.hStdError = write_pipe;
  startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

  PROCESS_INFORMATION process{};
  std::wstring command_line = command_line_for(executable, args);
  std::wstring cwd_wide = cwd.empty() ? L"" : cwd.wstring();

  BOOL created = CreateProcessW(
      nullptr, command_line.data(), nullptr, nullptr, TRUE, 0, nullptr,
      cwd_wide.empty() ? nullptr : cwd_wide.c_str(), &startup, &process);
  CloseHandle(write_pipe);
  if (!created) {
    CloseHandle(read_pipe);
    throw gitboard::error("CreateProcess failed");
  }

  process_result result;
  std::array<char, 4096> buffer{};
  DWORD read = 0;
  while (ReadFile(read_pipe, buffer.data(), static_cast<DWORD>(buffer.size()),
                  &read, nullptr) &&
         read > 0) {
    result.output.append(buffer.data(), static_cast<std::size_t>(read));
  }
  CloseHandle(read_pipe);

  WaitForSingleObject(process.hProcess, INFINITE);
  DWORD exit_code = 1;
  GetExitCodeProcess(process.hProcess, &exit_code);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  result.exit_code = static_cast<int>(exit_code);
  return result;
#else
  int pipefd[2];
  if (pipe(pipefd) != 0) {
    throw gitboard::error(std::string("pipe failed: ") + std::strerror(errno));
  }

  pid_t pid = fork();
  if (pid < 0) {
    close(pipefd[0]);
    close(pipefd[1]);
    throw gitboard::error(std::string("fork failed: ") + std::strerror(errno));
  }

  if (pid == 0) {
    close(pipefd[0]);
    dup2(pipefd[1], STDOUT_FILENO);
    dup2(pipefd[1], STDERR_FILENO);
    close(pipefd[1]);
    if (!cwd.empty()) {
      if (chdir(cwd.c_str()) != 0) {
        std::string message =
            "chdir failed: " + cwd.string() + ": " + std::strerror(errno) + "\n";
        write(STDERR_FILENO, message.data(), message.size());
        _exit(127);
      }
    }

    std::string exe = executable.string();
    std::vector<std::string> storage;
    storage.reserve(args.size() + 1);
    storage.push_back(exe);
    for (const auto& arg : args) storage.push_back(arg);

    std::vector<char*> argv;
    argv.reserve(storage.size() + 1);
    for (auto& item : storage) argv.push_back(item.data());
    argv.push_back(nullptr);

    execvp(exe.c_str(), argv.data());
    _exit(127);
  }

  close(pipefd[1]);
  process_result result;
  std::array<char, 4096> buffer{};
  while (true) {
    ssize_t n = read(pipefd[0], buffer.data(), buffer.size());
    if (n > 0) {
      result.output.append(buffer.data(), static_cast<std::size_t>(n));
    } else if (n == 0) {
      break;
    } else if (errno != EINTR) {
      close(pipefd[0]);
      throw gitboard::error(std::string("read failed: ") + std::strerror(errno));
    }
  }
  close(pipefd[0]);

  int status = 0;
  while (waitpid(pid, &status, 0) < 0) {
    if (errno != EINTR) {
      throw gitboard::error(std::string("waitpid failed: ") +
                            std::strerror(errno));
    }
  }
  if (WIFEXITED(status)) {
    result.exit_code = WEXITSTATUS(status);
  } else {
    result.exit_code = 1;
  }
  return result;
#endif
}

void open_url_in_browser(const std::string& url) {
#if defined(__APPLE__)
  std::filesystem::path opener = "open";
#elif defined(_WIN32)
  std::filesystem::path opener = "cmd";
#else
  std::filesystem::path opener = "xdg-open";
#endif
  try {
#if defined(_WIN32)
    process_result result = run_process(opener, {"/c", "start", "", url});
#else
    process_result result = run_process(opener, {url});
#endif
    if (result.exit_code != 0) {
      std::cerr << "failed to open browser: " << result.output << "\n";
    }
  } catch (const std::exception& ex) {
    std::cerr << "failed to open browser: " << ex.what() << "\n";
  }
}

}  // namespace gitboard::server
