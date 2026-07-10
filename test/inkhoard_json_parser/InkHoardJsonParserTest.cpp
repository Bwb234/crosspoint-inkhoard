#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#include "InkHoardJsonParser.h"

namespace {

std::string readFixture(const char* name) {
  std::string path = std::string(INKHOARD_FIXTURE_DIR) + "/" + name;
  std::ifstream in(path, std::ios::binary);
  EXPECT_TRUE(in.good()) << "missing fixture " << path;
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

void feedAll(InkHoardJsonParser& p, const std::string& json) { p.feed(json.data(), json.size()); }

void feedChunked(InkHoardJsonParser& p, const std::string& json, size_t chunk) {
  for (size_t i = 0; i < json.size(); i += chunk) {
    const size_t n = std::min(chunk, json.size() - i);
    p.feed(json.data() + i, n);
  }
}

}  // namespace

TEST(InkHoardJsonParser, LibraryPageFixture) {
  InkHoardJsonParser p;
  p.reset(InkHoardJsonParser::Kind::LibraryPage);
  feedAll(p, readFixture("library-page.json"));
  ASSERT_TRUE(p.ok());
  const auto& page = p.libraryPage();
  EXPECT_EQ(page.itemCount, 20);
  EXPECT_TRUE(page.hasMore);
  EXPECT_TRUE(page.hasNextCursor);
  EXPECT_STREQ(page.items[0].id, "00000000-0000-4000-8000-000000000001");
  EXPECT_STREQ(page.items[0].title, "Sample article 1");
  EXPECT_FALSE(page.items[0].titleIsNull);
  EXPECT_TRUE(page.items[0].epubAvailable);
  EXPECT_EQ(page.items[0].tagCount, 1);
  EXPECT_STREQ(page.items[0].tags[0], "news");
  // null title
  EXPECT_TRUE(page.items[4].titleIsNull);
  EXPECT_STREQ(page.items[4].title, "");
}

TEST(InkHoardJsonParser, LibraryPageChunked) {
  InkHoardJsonParser p;
  p.reset(InkHoardJsonParser::Kind::LibraryPage);
  feedChunked(p, readFixture("library-page.json"), 17);
  ASSERT_TRUE(p.ok());
  EXPECT_EQ(p.libraryPage().itemCount, 20);
}

TEST(InkHoardJsonParser, LibraryEmpty) {
  InkHoardJsonParser p;
  p.reset(InkHoardJsonParser::Kind::LibraryPage);
  feedAll(p, readFixture("library-empty.json"));
  ASSERT_TRUE(p.ok());
  EXPECT_EQ(p.libraryPage().itemCount, 0);
  EXPECT_FALSE(p.libraryPage().hasMore);
  EXPECT_FALSE(p.libraryPage().hasNextCursor);
}

TEST(InkHoardJsonParser, LibraryMaxFields) {
  InkHoardJsonParser p;
  p.reset(InkHoardJsonParser::Kind::LibraryPage);
  feedAll(p, readFixture("library-max-fields.json"));
  ASSERT_TRUE(p.ok());
  const auto& item = p.libraryPage().items[0];
  EXPECT_EQ(std::strlen(item.title), 256u);
  EXPECT_EQ(std::strlen(item.url), 1024u);
  EXPECT_EQ(item.tagCount, 8);
  for (uint8_t i = 0; i < 8; ++i) {
    EXPECT_EQ(std::strlen(item.tags[i]), 32u);
  }
}

TEST(InkHoardJsonParser, LibraryLastPage) {
  InkHoardJsonParser p;
  p.reset(InkHoardJsonParser::Kind::LibraryPage);
  feedAll(p, readFixture("library-last-page.json"));
  ASSERT_TRUE(p.ok());
  EXPECT_FALSE(p.libraryPage().hasMore);
}

TEST(InkHoardJsonParser, SearchPage) {
  InkHoardJsonParser p;
  p.reset(InkHoardJsonParser::Kind::SearchPage);
  feedAll(p, readFixture("search-page.json"));
  ASSERT_TRUE(p.ok());
  EXPECT_EQ(p.searchPage().itemCount, 2);
  EXPECT_EQ(p.searchPage().limit, 20);
  EXPECT_EQ(p.searchPage().offset, 0);
  EXPECT_FALSE(p.searchPage().hasMore);
  EXPECT_STREQ(p.searchPage().items[0].title, "Search hit one");
}

TEST(InkHoardJsonParser, ItemDetail) {
  InkHoardJsonParser p;
  p.reset(InkHoardJsonParser::Kind::CompactItem);
  feedAll(p, readFixture("item-detail.json"));
  ASSERT_TRUE(p.ok());
  EXPECT_STREQ(p.item().title, "Processed detail item");
  EXPECT_EQ(p.item().tagCount, 2);
  EXPECT_TRUE(p.item().epubAvailable);
}

TEST(InkHoardJsonParser, ItemDetailProcessing) {
  InkHoardJsonParser p;
  p.reset(InkHoardJsonParser::Kind::CompactItem);
  feedAll(p, readFixture("item-detail-processing.json"));
  ASSERT_TRUE(p.ok());
  EXPECT_STREQ(p.item().status, "processing");
  EXPECT_FALSE(p.item().epubAvailable);
}

TEST(InkHoardJsonParser, ApiErrors) {
  for (const char* name : {"error-unauthorized.json", "error-forbidden.json", "error-content-not-ready.json",
                           "error-content-failed.json"}) {
    InkHoardJsonParser p;
    p.reset(InkHoardJsonParser::Kind::ApiError);
    feedAll(p, readFixture(name));
    ASSERT_TRUE(p.ok()) << name;
    EXPECT_NE(p.apiError().code[0], '\0') << name;
    EXPECT_NE(p.apiError().error[0], '\0') << name;
  }
  InkHoardJsonParser p;
  p.reset(InkHoardJsonParser::Kind::ApiError);
  feedAll(p, readFixture("error-unauthorized.json"));
  EXPECT_STREQ(p.apiError().code, "unauthorized");
}

TEST(InkHoardJsonParser, OversizeAborts) {
  InkHoardJsonParser p;
  p.reset(InkHoardJsonParser::Kind::LibraryPage);
  std::string huge(inkhoard::MAX_JSON_RESPONSE_BYTES, 'x');
  p.feed(huge.data(), huge.size());
  EXPECT_TRUE(p.isOversize());
  EXPECT_TRUE(p.hasError());
  EXPECT_FALSE(p.ok());
}

TEST(InkHoardJsonParser, MalformedFails) {
  InkHoardJsonParser p;
  p.reset(InkHoardJsonParser::Kind::LibraryPage);
  feedAll(p, "{\"items\":[");
  // truncated — should not be marked valid
  EXPECT_FALSE(p.libraryPage().valid);
}

TEST(InkHoardJsonParser, TruncatesOverBoundTitle) {
  // Synthetic: title longer than 256 — StreamingJsonParser emits up to 1024;
  // InkHoardJsonParser truncates into the model field.
  std::string title(300, 'A');
  std::string json = R"({"items":[{"id":"x","title":")" + title +
                     R"(","url":"https://e.com","source":"web","type":"link","status":"processed",)"
                     R"("created_at":"t","updated_at":"t","content_version":"t","tags":[],"epub_available":true}],)"
                     R"("next_cursor":null,"has_more":false})";
  InkHoardJsonParser p;
  p.reset(InkHoardJsonParser::Kind::LibraryPage);
  feedAll(p, json);
  ASSERT_TRUE(p.ok());
  EXPECT_EQ(std::strlen(p.libraryPage().items[0].title), 256u);
}

TEST(InkHoardJsonParser, IgnoresTagsBeyondEight) {
  std::string json =
      R"({"items":[{"id":"x","title":"t","url":"https://e.com","source":"web","type":"link","status":"processed",)"
      R"("created_at":"t","updated_at":"t","content_version":"t",)"
      R"("tags":["0","1","2","3","4","5","6","7","8","9"],"epub_available":false}],)"
      R"("next_cursor":null,"has_more":false})";
  InkHoardJsonParser p;
  p.reset(InkHoardJsonParser::Kind::LibraryPage);
  feedAll(p, json);
  ASSERT_TRUE(p.ok());
  EXPECT_EQ(p.libraryPage().items[0].tagCount, 8);
}

TEST(InkHoardJsonParser, CapsPageAtMaxItems) {
  std::string items;
  for (int i = 0; i < 30; ++i) {
    if (i) items += ",";
    items += R"({"id":")" + std::to_string(i) +
             R"(","title":"t","url":"https://e.com","source":"web","type":"link","status":"processed",)"
             R"("created_at":"t","updated_at":"t","content_version":"t","tags":[],"epub_available":true})";
  }
  std::string json = R"({"items":[)" + items + R"(],"next_cursor":null,"has_more":true})";
  InkHoardJsonParser p;
  p.reset(InkHoardJsonParser::Kind::LibraryPage);
  feedAll(p, json);
  ASSERT_TRUE(p.ok());
  EXPECT_EQ(p.libraryPage().itemCount, inkhoard::MAX_PAGE_ITEMS);
}
