/*
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

module;

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

module lucene_core:ut.url_email_tokenizer;

import lucene_core;

using namespace lucene;

namespace {

struct Token {
    std::string term;
    std::string type;
    uint32_t start_offset{0};
    uint32_t end_offset{0};
};

std::vector<Token> tokenize(std::string_view text) {
    UrlEmailTokenizer tokenizer;
    std::vector<Token> tokens;
    EXPECT_TRUE(tokenizer.set_reader(text).has_value());
    EXPECT_TRUE(tokenizer.reset().has_value());
    while (true) {
        auto more = tokenizer.increment_token();
        EXPECT_TRUE(more.has_value()) << (more.has_value() ? "" : more.error().to_string());
        if (!more.has_value() || !more.value()) {
            break;
        }
        const auto *term = tokenizer.attributes().get_attribute<CharTermAttribute>();
        const auto *offset = tokenizer.attributes().get_attribute<OffsetAttribute>();
        tokens.push_back(Token{std::string(term->text()), std::string(UrlEmailTokenizer::type_name(tokenizer.current_type())),
                               offset->start_offset(), offset->end_offset()});
    }
    return tokens;
}

std::vector<std::string> terms(const std::vector<Token> &tokens) {
    std::vector<std::string> out;
    for (const Token &token : tokens) {
        out.push_back(token.term);
    }
    return out;
}

std::vector<std::string> types(const std::vector<Token> &tokens) {
    std::vector<std::string> out;
    for (const Token &token : tokens) {
        out.push_back(token.type);
    }
    return out;
}

} // namespace

TEST(UrlEmailTokenizerTest, RecognisesHttpUrls) {
    const auto tokens = tokenize("see http://example.com/path?q=1#frag now");
    EXPECT_EQ(terms(tokens), (std::vector<std::string>{"see", "http://example.com/path?q=1#frag", "now"}));
    EXPECT_EQ(types(tokens), (std::vector<std::string>{"<ALPHANUM>", "<URL>", "<ALPHANUM>"}));
    // Byte offsets point into the original text.
    EXPECT_EQ(tokens[1].start_offset, 4U);
    EXPECT_EQ(tokens[1].end_offset, 36U);
}

TEST(UrlEmailTokenizerTest, RecognisesSchemeLessHosts) {
    const auto bare = tokenize("example.com");
    ASSERT_EQ(bare.size(), 1U);
    EXPECT_EQ(bare[0].term, "example.com");
    EXPECT_EQ(bare[0].type, "<URL>");

    // A trailing root domain is accepted, like the reference implementation.
    const auto trailing = tokenize("visit www.example.com.");
    EXPECT_EQ(terms(trailing), (std::vector<std::string>{"visit", "www.example.com."}));

    // A host that is followed by a path or a port is a URL as well.
    const auto with_path = tokenize("example.com/a/b");
    ASSERT_EQ(with_path.size(), 1U);
    EXPECT_EQ(with_path[0].term, "example.com/a/b");
    EXPECT_EQ(with_path[0].type, "<URL>");

    const auto with_port = tokenize("example.com:8080/x");
    ASSERT_EQ(with_port.size(), 1U);
    EXPECT_EQ(with_port[0].term, "example.com:8080/x");
}

TEST(UrlEmailTokenizerTest, DoesNotInventHostsOutOfWords) {
    // `comx` is not a top level domain, so this stays a word.
    const auto tokens = tokenize("example.comx");
    EXPECT_EQ(types(tokens), (std::vector<std::string>{"<ALPHANUM>"}));
    EXPECT_EQ(terms(tokens), (std::vector<std::string>{"example.comx"}));

    // A bare host glued to a word character is not a URL either.
    const auto glued = tokenize("foo.example.com_bar");
    EXPECT_EQ(types(glued), (std::vector<std::string>{"<ALPHANUM>"}));
}

TEST(UrlEmailTokenizerTest, RecognisesEmails) {
    const auto tokens = tokenize("write to john.doe@example.org today");
    EXPECT_EQ(terms(tokens), (std::vector<std::string>{"write", "to", "john.doe@example.org", "today"}));
    EXPECT_EQ(types(tokens), (std::vector<std::string>{"<ALPHANUM>", "<ALPHANUM>", "<EMAIL>", "<ALPHANUM>"}));

    // `mailto:` is kept as a word and the address is still recognised.
    const auto mailto = tokenize("mailto:test@example.org");
    EXPECT_EQ(terms(mailto), (std::vector<std::string>{"mailto", "test@example.org"}));
    EXPECT_EQ(types(mailto), (std::vector<std::string>{"<ALPHANUM>", "<EMAIL>"}));

    // A domain literal host is accepted.
    const auto literal = tokenize("root@[192.168.0.1]");
    ASSERT_EQ(literal.size(), 1U);
    EXPECT_EQ(literal[0].type, "<EMAIL>");
}

TEST(UrlEmailTokenizerTest, RecognisesFtpAndFileUrls) {
    const auto ftp = tokenize("ftp://ftp.example.com/f.txt");
    ASSERT_EQ(ftp.size(), 1U);
    EXPECT_EQ(ftp[0].term, "ftp://ftp.example.com/f.txt");
    EXPECT_EQ(ftp[0].type, "<URL>");

    const auto file = tokenize("file:///tmp/x.txt");
    ASSERT_EQ(file.size(), 1U);
    EXPECT_EQ(file[0].term, "file:///tmp/x.txt");
    EXPECT_EQ(file[0].type, "<URL>");

    // An `ftp` URL needs a path.
    const auto bare_ftp = tokenize("ftp://");
    EXPECT_EQ(types(bare_ftp), (std::vector<std::string>{"<ALPHANUM>"}));
}

TEST(UrlEmailTokenizerTest, RecognisesIpAddresses) {
    const auto http = tokenize("http://192.168.0.1:8080/x");
    ASSERT_EQ(http.size(), 1U);
    EXPECT_EQ(http[0].type, "<URL>");
    EXPECT_EQ(http[0].term, "http://192.168.0.1:8080/x");

    const auto ipv6 = tokenize("http://[2001:db8::1]/x");
    ASSERT_EQ(ipv6.size(), 1U);
    EXPECT_EQ(ipv6[0].type, "<URL>");
}

TEST(UrlEmailTokenizerTest, FallsBackToTheWordRules) {
    const auto numbers = tokenize("abc 123 4.5");
    EXPECT_EQ(types(numbers), (std::vector<std::string>{"<ALPHANUM>", "<NUM>", "<NUM>"}));

    // The `@` alone does not make an email address.
    const auto plain = tokenize("a @ b");
    EXPECT_EQ(terms(plain), (std::vector<std::string>{"a", "b"}));

    // Non-ASCII text goes through the word rules untouched.
    const auto cjk = tokenize("中 文");
    EXPECT_EQ(terms(cjk), (std::vector<std::string>{"中", "文"}));
}

TEST(UrlEmailTokenizerTest, KeepsPositionsAndOffsets) {
    const auto tokens = tokenize("a http://x.example.com b");
    ASSERT_EQ(tokens.size(), 3U);
    EXPECT_EQ(tokens[1].start_offset, 2U);
    EXPECT_EQ(tokens[1].end_offset, 22U);
    EXPECT_EQ(tokens[2].start_offset, 23U);
    for (const Token &token : tokens) {
        EXPECT_FALSE(token.term.empty());
    }
}
