#include "src/server/ai_http_client.h"

#include <curl/curl.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include "src/core/error.h"

namespace gitboard::server {
namespace {

struct curl_global_state {
  curl_global_state() {
    CURLcode code = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (code != CURLE_OK) {
      throw gitboard::error("libcurl global initialization failed");
    }
  }

  ~curl_global_state() { curl_global_cleanup(); }
};

void ensure_curl_global_initialized() {
  static curl_global_state state;
}

std::size_t append_response(char* ptr, std::size_t size, std::size_t nmemb,
                            void* userdata) {
  auto* output = static_cast<std::string*>(userdata);
  std::size_t bytes = size * nmemb;
  output->append(ptr, bytes);
  return bytes;
}

bool should_retry(CURLcode code, long http_status) {
  if (code == CURLE_COULDNT_CONNECT ||
      code == CURLE_COULDNT_RESOLVE_HOST ||
      code == CURLE_COULDNT_RESOLVE_PROXY ||
      code == CURLE_GOT_NOTHING ||
      code == CURLE_OPERATION_TIMEDOUT ||
      code == CURLE_RECV_ERROR ||
      code == CURLE_SEND_ERROR) {
    return true;
  }
  return http_status == 408 || http_status == 429 || http_status == 500 ||
         http_status == 502 || http_status == 503 || http_status == 504;
}

struct curl_slist_deleter {
  void operator()(curl_slist* list) const { curl_slist_free_all(list); }
};

using curl_slist_ptr = std::unique_ptr<curl_slist, curl_slist_deleter>;

}  // namespace

process_result post_ai_http_json(const ai_config& config,
                                 const ai_http_request& request) {
  ensure_curl_global_initialized();

  int timeout = config.timeout_seconds > 0 ? config.timeout_seconds : 30;
  int retries = config.retry_attempts >= 0 ? config.retry_attempts : 5;
  int attempts = std::max(1, retries + 1);

  process_result result;
  CURLcode last_code = CURLE_OK;
  char error_buffer[CURL_ERROR_SIZE] = {};

  for (int attempt = 0; attempt < attempts; ++attempt) {
    result = {};
    error_buffer[0] = '\0';

    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(),
                                                             curl_easy_cleanup);
    if (!curl) throw gitboard::error("failed to create libcurl handle");

    curl_slist* raw_headers = nullptr;
    for (const std::string& header : request.headers) {
      curl_slist* next = curl_slist_append(raw_headers, header.c_str());
      if (!next) {
        curl_slist_free_all(raw_headers);
        throw gitboard::error("failed to allocate HTTP header");
      }
      raw_headers = next;
    }
    curl_slist_ptr headers(raw_headers);

    curl_easy_setopt(curl.get(), CURLOPT_URL, request.endpoint.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.get());
    curl_easy_setopt(curl.get(), CURLOPT_POST, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, request.payload.data());
    curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE_LARGE,
                     static_cast<curl_off_t>(request.payload.size()));
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, static_cast<long>(timeout));
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, append_response);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &result.output);
    curl_easy_setopt(curl.get(), CURLOPT_ERRORBUFFER, error_buffer);
    curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);

    last_code = curl_easy_perform(curl.get());

    long http_status = 0;
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &http_status);
    if (last_code == CURLE_OK && http_status >= 400) {
      result.exit_code = 22;
    } else {
      result.exit_code = last_code == CURLE_OK ? 0 : static_cast<int>(last_code);
    }

    bool retry = attempt + 1 < attempts && should_retry(last_code, http_status);
    if (!retry) break;

    auto delay = std::chrono::milliseconds(250 * (attempt + 1));
    std::this_thread::sleep_for(delay);
  }

  if (result.exit_code != 0 && result.output.empty()) {
    result.output = error_buffer[0] != '\0' ? error_buffer
                                            : curl_easy_strerror(last_code);
  }
  return result;
}

}  // namespace gitboard::server
