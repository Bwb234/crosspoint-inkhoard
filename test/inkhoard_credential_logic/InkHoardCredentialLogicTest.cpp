#include <gtest/gtest.h>

#include "InkHoardCredentialLogic.h"

using namespace inkhoard;

TEST(InkHoardCredentialLogic, NormalizeEmptyAndSchemeOnly) {
  std::string out;
  EXPECT_TRUE(normalizeServerUrl("", false, out));
  EXPECT_TRUE(out.empty());
  EXPECT_TRUE(normalizeServerUrl("https://", false, out));
  EXPECT_TRUE(out.empty());
  EXPECT_TRUE(normalizeServerUrl("http://", false, out));
  EXPECT_TRUE(out.empty());
}

TEST(InkHoardCredentialLogic, NormalizePrependsHttps) {
  std::string out;
  ASSERT_TRUE(normalizeServerUrl("example.com/ink", false, out));
  EXPECT_EQ(out, "https://example.com/ink");
}

TEST(InkHoardCredentialLogic, NormalizeStripsTrailingSlash) {
  std::string out;
  ASSERT_TRUE(normalizeServerUrl("https://example.com/ink/", false, out));
  EXPECT_EQ(out, "https://example.com/ink");
}

TEST(InkHoardCredentialLogic, RejectsHttpInRelease) {
  std::string out = "keep";
  EXPECT_FALSE(normalizeServerUrl("http://example.com", false, out));
  EXPECT_EQ(out, "keep");
}

TEST(InkHoardCredentialLogic, AllowsHttpWhenFlagSet) {
  std::string out;
  ASSERT_TRUE(normalizeServerUrl("http://192.168.1.5:8787", true, out));
  EXPECT_EQ(out, "http://192.168.1.5:8787");
}

TEST(InkHoardCredentialLogic, RejectsOversizedUrl) {
  std::string out = "keep";
  const std::string huge(MAX_URL_BYTES + 10, 'a');
  EXPECT_FALSE(normalizeServerUrl("https://" + huge, false, out));
  EXPECT_EQ(out, "keep");
}

TEST(InkHoardCredentialLogic, TruncateUtf8OnCodePointBoundary) {
  // "é" is 2 bytes in UTF-8; max 3 bytes should keep only "a"
  EXPECT_EQ(truncateUtf8("aé", 2), "a");
  EXPECT_EQ(truncateUtf8("abc", 10), "abc");
  EXPECT_EQ(truncateUtf8(std::string(MAX_TOKEN_BYTES + 5, 'x'), MAX_TOKEN_BYTES).size(), MAX_TOKEN_BYTES);
}

TEST(InkHoardCredentialLogic, RedactedTokenFormat) {
  EXPECT_EQ(redactedToken(""), "");
  EXPECT_EQ(redactedToken("abcd"), "…");
  const std::string token = "ink_dev_0123456789abcdef0123456789abcdef";
  EXPECT_EQ(redactedToken(token), "ink_dev_…cdef");
  // Never contains the middle of the secret
  EXPECT_EQ(redactedToken(token).find("0123456789"), std::string::npos);
}
