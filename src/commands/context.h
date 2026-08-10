#ifndef GITBOARD_COMMANDS_CONTEXT_H_
#define GITBOARD_COMMANDS_CONTEXT_H_

#include "src/core/task_store.h"

namespace gitboard {

struct context {
  task_store store;
};

}  // namespace gitboard

#endif  // GITBOARD_COMMANDS_CONTEXT_H_
