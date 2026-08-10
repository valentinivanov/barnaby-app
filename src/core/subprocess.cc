#include "src/core/subprocess.h"

#include <array>
#include <cerrno>
#include <cstring>
#ifdef _WIN32
#include <thread>
#include <windows.h>
#else
#include <sys/select.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <sstream>

#include "src/core/error.h"

namespace gitboard {
namespace {

std::string describe_command(const std::vector<std::string>& argv) {
  std::ostringstream out;
  for (std::size_t i = 0; i < argv.size(); ++i) {
    if (i != 0) out << ' ';
    out << argv[i];
  }
  return out.str();
}

#ifndef _WIN32
void close_if_open(int fd) {
  if (fd >= 0) close(fd);
}
#endif

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

std::wstring command_line_for(const std::vector<std::string>& argv) {
  std::wstring line;
  for (std::size_t i = 0; i < argv.size(); ++i) {
    if (i != 0) line.push_back(L' ');
    line += quote_arg(widen(argv[i]));
  }
  return line;
}

struct inherited_pipe {
  HANDLE read = nullptr;
  HANDLE write = nullptr;

  inherited_pipe() {
    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    if (!CreatePipe(&read, &write, &security, 0)) {
      throw error("CreatePipe failed");
    }
    SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
  }

  ~inherited_pipe() {
    if (read) CloseHandle(read);
    if (write) CloseHandle(write);
  }

  inherited_pipe(const inherited_pipe&) = delete;
  inherited_pipe& operator=(const inherited_pipe&) = delete;
};

std::string read_pipe_to_string(HANDLE pipe) {
  std::string output;
  std::array<char, 4096> buffer{};
  DWORD read = 0;
  while (ReadFile(pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &read,
                  nullptr) &&
         read > 0) {
    output.append(buffer.data(), static_cast<std::size_t>(read));
  }
  return output;
}
#endif

}  // namespace

process_output run_capture(const std::vector<std::string>& argv) {
  if (argv.empty()) throw error("cannot run empty command");
#ifdef _WIN32
  inherited_pipe stdout_pipe;
  inherited_pipe stderr_pipe;

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdOutput = stdout_pipe.write;
  startup.hStdError = stderr_pipe.write;
  startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

  PROCESS_INFORMATION process{};
  std::wstring command_line = command_line_for(argv);
  BOOL created = CreateProcessW(nullptr, command_line.data(), nullptr, nullptr,
                                TRUE, 0, nullptr, nullptr, &startup, &process);
  CloseHandle(stdout_pipe.write);
  stdout_pipe.write = nullptr;
  CloseHandle(stderr_pipe.write);
  stderr_pipe.write = nullptr;
  if (!created) throw error("CreateProcess failed: " + describe_command(argv));

  process_output result;
  std::thread stdout_reader([&] {
    result.stdout_text = read_pipe_to_string(stdout_pipe.read);
  });
  std::thread stderr_reader([&] {
    result.stderr_text = read_pipe_to_string(stderr_pipe.read);
  });

  WaitForSingleObject(process.hProcess, INFINITE);
  stdout_reader.join();
  stderr_reader.join();
  DWORD exit_code = 1;
  GetExitCodeProcess(process.hProcess, &exit_code);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  result.exit_code = static_cast<int>(exit_code);
  return result;
#else
  int stdout_pipe[2];
  int stderr_pipe[2];
  if (pipe(stdout_pipe) != 0) {
    throw error(std::string("pipe failed: ") + std::strerror(errno));
  }
  if (pipe(stderr_pipe) != 0) {
    close(stdout_pipe[0]);
    close(stdout_pipe[1]);
    throw error(std::string("pipe failed: ") + std::strerror(errno));
  }

  pid_t pid = fork();
  if (pid < 0) {
    close(stdout_pipe[0]);
    close(stdout_pipe[1]);
    close(stderr_pipe[0]);
    close(stderr_pipe[1]);
    throw error(std::string("fork failed: ") + std::strerror(errno));
  }

  if (pid == 0) {
    close(stdout_pipe[0]);
    close(stderr_pipe[0]);
    dup2(stdout_pipe[1], STDOUT_FILENO);
    dup2(stderr_pipe[1], STDERR_FILENO);
    close(stdout_pipe[1]);
    close(stderr_pipe[1]);

    std::vector<std::string> storage = argv;
    std::vector<char*> exec_argv;
    exec_argv.reserve(storage.size() + 1);
    for (auto& item : storage) exec_argv.push_back(item.data());
    exec_argv.push_back(nullptr);
    execvp(exec_argv[0], exec_argv.data());
    _exit(127);
  }

  close(stdout_pipe[1]);
  close(stderr_pipe[1]);
  process_output result;
  int stdout_fd = stdout_pipe[0];
  int stderr_fd = stderr_pipe[0];
  while (stdout_fd >= 0 || stderr_fd >= 0) {
    fd_set readfds;
    FD_ZERO(&readfds);
    int max_fd = -1;
    if (stdout_fd >= 0) {
      FD_SET(stdout_fd, &readfds);
      if (stdout_fd > max_fd) max_fd = stdout_fd;
    }
    if (stderr_fd >= 0) {
      FD_SET(stderr_fd, &readfds);
      if (stderr_fd > max_fd) max_fd = stderr_fd;
    }
    int ready = select(max_fd + 1, &readfds, nullptr, nullptr, nullptr);
    if (ready < 0) {
      if (errno == EINTR) continue;
      close_if_open(stdout_fd);
      close_if_open(stderr_fd);
      throw error(std::string("select failed: ") + std::strerror(errno));
    }
    if (stdout_fd >= 0 && FD_ISSET(stdout_fd, &readfds)) {
      std::array<char, 4096> buffer{};
      ssize_t n = read(stdout_fd, buffer.data(), buffer.size());
      if (n > 0) {
        result.stdout_text.append(buffer.data(), static_cast<std::size_t>(n));
      } else if (n == 0) {
        close(stdout_fd);
        stdout_fd = -1;
      } else if (errno != EINTR) {
        close_if_open(stdout_fd);
        close_if_open(stderr_fd);
        throw error(std::string("read failed: ") + std::strerror(errno));
      }
    }
    if (stderr_fd >= 0 && FD_ISSET(stderr_fd, &readfds)) {
      std::array<char, 4096> buffer{};
      ssize_t n = read(stderr_fd, buffer.data(), buffer.size());
      if (n > 0) {
        result.stderr_text.append(buffer.data(), static_cast<std::size_t>(n));
      } else if (n == 0) {
        close(stderr_fd);
        stderr_fd = -1;
      } else if (errno != EINTR) {
        close_if_open(stdout_fd);
        close_if_open(stderr_fd);
        throw error(std::string("read failed: ") + std::strerror(errno));
      }
    }
  }

  int status = 0;
  while (waitpid(pid, &status, 0) < 0) {
    if (errno != EINTR) {
      throw error(std::string("waitpid failed: ") + std::strerror(errno));
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

void run_checked(const std::vector<std::string>& argv) {
  process_output output = run_capture(argv);
  if (output.exit_code != 0) {
    std::string message = "command failed: " + describe_command(argv);
    if (!output.stderr_text.empty()) message += "\n" + output.stderr_text;
    if (!output.stdout_text.empty()) message += "\n" + output.stdout_text;
    throw error(message);
  }
}

}  // namespace gitboard
