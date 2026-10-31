#include "net/http.hpp"

#include <atomic>
#include <cctype>
#include <mutex>

namespace mapkern {

namespace {

std::mutex factoryMutex;
HttpFactory factory;
std::shared_ptr<HttpClient> client;
std::string agent = "Mapkern Native/0.1";
std::atomic<bool> reachable{true};

ThreadPool& networkPool() {
  static ThreadPool pool(24, "mapkern-net");
  return pool;
}

bool equalsIgnoreCase(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); i++)
    if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
  return true;
}

}

std::string HttpResponse::header(std::string_view name) const {
  for (const auto& [k, v] : headers)
    if (equalsIgnoreCase(k, name)) return v;
  return "";
}

void setHttpClientFactory(HttpFactory f) {
  std::lock_guard lock(factoryMutex);
  factory = std::move(f);
  client.reset();
}

std::shared_ptr<HttpClient> httpClient() {
  std::lock_guard lock(factoryMutex);
  if (!client) client = factory ? factory() : createPlatformHttpClient();
  return client;
}

void httpRequest(HttpRequest request, CancelToken cancel, HttpCallback callback) {
  networkPool().submit([request = std::move(request), cancel, callback = std::move(callback)]() {
    HttpResponse response;
    if (cancel.cancelled()) {
      response.cancelled = true;
    } else {
      try {
        response = httpClient()->perform(request, cancel);
      } catch (const std::exception& e) {
        response.networkError = true;
        response.error = e.what();
      }
      if (cancel.cancelled()) response.cancelled = true;
    }
    callback(std::move(response));
  });
}

std::string userAgent() {
  std::lock_guard lock(factoryMutex);
  return agent;
}

void setUserAgent(std::string a) {
  std::lock_guard lock(factoryMutex);
  agent = std::move(a);
}

bool networkReachable() { return reachable.load(); }
void setNetworkReachable(bool r) { reachable.store(r); }

}
