#include <curl/curl.h>

#include <mutex>

#include "net/http.hpp"

namespace mapkern {

namespace {

struct Transfer {
  HttpResponse* response;
  const CancelToken* cancel;
};

size_t onBody(char* data, size_t size, size_t count, void* user) {
  auto* t = static_cast<Transfer*>(user);
  const size_t n = size * count;
  t->response->body.insert(t->response->body.end(), data, data + n);
  if (t->response->body.size() > (512u << 20)) return 0;
  return n;
}

size_t onHeader(char* data, size_t size, size_t count, void* user) {
  auto* t = static_cast<Transfer*>(user);
  const size_t n = size * count;
  std::string line(data, n);
  while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
  if (line.starts_with("HTTP/")) {
    t->response->headers.clear();
    return n;
  }
  const size_t colon = line.find(':');
  if (colon != std::string::npos) {
    std::string value = line.substr(colon + 1);
    while (!value.empty() && value.front() == ' ') value.erase(value.begin());
    t->response->headers.emplace_back(line.substr(0, colon), value);
  }
  return n;
}

int onProgress(void* user, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
  return static_cast<Transfer*>(user)->cancel->cancelled() ? 1 : 0;
}

class CurlClient : public HttpClient {
 public:
  CurlClient() {
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
    share_ = curl_share_init();
    curl_share_setopt(share_, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
    curl_share_setopt(share_, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);
    curl_share_setopt(share_, CURLSHOPT_LOCKFUNC, lock);
    curl_share_setopt(share_, CURLSHOPT_UNLOCKFUNC, unlock);
  }

  ~CurlClient() override { curl_share_cleanup(share_); }

  HttpResponse perform(const HttpRequest& request, const CancelToken& cancel) override {
    HttpResponse response;
    CURL* curl = curl_easy_init();
    if (!curl) {
      response.networkError = true;
      response.error = "curl is not available";
      return response;
    }
    Transfer t{&response, &cancel};
    curl_easy_setopt(curl, CURLOPT_URL, request.url.c_str());
    curl_easy_setopt(curl, CURLOPT_SHARE, share_);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 8L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_USERAGENT, userAgent().c_str());
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(request.timeoutMs > 0 ? request.timeoutMs : 30000));
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 15000L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, onBody);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &t);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, onHeader);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &t);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, onProgress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &t);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
#ifdef CURL_HTTP_VERSION_2TLS
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2TLS);
#endif
    curl_slist* headers = nullptr;
    for (const auto& [k, v] : request.headers) headers = curl_slist_append(headers, (k + ": " + v).c_str());
    if (headers) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    std::string range;
    if (request.range) {
      range = std::to_string(request.range->first) + "-" + std::to_string(request.range->first + request.range->second - 1);
      curl_easy_setopt(curl, CURLOPT_RANGE, range.c_str());
    }
    const CURLcode code = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    response.status = static_cast<int>(status);
    if (code != CURLE_OK) {
      response.cancelled = code == CURLE_ABORTED_BY_CALLBACK;
      response.timedOut = code == CURLE_OPERATION_TIMEDOUT;
      response.networkError = !response.cancelled;
      response.error = curl_easy_strerror(code);
    }
    if (headers) curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return response;
  }

 private:
  static void lock(CURL*, curl_lock_data data, curl_lock_access, void*) { locks()[data % 8].lock(); }
  static void unlock(CURL*, curl_lock_data data, void*) { locks()[data % 8].unlock(); }
  static std::mutex* locks() {
    static std::mutex m[8];
    return m;
  }
  CURLSH* share_ = nullptr;
};

}

std::shared_ptr<HttpClient> createPlatformHttpClient() { return std::make_shared<CurlClient>(); }

}
