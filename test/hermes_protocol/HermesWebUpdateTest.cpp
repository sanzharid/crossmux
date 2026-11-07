#include <gtest/gtest.h>

#include <string>

#include "HermesWebUpdate.h"

using hermesweb::applyUrlKeyUpdate;
using hermesweb::isSameOrigin;
using hermesweb::OptionalText;
using hermesweb::UpdateResult;

namespace {
OptionalText set(const std::string& value) { return OptionalText{true, value}; }
const OptionalText kKeep{};
}  // namespace

TEST(HermesWebUpdate, SavesNewUrlAndKeyTogether) {
  std::string url = "http://old:8642";
  std::string key = "old-key";
  EXPECT_EQ(applyUrlKeyUpdate(url, key, set("http://new:8642"), set("new-key")), UpdateResult::Ok);
  EXPECT_EQ(url, "http://new:8642");
  EXPECT_EQ(key, "new-key");
}

TEST(HermesWebUpdate, FirstSetupNeedsNoExistingKey) {
  std::string url;
  std::string key;
  EXPECT_EQ(applyUrlKeyUpdate(url, key, set("http://host:8642"), kKeep), UpdateResult::Ok);
  EXPECT_EQ(url, "http://host:8642");
}

TEST(HermesWebUpdate, RefusesToMoveSavedKeyToNewUrl) {
  std::string url = "http://hermes:8642";
  std::string key = "secret";
  EXPECT_EQ(applyUrlKeyUpdate(url, key, set("http://attacker:8642"), kKeep), UpdateResult::KeyRequired);
  EXPECT_EQ(url, "http://hermes:8642");
  EXPECT_EQ(key, "secret");
}

TEST(HermesWebUpdate, RefusesToMoveSavedKeyAfterUrlWasCleared) {
  std::string url = "http://hermes:8642";
  std::string key = "secret";
  EXPECT_EQ(applyUrlKeyUpdate(url, key, set(""), kKeep), UpdateResult::Ok);
  EXPECT_EQ(url, "");
  EXPECT_EQ(applyUrlKeyUpdate(url, key, set("http://attacker:8642"), kKeep), UpdateResult::KeyRequired);
  EXPECT_EQ(url, "");
}

TEST(HermesWebUpdate, SameUrlKeepsSavedKey) {
  std::string url = "http://hermes:8642";
  std::string key = "secret";
  EXPECT_EQ(applyUrlKeyUpdate(url, key, set("http://hermes:8642"), kKeep), UpdateResult::Ok);
  EXPECT_EQ(key, "secret");
}

TEST(HermesWebUpdate, KeyAloneCanBeReplacedOrCleared) {
  std::string url = "http://hermes:8642";
  std::string key = "old";
  EXPECT_EQ(applyUrlKeyUpdate(url, key, kKeep, set("new")), UpdateResult::Ok);
  EXPECT_EQ(key, "new");
  EXPECT_EQ(applyUrlKeyUpdate(url, key, kKeep, set("")), UpdateResult::Ok);
  EXPECT_EQ(key, "");
  EXPECT_EQ(url, "http://hermes:8642");
}

TEST(HermesWebUpdate, TrimsPastedWhitespace) {
  std::string url;
  std::string key;
  EXPECT_EQ(applyUrlKeyUpdate(url, key, set("  http://host:8642\n"), set(" sk-abc\r\n")), UpdateResult::Ok);
  EXPECT_EQ(url, "http://host:8642");
  EXPECT_EQ(key, "sk-abc");
}

TEST(HermesWebUpdate, RejectsBadUrlsWithoutChangingAnything) {
  for (const std::string bad :
       {"hermes:8642", "ftp://host", "http://", "https:///path", "http://ho st", "javascript:alert(1)"}) {
    std::string url = "http://hermes:8642";
    std::string key = "secret";
    EXPECT_EQ(applyUrlKeyUpdate(url, key, set(bad), set("new-key")), UpdateResult::InvalidUrl) << bad;
    EXPECT_EQ(url, "http://hermes:8642");
    EXPECT_EQ(key, "secret");
  }
}

TEST(HermesWebUpdate, RejectsOverlongValues) {
  std::string url;
  std::string key;
  EXPECT_EQ(applyUrlKeyUpdate(url, key, set("http://" + std::string(hermesweb::kMaxUrlLength, 'a')), kKeep),
            UpdateResult::InvalidUrl);
  EXPECT_EQ(applyUrlKeyUpdate(url, key, kKeep, set(std::string(hermesweb::kMaxKeyLength + 1, 'a'))),
            UpdateResult::InvalidKey);
}

TEST(HermesWebUpdate, RejectsKeysThatCouldBreakTheHeader) {
  for (const std::string bad : {"a b", "a\r\nX-Evil: 1", "a\tb", "caf\xC3\xA9"}) {
    std::string url = "http://hermes:8642";
    std::string key = "secret";
    EXPECT_EQ(applyUrlKeyUpdate(url, key, kKeep, set(bad)), UpdateResult::InvalidKey) << bad;
    EXPECT_EQ(key, "secret");
  }
}

TEST(HermesWebUpdate, SameOriginCheck) {
  EXPECT_TRUE(isSameOrigin("", "192.168.1.5"));  // curl and other non-browser clients
  EXPECT_TRUE(isSameOrigin("http://192.168.1.5", "192.168.1.5"));
  EXPECT_TRUE(isSameOrigin("http://CrossPoint.local", "crosspoint.local"));
  EXPECT_FALSE(isSameOrigin("https://evil.example", "192.168.1.5"));
  EXPECT_FALSE(isSameOrigin("http://192.168.1.5.evil.example", "192.168.1.5"));
  EXPECT_FALSE(isSameOrigin("null", "192.168.1.5"));
}
