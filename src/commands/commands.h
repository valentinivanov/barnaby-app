#ifndef GITBOARD_COMMANDS_COMMANDS_H_
#define GITBOARD_COMMANDS_COMMANDS_H_

#include <string>
#include <vector>

#include "src/commands/context.h"

namespace gitboard {

std::string execute(context& ctx, const std::string& cmd,
                    const std::vector<std::string>& args);

}  // namespace gitboard

#endif  // GITBOARD_COMMANDS_COMMANDS_H_
