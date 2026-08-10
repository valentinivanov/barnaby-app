#ifndef GITBOARD_SERVER_ASSETS_H_
#define GITBOARD_SERVER_ASSETS_H_

#include <cstddef>
#include <string_view>

namespace gitboard::server {

struct asset {
  std::string_view path;
  std::string_view content_type;
  const unsigned char* content;
  std::size_t content_length;
};

const asset* find_asset(std::string_view path);

}  // namespace gitboard::server

#endif  // GITBOARD_SERVER_ASSETS_H_
