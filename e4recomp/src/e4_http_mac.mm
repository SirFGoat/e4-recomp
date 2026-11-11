// e4 - HTTPS POST via NSURLSession (macOS).

#include "e4_http.h"

#import <Foundation/Foundation.h>

namespace e4::http {

Response Post(const std::string& url, const std::vector<std::pair<std::string, std::string>>& headers,
              const std::string& body, int timeout_ms) {
  Response r;
  @autoreleasepool {
    NSURL* nsurl = [NSURL URLWithString:[NSString stringWithUTF8String:url.c_str()]];
    if (!nsurl) {
      r.error = "bad url";
      return r;
    }
    NSMutableURLRequest* request = [NSMutableURLRequest requestWithURL:nsurl];
    request.HTTPMethod = @"POST";
    request.timeoutInterval = timeout_ms / 1000.0;
    for (const auto& [k, v] : headers) {
      [request setValue:[NSString stringWithUTF8String:v.c_str()]
          forHTTPHeaderField:[NSString stringWithUTF8String:k.c_str()]];
    }
    request.HTTPBody = [NSData dataWithBytes:body.data() length:body.size()];
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    __block Response* out = &r;
    NSURLSessionDataTask* task = [[NSURLSession sharedSession]
        dataTaskWithRequest:request
          completionHandler:^(NSData* data, NSURLResponse* response, NSError* error) {
            if (error) {
              out->error = error.localizedDescription.UTF8String ?: "error";
            } else {
              out->status = int(((NSHTTPURLResponse*)response).statusCode);
              if (data) out->body.assign((const char*)data.bytes, data.length);
            }
            dispatch_semaphore_signal(done);
          }];
    [task resume];
    if (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW,
                                                    int64_t(timeout_ms + 2000) * NSEC_PER_MSEC))) {
      [task cancel];
      dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
    }
  }
  return r;
}

}  // namespace e4::http
