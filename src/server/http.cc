#include "src/server/http.h"

#ifdef _WIN32
#include <winsock2.h>
#else
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <array>
#include <cerrno>
#include <cstdlib>
#include <sstream>
#include <stdexcept>

#include "src/core/error.h"
#include "src/core/strings.h"

namespace gitboard::server {
namespace {

constexpr std::size_t kRequestBufferSize = 4096;
constexpr std::size_t kMaxRequestHeaderSize = 1024 * 1024;

std::string status_text(int status) {
  switch (status) {
    case 200:
      return "OK";
    case 201:
      return "Created";
    case 204:
      return "No Content";
    case 400:
      return "Bad Request";
    case 403:
      return "Forbidden";
    case 404:
      return "Not Found";
    case 405:
      return "Method Not Allowed";
    case 409:
      return "Conflict";
    case 415:
      return "Unsupported Media Type";
    case 500:
      return "Internal Server Error";
    case 501:
      return "Not Implemented";
    case 502:
      return "Bad Gateway";
    default:
      return "Error";
  }
}

std::optional<std::string> header_value(const http_request& request,
                                        const std::string& name) {
  auto it = request.headers.find(gitboard::lower_ascii(name));
  if (it == request.headers.end()) return std::nullopt;
  return it->second;
}

bool send_all(socket_handle fd, std::string_view data) {
  const char* p = data.data();
  std::size_t left = data.size();
  while (left > 0) {
#ifdef _WIN32
    int n = send(fd, p, static_cast<int>(left), 0);
    if (n == SOCKET_ERROR) return false;
#else
    ssize_t n = send(fd, p, left, 0);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return false;
#endif
    p += n;
    left -= static_cast<std::size_t>(n);
  }
  return true;
}

}  // namespace

std::string error_json(std::string_view message) {
  return "{\n  \"ok\": false,\n  \"error\": \"" +
         gitboard::json_escape(message) + "\"\n}\n";
}

http_response json_response(int status, std::string body) {
  return http_response{status, "application/json", std::move(body)};
}

std::optional<http_request> read_request(socket_handle fd) {
  std::string data;
  char buffer[kRequestBufferSize];
  while (data.find("\r\n\r\n") == std::string::npos) {
#ifdef _WIN32
    int n = recv(fd, buffer, sizeof(buffer), 0);
#else
    ssize_t n = recv(fd, buffer, sizeof(buffer), 0);
#endif
    if (n <= 0) return std::nullopt;
    data.append(buffer, static_cast<std::size_t>(n));
    if (data.size() > kMaxRequestHeaderSize) {
      throw gitboard::error("request headers too large");
    }
  }

  std::size_t header_end = data.find("\r\n\r\n");
  std::string headers = data.substr(0, header_end);
  http_request request;
  std::istringstream in(headers);
  std::string line;
  if (!std::getline(in, line)) throw gitboard::error("empty request");
  if (!line.empty() && line.back() == '\r') line.pop_back();
  std::istringstream first(line);
  first >> request.method >> request.path;
  if (request.method.empty() || request.path.empty()) {
    throw gitboard::error("invalid request line");
  }
  std::size_t query = request.path.find('?');
  if (query != std::string::npos) request.path.erase(query);

  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::size_t colon = line.find(':');
    if (colon == std::string::npos) continue;
    std::string key = gitboard::lower_ascii(gitboard::trim(line.substr(0, colon)));
    std::string value = gitboard::trim(line.substr(colon + 1));
    request.headers[key] = value;
  }

  std::size_t content_length = 0;
  if (auto length = header_value(request, "content-length")) {
    content_length = static_cast<std::size_t>(std::stoul(*length));
  }
  request.body = data.substr(header_end + 4);
  while (request.body.size() < content_length) {
#ifdef _WIN32
    int n = recv(fd, buffer, sizeof(buffer), 0);
#else
    ssize_t n = recv(fd, buffer, sizeof(buffer), 0);
#endif
    if (n <= 0) throw gitboard::error("unexpected end of request body");
    request.body.append(buffer, static_cast<std::size_t>(n));
  }
  if (request.body.size() > content_length) request.body.resize(content_length);
  return request;
}

void send_response(socket_handle fd, const http_response& response) {
  std::ostringstream headers;
  headers << "HTTP/1.1 " << response.status << " " << status_text(response.status)
          << "\r\n"
          << "Content-Type: " << response.content_type << "\r\n"
          << "Content-Length: " << response.body.size() << "\r\n"
          << "Connection: close\r\n"
          << "\r\n";
  if (!send_all(fd, headers.str())) return;
  send_all(fd, response.body);
}

}  // namespace gitboard::server
