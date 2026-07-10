#include <gtest/gtest.h>

#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "InkHoardClient.h"

namespace {

std::string readFixture(const char* name) {
  std::string path = std::string(INKHOARD_FIXTURE_DIR) + "/" + name;
  std::ifstream in(path, std::ios::binary);
  EXPECT_TRUE(in.good()) << path;
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

class MockTransport : public inkhoard::InkHoardTransport {
 public:
  struct Scripted {
    inkhoard::TransportResponse resp;
  };
  std::vector<Scripted> script;
  size_t calls = 0;
  std::vector<std::string> urls;

  inkhoard::TransportResponse getJson(const std::string& url, const std::string& bearerToken) override {
    (void)bearerToken;
    urls.push_back(url);
    if (calls >= script.size()) {
      inkhoard::TransportResponse r;
      r.transportFailure = true;
      return r;
    }
    return script[calls++].resp;
  }
};

inkhoard::TransportResponse okBody(const std::string& body, int code = 200) {
  inkhoard::TransportResponse r;
  r.ok = true;
  r.httpCode = code;
  r.body = body;
  return r;
}

inkhoard::TransportResponse failTransport() {
  inkhoard::TransportResponse r;
  r.transportFailure = true;
  return r;
}

inkhoard::TransportResponse failTls() {
  inkhoard::TransportResponse r;
  r.tlsFailure = true;
  return r;
}

}  // namespace

TEST(InkHoardTransport, MapHttpStatus) {
  EXPECT_EQ(inkhoard::mapHttpStatus(200), inkhoard::ClientResult::Ok);
  EXPECT_EQ(inkhoard::mapHttpStatus(401), inkhoard::ClientResult::AuthInvalid);
  EXPECT_EQ(inkhoard::mapHttpStatus(403), inkhoard::ClientResult::AuthForbidden);
  EXPECT_EQ(inkhoard::mapHttpStatus(404), inkhoard::ClientResult::NotFound);
  EXPECT_EQ(inkhoard::mapHttpStatus(409), inkhoard::ClientResult::ContentNotReady);
  EXPECT_EQ(inkhoard::mapHttpStatus(422), inkhoard::ClientResult::ContentFailed);
  EXPECT_EQ(inkhoard::mapHttpStatus(400), inkhoard::ClientResult::BadRequest);
  EXPECT_EQ(inkhoard::mapHttpStatus(500), inkhoard::ClientResult::ServerError);
  EXPECT_EQ(inkhoard::mapHttpStatus(304), inkhoard::ClientResult::NotModified);
}

TEST(InkHoardTransport, ShouldRetryOnlyTransient) {
  EXPECT_TRUE(inkhoard::shouldRetry(inkhoard::ClientResult::TransportError));
  EXPECT_TRUE(inkhoard::shouldRetry(inkhoard::ClientResult::TlsError));
  EXPECT_TRUE(inkhoard::shouldRetry(inkhoard::ClientResult::ServerError));
  EXPECT_FALSE(inkhoard::shouldRetry(inkhoard::ClientResult::AuthInvalid));
  EXPECT_FALSE(inkhoard::shouldRetry(inkhoard::ClientResult::AuthForbidden));
  EXPECT_FALSE(inkhoard::shouldRetry(inkhoard::ClientResult::NotFound));
  EXPECT_FALSE(inkhoard::shouldRetry(inkhoard::ClientResult::Ok));
}

TEST(InkHoardClient, FetchLibraryPageParsesFixture) {
  MockTransport t;
  t.script.push_back({okBody(readFixture("library-page.json"))});
  InkHoardClient client(&t);
  std::unique_ptr<inkhoard::LibraryPage> page;
  const auto o = client.fetchLibraryPage(page);
  ASSERT_EQ(o.result, inkhoard::ClientResult::Ok);
  ASSERT_TRUE(page);
  EXPECT_EQ(page->itemCount, 20);
  EXPECT_EQ(o.attempts, 1);
  ASSERT_EQ(t.urls.size(), 1u);
  EXPECT_NE(t.urls[0].find("/api/device/v1/library?limit=20"), std::string::npos);
}

TEST(InkHoardClient, AuthInvalidNoRetry) {
  MockTransport t;
  t.script.push_back({okBody(readFixture("error-unauthorized.json"), 401)});
  t.script.push_back({okBody(readFixture("library-page.json"))});  // must not be consumed
  InkHoardClient client(&t);
  std::unique_ptr<inkhoard::LibraryPage> page;
  const auto o = client.fetchLibraryPage(page);
  EXPECT_EQ(o.result, inkhoard::ClientResult::AuthInvalid);
  EXPECT_EQ(o.attempts, 1);
  EXPECT_EQ(t.calls, 1u);
  EXPECT_STREQ(o.error.code, "unauthorized");
}

TEST(InkHoardClient, AuthForbiddenNoRetry) {
  MockTransport t;
  t.script.push_back({okBody(readFixture("error-forbidden.json"), 403)});
  InkHoardClient client(&t);
  std::unique_ptr<inkhoard::LibraryPage> page;
  const auto o = client.fetchLibraryPage(page);
  EXPECT_EQ(o.result, inkhoard::ClientResult::AuthForbidden);
  EXPECT_EQ(o.attempts, 1);
}

TEST(InkHoardClient, ServerErrorRetriesThrice) {
  MockTransport t;
  t.script.push_back({okBody("{\"error\":\"x\",\"code\":\"server_error\"}", 500)});
  t.script.push_back({okBody("{\"error\":\"x\",\"code\":\"server_error\"}", 500)});
  t.script.push_back({okBody("{\"error\":\"x\",\"code\":\"server_error\"}", 500)});
  InkHoardClient client(&t);
  std::unique_ptr<inkhoard::LibraryPage> page;
  const auto o = client.fetchLibraryPage(page);
  EXPECT_EQ(o.result, inkhoard::ClientResult::ServerError);
  EXPECT_EQ(o.attempts, 3);
  EXPECT_EQ(t.calls, 3u);
}

TEST(InkHoardClient, TransportErrorRetriesThenOk) {
  MockTransport t;
  t.script.push_back({failTransport()});
  t.script.push_back({okBody(readFixture("library-empty.json"))});
  InkHoardClient client(&t);
  std::unique_ptr<inkhoard::LibraryPage> page;
  const auto o = client.fetchLibraryPage(page);
  EXPECT_EQ(o.result, inkhoard::ClientResult::Ok);
  EXPECT_EQ(o.attempts, 2);
  ASSERT_TRUE(page);
  EXPECT_EQ(page->itemCount, 0);
}

TEST(InkHoardClient, TlsErrorRetries) {
  MockTransport t;
  t.script.push_back({failTls()});
  t.script.push_back({failTls()});
  t.script.push_back({failTls()});
  InkHoardClient client(&t);
  std::unique_ptr<inkhoard::LibraryPage> page;
  const auto o = client.fetchLibraryPage(page);
  EXPECT_EQ(o.result, inkhoard::ClientResult::TlsError);
  EXPECT_EQ(o.attempts, 3);
}

TEST(InkHoardClient, SearchAndDetail) {
  MockTransport t;
  t.script.push_back({okBody(readFixture("search-page.json"))});
  t.script.push_back({okBody(readFixture("item-detail.json"))});
  InkHoardClient client(&t);
  std::unique_ptr<inkhoard::SearchPage> search;
  ASSERT_EQ(client.search(search, "hello world").result, inkhoard::ClientResult::Ok);
  ASSERT_TRUE(search);
  EXPECT_EQ(search->itemCount, 2);
  EXPECT_NE(t.urls[0].find("q=hello%20world"), std::string::npos);

  inkhoard::CompactItem item;
  ASSERT_EQ(client.fetchItemDetail(item, "00000000-0000-4000-8000-000000000001").result, inkhoard::ClientResult::Ok);
  EXPECT_STREQ(item.title, "Processed detail item");
}

TEST(InkHoardClient, TestConnection) {
  MockTransport t;
  t.script.push_back({okBody(readFixture("library-empty.json"))});
  InkHoardClient client(&t);
  const auto o = client.testConnection();
  EXPECT_EQ(o.result, inkhoard::ClientResult::Ok);
  EXPECT_NE(t.urls[0].find("limit=1"), std::string::npos);
}

TEST(InkHoardClient, OversizeResponse) {
  MockTransport t;
  inkhoard::TransportResponse r;
  r.ok = true;
  r.oversize = true;
  r.httpCode = 200;
  t.script.push_back({r});
  InkHoardClient client(&t);
  std::unique_ptr<inkhoard::LibraryPage> page;
  EXPECT_EQ(client.fetchLibraryPage(page).result, inkhoard::ClientResult::OversizeResponse);
}
