#ifndef GITBOARD_CORE_JSON_H_
#define GITBOARD_CORE_JSON_H_

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace gitboard {

struct json_value {
  enum value_type { k_null, k_bool, k_string, k_array, k_object, k_number };
  value_type type = k_null;
  bool boolean = false;
  std::string string;
  std::string number;
  std::vector<json_value> array;
  std::map<std::string, json_value> object;
};

class json_parser {
 public:
  explicit json_parser(std::string_view input);

  json_value parse();

 private:
  json_value parse_value(int depth = 0);
  json_value parse_array(int depth);
  json_value parse_object(int depth);
  json_value parse_number();
  std::string parse_string();
  void skip_ws();
  void expect(char c);
  bool consume(char c);

  std::string_view input_;
  size_t pos_ = 0;
};

std::string json_quote(std::string_view s);
std::string stringify_json(const json_value& value);
std::string json_string_member(const json_value& object,
                               const std::string& name);
int json_int_member(const json_value& object, const std::string& name);
bool json_bool_member_or_default(const json_value& object,
                                 const std::string& name,
                                 bool fallback);
std::string json_array_string_member(const json_value& object,
                                     const std::string& name);
std::optional<json_value> extract_json_object_from_text(std::string text);

}  // namespace gitboard

#endif  // GITBOARD_CORE_JSON_H_
