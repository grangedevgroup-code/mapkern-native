#include "net/http.hpp"

namespace mapkern {

namespace {

class MissingClient : public HttpClient {
 public:
  HttpResponse perform(const HttpRequest&, const CancelToken&) override {
    HttpResponse r;
    r.networkError = true;
    r.error = "No HTTP client has been installed, call setHttpClientFactory from the Java side";
    return r;
  }
};

}

std::shared_ptr<HttpClient> createPlatformHttpClient() { return std::make_shared<MissingClient>(); }

}
