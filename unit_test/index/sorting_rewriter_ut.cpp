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
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module lucene_core:ut.sorting_rewriter;

import lucene_core;

using namespace lucene;

namespace {

constexpr std::string_view kIdField = "id";
constexpr std::string_view kTextField = "text";
constexpr std::string_view kNumField = "num";
constexpr std::string_view kTagField = "tag";

FieldType text_type() {
    FieldType type;
    EXPECT_TRUE(type.set_index_options(IndexOptions::DocsAndFreqsAndPositions).has_value());
    EXPECT_TRUE(type.set_stored(true).has_value());
    return type;
}

FieldType numeric_doc_values_type() {
    FieldType type;
    EXPECT_TRUE(type.set_doc_values_type(DocValuesType::Numeric).has_value());
    return type;
}

/// A document with a stored/indexed text field, another indexed text field, a
/// sorted doc values field and a numeric doc values field.
Document make_doc(std::string id, std::string text, int64_t num) {
    Document doc;
    doc.add(std::make_unique<Field>(std::string(kIdField), FieldValue{id}, text_type()));
    auto tag = SortedDocValuesField::create(std::string(kTagField), text);
    EXPECT_TRUE(tag.has_value()) << (tag ? "" : tag.error().to_string());
    if (tag.has_value()) {
        doc.add(std::make_unique<Field>(std::move(tag.value())));
    }
    doc.add(std::make_unique<Field>(std::string(kTextField), FieldValue{std::move(text)}, text_type()));
    auto numeric = Field::from_i64(std::string(kNumField), num, numeric_doc_values_type());
    EXPECT_TRUE(numeric.has_value()) << (numeric ? "" : numeric.error().to_string());
    if (numeric.has_value()) {
        doc.add(std::make_unique<Field>(std::move(numeric.value())));
    }
    return doc;
}

/// The index sort the tests rewrite by: `num`, ascending or descending.
Sort num_sort(bool reverse) {
    auto field = SortField::with_reverse(std::string(kNumField), SortFieldType::kInt, reverse);
    EXPECT_TRUE(field.has_value()) << (field ? "" : field.error().to_string());
    auto sort = Sort::with_fields({field.value()});
    EXPECT_TRUE(sort.has_value()) << (sort ? "" : sort.error().to_string());
    return sort.value();
}

Term id_term(std::string_view value) {
    return Term(std::string(kIdField), BytesRef::from_string(value));
}

std::shared_ptr<SegmentReader> open_segment(ByteBuffersDirectory &dir, const SegmentCommitInfo &commit) {
    auto reader = SegmentReader::open(dir, commit, LUCENE_CURRENT_VERSION.major, io_context_default());
    EXPECT_TRUE(reader.has_value()) << (reader ? "" : reader.error().to_string());
    return reader.has_value() ? reader.value() : nullptr;
}

/// The `id` stored in each document of `leaf`, in doc order.
std::vector<std::string> stored_ids(LeafReader &leaf) {
    std::vector<std::string> ids;
    auto stored = leaf.stored_fields();
    EXPECT_TRUE(stored.has_value()) << (stored ? "" : stored.error().to_string());
    if (!stored.has_value()) {
        return ids;
    }
    auto max = leaf.max_doc();
    EXPECT_TRUE(max.has_value());
    for (int32_t doc = 0; doc < max.value(); ++doc) {
        auto fields = stored.value()->document(doc);
        EXPECT_TRUE(fields.has_value()) << (fields ? "" : fields.error().to_string());
        if (!fields.has_value()) {
            continue;
        }
        auto id = fields.value().get(kIdField);
        if (id.has_value() && id.value().has_value()) {
            ids.push_back(id.value().value());
        }
    }
    return ids;
}

/// The numeric doc values value of every document of `leaf`, in doc order.
std::vector<int64_t> numeric_values(LeafReader &leaf) {
    std::vector<int64_t> values;
    auto cursor = leaf.get_numeric_doc_values(kNumField);
    EXPECT_TRUE(cursor.has_value()) << (cursor ? "" : cursor.error().to_string());
    if (!cursor.has_value() || cursor.value() == nullptr) {
        return values;
    }
    while (true) {
        auto doc = cursor.value()->next_doc();
        EXPECT_TRUE(doc.has_value());
        if (!doc.has_value() || doc.value() == NO_MORE_DOCS) {
            break;
        }
        auto value = cursor.value()->long_value();
        EXPECT_TRUE(value.has_value());
        values.push_back(value.value());
    }
    return values;
}

/// The documents that contain `text`, in doc order.
std::vector<int32_t> postings_docs(LeafReader &leaf, std::string_view field, std::string_view text) {
    std::vector<int32_t> docs;
    auto terms = leaf.terms(std::string(field));
    EXPECT_TRUE(terms.has_value()) << (terms ? "" : terms.error().to_string());
    if (!terms.has_value() || terms.value() == nullptr) {
        return docs;
    }
    auto tenum = terms.value()->iterator();
    EXPECT_TRUE(tenum.has_value());
    auto found = tenum.value()->seek_exact(BytesRef::from_string(text).as_slice());
    EXPECT_TRUE(found.has_value());
    if (!found.has_value() || !found.value()) {
        return docs;
    }
    auto postings = tenum.value()->postings(nullptr);
    EXPECT_TRUE(postings.has_value());
    if (!postings.has_value()) {
        return docs;
    }
    while (true) {
        auto doc = postings.value()->next_doc();
        EXPECT_TRUE(doc.has_value());
        if (!doc.has_value() || doc.value() == NO_MORE_DOCS) {
            break;
        }
        docs.push_back(doc.value());
    }
    return docs;
}

/// A segment holding `docs` (id and numeric value; the `text` field is the id too)
/// in a directory whose writer does not merge, so it stays one segment.
struct Fixture {
    ByteBuffersDirectory dir;
    WhitespaceAnalyzer analyzer;
    std::unique_ptr<IndexWriter> writer;
    std::shared_ptr<SegmentReader> segment;
};

[[nodiscard]] std::unique_ptr<Fixture> make_fixture_from(const std::vector<std::pair<std::string, int64_t>> &docs) {
    auto fixture = std::make_unique<Fixture>();
    IndexWriterConfig config(fixture->analyzer);
    config.set_merge_policy(NoMergePolicy::create());

    auto writer = IndexWriter::open(fixture->dir, config);
    EXPECT_TRUE(writer.has_value()) << (writer ? "" : writer.error().to_string());
    if (!writer.has_value()) {
        return fixture;
    }
    fixture->writer = std::move(writer.value());

    for (const auto &[id, num] : docs) {
        EXPECT_TRUE(fixture->writer->add_document(make_doc(id, id, num)).has_value());
    }
    EXPECT_TRUE(fixture->writer->commit().has_value());
    EXPECT_EQ(fixture->writer->segment_infos().segments.size(), 1U);
    if (fixture->writer->segment_infos().segments.empty()) {
        return fixture;
    }
    fixture->segment = open_segment(fixture->dir, fixture->writer->segment_infos().segments[0]);
    return fixture;
}

/// The standard three-document segment: `a`/3, `b`/1, `c`/2.
[[nodiscard]] std::unique_ptr<Fixture> make_fixture() {
    return make_fixture_from({{"a", 3}, {"b", 1}, {"c", 2}});
}

} // namespace

TEST(SortingRewriterTest, WritesTheDocumentsInSortedOrder) {
    auto fixture = make_fixture();
    ASSERT_NE(fixture->segment, nullptr);

    const Sort sort = num_sort(false);

    CodecReader *sources[] = {fixture->segment.get()};
    auto sorted = rewrite_sorted_segment(sources, sort, fixture->dir, "_77", std::vector<uint8_t>(16, 0x5A));
    ASSERT_TRUE(sorted.has_value()) << (sorted ? "" : sorted.error().to_string());
    ASSERT_NE(sorted.value(), nullptr);

    // `num` sorts b(1), c(2), a(3).
    EXPECT_EQ(sorted.value()->max_doc().value(), 3);
    EXPECT_EQ(stored_ids(*sorted.value()), (std::vector<std::string>{"b", "c", "a"}));
    EXPECT_EQ(numeric_values(*sorted.value()), (std::vector<int64_t>{1, 2, 3}));
    // The postings follow the same order: `c` is document 1 now, `a` document 2.
    EXPECT_EQ(postings_docs(*sorted.value(), kTextField, "c"), (std::vector<int32_t>{1}));
    EXPECT_EQ(postings_docs(*sorted.value(), kTextField, "a"), (std::vector<int32_t>{2}));

    // The rewritten segment records the sort it was written with.
    const std::optional<Sort> &recorded = sorted.value()->segment_info().info.sort;
    ASSERT_TRUE(recorded.has_value());
    EXPECT_TRUE(recorded.value().equals(sort));
}

TEST(SortingRewriterTest, SortsInReverseOrder) {
    auto fixture = make_fixture();
    ASSERT_NE(fixture->segment, nullptr);

    const Sort sort = num_sort(true);

    CodecReader *sources[] = {fixture->segment.get()};
    auto sorted = rewrite_sorted_segment(sources, sort, fixture->dir, "_78", std::vector<uint8_t>(16, 0x5B));
    ASSERT_TRUE(sorted.has_value()) << (sorted ? "" : sorted.error().to_string());

    EXPECT_EQ(stored_ids(*sorted.value()), (std::vector<std::string>{"a", "c", "b"}));
    EXPECT_EQ(numeric_values(*sorted.value()), (std::vector<int64_t>{3, 2, 1}));
}

TEST(SortingRewriterTest, SortsBySeveralFields) {
    // `b` and `a` share the numeric value, so the second field decides.
    auto fixture = make_fixture_from({{"a", 1}, {"b", 1}, {"c", 0}});
    ASSERT_NE(fixture->segment, nullptr);

    auto num = SortField::with_reverse(std::string(kNumField), SortFieldType::kInt, false);
    ASSERT_TRUE(num.has_value());
    auto text = SortField::with_reverse(std::string(kTagField), SortFieldType::kString, true);
    ASSERT_TRUE(text.has_value());
    auto sort = Sort::with_fields({num.value(), text.value()});
    ASSERT_TRUE(sort.has_value()) << (sort ? "" : sort.error().to_string());

    CodecReader *sources[] = {fixture->segment.get()};
    auto sorted = rewrite_sorted_segment(sources, sort.value(), fixture->dir, "_83",
                                        std::vector<uint8_t>(16, 0x5D));
    ASSERT_TRUE(sorted.has_value()) << (sorted ? "" : sorted.error().to_string());

    // `c` (0) first, then the two documents with `num` 1 in descending text order.
    EXPECT_EQ(stored_ids(*sorted.value()), (std::vector<std::string>{"c", "b", "a"}));
    EXPECT_EQ(numeric_values(*sorted.value()), (std::vector<int64_t>{0, 1, 1}));

    const std::optional<Sort> &recorded = sorted.value()->segment_info().info.sort;
    ASSERT_TRUE(recorded.has_value());
    EXPECT_TRUE(recorded.value().equals(sort.value()));
    EXPECT_EQ(recorded.value().fields().size(), 2U);
}

TEST(SortingRewriterTest, MergesAnAlreadySortedSegmentUnchanged) {
    auto fixture = make_fixture();
    ASSERT_NE(fixture->segment, nullptr);

    // `num` is 3, 1, 2 in insertion order, so a sort by it has work to do; a sort
    // by the document id would report "already sorted", but the `doc` type cannot
    // drive an index sort, which is what the next check shows.
    const Sort sort = num_sort(false);
    auto impossible = Sort::with_fields({SortField::doc().value()});
    ASSERT_TRUE(impossible.has_value());
    auto rejected = impossible.value().sort_documents(*fixture->segment, 3);
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(rejected.error().kind(), ErrorKind::kIllegalArgument);

    auto first = sort.sort_documents(*fixture->segment, 3);
    ASSERT_TRUE(first.has_value()) << (first ? "" : first.error().to_string());
    EXPECT_TRUE(first.value().has_value());
}

TEST(SortingRewriterTest, RejectsSegmentsWithDeletions) {
    auto fixture = make_fixture();
    ASSERT_NE(fixture->segment, nullptr);

    ASSERT_TRUE(fixture->writer->delete_documents(id_term("b")).has_value());
    ASSERT_TRUE(fixture->writer->commit().has_value());
    ASSERT_EQ(fixture->writer->segment_infos().segments.size(), 1U);
    auto with_deletions = open_segment(fixture->dir, fixture->writer->segment_infos().segments[0]);
    ASSERT_NE(with_deletions, nullptr);
    ASSERT_EQ(with_deletions->num_deleted_docs().value(), 1);

    const Sort sort = num_sort(false);

    CodecReader *sources[] = {with_deletions.get()};
    auto sorted = rewrite_sorted_segment(sources, sort, fixture->dir, "_79", std::vector<uint8_t>(16, 0x5C));
    ASSERT_FALSE(sorted.has_value());
    EXPECT_EQ(sorted.error().kind(), ErrorKind::kIllegalArgument);
}

TEST(SortingRewriterTest, RejectsMissingArguments) {
    auto fixture = make_fixture();
    ASSERT_NE(fixture->segment, nullptr);

    const Sort sort = num_sort(false);

    auto no_readers = rewrite_sorted_segment({}, sort, fixture->dir, "_80", std::vector<uint8_t>(16, 0x01));
    ASSERT_FALSE(no_readers.has_value());
    EXPECT_EQ(no_readers.error().kind(), ErrorKind::kIllegalArgument);

    CodecReader *null_reader[] = {nullptr};
    auto null_readers = rewrite_sorted_segment(null_reader, sort, fixture->dir, "_81", std::vector<uint8_t>(16, 0x02));
    ASSERT_FALSE(null_readers.has_value());
    EXPECT_EQ(null_readers.error().kind(), ErrorKind::kIllegalArgument);

    CodecReader *sources[] = {fixture->segment.get()};
    auto no_name = rewrite_sorted_segment(sources, sort, fixture->dir, "", std::vector<uint8_t>(16, 0x03));
    ASSERT_FALSE(no_name.has_value());
    EXPECT_EQ(no_name.error().kind(), ErrorKind::kIllegalArgument);

    auto no_id = rewrite_sorted_segment(sources, sort, fixture->dir, "_82", std::vector<uint8_t>{});
    ASSERT_FALSE(no_id.has_value());
    EXPECT_EQ(no_id.error().kind(), ErrorKind::kIllegalArgument);
}
