// e4 - minimal blocking HTTPS POST (WinHTTP on Windows, NSURLSession on macOS).
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace e4::http {

struct Response {
  int status = 0;    // HTTP status, 0 = no response (offline, DNS, TLS, timeout)
  std::string body;
  std::string error;  // set when status == 0
};

// POST `body` to an https:// URL. Blocks for at most about `timeout_ms`.
Response Post(const std::string& url, const std::vector<std::pair<std::string, std::string>>& headers,
              const std::string& body, int timeout_ms);

}  // namespace e4::http
