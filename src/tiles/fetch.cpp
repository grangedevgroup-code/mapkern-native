#include "tiles/fetch.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <mutex>
#include <random>
#include <unordered_map>

#include "core/url.hpp"
#include "diag/diagnostics.hpp"
#include "net/http.hpp"

namespace mapkern {

namespace {

std::mutex protocolMutex;
std::unordered_map<std::string, ProtocolHandler> handlers;
std::unordered_map<std::string, ProtocolHandler> builtins;

std::string normalizeScheme(std::string scheme) {
  while (!scheme.empty() && (scheme.back() == '/' || scheme.back() == ':')) scheme.pop_back();
  for (auto& c : scheme) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return scheme;
}

std::string checkedScheme(const std::string& scheme) {
  const std::string key = normalizeScheme(scheme);
  bool ok = !key.empty() && std::isalpha(static_cast<unsigned char>(key[0]));
  for (char c : key) ok = ok && (std::isalnum(static_cast<unsigned char>(c)) || c == '+' || c == '.' || c == '-');
  if (!ok) throw std::invalid_argument("Invalid protocol name \"" + scheme + "\"");
  if (key == "http" || key == "https") throw std::invalid_argument("http and https cannot be replaced");
  return key;
}

bool transient(const TileFailure& f) {
  return f.kind == FailureKind::Network || f.kind == FailureKind::Timeout || f.kind == FailureKind::RateLimit ||
         (f.kind == FailureKind::Http && f.status >= 500);
}

double randomUnit() {
  static thread_local std::mt19937 rng{std::random_device{}()};
  return std::uniform_real_distribution<double>(0, 1)(rng);
}

FetchOutcome fileOutcome(const std::string& path) {
  auto bytes = readFile(path);
  if (!bytes) return FetchResult{};
  if (bytes->empty()) return FetchResult{};
  FetchResult r;
  r.empty = false;
  r.data = std::make_shared<const Bytes>(std::move(*bytes));
  return r;
}

void fetchNetwork(const RequestParameters& params, CancelToken cancel, int timeoutMs, std::shared_ptr<ResponseStore> cache,
                  std::function<void(FetchOutcome)> done) {
  HttpRequest request;
  request.url = params.url;
  request.headers = params.headers;
  request.timeoutMs = timeoutMs;
  const double started = diag::steadyMs();
  httpRequest(std::move(request), cancel, [url = params.url, timeoutMs, cache, started, done = std::move(done)](HttpResponse response) {
    const double ms = diag::steadyMs() - started;
    const uint64_t bytes = response.body.size();
    if (response.cancelled) {
      diag::response(diag::NetResult::Cancelled, bytes, ms);
      TileFailure f;
      f.kind = FailureKind::Network;
      f.message = "cancelled";
      f.retryable = false;
      done(f);
      return;
    }
    if (response.networkError) {
      diag::response(response.timedOut ? diag::NetResult::Timeout : diag::NetResult::Error, bytes, ms);
      TileFailure f;
      f.kind = response.timedOut ? FailureKind::Timeout : FailureKind::Network;
      f.message = response.timedOut ? "Request timed out after " + std::to_string(timeoutMs) + " ms"
                                    : "Network request failed: " + response.error;
      f.retryable = true;
      done(f);
      return;
    }
    const int status = response.status;
    if (status == 204 || status == 404 || status == 410) {
      diag::response(diag::NetResult::Empty, bytes, ms);
      done(FetchResult{});
      return;
    }
    if (status < 200 || status >= 300) {
      diag::response(status >= 500 ? diag::NetResult::ServerError : diag::NetResult::ClientError, bytes, ms);
      done(classifyStatus(status, response.header("retry-after")));
      return;
    }
    diag::response(response.body.empty() ? diag::NetResult::Empty : diag::NetResult::Ok, bytes, ms);
    if (response.body.empty()) {
      done(FetchResult{});
      return;
    }
    FetchResult r;
    r.empty = false;
    r.contentType = response.header("content-type");
    r.data = std::make_shared<const Bytes>(std::move(response.body));
    if (cache) {
      HeaderLookup lookup{[&response](std::string_view name) { return response.header(name); }};
      if (auto expires = freshUntil(lookup, wallMs())) cache->write(url, r.data, r.contentType, *expires);
    }
    done(r);
  });
}

}

void addProtocol(const std::string& scheme, ProtocolHandler handler) {
  const std::string key = checkedScheme(scheme);
  std::lock_guard lock(protocolMutex);
  handlers[key] = std::move(handler);
}

void addBuiltinProtocol(const std::string& scheme, ProtocolHandler handler) {
  const std::string key = checkedScheme(scheme);
  std::lock_guard lock(protocolMutex);
  builtins[key] = std::move(handler);
}

void removeProtocol(const std::string& scheme) {
  std::lock_guard lock(protocolMutex);
  handlers.erase(normalizeScheme(scheme));
}

std::optional<ProtocolHandler> protocolFor(const std::string& url) {
  const size_t i = url.find("://");
  if (i == std::string::npos || i == 0) return std::nullopt;
  const std::string scheme = normalizeScheme(url.substr(0, i));
  std::lock_guard lock(protocolMutex);
  if (auto it = handlers.find(scheme); it != handlers.end()) return it->second;
  if (auto it = builtins.find(scheme); it != builtins.end()) return it->second;
  return std::nullopt;
}

bool isLocalPath(const std::string& url) {
  if (url.starts_with("file://")) return true;
  if (url.size() > 2 && std::isalpha(static_cast<unsigned char>(url[0])) && url[1] == ':' && (url[2] == '\\' || url[2] == '/'))
    return true;
  if (url.starts_with("/")) return true;
  return !hasScheme(url) && !url.starts_with("//");
}

std::string localPath(const std::string& url) {
  if (url.starts_with("file:///")) {
    std::string p = percentDecode(url.substr(7));
    if (p.size() > 3 && p[0] == '/' && std::isalpha(static_cast<unsigned char>(p[1])) && p[2] == ':') p = p.substr(1);
    return p;
  }
  if (url.starts_with("file://")) return percentDecode(url.substr(7));
  return url;
}

RequestParameters resolveRequest(const std::string& url, ResourceKind kind, const TransformRequest& transform) {
  if (transform) {
    if (auto custom = transform(url, kind)) return *custom;
  }
  return {url, {}};
}

void fetchBytes(const RequestParameters& params, CancelToken cancel, int timeoutMs, ResourceKind kind,
                std::shared_ptr<ResponseStore> store, FetchCallback callback) {
  if (auto handler = protocolFor(params.url)) {
    workerPool().submit([handler = *handler, params, kind, cancel, callback]() {
      FetchOutcome out;
      try {
        out = handler(ProtocolRequest{params.url, kind, params.headers}, cancel);
      } catch (const std::exception& e) {
        TileFailure f;
        f.kind = FailureKind::Network;
        f.message = e.what();
        f.retryable = true;
        out = f;
      }
      callback(std::move(out));
    });
    return;
  }
  if (isLocalPath(params.url)) {
    workerPool().submit([path = localPath(params.url), callback]() { callback(fileOutcome(path)); });
    return;
  }
  std::shared_ptr<ResponseStore> cache = store && cacheable(params.url) ? store : nullptr;
  if (!cache) {
    fetchNetwork(params, cancel, timeoutMs, nullptr, callback);
    return;
  }
  workerPool().submit([params, cancel, timeoutMs, cache, callback]() {
    const StoredState state = cache->state(params.url);
    const double now = wallMs();
    if (state == StoredState::Fresh || (state != StoredState::Missing && (offline() || serverDown(params.url, now)))) {
      if (auto hit = cache->read(params.url)) {
        FetchResult r;
        r.empty = false;
        r.data = hit->data;
        r.contentType = hit->type;
        r.fromCache = true;
        callback(r);
        return;
      }
    }
    if (offline() && state == StoredState::Unknown) {
      if (auto hit = cache->read(params.url)) {
        FetchResult r;
        r.empty = false;
        r.data = hit->data;
        r.contentType = hit->type;
        r.fromCache = true;
        callback(r);
        return;
      }
    }
    fetchNetwork(params, cancel, timeoutMs, cache, [params, cache, callback](FetchOutcome out) {
      if (auto* f = std::get_if<TileFailure>(&out)) {
        if (f->message == "cancelled" || !transient(*f)) {
          callback(std::move(out));
          return;
        }
        markServer(params.url, false, wallMs());
        if (cache->state(params.url) != StoredState::Missing) {
          if (auto hit = cache->read(params.url)) {
            FetchResult r;
            r.empty = false;
            r.data = hit->data;
            r.contentType = hit->type;
            r.fromCache = true;
            callback(r);
            return;
          }
        }
        callback(std::move(out));
        return;
      }
      markServer(params.url, true, wallMs());
      callback(std::move(out));
    });
  });
}

TileFailure classifyStatus(int status, const std::string& retryAfter) {
  TileFailure f;
  f.status = status;
  if (status == 401 || status == 403) {
    f.kind = FailureKind::Auth;
    f.message = "Access denied (HTTP " + std::to_string(status) + ")";
    f.retryable = false;
    return f;
  }
  if (status == 429) {
    f.kind = FailureKind::RateLimit;
    f.message = "Rate limited (HTTP 429)";
    f.retryable = true;
    if (auto wait = parseRetryAfter(retryAfter, wallMs())) f.retryAfterMs = *wait;
    return f;
  }
  f.kind = FailureKind::Http;
  f.message = "HTTP " + std::to_string(status);
  f.retryable = status >= 500 || status == 408 || status == 425;
  if (status == 503) {
    if (auto wait = parseRetryAfter(retryAfter, wallMs())) f.retryAfterMs = *wait;
  }
  return f;
}

std::optional<double> parseRetryAfter(const std::string& value, double now) {
  std::string v = value;
  while (!v.empty() && std::isspace(static_cast<unsigned char>(v.front()))) v.erase(v.begin());
  while (!v.empty() && std::isspace(static_cast<unsigned char>(v.back()))) v.pop_back();
  if (v.empty()) return std::nullopt;
  if (std::all_of(v.begin(), v.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); }))
    return std::min(std::stod(v) * 1000, 10.0 * 60 * 1000);
  HeaderLookup lookup{[&v](std::string_view name) { return name == "expires" ? v : std::string(); }};
  auto date = freshUntil(lookup, now);
  if (!date || *date == 0) return std::nullopt;
  return std::clamp(*date - now, 0.0, 10.0 * 60 * 1000);
}

double retryDelay(int attempt, const TileFailure& failure) {
  const double base = std::min(30000.0, 1000 * std::exp2(std::max(0, attempt - 1)));
  const double jittered = base * (0.8 + 0.4 * randomUnit());
  return std::max(jittered, failure.retryAfterMs);
}

std::string expandTemplate(const std::string& tmpl, int z, int x, int y,
                           const std::function<std::optional<std::string>(const std::string&)>& extra) {
  std::string out;
  size_t i = 0;
  while (i < tmpl.size()) {
    if (tmpl[i] == '{') {
      const size_t close = tmpl.find('}', i + 1);
      if (close != std::string::npos) {
        const std::string key = tmpl.substr(i + 1, close - i - 1);
        bool valid = !key.empty();
        for (char c : key) valid = valid && (std::isalpha(static_cast<unsigned char>(c)) || c == '-');
        if (valid) {
          std::optional<std::string> value;
          if (key == "z") value = std::to_string(z);
          else if (key == "x") value = std::to_string(x);
          else if (key == "y") value = std::to_string(y);
          else if (key == "-y") value = std::to_string((int64_t{1} << z) - 1 - y);
          else if (extra) value = extra(key);
          if (value) {
            out += *value;
            i = close + 1;
            continue;
          }
        }
      }
    }
    out += tmpl[i++];
  }
  return out;
}

}
