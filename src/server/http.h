#ifndef GITBOARD_SERVER_HTTP_H_
#define GITBOARD_SERVER_HTTP_H_

#include <map>
#include <optional>
#include <cstdint>
#include <string>
#include <string_view>

#ifdef _WIN32
#include <winsock2.h>
#endif

namespace gitboard::server {

#ifdef _WIN32
using socket_handle = SOCKET;
#else
using socket_handle = int;
#endif

struct http_request {
  std::string method;
  std::string path;
  std::string body;
  std::map<std::string, std::string> headers;
};

struct http_response {
  int status = 200;
  std::string content_type = "application/json";
  std::string body;
};

std::string error_json(std::string_view message);
http_response json_response(int status, std::string body);
std::optional<http_request> read_request(socket_handle fd);
void send_response(socket_handle fd, const http_response& response);

}  // namespace gitboard::server

#endif  // GITBOARD_SERVER_HTTP_H_
