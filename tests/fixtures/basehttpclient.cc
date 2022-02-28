namespace fixtures {

class BaseHttpClient: public HttpInterface {
 public:
  HttpResponse get(const std::string &url, int64_t maxsize) override { return HttpResponse("", 500, CURLE_OK, "not supported"); }
  HttpResponse post(const std::string&, const std::string&, const std::string&, bool follow_redirect = false) override { return HttpResponse("", 500, CURLE_OK, "not supported"); }
  HttpResponse post(const std::string&, const Json::Value&, bool follow_redirect = false) override { return HttpResponse("", 500, CURLE_OK, "not supported"); }
  HttpResponse put(const std::string&, const std::string&, const std::string&) override { return HttpResponse("", 500, CURLE_OK, "not supported"); }
  HttpResponse put(const std::string&, const Json::Value&) override { return HttpResponse("", 500, CURLE_OK, "not supported"); }

  HttpResponse download(const std::string &url, curl_write_callback write_cb, curl_xferinfo_callback progress_cb, void *userp, curl_off_t from) override {
    return HttpResponse("resp", 200, CURLE_OK, "not supported");
  }
  std::future<HttpResponse> downloadAsync(const std::string&, curl_write_callback, curl_xferinfo_callback, void*, curl_off_t, CurlHandler*) override {
    std::promise<HttpResponse> resp_promise;
    resp_promise.set_value(HttpResponse("", 500, CURLE_OK, ""));
    return resp_promise.get_future();
  }
  void setCerts(const std::string&, CryptoSource, const std::string&, CryptoSource, const std::string&, CryptoSource) override {}
};

} // namespace fixtures
