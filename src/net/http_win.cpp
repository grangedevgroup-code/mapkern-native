#include <windows.h>
#include <winhttp.h>

#include <mutex>
#include <string>
#include <unordered_map>

#include "core/url.hpp"
#include "net/http.hpp"

namespace mapkern {

namespace {

std::wstring widen(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring out(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
  return out;
}

std::string narrow(const wchar_t* s, size_t len) {
  if (len == 0) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, s, static_cast<int>(len), nullptr, 0, nullptr, nullptr);
  std::string out(static_cast<size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, s, static_cast<int>(len), out.data(), n, nullptr, nullptr);
  return out;
}

struct Handle {
  HINTERNET h = nullptr;
  explicit Handle(HINTERNET v) : h(v) {}
  ~Handle() {
    if (h) WinHttpCloseHandle(h);
  }
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
};

class WinHttpClient : public HttpClient {
 public:
  WinHttpClient() {
    session_ = WinHttpOpen(widen(userAgent()).c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                           WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session_)
      session_ = WinHttpOpen(widen(userAgent()).c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                             WINHTTP_NO_PROXY_BYPASS, 0);
    if (session_) {
      DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | 0x00002000;
      WinHttpSetOption(session_, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof protocols);
      DWORD decompression = WINHTTP_DECOMPRESSION_FLAG_ALL;
      WinHttpSetOption(session_, WINHTTP_OPTION_DECOMPRESSION, &decompression, sizeof decompression);
      DWORD http2 = WINHTTP_PROTOCOL_FLAG_HTTP2;
      WinHttpSetOption(session_, WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL, &http2, sizeof http2);
      DWORD max = 32;
      WinHttpSetOption(session_, WINHTTP_OPTION_MAX_CONNS_PER_SERVER, &max, sizeof max);
    }
  }

  ~WinHttpClient() override {
    for (auto& [k, c] : connections_) WinHttpCloseHandle(c);
    if (session_) WinHttpCloseHandle(session_);
  }

  HttpResponse perform(const HttpRequest& request, const CancelToken& cancel) override {
    HttpResponse response;
    if (!session_) {
      response.networkError = true;
      response.error = "WinHTTP is not available";
      return response;
    }
    const std::wstring wurl = widen(request.url);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof parts;
    wchar_t host[512], path[8192];
    parts.lpszHostName = host;
    parts.dwHostNameLength = 512;
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = 8192;
    wchar_t extra[8192];
    parts.lpszExtraInfo = extra;
    parts.dwExtraInfoLength = 8192;
    if (!WinHttpCrackUrl(wurl.c_str(), static_cast<DWORD>(wurl.size()), 0, &parts)) {
      response.networkError = true;
      response.error = "Bad URL " + request.url;
      return response;
    }
    const bool secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
    Handle connect(WinHttpConnect(session_, host, parts.nPort, 0));
    if (!connect.h) {
      response.networkError = true;
      response.error = "Could not connect";
      return response;
    }
    std::wstring object(path, parts.dwUrlPathLength);
    object.append(extra, parts.dwExtraInfoLength);
    Handle req(WinHttpOpenRequest(connect.h, L"GET", object.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                  secure ? WINHTTP_FLAG_SECURE : 0));
    if (!req.h) {
      response.networkError = true;
      response.error = "Could not open request";
      return response;
    }
    const int t = request.timeoutMs > 0 ? request.timeoutMs : 30000;
    WinHttpSetTimeouts(req.h, t, t, t, t);
    std::wstring headers;
    for (const auto& [k, v] : request.headers) headers += widen(k) + L": " + widen(v) + L"\r\n";
    if (request.range)
      headers += L"Range: bytes=" + std::to_wstring(request.range->first) + L"-" +
                 std::to_wstring(request.range->first + request.range->second - 1) + L"\r\n";
    const BOOL sent = WinHttpSendRequest(req.h, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                                         headers.empty() ? 0 : static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    if (!sent || !WinHttpReceiveResponse(req.h, nullptr)) {
      const DWORD err = GetLastError();
      response.timedOut = err == ERROR_WINHTTP_TIMEOUT;
      response.networkError = true;
      response.error = "Request failed (" + std::to_string(err) + ")";
      return response;
    }
    DWORD status = 0, size = sizeof status;
    WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                        WINHTTP_NO_HEADER_INDEX);
    response.status = static_cast<int>(status);
    DWORD rawSize = 0;
    WinHttpQueryHeaders(req.h, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &rawSize, WINHTTP_NO_HEADER_INDEX);
    if (rawSize > 0) {
      std::wstring raw(rawSize / sizeof(wchar_t) + 1, L'\0');
      if (WinHttpQueryHeaders(req.h, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, raw.data(), &rawSize,
                              WINHTTP_NO_HEADER_INDEX)) {
        const std::string text = narrow(raw.c_str(), rawSize / sizeof(wchar_t));
        size_t pos = text.find("\r\n");
        while (pos != std::string::npos && pos + 2 < text.size()) {
          const size_t end = text.find("\r\n", pos + 2);
          const std::string line = text.substr(pos + 2, end == std::string::npos ? std::string::npos : end - pos - 2);
          const size_t colon = line.find(':');
          if (colon != std::string::npos) {
            std::string value = line.substr(colon + 1);
            while (!value.empty() && value.front() == ' ') value.erase(value.begin());
            response.headers.emplace_back(line.substr(0, colon), value);
          }
          pos = end;
        }
      }
    }
    while (true) {
      if (cancel.cancelled()) {
        response.cancelled = true;
        return response;
      }
      DWORD available = 0;
      if (!WinHttpQueryDataAvailable(req.h, &available)) {
        const DWORD err = GetLastError();
        response.timedOut = err == ERROR_WINHTTP_TIMEOUT;
        response.networkError = true;
        response.error = "Body read failed (" + std::to_string(err) + ")";
        return response;
      }
      if (available == 0) break;
      const size_t at = response.body.size();
      response.body.resize(at + available);
      DWORD read = 0;
      if (!WinHttpReadData(req.h, response.body.data() + at, available, &read)) {
        response.networkError = true;
        response.error = "Body read failed";
        return response;
      }
      response.body.resize(at + read);
      if (response.body.size() > (512u << 20)) {
        response.networkError = true;
        response.error = "Response is too large";
        return response;
      }
    }
    return response;
  }

 private:
  HINTERNET session_ = nullptr;
  std::unordered_map<std::wstring, HINTERNET> connections_;
};

}

std::shared_ptr<HttpClient> createPlatformHttpClient() { return std::make_shared<WinHttpClient>(); }

}
