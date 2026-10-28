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

// The soft deletes of a segment, read back from the field the writer stamps: the
// generations they carry are what a retention policy compares against its age, and
// rewriting the live docs from the field is what reviving a document means.

module;

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module lucene_core:ut.pending_soft_deletes;

import lucene_core;

using namespace lucene;

namespace {

constexpr std::string_view kSoftDeletesField = "sd";

FieldType id_type() {
    FieldType type;
    EXPECT_TRUE(type.set_index_options(IndexOptions::Docs).has_value());
    return type;
}

/// A document that is live and carries no soft delete yet.
Document live_version(std::string id) {
    Document doc;
    doc.add(std::make_unique<Field>("id", FieldValue{std::move(id)}, id_type()));
    auto sd = NumericDocValuesField::create(std::string(kSoftDeletesField), 0);
    EXPECT_TRUE(sd.has_value());
    doc.add(std::make_unique<Field>(std::move(sd.value())));
    return doc;
}

/// Writes `a` and `b`, then soft updates `a`, so the first segment holds one soft
/// deleted document next to one live one.
std::shared_ptr<DirectoryReader> build_index(Directory &directory) {
    WhitespaceAnalyzer analyzer;
    IndexWriterConfig config(analyzer);
    config.set_soft_deletes_field(std::string(kSoftDeletesField));
    config.set_merge_policy(NoMergePolicy::create());
    auto writer = IndexWriter::open(directory, config);
    EXPECT_TRUE(writer.has_value());
    if (!writer.has_value()) {
        return nullptr;
    }
    EXPECT_TRUE(writer.value()->add_document(live_version("a")).has_value());
    EXPECT_TRUE(writer.value()->add_document(live_version("b")).has_value());
    EXPECT_TRUE(writer.value()->commit().has_value());
    EXPECT_TRUE(writer.value()->soft_update_document(Term::from_text("id", "a"), live_version("a")).has_value());
    EXPECT_TRUE(writer.value()->commit().has_value());
    auto reader = DirectoryReader::open(directory);
    EXPECT_TRUE(reader.has_value()) << (reader.has_value() ? "" : reader.error().to_string());
    return reader.has_value() ? reader.value() : nullptr;
}

/// A [`DocIdSetIterator`] over a fixed list of documents.
class ListIterator final : public DocIdSetIterator {
public:
    explicit ListIterator(std::vector<int32_t> docs) : docs_(std::move(docs)) {}

    /// `-1` before the first [`next_doc`], as every iterator of the port is.
    [[nodiscard]] int32_t doc_id() const override {
        if (!started_) {
            return -1;
        }
        return position_ < docs_.size() ? docs_[position_] : NO_MORE_DOCS;
    }

    [[nodiscard]] Result<int32_t> next_doc() override {
        if (!started_) {
            started_ = true;
            position_ = 0;
        } else {
            ++position_;
        }
        return doc_id();
    }

    [[nodiscard]] Result<int32_t> advance(int32_t target) override {
        if (!started_) {
            started_ = true;
            position_ = 0;
        }
        while (position_ < docs_.size() && docs_[position_] < target) {
            ++position_;
        }
        return doc_id();
    }

    [[nodiscard]] Result<int64_t> cost() const override { return static_cast<int64_t>(docs_.size()); }

private:
    std::vector<int32_t> docs_;
    size_t position_{0};
    bool started_{false};
};

} // namespace

TEST(PendingSoftDeletesTest, ReadsTheGenerationOfEverySoftDelete) {
    ByteBuffersDirectory directory;
    auto reader = build_index(directory);
    ASSERT_NE(reader, nullptr);
    const auto leaves = reader->leaves().value();
    ASSERT_EQ(leaves.size(), 2U);
    LeafReader *old_segment = leaves.front().reader();
    LeafReader *new_segment = leaves.back().reader();

    auto pending = PendingSoftDeletes::create(std::string(kSoftDeletesField), *old_segment);
    ASSERT_TRUE(pending.has_value()) << pending.error().to_string();
    EXPECT_EQ(pending.value().field(), kSoftDeletesField);
    ASSERT_EQ(pending.value().num_soft_deletes(), 1);
    EXPECT_FALSE(pending.value().is_empty());

    const int64_t generation = pending.value().generation_of(0);
    ASSERT_GT(generation, 0);
    EXPECT_EQ(pending.value().generation_of(1), 0) << "the second document is live";
    EXPECT_EQ(pending.value().min_generation(), generation);
    EXPECT_EQ(pending.value().max_generation(), generation);

    // A retention policy asks how many deletes are old enough to be dropped.
    EXPECT_EQ(pending.value().num_soft_deletes_below(generation), 1);
    EXPECT_EQ(pending.value().num_soft_deletes_below(generation - 1), 0);
    EXPECT_EQ(pending.value().doc_ids_below(generation), (std::vector<int32_t>{0}));
    EXPECT_TRUE(pending.value().doc_ids_below(generation - 1).empty());

    // The segment that holds the new version has no soft delete at all.
    auto fresh = PendingSoftDeletes::create(std::string(kSoftDeletesField), *new_segment);
    ASSERT_TRUE(fresh.has_value()) << fresh.error().to_string();
    EXPECT_TRUE(fresh.value().is_empty());
    EXPECT_EQ(fresh.value().num_soft_deletes(), 0);
    EXPECT_EQ(fresh.value().min_generation(), 0);
}

TEST(PendingSoftDeletesTest, RewritingTheLiveDocsFromTheFieldDeletesAndRevives) {
    ByteBuffersDirectory directory;
    auto reader = build_index(directory);
    ASSERT_NE(reader, nullptr);
    const auto leaves = reader->leaves().value();
    ASSERT_EQ(leaves.size(), 2U);

    // The old segment: both documents are live as far as the deletion bits go, the
    // field soft deletes the first one.
    LeafReader *old_segment = leaves.front().reader();
    auto old_values = old_segment->get_numeric_doc_values(std::string(kSoftDeletesField));
    ASSERT_TRUE(old_values.has_value());
    ASSERT_NE(old_values.value(), nullptr);
    FixedBitSet old_live(static_cast<size_t>(old_segment->max_doc().value()));
    for (size_t i = 0; i < old_live.length(); ++i) {
        old_live.set(i);
    }
    auto deleted = PendingSoftDeletes::apply_soft_deletes(*old_values.value(), old_live);
    ASSERT_TRUE(deleted.has_value()) << deleted.error().to_string();
    EXPECT_EQ(deleted.value(), 1);
    EXPECT_FALSE(old_live.get_bit(0));
    EXPECT_TRUE(old_live.get_bit(1));

    // The new segment: the field is 0, so a document that was hidden is revived.
    LeafReader *new_segment = leaves.back().reader();
    auto new_values = new_segment->get_numeric_doc_values(std::string(kSoftDeletesField));
    ASSERT_TRUE(new_values.has_value());
    ASSERT_NE(new_values.value(), nullptr);
    FixedBitSet new_live(static_cast<size_t>(new_segment->max_doc().value()));
    new_live.set(0);
    new_live.clear(0); // hidden because the document it replaced was deleted
    auto revived = PendingSoftDeletes::apply_soft_deletes(*new_values.value(), new_live);
    ASSERT_TRUE(revived.has_value()) << revived.error().to_string();
    EXPECT_EQ(revived.value(), -1);
    EXPECT_TRUE(new_live.get_bit(0));
}

TEST(PendingSoftDeletesTest, HardDeletedDocumentsDoNotCountAsSoftDeletes) {
    ListIterator docs({0, 2});
    auto counted = PendingSoftDeletes::count_soft_deletes(&docs, nullptr);
    ASSERT_TRUE(counted.has_value()) << counted.error().to_string();
    EXPECT_EQ(counted.value(), 2);

    // Doc 0 is hard deleted as well, so only doc 2 is a soft delete that still
    // holds data.
    FixedBitSet hard_deletes(4);
    hard_deletes.set(0);
    ListIterator again({0, 2});
    auto filtered = PendingSoftDeletes::count_soft_deletes(&again, &hard_deletes);
    ASSERT_TRUE(filtered.has_value()) << filtered.error().to_string();
    EXPECT_EQ(filtered.value(), 1);

    // No iterator means no soft deletes.
    auto none = PendingSoftDeletes::count_soft_deletes(nullptr, nullptr);
    ASSERT_TRUE(none.has_value());
    EXPECT_EQ(none.value(), 0);
}
