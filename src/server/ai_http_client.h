#ifndef GITBOARD_SERVER_AI_HTTP_CLIENT_H_
#define GITBOARD_SERVER_AI_HTTP_CLIENT_H_

#include "src/server/ai_agent.h"
#include "src/server/config.h"
#include "src/server/process.h"

namespace gitboard::server {

process_result post_ai_http_json(const ai_config& config,
                                 const ai_http_request& request);

}  // namespace gitboard::server

#endif  // GITBOARD_SERVER_AI_HTTP_CLIENT_H_
