#import <Foundation/Foundation.h>

#include <atomic>

#include "net/http.hpp"

namespace mapkern {

namespace {

class AppleHttpClient : public HttpClient {
 public:
  AppleHttpClient() {
    @autoreleasepool {
      NSURLSessionConfiguration* config = [NSURLSessionConfiguration defaultSessionConfiguration];
      config.HTTPMaximumConnectionsPerHost = 16;
      config.requestCachePolicy = NSURLRequestReloadIgnoringLocalCacheData;
      config.URLCache = nil;
      config.HTTPAdditionalHeaders = @{@"User-Agent" : [NSString stringWithUTF8String:userAgent().c_str()]};
      session_ = [NSURLSession sessionWithConfiguration:config];
    }
  }

  HttpResponse perform(const HttpRequest& request, const CancelToken& cancel) override {
    __block HttpResponse response;
    @autoreleasepool {
      NSURL* url = [NSURL URLWithString:[NSString stringWithUTF8String:request.url.c_str()]];
      if (!url) {
        response.networkError = true;
        response.error = "Bad URL " + request.url;
        return response;
      }
      NSMutableURLRequest* req = [NSMutableURLRequest requestWithURL:url];
      req.timeoutInterval = (request.timeoutMs > 0 ? request.timeoutMs : 30000) / 1000.0;
      for (const auto& [k, v] : request.headers)
        [req setValue:[NSString stringWithUTF8String:v.c_str()] forHTTPHeaderField:[NSString stringWithUTF8String:k.c_str()]];
      if (request.range) {
        NSString* range = [NSString stringWithFormat:@"bytes=%llu-%llu", (unsigned long long)request.range->first,
                                                     (unsigned long long)(request.range->first + request.range->second - 1)];
        [req setValue:range forHTTPHeaderField:@"Range"];
      }
      dispatch_semaphore_t done = dispatch_semaphore_create(0);
      NSURLSessionDataTask* task =
          [session_ dataTaskWithRequest:req
                      completionHandler:^(NSData* data, NSURLResponse* r, NSError* error) {
                        if (error) {
                          response.cancelled = error.code == NSURLErrorCancelled;
                          response.timedOut = error.code == NSURLErrorTimedOut;
                          response.networkError = !response.cancelled;
                          response.error = error.localizedDescription.UTF8String ?: "network error";
                        }
                        if ([r isKindOfClass:[NSHTTPURLResponse class]]) {
                          NSHTTPURLResponse* http = (NSHTTPURLResponse*)r;
                          response.status = (int)http.statusCode;
                          [http.allHeaderFields enumerateKeysAndObjectsUsingBlock:^(id key, id value, BOOL*) {
                            response.headers.emplace_back([[key description] UTF8String], [[value description] UTF8String]);
                          }];
                        }
                        if (data.length > 0) {
                          const uint8_t* bytes = (const uint8_t*)data.bytes;
                          response.body.assign(bytes, bytes + data.length);
                        }
                        dispatch_semaphore_signal(done);
                      }];
      [task resume];
      while (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 50 * NSEC_PER_MSEC)) != 0) {
        if (cancel.cancelled()) [task cancel];
      }
    }
    return response;
  }

 private:
  NSURLSession* session_;
};

}

std::shared_ptr<HttpClient> createPlatformHttpClient() { return std::make_shared<AppleHttpClient>(); }

}
