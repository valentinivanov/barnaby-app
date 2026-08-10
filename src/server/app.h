#ifndef GITBOARD_SERVER_APP_H_
#define GITBOARD_SERVER_APP_H_

#include <filesystem>
#include <string>

#include "src/server/http.h"

namespace gitboard::server {

void configure_ai_debug_log_file(const std::filesystem::path& requested_path);
const std::filesystem::path& ai_debug_log_path();
std::filesystem::path find_gitboard_path(const std::filesystem::path& explicit_path,
                                         char* argv0);
http_response handle_request(const http_request& request,
                             const std::filesystem::path& gitboard_path,
                             const std::string& api_token);

}  // namespace gitboard::server

#endif  // GITBOARD_SERVER_APP_H_
