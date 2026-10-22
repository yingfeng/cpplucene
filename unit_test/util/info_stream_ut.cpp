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

#include <memory>
#include <string>
#include <utility>
#include <vector>

module lucene_core:ut.info_stream;

import lucene_core;

using namespace lucene;

namespace {

/// Collects every message; the `Vec<u8>` sink of rlucene's tests.
class RecordingInfoStream : public InfoStream {
public:
    [[nodiscard]] VoidResult message(std::string_view component, std::string_view text) const override {
        messages.emplace_back(std::string(component) + ": " + std::string(text));
        return {};
    }

    [[nodiscard]] bool is_enabled(std::string_view /*component*/) const override { return true; }

    mutable std::vector<std::string> messages;
};

} // namespace

TEST(NoOutputTest, DiscardsEverything) {
    NoOutput stream;
    EXPECT_FALSE(stream.is_enabled("IW"));
    EXPECT_TRUE(stream.message("IW", "ignored").has_value());
    EXPECT_EQ(stream.to_string(), "NoOutput");
}

TEST(InfoStreamEnumTest, DefaultsToNoOutput) {
    InfoStreamEnum stream;
    EXPECT_TRUE(stream.is_no_output());
    EXPECT_EQ(stream.to_string(), "InfoStreamEnum(NoOutput)");
    EXPECT_FALSE(stream.is_enabled("IW"));
    EXPECT_TRUE(stream.message("IW", "ignored").has_value());
    EXPECT_TRUE(stream.close().has_value());
}

TEST(InfoStreamEnumTest, ForwardsToCustomStream) {
    auto recording = std::make_shared<RecordingInfoStream>();
    InfoStreamEnum stream{recording};
    EXPECT_FALSE(stream.is_no_output());
    EXPECT_EQ(stream.to_string(), "InfoStreamEnum(CustomInfoStream)");
    EXPECT_TRUE(stream.is_enabled("IW"));
    ASSERT_TRUE(stream.message("IW", "flush").has_value());
    ASSERT_EQ(recording->messages.size(), 1u);
    EXPECT_EQ(recording->messages[0], "IW: flush");
}

TEST(InfoStreamEnumTest, NullCustomStreamBehavesLikeNoOutput) {
    InfoStreamEnum stream{nullptr};
    EXPECT_TRUE(stream.is_no_output());
    EXPECT_FALSE(stream.is_enabled("IW"));
    EXPECT_TRUE(stream.message("IW", "ignored").has_value());
    EXPECT_TRUE(stream.close().has_value());
}

TEST(InfoStreamTest, GlobalDefaultIsSettableAndRestorable) {
    const InfoStreamMT original = get_default_info_stream();
    ASSERT_NE(original, nullptr);

    auto recording = std::make_shared<RecordingInfoStream>();
    set_default_info_stream(recording);
    InfoStreamMT current = get_default_info_stream();
    ASSERT_NE(current, nullptr);
    EXPECT_FALSE(current->is_no_output());
    ASSERT_TRUE(current->message("MP", "merge").has_value());
    EXPECT_EQ(recording->messages.size(), 1u);

    set_default_info_stream(std::make_shared<InfoStreamEnum>());
    EXPECT_TRUE(get_default_info_stream()->is_no_output());

    set_default_info_stream(original);
    EXPECT_EQ(get_default_info_stream(), original);
}
