#include <gtest/gtest.h>

#include <cstring>

#include "InkHoardDownloadPaths.h"
#include "InkHoardUiLogic.h"

TEST(InkHoardDownloadPaths, SafeIdRejectsTraversal) {
  EXPECT_TRUE(inkhoard::isSafeItemId("abc-123"));
  EXPECT_TRUE(inkhoard::isSafeItemId("note.with.dots"));
  EXPECT_FALSE(inkhoard::isSafeItemId(""));
  EXPECT_FALSE(inkhoard::isSafeItemId("../x"));
  EXPECT_FALSE(inkhoard::isSafeItemId("a/b"));
  EXPECT_FALSE(inkhoard::isSafeItemId(".."));
  EXPECT_FALSE(inkhoard::isSafeItemId("."));
}

TEST(InkHoardDownloadPaths, Paths) {
  EXPECT_EQ(inkhoard::itemEpubPath("abc"), "/InkHoard/items/abc.epub");
  EXPECT_EQ(inkhoard::itemSidecarPath("abc"), "/InkHoard/items/abc.json");
  EXPECT_EQ(inkhoard::itemPartPath("abc"), "/InkHoard/tmp/abc.part");
  EXPECT_TRUE(inkhoard::itemEpubPath("../x").empty());
}

TEST(InkHoardDownloadPaths, SidecarRoundTrip) {
  inkhoard::Sidecar s{};
  std::strncpy(s.id, "item1", sizeof(s.id) - 1);
  std::strncpy(s.title, "Hello \"World\"", sizeof(s.title) - 1);
  std::strncpy(s.etag, "\"v1\"", sizeof(s.etag) - 1);
  std::strncpy(s.contentVersion, "2026-01-01T00:00:00.000Z", sizeof(s.contentVersion) - 1);
  s.size = 12345;
  std::strncpy(s.downloadedAt, "2026-07-09T12:00:00Z", sizeof(s.downloadedAt) - 1);
  s.valid = true;

  char buf[inkhoard::MAX_SIDECAR_BYTES];
  const size_t n = inkhoard::formatSidecarJson(buf, sizeof(buf), s);
  ASSERT_GT(n, 0u);

  inkhoard::Sidecar out{};
  ASSERT_TRUE(inkhoard::parseSidecarJson(buf, n, out));
  EXPECT_STREQ(out.id, "item1");
  EXPECT_STREQ(out.title, "Hello \"World\"");
  EXPECT_EQ(out.size, 12345u);
  EXPECT_TRUE(out.valid);
}

TEST(InkHoardDownloadPaths, CorruptSidecar) {
  inkhoard::Sidecar out{};
  EXPECT_FALSE(inkhoard::parseSidecarJson("{not json", 10, out));
  EXPECT_FALSE(out.valid);
  EXPECT_FALSE(inkhoard::parseSidecarJson("{\"title\":\"x\"}", 14, out));
}

TEST(InkHoardUiLogic, CursorStack) {
  inkhoard::CursorStack stack;
  stack.reset();
  EXPECT_EQ(stack.pageIndex(), 0u);
  EXPECT_EQ(stack.currentCursor(), nullptr);
  EXPECT_FALSE(stack.canGoPrev());
  stack.pushNext("cur1");
  EXPECT_EQ(stack.pageIndex(), 1u);
  EXPECT_STREQ(stack.currentCursor(), "cur1");
  EXPECT_TRUE(stack.canGoPrev());
  stack.pushNext("cur2");
  EXPECT_TRUE(stack.popPrev());
  EXPECT_STREQ(stack.currentCursor(), "cur1");
  EXPECT_TRUE(stack.popPrev());
  EXPECT_FALSE(stack.popPrev());
}

TEST(InkHoardUiLogic, MapFailures) {
  EXPECT_EQ(inkhoard::mapClientToUiFailure(inkhoard::ClientResult::AuthInvalid), inkhoard::UiFailureKind::Auth);
  EXPECT_EQ(inkhoard::mapClientToUiFailure(inkhoard::ClientResult::FileError), inkhoard::UiFailureKind::SdError);
  EXPECT_EQ(inkhoard::mapClientToUiFailure(inkhoard::ClientResult::TransportError),
            inkhoard::UiFailureKind::Transport);
}
