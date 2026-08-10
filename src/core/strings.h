#ifndef GITBOARD_CORE_STRINGS_H_
#define GITBOARD_CORE_STRINGS_H_

#include <string>
#include <string_view>
#include <vector>

namespace gitboard {

std::string ltrim(std::string_view s);
std::string rtrim(std::string_view s);
std::string trim(std::string_view s);
std::string lower_ascii(std::string_view s);
bool starts_with(std::string_view s, std::string_view prefix);
std::vector<std::string> split(const std::string& s, char delim);
std::vector<std::string> split_list(const std::string& decoded);
std::string json_escape(std::string_view s);
std::string shell_quote(std::string_view s);
std::string slugify(const std::string& title);

}  // namespace gitboard

#endif  // GITBOARD_CORE_STRINGS_H_
