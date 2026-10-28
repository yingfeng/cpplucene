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

module lucene_core:ut.buffered_updates;

import lucene_core;

using namespace lucene;

namespace {

Term make_term(std::string field, std::string_view text) {
    return Term::from_text(std::move(field), text);
}

} // namespace

TEST(DeletedTermsTest, KeepsTheNewestDocumentCount) {
    DeletedTerms terms;
    EXPECT_TRUE(terms.is_empty());
    EXPECT_EQ(terms.get(make_term("body", "a")).value(), -1);

    ASSERT_TRUE(terms.put(make_term("body", "a"), 5).has_value());
    EXPECT_EQ(terms.get(make_term("body", "a")).value(), 5);
    EXPECT_EQ(terms.size(), 1);

    // A later delete with a higher count replaces the recorded one.
    ASSERT_TRUE(terms.put(make_term("body", "a"), 9).has_value());
    EXPECT_EQ(terms.get(make_term("body", "a")).value(), 9);
    EXPECT_EQ(terms.size(), 1);

    ASSERT_TRUE(terms.put(make_term("body", "b"), 2).has_value());
    ASSERT_TRUE(terms.put(make_term("title", "a"), 3).has_value());
    EXPECT_EQ(terms.size(), 3);
    // Fields are independent.
    EXPECT_EQ(terms.get(make_term("title", "a")).value(), 3);
    EXPECT_EQ(terms.get(make_term("title", "b")).value(), -1);
}

TEST(DeletedTermsTest, IteratesFieldsAndTermsInOrder) {
    DeletedTerms terms;
    ASSERT_TRUE(terms.put(make_term("zfield", "b"), 2).has_value());
    ASSERT_TRUE(terms.put(make_term("afield", "c"), 3).has_value());
    ASSERT_TRUE(terms.put(make_term("afield", "a"), 1).has_value());
    ASSERT_TRUE(terms.put(make_term("afield", "b"), 2).has_value());

    std::vector<std::string> visited;
    ASSERT_TRUE(terms
                        .for_each_ordered([&](std::string_view field, const BytesRef &bytes, int32_t count) -> VoidResult {
                            visited.push_back(std::string(field) + ":" + bytes.utf8_to_string().value() + "=" +
                                              std::to_string(count));
                            return {};
                        })
                        .has_value());
    EXPECT_EQ(visited, (std::vector<std::string>{"afield:a=1", "afield:b=2", "afield:c=3", "zfield:b=2"}));

    auto keys = terms.key_set();
    ASSERT_TRUE(keys.has_value());
    ASSERT_EQ(keys.value().size(), 4U);
    EXPECT_EQ(keys.value().front().field(), "afield");
    EXPECT_EQ(keys.value().back().field(), "zfield");
}

TEST(DeletedTermsTest, ClearsEverything) {
    DeletedTerms terms;
    ASSERT_TRUE(terms.put(make_term("body", "a"), 5).has_value());
    EXPECT_GT(terms.ram_bytes_used(), 0);
    terms.clear();
    EXPECT_TRUE(terms.is_empty());
    EXPECT_EQ(terms.size(), 0);
    EXPECT_EQ(terms.get(make_term("body", "a")).value(), -1);
    // The pool was reset, so the same term can be buffered again.
    ASSERT_TRUE(terms.put(make_term("body", "a"), 7).has_value());
    EXPECT_EQ(terms.get(make_term("body", "a")).value(), 7);
}

TEST(BufferedUpdatesTest, AppliesTheNewestDeletePerTerm) {
    BufferedUpdates updates;
    EXPECT_FALSE(updates.any());

    ASSERT_TRUE(updates.add_term(make_term("body", "a"), 5).has_value());
    EXPECT_TRUE(updates.any());
    EXPECT_EQ(updates.delete_terms().get(make_term("body", "a")).value(), 5);

    // An older count does not overwrite the newer one.
    ASSERT_TRUE(updates.add_term(make_term("body", "a"), 3).has_value());
    EXPECT_EQ(updates.delete_terms().get(make_term("body", "a")).value(), 5);

    ASSERT_TRUE(updates.add_term(make_term("body", "a"), 8).has_value());
    EXPECT_EQ(updates.delete_terms().get(make_term("body", "a")).value(), 8);
}

TEST(BufferedUpdatesTest, BuffersQueriesWithoutDuplicates) {
    BufferedUpdates updates;
    auto first = std::make_shared<const TermQuery>(make_term("body", "a"));
    auto same = std::make_shared<const TermQuery>(make_term("body", "a"));
    auto other = std::make_shared<const TermQuery>(make_term("body", "b"));

    ASSERT_TRUE(updates.add_query(first, 4).has_value());
    ASSERT_TRUE(updates.add_query(same, 7).has_value());
    EXPECT_EQ(updates.delete_queries().size(), 1U);
    EXPECT_EQ(updates.delete_queries().front().second, 7);

    ASSERT_TRUE(updates.add_query(other, 9).has_value());
    EXPECT_EQ(updates.delete_queries().size(), 2U);
    ASSERT_TRUE(updates.add_query(nullptr, 9).has_value() == false);
}

TEST(BufferedUpdatesTest, BuffersNumericAndBinaryUpdates) {
    BufferedUpdates updates;
    const Term term = make_term("id", "1");

    auto numeric = DocValuesUpdate::create(DocValuesType::Numeric, term, "price", 10,
                                           NumericDocValuesUpdate::of(42));
    ASSERT_TRUE(updates.add_numeric_update(numeric, 10).has_value());
    EXPECT_EQ(updates.num_field_updates(), 1);
    ASSERT_EQ(updates.field_updates().count("price"), 1U);
    const auto &price_entries = updates.field_updates().at("price").entries();
    ASSERT_EQ(price_entries.size(), 1U);
    EXPECT_EQ(price_entries.front().numeric_value.value(), 42);
    EXPECT_EQ(price_entries.front().doc_id_upto, 10);

    // A "no value" update removes the value.
    auto removal = DocValuesUpdate::create(DocValuesType::Numeric, term, "price", 11,
                                           NumericDocValuesUpdate::no_value());
    ASSERT_TRUE(updates.add_numeric_update(removal, 11).has_value());
    EXPECT_EQ(updates.num_field_updates(), 2);
    const auto &after_removal = updates.field_updates().at("price").entries();
    ASSERT_EQ(after_removal.size(), 2U);
    EXPECT_FALSE(after_removal.back().numeric_value.has_value());

    BytesRef payload;
    payload.copy_from_slice(std::span<const uint8_t>(reinterpret_cast<const uint8_t *>("blob"), 4));
    auto binary = DocValuesUpdate::create(DocValuesType::Binary, term, "data", 12,
                                          BinaryDocValuesUpdate::of(payload));
    ASSERT_TRUE(updates.add_binary_update(binary, 12).has_value());
    EXPECT_EQ(updates.num_field_updates(), 3);
    ASSERT_EQ(updates.field_updates().count("data"), 1U);
    EXPECT_EQ(updates.field_updates().at("data").entries().front().binary_value.value().utf8_to_string().value(),
              "blob");

    EXPECT_GT(updates.ram_bytes_used(), 0);
}

TEST(BufferedUpdatesTest, ReportsItsContentAndClears) {
    BufferedUpdates updates;
    updates.set_gen(7);
    ASSERT_TRUE(updates.add_term(make_term("body", "a"), 5).has_value());
    ASSERT_TRUE(updates.add_query(std::make_shared<const TermQuery>(make_term("body", "b")), 6).has_value());
    auto numeric = DocValuesUpdate::create(DocValuesType::Numeric, make_term("id", "1"), "price", 7,
                                           NumericDocValuesUpdate::of(1));
    ASSERT_TRUE(updates.add_numeric_update(numeric, 7).has_value());

    const std::string summary = updates.to_string();
    EXPECT_NE(summary.find("gen=7"), std::string::npos);
    EXPECT_NE(summary.find("1 unique deleted terms"), std::string::npos);
    EXPECT_NE(summary.find("1 deleted queries"), std::string::npos);
    EXPECT_NE(summary.find("1 field updates"), std::string::npos);

    updates.set_verbose_deletes(true);
    EXPECT_NE(updates.to_string().find("deleteTerms={"), std::string::npos);

    updates.clear();
    EXPECT_FALSE(updates.any());
    EXPECT_EQ(updates.num_field_updates(), 0);
    EXPECT_TRUE(updates.field_updates().empty());
    EXPECT_TRUE(updates.delete_queries().empty());
    EXPECT_TRUE(updates.delete_terms().is_empty());
}
