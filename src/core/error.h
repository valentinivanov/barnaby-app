#ifndef GITBOARD_CORE_ERROR_H_
#define GITBOARD_CORE_ERROR_H_

#include <stdexcept>

namespace gitboard {

struct error : std::runtime_error {
  using std::runtime_error::runtime_error;
};

}  // namespace gitboard

#endif  // GITBOARD_CORE_ERROR_H_
