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
#include <vector>

module lucene_core:ut.frozen_buffered_updates;

import lucene_core;

using namespace lucene;

namespace {

Term make_term(std::string field, std::string_view text) {
    return Term::from_text(std::move(field), text);
}

/// A buffered set with two term deletes, one query delete and one doc values
/// update.
std::unique_ptr<BufferedUpdates> sample_updates() {
    auto updates = std::make_unique<BufferedUpdates>();
    EXPECT_TRUE(updates->add_term(make_term("body", "b"), 5).has_value());
    EXPECT_TRUE(updates->add_term(make_term("body", "a"), 3).has_value());
    EXPECT_TRUE(updates->add_term(make_term("title", "x"), 4).has_value());
    EXPECT_TRUE(updates->add_query(std::make_shared<const TermQuery>(make_term("body", "c")), 6).has_value());
    auto numeric = DocValuesUpdate::create(DocValuesType::Numeric, make_term("id", "1"), "price", 7,
                                           NumericDocValuesUpdate::of(42));
    EXPECT_TRUE(updates->add_numeric_update(numeric, 7).has_value());
    return updates;
}

} // namespace

TEST(FrozenBufferedUpdatesTest, CompressesTermsAndKeepsTheirLimits) {
    auto updates = sample_updates();
    EXPECT_TRUE(updates->any());
    auto packet = FrozenBufferedUpdates::freeze(std::move(updates), std::nullopt);
    ASSERT_TRUE(packet.has_value()) << (packet ? "" : packet.error().to_string());

    // The terms are sorted field-then-term, and the limits follow that order.
    EXPECT_EQ(packet.value()->delete_terms().size(), 3);
    std::vector<std::string> visited;
    auto iterator = packet.value()->delete_terms().iterator();
    while (true) {
        auto term = iterator.next();
        ASSERT_TRUE(term.has_value()) << (term ? "" : term.error().to_string());
        if (term.value() == nullptr) {
            break;
        }
        visited.push_back(iterator.field() + ":" + term.value()->utf8_to_string().value());
    }
    EXPECT_EQ(visited, (std::vector<std::string>{"body:a", "body:b", "title:x"}));
    EXPECT_EQ(packet.value()->term_limits(), (std::vector<int32_t>{3, 5, 4}));

    EXPECT_EQ(packet.value()->delete_queries().size(), 1U);
    EXPECT_EQ(packet.value()->delete_query_limits(), (std::vector<int32_t>{6}));
    ASSERT_EQ(packet.value()->field_updates().count("price"), 1U);
    EXPECT_EQ(packet.value()->field_updates_count(), 1);
    EXPECT_GT(packet.value()->bytes_used(), 0);

    // A packet is unassigned and unapplied until the stream takes it.
    EXPECT_EQ(packet.value()->del_gen(), -1);
    EXPECT_FALSE(packet.value()->is_applied());
    EXPECT_EQ(packet.value()->total_del_count(), 0);
}

TEST(FrozenBufferedUpdatesTest, RejectsTermDeletesInAPrivatePacket) {
    auto updates = sample_updates();
    auto packet = FrozenBufferedUpdates::freeze(std::move(updates), std::string("_0"));
    ASSERT_FALSE(packet.has_value());
    EXPECT_EQ(packet.error().kind(), ErrorKind::kIllegalArgument);

    // A query-only packet may be private to a segment.
    auto queries = std::make_unique<BufferedUpdates>();
    ASSERT_TRUE(queries->add_query(std::make_shared<const TermQuery>(make_term("body", "c")), 2).has_value());
    auto private_packet = FrozenBufferedUpdates::freeze(std::move(queries), std::string("_0"));
    ASSERT_TRUE(private_packet.has_value());
    ASSERT_TRUE(private_packet.value()->private_segment().has_value());
    EXPECT_EQ(private_packet.value()->private_segment().value(), "_0");
    EXPECT_NE(private_packet.value()->to_string().find("privateSegment=_0"), std::string::npos);
}

TEST(BufferedUpdatesStreamTest, HandsOutIncreasingGenerations) {
    BufferedUpdatesStream stream;
    EXPECT_EQ(stream.next_del_gen(), 1);
    EXPECT_EQ(stream.applied_gen(), 0);

    auto first = FrozenBufferedUpdates::freeze(sample_updates(), std::nullopt);
    ASSERT_TRUE(first.has_value());
    auto gen = stream.push(first.value());
    ASSERT_TRUE(gen.has_value());
    EXPECT_EQ(gen.value(), 1);
    EXPECT_EQ(first.value()->del_gen(), 1);
    EXPECT_EQ(stream.next_del_gen(), 2);

    auto second = FrozenBufferedUpdates::freeze(sample_updates(), std::nullopt);
    ASSERT_TRUE(second.has_value());
    ASSERT_TRUE(stream.push(second.value()).has_value());
    EXPECT_EQ(second.value()->del_gen(), 2);
    EXPECT_EQ(stream.num_packets(), 2U);
    EXPECT_EQ(stream.num_unapplied_packets(), 2U);

    // The same packet cannot be pushed twice.
    auto again = stream.push(first.value());
    ASSERT_FALSE(again.has_value());
    EXPECT_EQ(again.error().kind(), ErrorKind::kIllegalArgument);

    // A null packet is rejected as well.
    EXPECT_FALSE(stream.push(nullptr).has_value());
}

TEST(BufferedUpdatesStreamTest, ForgetsAppliedPackets) {
    BufferedUpdatesStream stream;
    auto packet = FrozenBufferedUpdates::freeze(sample_updates(), std::nullopt);
    ASSERT_TRUE(packet.has_value());
    auto gen = stream.push(packet.value());
    ASSERT_TRUE(gen.has_value());

    packet.value()->add_deleted(3);
    packet.value()->mark_applied();
    stream.set_applied_gen(gen.value());
    EXPECT_EQ(stream.applied_gen(), gen.value());
    EXPECT_EQ(stream.num_unapplied_packets(), 0U);
    EXPECT_EQ(stream.total_del_count(), 3);

    // The generation never goes backwards.
    stream.set_applied_gen(0);
    EXPECT_EQ(stream.applied_gen(), gen.value());

    stream.prune_applied();
    EXPECT_EQ(stream.num_packets(), 0U);
    // The counter survives pruning, so a segment that compares against it still
    // sees a meaningful "everything below this is applied".
    EXPECT_EQ(stream.applied_gen(), gen.value());
    EXPECT_NE(stream.to_string().find("appliedGen=1"), std::string::npos);

    stream.clear();
    EXPECT_EQ(stream.next_del_gen(), 1);
    EXPECT_EQ(stream.applied_gen(), 0);
    EXPECT_EQ(stream.num_packets(), 0U);
}
