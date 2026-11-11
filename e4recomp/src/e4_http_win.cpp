// e4 - HTTPS POST via WinHTTP.

#include "e4_http.h"

#ifdef _WIN32

#include <windows.h>
#include <winhttp.h>

#include <string>

namespace e4::http {
namespace {

std::wstring Widen(const std::string& s) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
  std::wstring w(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
  return w;
}

struct Handle {
  HINTERNET h = nullptr;
  ~Handle() {
    if (h) WinHttpCloseHandle(h);
  }
};

}  // namespace

Response Post(const std::string& url, const std::vector<std::pair<std::string, std::string>>& headers,
              const std::string& body, int timeout_ms) {
  Response r;
  std::wstring wurl = Widen(url);
  URL_COMPONENTS uc = {};
  uc.dwStructSize = sizeof(uc);
  wchar_t host[256] = {}, path[2048] = {};
  uc.lpszHostName = host;
  uc.dwHostNameLength = DWORD(std::size(host));
  uc.lpszUrlPath = path;
  uc.dwUrlPathLength = DWORD(std::size(path));
  if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) {
    r.error = "bad url";
    return r;
  }
  Handle session{WinHttpOpen(L"E4Recomp/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                             WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
  if (!session.h) {
    r.error = "WinHttpOpen failed";
    return r;
  }
  WinHttpSetTimeouts(session.h, timeout_ms, timeout_ms, timeout_ms, timeout_ms);
  Handle connect{WinHttpConnect(session.h, host, uc.nPort, 0)};
  if (!connect.h) {
    r.error = "connect failed";
    return r;
  }
  Handle request{WinHttpOpenRequest(connect.h, L"POST", path, nullptr, WINHTTP_NO_REFERER,
                                    WINHTTP_DEFAULT_ACCEPT_TYPES,
                                    uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)};
  if (!request.h) {
    r.error = "request failed";
    return r;
  }
  std::wstring header_text;
  for (const auto& [k, v] : headers) header_text += Widen(k) + L": " + Widen(v) + L"\r\n";
  if (!WinHttpSendRequest(request.h, header_text.c_str(), DWORD(header_text.size()),
                          const_cast<char*>(body.data()), DWORD(body.size()), DWORD(body.size()),
                          0) ||
      !WinHttpReceiveResponse(request.h, nullptr)) {
    r.error = "send failed (" + std::to_string(GetLastError()) + ")";
    return r;
  }
  DWORD status = 0, size = sizeof(status);
  WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                      WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
  for (;;) {
    DWORD avail = 0;
    if (!WinHttpQueryDataAvailable(request.h, &avail) || !avail) break;
    std::string chunk(avail, '\0');
    DWORD read = 0;
    if (!WinHttpReadData(request.h, chunk.data(), avail, &read) || !read) break;
    r.body.append(chunk.data(), read);
  }
  r.status = int(status);
  return r;
}

}  // namespace e4::http

#endif  // _WIN32
