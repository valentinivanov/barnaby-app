#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct failure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

void expect(bool condition, const std::string& message) {
  if (!condition) throw failure(message);
}

fs::path executable_path(fs::path path) {
#ifdef _WIN32
  if (!fs::exists(path) && path.extension().empty()) {
    fs::path with_extension = path;
    with_extension += ".exe";
    if (fs::exists(with_extension)) return with_extension;
  }
  if (!fs::exists(path)) {
    const char* manifest_path = std::getenv("RUNFILES_MANIFEST_FILE");
    if (manifest_path) {
      const std::string key = path.generic_string();
      const std::string exe_key =
          path.extension().empty() ? key + ".exe" : std::string();
      std::ifstream manifest(manifest_path);
      std::string line;
      while (std::getline(manifest, line)) {
        const std::size_t space = line.find(' ');
        if (space == std::string::npos) continue;
        const std::string runfile = line.substr(0, space);
        if (runfile == key || (!exe_key.empty() && runfile == exe_key) ||
            runfile.size() > key.size() &&
                runfile.compare(runfile.size() - key.size(), key.size(), key) == 0 ||
            (!exe_key.empty() && runfile.size() > exe_key.size() &&
             runfile.compare(runfile.size() - exe_key.size(), exe_key.size(),
                             exe_key) == 0)) {
          return fs::path(line.substr(space + 1));
        }
      }
    }
  }
#endif
  return path;
}

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

std::string windows_error_message(const std::string& operation) {
  const DWORD code = GetLastError();
  return operation + " failed with Win32 error " + std::to_string(code);
}

std::wstring quote_arg(const std::wstring& arg) {
  if (arg.empty()) return L"\"\"";
  if (arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) return arg;

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

std::wstring command_line_for(const fs::path& executable,
                              const std::vector<std::string>& args) {
  std::wstring line = quote_arg(executable.wstring());
  for (const auto& arg : args) {
    line.push_back(L' ');
    line += quote_arg(widen(arg));
  }
  return line;
}

struct winsock_runtime {
  winsock_runtime() {
    WSADATA data{};
    expect(WSAStartup(MAKEWORD(2, 2), &data) == 0, "WSAStartup failed");
  }
  ~winsock_runtime() { WSACleanup(); }
};

struct child_process {
  PROCESS_INFORMATION process{};
  bool started = false;

  child_process() = default;
  child_process(const child_process&) = delete;
  child_process& operator=(const child_process&) = delete;
  child_process(child_process&& other) noexcept
      : process(other.process), started(other.started) {
    other.process = PROCESS_INFORMATION{};
    other.started = false;
  }
  child_process& operator=(child_process&& other) noexcept {
    if (this != &other) {
      if (started) {
        TerminateProcess(process.hProcess, 0);
        WaitForSingleObject(process.hProcess, INFINITE);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
      }
      process = other.process;
      started = other.started;
      other.process = PROCESS_INFORMATION{};
      other.started = false;
    }
    return *this;
  }

  void stop() {
    if (!started) return;
    TerminateProcess(process.hProcess, 0);
    WaitForSingleObject(process.hProcess, INFINITE);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    started = false;
  }

  ~child_process() {
    stop();
  }
};

std::string read_line(HANDLE pipe) {
  std::string line;
  char c = '\0';
  DWORD read = 0;
  while (ReadFile(pipe, &c, 1, &read, nullptr) && read == 1) {
    if (c == '\n') return line;
    if (c != '\r') line.push_back(c);
  }
  return line;
}

child_process start_server_process(const fs::path& server, const fs::path& gitboard,
                                   const fs::path& config, HANDLE* output) {
  SECURITY_ATTRIBUTES security{};
  security.nLength = sizeof(security);
  security.bInheritHandle = TRUE;

  HANDLE read_pipe = nullptr;
  HANDLE write_pipe = nullptr;
  expect(CreatePipe(&read_pipe, &write_pipe, &security, 0) != 0,
         "could not create startup pipe");
  SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdOutput = write_pipe;
  startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
  startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

  std::wstring command = command_line_for(
      server, {"--port", "0", "--gitboard-path", gitboard.string(),
               "--config-dir", config.string(), "--no-open-browser"});

  child_process child;
  BOOL created = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, 0,
                                nullptr, nullptr, &startup, &child.process);
  const std::string create_error = windows_error_message("CreateProcess");
  CloseHandle(write_pipe);
  if (!created) {
    CloseHandle(read_pipe);
    throw failure(create_error);
  }

  child.started = true;
  *output = read_pipe;
  return child;
}

std::uint32_t process_id() {
  return static_cast<std::uint32_t>(GetCurrentProcessId());
}

#else
struct child_process {
  pid_t pid = -1;

  child_process() = default;
  child_process(const child_process&) = delete;
  child_process& operator=(const child_process&) = delete;
  child_process(child_process&& other) noexcept : pid(other.pid) {
    other.pid = -1;
  }
  child_process& operator=(child_process&& other) noexcept {
    if (this != &other) {
      if (pid > 0) {
        kill(pid, SIGTERM);
        waitpid(pid, nullptr, 0);
      }
      pid = other.pid;
      other.pid = -1;
    }
    return *this;
  }

  void stop() {
    if (pid > 0) {
      kill(pid, SIGTERM);
      waitpid(pid, nullptr, 0);
      pid = -1;
    }
  }

  ~child_process() {
    stop();
  }
};

std::string read_line(int fd) {
  std::string line;
  char c = '\0';
  while (read(fd, &c, 1) == 1) {
    if (c == '\n') return line;
    line.push_back(c);
  }
  return line;
}

child_process start_server_process(const fs::path& server, const fs::path& gitboard,
                                   const fs::path& config, int* output) {
  int pipefd[2];
  expect(pipe(pipefd) == 0, "could not create startup pipe");
  child_process child;
  child.pid = fork();
  expect(child.pid >= 0, "could not fork server process");
  if (child.pid == 0) {
    close(pipefd[0]);
    dup2(pipefd[1], STDOUT_FILENO);
    close(pipefd[1]);
    const std::string config_text = config.string();
    execl(server.c_str(), server.c_str(), "--port", "0", "--gitboard-path",
          gitboard.c_str(), "--config-dir", config_text.c_str(), "--no-open-browser",
          static_cast<char*>(nullptr));
    _exit(127);
  }

  close(pipefd[1]);
  *output = pipefd[0];
  return child;
}

std::uint32_t process_id() {
  return static_cast<std::uint32_t>(getpid());
}
#endif

std::string request_http(int port, const std::string& request) {
#ifdef _WIN32
  winsock_runtime winsock;
  SOCKET fd = socket(AF_INET, SOCK_STREAM, 0);
  expect(fd != INVALID_SOCKET, "could not create client socket");
#else
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  expect(fd >= 0, "could not create client socket");
#endif

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(static_cast<uint16_t>(port));
#ifdef _WIN32
  if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
    closesocket(fd);
    throw failure("could not connect to the reported server port");
  }
#else
  if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    close(fd);
    throw failure("could not connect to the reported server port");
  }
#endif

#ifdef _WIN32
  expect(send(fd, request.data(), static_cast<int>(request.size()), 0) ==
             static_cast<int>(request.size()),
         "could not write HTTP request");
#else
  expect(write(fd, request.data(), request.size()) ==
             static_cast<ssize_t>(request.size()),
         "could not write HTTP request");
#endif

  std::string response;
  std::array<char, 4096> buffer{};
#ifdef _WIN32
  int count = 0;
  while ((count = recv(fd, buffer.data(), static_cast<int>(buffer.size()), 0)) > 0) {
    response.append(buffer.data(), static_cast<std::size_t>(count));
  }
  closesocket(fd);
#else
  ssize_t count = 0;
  while ((count = read(fd, buffer.data(), buffer.size())) > 0) {
    response.append(buffer.data(), static_cast<std::size_t>(count));
  }
  close(fd);
#endif
  return response;
}

std::string request_index(int port) {
  return request_http(
      port, "GET / HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n");
}

void test_ephemeral_port_startup(const fs::path& server, const fs::path& gitboard) {
  fs::path temp = fs::temp_directory_path() /
                  ("gitboard-server-startup-" + std::to_string(process_id()));
  fs::remove_all(temp);
  fs::create_directories(temp);

#ifdef _WIN32
  HANDLE output = nullptr;
#else
  int output = -1;
#endif
  child_process child =
      start_server_process(server, gitboard, temp / "config", &output);

  const std::string display_line = read_line(output);
  const std::string startup_line = read_line(output);
  const std::string base_line = read_line(output);
  const std::string token_line = read_line(output);
#ifdef _WIN32
  CloseHandle(output);
#else
  close(output);
#endif
  expect(display_line.find("GitBoard server listening at ") == 0,
         "server did not print its display URL");

  const std::string prefix = "GITBOARD_SERVER_URL=http://127.0.0.1:";
  expect(startup_line.find(prefix) == 0 &&
             startup_line.find("/?token=", prefix.size()) != std::string::npos,
         "server did not print its tokenized machine-readable startup URL");
  const std::string base_prefix = "GITBOARD_SERVER_BASE_URL=http://127.0.0.1:";
  expect(base_line.find(base_prefix) == 0 && base_line.back() == '/',
         "server did not print its base startup URL");
  const std::string token_prefix = "GITBOARD_SERVER_TOKEN=";
  expect(token_line.find(token_prefix) == 0,
         "server did not print its API token");
  const std::string token = token_line.substr(token_prefix.size());
  expect(token.size() >= 32, "server API token is too short");
  const std::size_t port_end = startup_line.find('/', prefix.size());
  const std::string port_text =
      startup_line.substr(prefix.size(), port_end - prefix.size());
  const int port = std::stoi(port_text);
  expect(port > 0 && port <= 65535, "server did not select a valid port");
  expect(request_index(port).find("HTTP/1.1 200 OK") != std::string::npos,
         "server did not answer HTTP requests on its selected port");
  expect(request_http(port,
                      "GET /api/config HTTP/1.1\r\n"
                      "Host: 127.0.0.1\r\n"
                      "Connection: close\r\n\r\n")
             .find("HTTP/1.1 403 Forbidden") != std::string::npos,
         "server accepted API request without token");
  expect(request_http(port,
                      "GET /api/config HTTP/1.1\r\n"
                      "Host: 127.0.0.1\r\n"
                      "X-Barnaby-Token: " + token + "\r\n"
                      "Connection: close\r\n\r\n")
             .find("HTTP/1.1 200 OK") != std::string::npos,
         "server rejected API request with token");
  expect(request_http(port,
                      "POST /batch HTTP/1.1\r\n"
                      "Host: 127.0.0.1\r\n"
                      "X-Barnaby-Token: " + token + "\r\n"
                      "Content-Length: 2\r\n"
                      "Connection: close\r\n\r\n{}")
             .find("HTTP/1.1 415 Unsupported Media Type") != std::string::npos,
         "server accepted mutating API request without JSON content type");
  expect(request_http(port,
                      "POST /batch HTTP/1.1\r\n"
                      "Host: 127.0.0.1\r\n"
                      "Origin: https://example.invalid\r\n"
                      "Content-Type: application/json\r\n"
                      "X-Barnaby-Token: " + token + "\r\n"
                      "Content-Length: 2\r\n"
                      "Connection: close\r\n\r\n{}")
             .find("HTTP/1.1 403 Forbidden") != std::string::npos,
         "server accepted mutating API request from a non-loopback origin");

  child.stop();
  fs::remove_all(temp);
}

}  // namespace

int main(int argc, char** argv) {
  try {
    expect(argc == 3, "expected server and gitboard executable arguments");
    test_ephemeral_port_startup(executable_path(argv[1]), executable_path(argv[2]));
  } catch (const std::exception& ex) {
    std::cerr << "server_startup_test: " << ex.what() << "\n";
    return 1;
  }
  std::cout << "server startup tests passed\n";
  return 0;
}
