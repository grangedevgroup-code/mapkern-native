#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/async.hpp"
#include "formats/bytes.hpp"

namespace mapkern {

struct HttpRequest {
  std::string url;
  std::vector<std::pair<std::string, std::string>> headers;
  std::optional<std::pair<uint64_t, uint64_t>> range;
  int timeoutMs = 30000;
};

struct HttpResponse {
  int status = 0;
  std::vector<std::pair<std::string, std::string>> headers;
  Bytes body;
  bool networkError = false;
  bool timedOut = false;
  bool cancelled = false;
  std::string error;
  std::string header(std::string_view name) const;
};

using HttpCallback = std::function<void(HttpResponse)>;

class HttpClient {
 public:
  virtual ~HttpClient() = default;
  virtual HttpResponse perform(const HttpRequest& request, const CancelToken& cancel) = 0;
};

using HttpFactory = std::function<std::shared_ptr<HttpClient>()>;

std::shared_ptr<HttpClient> createPlatformHttpClient();
void setHttpClientFactory(HttpFactory factory);
std::shared_ptr<HttpClient> httpClient();
void httpRequest(HttpRequest request, CancelToken cancel, HttpCallback callback);
std::string userAgent();
void setUserAgent(std::string agent);
bool networkReachable();
void setNetworkReachable(bool reachable);

}
