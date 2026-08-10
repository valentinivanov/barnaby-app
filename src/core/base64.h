#ifndef GITBOARD_CORE_BASE64_H_
#define GITBOARD_CORE_BASE64_H_

#include <string>

namespace gitboard {

std::string base64_encode(const std::string& input);
std::string base64_decode(const std::string& input);

}  // namespace gitboard

#endif  // GITBOARD_CORE_BASE64_H_
