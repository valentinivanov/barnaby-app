#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <thread>

#include "src/core/error.h"
#include "src/server/app.h"
#include "src/server/config.h"
#include "src/server/http.h"
#include "src/server/process.h"

namespace fs = std::filesystem;

namespace gitboard::server {
namespace {

#ifdef _WIN32
std::string socket_error_message(const std::string& operation) {
  return operation + " failed: WSA error " + std::to_string(WSAGetLastError());
}

class winsock_runtime {
 public:
  winsock_runtime() {
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
      throw error("WSAStartup failed");
    }
  }

  ~winsock_runtime() { WSACleanup(); }

  winsock_runtime(const winsock_runtime&) = delete;
  winsock_runtime& operator=(const winsock_runtime&) = delete;
};
#endif

#ifndef NDEBUG
constexpr bool kAiDebugLogSupported = true;
#else
constexpr bool kAiDebugLogSupported = false;
#endif

struct options {
  int port = 8080;
  bool open_browser = true;
  fs::path gitboard_path;
  fs::path config_dir;
  bool ai_debug_log = false;
  fs::path ai_debug_log_path;
  std::string api_token;
};

class instance_lock {
 public:
  explicit instance_lock(const fs::path& path) {
    fs::create_directories(path.parent_path());
#ifdef _WIN32
    handle_ = CreateFileW(path.wstring().c_str(), GENERIC_READ | GENERIC_WRITE,
                          0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                          nullptr);
    if (handle_ == INVALID_HANDLE_VALUE) {
      throw error("another gitboard-server instance is already running");
    }
#else
    fd_ = open(path.c_str(), O_CREAT | O_RDWR, 0600);
    if (fd_ < 0) {
      throw error(std::string("failed to open lock file: ") + std::strerror(errno));
    }
    if (flock(fd_, LOCK_EX | LOCK_NB) != 0) {
      close(fd_);
      fd_ = -1;
      throw error("another gitboard-server instance is already running");
    }
#endif
  }

  ~instance_lock() {
#ifdef _WIN32
    if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
#else
    if (fd_ >= 0) {
      flock(fd_, LOCK_UN);
      close(fd_);
    }
#endif
  }

  instance_lock(const instance_lock&) = delete;
  instance_lock& operator=(const instance_lock&) = delete;

 private:
#ifdef _WIN32
  HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
  int fd_ = -1;
#endif
};

socket_handle listen_socket(int port) {
  socket_handle fd = socket(AF_INET, SOCK_STREAM, 0);
#ifdef _WIN32
  if (fd == INVALID_SOCKET) throw error(socket_error_message("socket"));
#else
  if (fd < 0) throw error(std::string("socket failed: ") + std::strerror(errno));
#endif
  int yes = 1;
#ifdef _WIN32
  if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes),
                 sizeof(yes)) == SOCKET_ERROR) {
    closesocket(fd);
    throw error(socket_error_message("setsockopt"));
  }
#else
  if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) != 0) {
    close(fd);
    throw error(std::string("setsockopt failed: ") + std::strerror(errno));
  }
#endif
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
#ifdef _WIN32
  if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
    closesocket(fd);
    throw error(socket_error_message("bind"));
  }
  if (listen(fd, 16) == SOCKET_ERROR) {
    closesocket(fd);
    throw error(socket_error_message("listen"));
  }
#else
  if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    close(fd);
    throw error(std::string("bind failed: ") + std::strerror(errno));
  }
  if (listen(fd, 16) != 0) {
    close(fd);
    throw error(std::string("listen failed: ") + std::strerror(errno));
  }
#endif
  return fd;
}

int listening_port(socket_handle fd) {
  sockaddr_in addr{};
#ifdef _WIN32
  int length = sizeof(addr);
  if (getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &length) ==
      SOCKET_ERROR) {
    throw error(socket_error_message("getsockname"));
  }
#else
  socklen_t length = sizeof(addr);
  if (getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &length) != 0) {
    throw error(std::string("getsockname failed: ") + std::strerror(errno));
  }
#endif
  return ntohs(addr.sin_port);
}

std::string random_api_token() {
  std::random_device random;
  const auto now = static_cast<unsigned long long>(
      std::chrono::high_resolution_clock::now().time_since_epoch().count());
  std::uniform_int_distribution<unsigned int> byte(0, 255);
  std::ostringstream out;
  out << std::hex;
  for (int i = 0; i < 32; ++i) {
    const unsigned int value = byte(random) ^ ((now >> ((i % 8) * 8)) & 0xffu);
    out.width(2);
    out.fill('0');
    out << (value & 0xffu);
  }
  return out.str();
}

void handle_client(socket_handle client, fs::path gitboard_path,
                   std::string api_token) {
  try {
    auto request = read_request(client);
    if (request) {
      send_response(client, handle_request(*request, gitboard_path, api_token));
    }
  } catch (const std::exception& ex) {
    send_response(client, json_response(500, error_json(ex.what())));
  }
#ifdef _WIN32
  closesocket(client);
#else
  close(client);
#endif
}

options parse_options(int argc, char** argv) {
  options opts;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--port") {
      if (++i >= argc) throw error("--port requires a value");
      opts.port = std::stoi(argv[i]);
    } else if (arg == "--gitboard-path") {
      if (++i >= argc) throw error("--gitboard-path requires a value");
      opts.gitboard_path = argv[i];
    } else if (arg == "--config-dir") {
      if (++i >= argc) throw error("--config-dir requires a value");
      opts.config_dir = argv[i];
    } else if (arg == "--api-token") {
      if (++i >= argc) throw error("--api-token requires a value");
      opts.api_token = argv[i];
    } else if (arg == "--ai-debug-log") {
      if (!kAiDebugLogSupported) {
        throw error("--ai-debug-log is not available in release builds");
      }
      opts.ai_debug_log = true;
      if (i + 1 < argc && std::string(argv[i + 1]).rfind("--", 0) != 0) {
        opts.ai_debug_log_path = argv[++i];
      }
    } else if (arg == "--no-open-browser") {
      opts.open_browser = false;
    } else if (arg == "--help" || arg == "-h") {
      std::cout << "usage: gitboard-server [--port PORT] "
                   "[--gitboard-path PATH] [--config-dir PATH] "
                   "[--api-token TOKEN] "
#ifndef NDEBUG
                   "[--ai-debug-log [PATH]] "
#endif
                   "[--no-open-browser]\n";
      std::exit(0);
    } else {
      throw error("unknown option: " + arg);
    }
  }
  if (opts.port < 0 || opts.port > 65535) throw error("invalid port");
  if (!opts.api_token.empty() && opts.api_token.size() < 32) {
    throw error("--api-token must be at least 32 characters");
  }
  return opts;
}

int run_server(int argc, char** argv) {
#ifdef _WIN32
  winsock_runtime winsock;
#endif
  options opts = parse_options(argc, argv);
  if (!opts.config_dir.empty()) set_config_dir_for_process(opts.config_dir);
  fs::create_directories(config_dir());
  if (kAiDebugLogSupported && opts.ai_debug_log) {
    configure_ai_debug_log_file(opts.ai_debug_log_path);
  }
  instance_lock lock(config_dir() / "server.lock");
  fs::path gitboard_path = find_gitboard_path(opts.gitboard_path, argv[0]);
  std::string api_token =
      opts.api_token.empty() ? random_api_token() : opts.api_token;
  socket_handle server_fd = listen_socket(opts.port);
  std::string url = "http://127.0.0.1:" + std::to_string(listening_port(server_fd)) + "/";
  std::string app_url = url + "?token=" + api_token;
  std::cout << "GitBoard server listening at " << url << "\n"
            << "GITBOARD_SERVER_URL=" << app_url << "\n"
            << "GITBOARD_SERVER_BASE_URL=" << url << "\n"
            << "GITBOARD_SERVER_TOKEN=" << api_token << "\n";
  if (kAiDebugLogSupported && opts.ai_debug_log) {
    std::cout << "AI debug log: " << ai_debug_log_path().string() << "\n"
              << "GITBOARD_AI_DEBUG_LOG=" << ai_debug_log_path().string() << "\n";
  }
  std::cout << std::flush;
  if (opts.open_browser) open_url_in_browser(app_url);

  while (true) {
    socket_handle client = accept(server_fd, nullptr, nullptr);
#ifdef _WIN32
    if (client == INVALID_SOCKET) {
      throw error(socket_error_message("accept"));
    }
#else
    if (client < 0) {
      if (errno == EINTR) continue;
      throw error(std::string("accept failed: ") + std::strerror(errno));
    }
#endif
    try {
      std::thread(handle_client, client, gitboard_path, api_token).detach();
    } catch (...) {
#ifdef _WIN32
      closesocket(client);
#else
      close(client);
#endif
      throw;
    }
  }
}

}  // namespace
}  // namespace gitboard::server

int main(int argc, char** argv) {
  try {
    return gitboard::server::run_server(argc, argv);
  } catch (const std::exception& ex) {
    std::cerr << "gitboard-server: " << ex.what() << "\n";
    return 1;
  }
}
