#include "src/core/user.h"

#include <cstdlib>

#if defined(_WIN32)
#include <process.h>
#else
#include <pwd.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace gitboard {

std::string current_user() {
#if defined(_WIN32)
  const char* user = std::getenv("USERNAME");
  return user ? user : "";
#else
  const char* user = std::getenv("USER");
  if (user && *user) return user;
  passwd* pw = getpwuid(getuid());
  return pw ? pw->pw_name : "";
#endif
}

}  // namespace gitboard
