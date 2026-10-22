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
#include <expected>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

module lucene_core:ut.merge_state;

import lucene_core;

using namespace lucene;

namespace {

/// A [`DocMap`] that adds a fixed base, or reports `-1` for the ids listed in
/// `deleted`.
class TestDocMap final : public DocMap {
public:
    TestDocMap(int32_t doc_base, std::vector<int32_t> deleted = {})
            : doc_base_(doc_base), deleted_(std::move(deleted)) {}

    [[nodiscard]] Result<int32_t> get(int32_t doc_id) const override {
        for (const auto deleted : deleted_) {
            if (deleted == doc_id) {
                return -1;
            }
        }
        return doc_base_ + doc_id;
    }

private:
    int32_t doc_base_;
    std::vector<int32_t> deleted_;
};

/// A sub-reader over a fixed doc id array.  `next_doc` is "read then advance",
/// which is what `DocIdMergerSub` expects.
class ArraySub {
public:
    ArraySub(std::vector<int32_t> docs, std::shared_ptr<DocMap> map)
            : docs_(std::move(docs)), map_(std::move(map)) {}

    [[nodiscard]] Result<int32_t> next_doc() {
        if (position_ >= docs_.size()) {
            return NO_MORE_DOCS;
        }
        return docs_[position_++];
    }

    [[nodiscard]] const DocMap &get_doc_map() const { return *map_; }

    void reset() { position_ = 0; }

private:
    std::vector<int32_t> docs_;
    std::shared_ptr<DocMap> map_;
    size_t position_{0};
};

/// Minimal [`CodecReader`] for merge-state tests: it only carries the metadata
/// and lets each codec reader slot be left empty.
class TestCodecReader final : public CodecReader {
public:
    TestCodecReader(int32_t max_doc, int32_t num_docs, std::shared_ptr<const Bits> live,
                    std::shared_ptr<const FieldInfos> infos)
            : max_doc_(max_doc), num_docs_(num_docs), live_(std::move(live)), infos_(std::move(infos)) {}

    [[nodiscard]] StoredFieldsReader *get_fields_reader() override { return nullptr; }
    [[nodiscard]] TermVectorsReader *get_term_vectors_reader() override { return nullptr; }
    [[nodiscard]] NormsProducer *get_norms_reader() override { return nullptr; }
    [[nodiscard]] DocValuesProducer *get_doc_values_reader() override { return nullptr; }
    [[nodiscard]] FieldsProducer *get_postings_reader() override { return nullptr; }
    [[nodiscard]] PointsReader *get_points_reader() override { return nullptr; }

    [[nodiscard]] Result<int32_t> max_doc() const override { return max_doc_; }
    [[nodiscard]] Result<int32_t> num_docs() const override { return num_docs_; }
    [[nodiscard]] Result<const FieldInfos *> field_infos() const override { return infos_.get(); }
    [[nodiscard]] Result<const Bits *> live_docs() const override { return live_.get(); }

private:
    int32_t max_doc_;
    int32_t num_docs_;
    std::shared_ptr<const Bits> live_;
    std::shared_ptr<const FieldInfos> infos_;
};

/// `max_doc` bits, all set except the positions in `deleted`.
std::shared_ptr<const Bits> live_docs_of(int32_t max_doc, std::vector<int32_t> deleted) {
    auto bits = std::make_shared<FixedBitSet>(static_cast<size_t>(max_doc));
    for (int32_t i = 0; i < max_doc; ++i) {
        bits->set(static_cast<size_t>(i));
    }
    for (const auto doc : deleted) {
        bits->clear(static_cast<size_t>(doc));
    }
    return bits;
}

FieldInfos one_field_infos(std::string_view name) {
    FieldInfo info;
    info.number = 0;
    info.name = std::string(name);
    info.index_options = IndexOptions::DocsAndFreqs;
    return FieldInfos::of({info}).value();
}

// ---------------------------------------------------------------------------
// remove_deletes / MergeStateDocMap
// ---------------------------------------------------------------------------

TEST(RemoveDeletesTest, MapsLiveDocIdsAroundDeletions) {
    // 5 documents, documents 1 and 3 deleted: 2 -> 1, 4 -> 2, deleted -> shifted.
    auto live = live_docs_of(5, {1, 3});

    auto map = remove_deletes(5, *live);
    ASSERT_TRUE(map.has_value()) << map.error().to_string();
    ASSERT_EQ(map.value()->size(), 5);

    EXPECT_EQ(map.value()->get(0).value(), 0);
    EXPECT_EQ(map.value()->get(1).value(), 1); // deleted, but still "doc id - deletions so far"
    EXPECT_EQ(map.value()->get(2).value(), 1);
    EXPECT_EQ(map.value()->get(3).value(), 2);
    EXPECT_EQ(map.value()->get(4).value(), 2);
}

TEST(RemoveDeletesTest, HandlesEmptyAndAllDeleted) {
    auto none = remove_deletes(0, *live_docs_of(0, {}));
    ASSERT_TRUE(none.has_value());
    EXPECT_EQ(none.value()->size(), 0);

    auto all = remove_deletes(3, *live_docs_of(3, {0, 1, 2}));
    ASSERT_TRUE(all.has_value());
    ASSERT_EQ(all.value()->size(), 3);
    EXPECT_EQ(all.value()->get(0).value(), 0);
    EXPECT_EQ(all.value()->get(2).value(), 0);
}

TEST(MergeStateDocMapTest, RebasesWithoutDeletions) {
    auto map = MergeStateDocMap::new_deletions(nullptr, nullptr, 10);
    EXPECT_EQ(map->get(0).value(), 10);
    EXPECT_EQ(map->get(7).value(), 17);
}

TEST(MergeStateDocMapTest, DropsDeletedDocuments) {
    auto live = live_docs_of(4, {2});
    auto del_map = remove_deletes(4, *live);
    ASSERT_TRUE(del_map.has_value());

    auto map = MergeStateDocMap::new_deletions(live, del_map.value(), 5);
    EXPECT_EQ(map->get(0).value(), 5);
    EXPECT_EQ(map->get(1).value(), 6);
    EXPECT_EQ(map->get(2).value(), -1);
    EXPECT_EQ(map->get(3).value(), 7);
}

TEST(MergeStateDocMapTest, RejectsHalfInitialisedMap) {
    auto map = MergeStateDocMap::new_deletions(std::shared_ptr<const Bits>{},
                                               std::make_shared<const PackedLongValues>(), 0);
    auto mapped = map->get(0);
    ASSERT_FALSE(mapped.has_value());
    EXPECT_EQ(mapped.error().kind(), ErrorKind::kIllegalState);
}

// ---------------------------------------------------------------------------
// MergeState
// ---------------------------------------------------------------------------

TEST(MergeStateTest, AggregatesReadersAndBuildsDocMaps) {
    auto infos_a = std::make_shared<const FieldInfos>(one_field_infos("body"));
    auto infos_b = std::make_shared<const FieldInfos>(one_field_infos("title"));

    TestCodecReader reader_a(3, 3, nullptr, infos_a);
    TestCodecReader reader_b(4, 3, live_docs_of(4, {1}), infos_b);

    std::vector<CodecReader *> readers{&reader_a, &reader_b};
    SegmentInfo segment_info;

    auto state = MergeState::create(readers, segment_info, std::make_shared<InfoStreamEnum>());
    ASSERT_TRUE(state.has_value()) << state.error().to_string();
    auto &merge_state = *state.value();

    // 3 + 3 live documents.
    EXPECT_EQ(merge_state.segment_info.max_doc, 6);
    EXPECT_EQ(merge_state.num_readers(), 2);
    EXPECT_FALSE(merge_state.needs_index_sort);

    ASSERT_EQ(merge_state.max_docs.size(), 2);
    EXPECT_EQ(merge_state.max_docs[0], 3);
    EXPECT_EQ(merge_state.max_docs[1], 4);

    ASSERT_EQ(merge_state.field_infos->size(), 2);
    ASSERT_EQ(merge_state.field_infos->at(1)->size(), 1);
    EXPECT_EQ(merge_state.field_infos->at(1)->fields()[0].name, "title");

    EXPECT_EQ(merge_state.live_docs[0], nullptr);
    ASSERT_NE(merge_state.live_docs[1], nullptr);

    // Every codec reader slot is empty for this fake reader.
    EXPECT_EQ(merge_state.stored_fields_readers.size(), 2);
    EXPECT_EQ(merge_state.stored_fields_readers[0], nullptr);
    EXPECT_EQ(merge_state.fields_producers[1], nullptr);
    EXPECT_EQ(merge_state.points_readers.size(), 2);

    // Doc maps: the first segment is rebased as is, the second one drops doc 1.
    ASSERT_EQ(merge_state.doc_maps->size(), 2);
    EXPECT_EQ(merge_state.doc_maps->at(0)->get(2).value(), 2);
    EXPECT_EQ(merge_state.doc_maps->at(1)->get(0).value(), 3);
    EXPECT_EQ(merge_state.doc_maps->at(1)->get(1).value(), -1);
    EXPECT_EQ(merge_state.doc_maps->at(1)->get(2).value(), 4);
    EXPECT_EQ(merge_state.doc_maps->at(1)->get(3).value(), 5);

    auto meta = merge_state.get_meta();
    EXPECT_EQ(meta.fields_producers_len, 2);
    EXPECT_EQ(meta.doc_maps, merge_state.doc_maps);
    EXPECT_EQ(meta.merge_field_infos->size(), 0);
}

TEST(MergeStateTest, EmptyReaderListProducesEmptyState) {
    SegmentInfo segment_info;
    segment_info.max_doc = 42;
    std::vector<CodecReader *> readers;

    auto state = MergeState::create(readers, segment_info, std::make_shared<InfoStreamEnum>());
    ASSERT_TRUE(state.has_value());
    EXPECT_EQ(state.value()->segment_info.max_doc, 0);
    EXPECT_TRUE(state.value()->doc_maps->empty());
    EXPECT_TRUE(state.value()->field_infos->empty());
}

// ---------------------------------------------------------------------------
// DocIdMerger
// ---------------------------------------------------------------------------

using ArrayMerger = DocIdMerger<ArraySub>;

std::vector<DocIdMergerSub<ArraySub>> make_subs() {
    std::vector<DocIdMergerSub<ArraySub>> subs;
    subs.emplace_back(ArraySub({0, 2, 4}, std::make_shared<TestDocMap>(0)));
    subs.emplace_back(ArraySub({1, 3, 5}, std::make_shared<TestDocMap>(0)));
    return subs;
}

TEST(DocIdMergerTest, SequentialVisitsEverySubInOrder) {
    auto merger = ArrayMerger::of(make_subs(), false);
    ASSERT_TRUE(merger.has_value()) << merger.error().to_string();

    std::vector<size_t> visited;
    while (true) {
        auto next = merger.value().next();
        ASSERT_TRUE(next.has_value()) << next.error().to_string();
        if (!next.value().has_value()) {
            break;
        }
        visited.push_back(next.value().value());
    }
    EXPECT_EQ(visited, (std::vector<size_t>{0, 0, 0, 1, 1, 1}));
}

TEST(DocIdMergerTest, SequentialSkipsDeletedDocuments) {
    std::vector<DocIdMergerSub<ArraySub>> subs;
    subs.emplace_back(ArraySub({0, 1, 2}, std::make_shared<TestDocMap>(0, std::vector<int32_t>{1})));
    subs.emplace_back(ArraySub({0, 1}, std::make_shared<TestDocMap>(2)));

    auto merger = ArrayMerger::of(std::move(subs), false);
    ASSERT_TRUE(merger.has_value());

    std::vector<int32_t> mapped;
    while (true) {
        auto next = merger.value().next();
        ASSERT_TRUE(next.has_value());
        if (!next.value().has_value()) {
            break;
        }
        mapped.push_back(merger.value().subs()[next.value().value()].mapped_doc_id);
    }
    // doc 1 of the first sub is deleted, so its remaining docs map to 0 and 2;
    // the second sub then continues at 2.
    EXPECT_EQ(mapped, (std::vector<int32_t>{0, 2, 2, 3}));
}

TEST(DocIdMergerTest, SortedMergesByMappedDocId) {
    auto merger = ArrayMerger::of(make_subs(), true);
    ASSERT_TRUE(merger.has_value()) << merger.error().to_string();

    std::vector<size_t> visited;
    std::vector<int32_t> mapped;
    while (true) {
        auto next = merger.value().next();
        ASSERT_TRUE(next.has_value()) << next.error().to_string();
        if (!next.value().has_value()) {
            break;
        }
        const size_t index = next.value().value();
        visited.push_back(index);
        mapped.push_back(merger.value().subs()[index].mapped_doc_id);
    }

    // The two subs interleave: 0 (sub 0), 1 (sub 1), 2 (sub 0), ...
    EXPECT_EQ(visited, (std::vector<size_t>{0, 1, 0, 1, 0, 1}));
    EXPECT_EQ(mapped, (std::vector<int32_t>{0, 1, 2, 3, 4, 5}));
}

TEST(DocIdMergerTest, SortedSkipsDeletedDocuments) {
    std::vector<DocIdMergerSub<ArraySub>> subs;
    // sub 0 -> 0, 2 (doc 1 deleted); sub 1 -> 3, 4
    subs.emplace_back(ArraySub({0, 1, 2}, std::make_shared<TestDocMap>(0, std::vector<int32_t>{1})));
    subs.emplace_back(ArraySub({0, 1}, std::make_shared<TestDocMap>(3)));

    auto merger = ArrayMerger::of(std::move(subs), true);
    ASSERT_TRUE(merger.has_value());

    std::vector<int32_t> mapped;
    while (true) {
        auto next = merger.value().next();
        ASSERT_TRUE(next.has_value());
        if (!next.value().has_value()) {
            break;
        }
        mapped.push_back(merger.value().subs()[next.value().value()].mapped_doc_id);
    }
    EXPECT_EQ(mapped, (std::vector<int32_t>{0, 2, 3, 4}));
}

TEST(DocIdMergerTest, ResetRestartsIteration) {
    auto merger = ArrayMerger::of(make_subs(), true);
    ASSERT_TRUE(merger.has_value());

    size_t first_pass = 0;
    while (true) {
        auto next = merger.value().next();
        ASSERT_TRUE(next.has_value());
        if (!next.value().has_value()) {
            break;
        }
        ++first_pass;
    }
    EXPECT_EQ(first_pass, 6);

    // The merger only resets its own bookkeeping: the callers must rewind the
    // sub-readers themselves first (the reference implementation has the same contract).
    for (auto &sub : merger.value().subs()) {
        sub.sub.reset();
    }
    ASSERT_TRUE(merger.value().reset().has_value());
    size_t second_pass = 0;
    while (true) {
        auto next = merger.value().next();
        ASSERT_TRUE(next.has_value());
        if (!next.value().has_value()) {
            break;
        }
        ++second_pass;
    }
    EXPECT_EQ(second_pass, 6);
}

TEST(DocIdMergerTest, SortedNeedsMoreThanOneSub) {
    auto single = SortedDocIdMerger<ArraySub>::create(make_subs(), 1);
    ASSERT_FALSE(single.has_value());
    EXPECT_EQ(single.error().kind(), ErrorKind::kIllegalArgument);

    // `of_with_max_count` only picks the sorted merger when max_count > 1.
    auto merger = ArrayMerger::of_with_max_count(make_subs(), 1, true);
    ASSERT_TRUE(merger.has_value());
}

TEST(DocIdMergerTest, EmptySubListIsAnErrorOnNext) {
    // The reference implementation's merge state never builds a merger without subs, and both
    // variants report `illegal_state` rather than silently ending.
    std::vector<DocIdMergerSub<ArraySub>> subs;
    auto merger = ArrayMerger::of(std::move(subs), false);
    ASSERT_TRUE(merger.has_value());

    auto next = merger.value().next();
    ASSERT_FALSE(next.has_value());
    EXPECT_EQ(next.error().kind(), ErrorKind::kIllegalState);
}

} // namespace
