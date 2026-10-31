#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <variant>

#include "core/async.hpp"
#include "formats/bytes.hpp"
#include "tiles/cache.hpp"

namespace mapkern {

enum class ResourceKind { Style, Source, Tile, Glyphs, Sprite, Image };

enum class FailureKind { Network, Http, Auth, RateLimit, Timeout, Decode };

struct TileFailure {
  FailureKind kind = FailureKind::Network;
  int status = 0;
  std::string message;
  bool retryable = false;
  double retryAfterMs = 0;
};

struct RequestParameters {
  std::string url;
  std::vector<std::pair<std::string, std::string>> headers;
};

using TransformRequest = std::function<std::optional<RequestParameters>(const std::string& url, ResourceKind kind)>;

struct FetchResult {
  bool empty = true;
  SharedBytes data;
  std::string contentType;
  bool fromCache = false;
};

using FetchOutcome = std::variant<FetchResult, TileFailure>;
using FetchCallback = std::function<void(FetchOutcome)>;

struct ProtocolRequest {
  std::string url;
  ResourceKind kind;
  std::vector<std::pair<std::string, std::string>> headers;
};

using ProtocolHandler = std::function<FetchOutcome(const ProtocolRequest& request, const CancelToken& cancel)>;

void addProtocol(const std::string& scheme, ProtocolHandler handler);
void addBuiltinProtocol(const std::string& scheme, ProtocolHandler handler);
void removeProtocol(const std::string& scheme);
std::optional<ProtocolHandler> protocolFor(const std::string& url);

RequestParameters resolveRequest(const std::string& url, ResourceKind kind, const TransformRequest& transform);
void fetchBytes(const RequestParameters& params, CancelToken cancel, int timeoutMs, ResourceKind kind,
                std::shared_ptr<ResponseStore> store, FetchCallback callback);
TileFailure classifyStatus(int status, const std::string& retryAfter);
std::optional<double> parseRetryAfter(const std::string& value, double now);
double retryDelay(int attempt, const TileFailure& failure);
std::string expandTemplate(const std::string& tmpl, int z, int x, int y,
                           const std::function<std::optional<std::string>(const std::string&)>& extra = {});
bool isLocalPath(const std::string& url);
std::string localPath(const std::string& url);

}
