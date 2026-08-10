#include "src/core/time.h"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace gitboard {

std::string current_utc_iso() {
  const auto now = std::chrono::system_clock::now();
  const auto seconds = std::chrono::time_point_cast<std::chrono::seconds>(now);
  const auto micros =
      std::chrono::duration_cast<std::chrono::microseconds>(now - seconds)
          .count();
  std::time_t tt = std::chrono::system_clock::to_time_t(seconds);
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &tt);
#else
  gmtime_r(&tt, &tm);
#endif
  std::ostringstream out;
  out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S") << "." << std::setw(6)
      << std::setfill('0') << micros << "Z";
  return out.str();
}

}  // namespace gitboard
