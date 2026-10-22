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

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

module lucene_core:ut.postings_merge;

import lucene_core;

using namespace lucene;

namespace {

/// One term of a source segment: its text and the documents it occurs in.
///
/// `positions` is parallel to `docs`; when it is empty the field was indexed
/// without positions and every document reports a frequency of 1.
struct TermSpec {
    std::string text;
    std::vector<int32_t> docs;
    std::vector<std::vector<int32_t>> positions{};
};

/// A [`PostingsEnum`] over a fixed doc list and position lists.
class ArrayPostingsEnum final : public PostingsEnum {
public:
    ArrayPostingsEnum(std::vector<int32_t> docs, std::vector<std::vector<int32_t>> positions)
            : docs_(std::move(docs)), positions_(std::move(positions)) {}

    [[nodiscard]] int32_t doc_id() const override { return doc_id_; }

    [[nodiscard]] Result<int32_t> next_doc() override {
        if (position_ >= docs_.size()) {
            doc_id_ = NO_MORE_DOCS;
            return doc_id_;
        }
        doc_id_ = docs_[position_++];
        next_position_ = 0;
        return doc_id_;
    }

    [[nodiscard]] Result<int32_t> advance(int32_t target) override {
        while (true) {
            if (position_ >= docs_.size()) {
                doc_id_ = NO_MORE_DOCS;
                return doc_id_;
            }
            if (docs_[position_] >= target) {
                doc_id_ = docs_[position_++];
                next_position_ = 0;
                return doc_id_;
            }
            ++position_;
        }
    }

    [[nodiscard]] Result<int32_t> slow_advance(int32_t target) override { return advance(target); }
    [[nodiscard]] Result<int64_t> cost() const override { return static_cast<int64_t>(docs_.size()); }

    [[nodiscard]] Result<int32_t> freq() override {
        if (positions_.empty()) {
            return 1;
        }
        return static_cast<int32_t>(positions_[position_ - 1].size());
    }

    [[nodiscard]] Result<int32_t> next_position() override {
        if (positions_.empty()) {
            return -1;
        }
        const auto &positions = positions_[position_ - 1];
        if (next_position_ >= positions.size()) {
            return -1;
        }
        return positions[next_position_++];
    }

    [[nodiscard]] Result<int32_t> start_offset() const override { return -1; }
    [[nodiscard]] Result<int32_t> end_offset() const override { return -1; }
    [[nodiscard]] Result<std::optional<std::span<const uint8_t>>> get_payload() const override {
        return std::optional<std::span<const uint8_t>>{};
    }

private:
    std::vector<int32_t> docs_;
    std::vector<std::vector<int32_t>> positions_;
    size_t position_{0};
    size_t next_position_{0};
    int32_t doc_id_{-1};
};

/// A [`TermsEnum`] over a fixed term list.
class ArrayTermsEnum final : public TermsEnum {
public:
    explicit ArrayTermsEnum(const std::vector<TermSpec> *terms) : terms_(terms) {}

    [[nodiscard]] Result<const BytesRef *> next() override {
        if (position_ >= terms_->size()) {
            current_ = BytesRef{};
            return static_cast<const BytesRef *>(nullptr);
        }
        current_ = BytesRef::from_string((*terms_)[position_].text);
        ++position_;
        return &current_;
    }

    [[nodiscard]] Result<const BytesRef *> term() const override { return &current_; }
    [[nodiscard]] Result<int64_t> ord() const override { return static_cast<int64_t>(position_) - 1; }
    [[nodiscard]] Result<int32_t> doc_freq() override { return count(); }
    [[nodiscard]] Result<int64_t> total_term_freq() override {
        const auto &positions = (*terms_)[position_ - 1].positions;
        if (positions.empty()) {
            return count();
        }
        int64_t total = 0;
        for (const auto &doc_positions : positions) {
            total += static_cast<int64_t>(doc_positions.size());
        }
        return total;
    }

    [[nodiscard]] Result<std::unique_ptr<PostingsEnum>> postings_with_flags(std::unique_ptr<PostingsEnum> /*reuse*/,
                                                                           int32_t /*flags*/) override {
        const auto &spec = (*terms_)[position_ - 1];
        return std::unique_ptr<PostingsEnum>(new ArrayPostingsEnum(spec.docs, spec.positions));
    }

    [[nodiscard]] Result<std::unique_ptr<ImpactsEnum>> impacts(int32_t /*flags*/) override {
        return std::unique_ptr<ImpactsEnum>{};
    }

    [[nodiscard]] Result<AttributeSource *> attributes() override {
        return err(LuceneError::unsupported_operation("ArrayTermsEnum has no attributes"));
    }
    [[nodiscard]] Result<AttributeSource *> attributes_mut() override { return attributes(); }
    [[nodiscard]] Result<bool> seek_exact(std::span<const uint8_t> /*term*/) override {
        return err(LuceneError::unsupported_operation("no seeks"));
    }
    [[nodiscard]] Result<bool> prepare_seek_exact(std::span<const uint8_t> /*term*/) override {
        return err(LuceneError::unsupported_operation("no seeks"));
    }
    [[nodiscard]] Result<bool> get_prepare_seek_exact_status(std::span<const uint8_t> /*target*/) override {
        return err(LuceneError::unsupported_operation("no seeks"));
    }
    [[nodiscard]] Result<SeekStatus> seek_ceil(std::span<const uint8_t> /*term*/) override {
        return err(LuceneError::unsupported_operation("no seeks"));
    }
    [[nodiscard]] VoidResult seek_exact_with_ord(int64_t /*ord*/) override {
        return err(LuceneError::unsupported_operation("no seeks"));
    }
    [[nodiscard]] VoidResult seek_exact_with_state(std::span<const uint8_t> /*term*/,
                                                   const TermStateEnum & /*state*/) override {
        return err(LuceneError::unsupported_operation("no seeks"));
    }
    [[nodiscard]] Result<TermStateEnum> term_state() override {
        return err(LuceneError::unsupported_operation("no term state"));
    }

private:
    [[nodiscard]] int32_t count() const {
        return static_cast<int32_t>((*terms_)[position_ - 1].docs.size());
    }

    const std::vector<TermSpec> *terms_;
    size_t position_{0};
    BytesRef current_{};
};

class ArrayTerms final : public Terms {
public:
    ArrayTerms(const std::vector<TermSpec> *terms, bool has_freqs, bool has_positions)
            : terms_(terms), has_freqs_(has_freqs), has_positions_(has_positions) {}

    [[nodiscard]] Result<std::unique_ptr<TermsEnum>> iterator() const override {
        return std::unique_ptr<TermsEnum>(new ArrayTermsEnum(terms_));
    }
    [[nodiscard]] Result<int64_t> size() const override { return static_cast<int64_t>(terms_->size()); }
    [[nodiscard]] Result<int64_t> sum_total_term_freq() const override { return 0; }
    [[nodiscard]] Result<int64_t> sum_doc_freq() const override { return 0; }
    [[nodiscard]] Result<int32_t> doc_count() const override { return 0; }
    [[nodiscard]] bool has_freqs() const override { return has_freqs_; }
    [[nodiscard]] bool has_offsets() const override { return false; }
    [[nodiscard]] bool has_positions() const override { return has_positions_; }
    [[nodiscard]] bool has_payloads() const override { return false; }

private:
    const std::vector<TermSpec> *terms_;
    bool has_freqs_;
    bool has_positions_;
};

class ArrayFieldNames final : public FieldNameIterator {
public:
    explicit ArrayFieldNames(std::vector<std::string> names) : names_(std::move(names)) {}

    [[nodiscard]] Result<const std::string *> next() override {
        if (position_ >= names_.size()) {
            return static_cast<const std::string *>(nullptr);
        }
        return &names_[position_++];
    }

private:
    std::vector<std::string> names_;
    size_t position_{0};
};

/// A [`Fields`] over one field with a fixed term list.
class ArrayFields final : public Fields {
public:
    ArrayFields(std::string field, const std::vector<TermSpec> *terms, IndexOptions options)
            : field_(std::move(field)), terms_(terms), options_(options) {}

    [[nodiscard]] Result<std::unique_ptr<FieldNameIterator>> iterator() const override {
        return std::unique_ptr<FieldNameIterator>(new ArrayFieldNames({field_}));
    }

    [[nodiscard]] Result<std::unique_ptr<Terms>> terms(std::string_view field) const override {
        if (field != field_) {
            return std::unique_ptr<Terms>{};
        }
        const bool freqs = options_ != IndexOptions::Docs;
        const bool positions = options_ == IndexOptions::DocsAndFreqsAndPositions;
        return std::unique_ptr<Terms>(new ArrayTerms(terms_, freqs, positions));
    }

    [[nodiscard]] Result<int32_t> size() const override { return 1; }

private:
    std::string field_;
    const std::vector<TermSpec> *terms_;
    IndexOptions options_;
};

std::shared_ptr<const FieldInfos> postings_infos(IndexOptions options = IndexOptions::DocsAndFreqs) {
    FieldInfo field;
    field.number = 0;
    field.name = "f";
    field.index_options = options;
    field.omit_norms = true;
    return std::make_shared<const FieldInfos>(FieldInfos::of({field}).value());
}

SegmentInfo segment(std::string name, int32_t max_doc) {
    SegmentInfo info;
    info.name = std::move(name);
    info.id.assign(ID_LENGTH, 0x77);
    info.max_doc = max_doc;
    return info;
}

/// Writes one segment's postings.
void write_segment(ByteBuffersDirectory &directory, const std::string &name, int32_t max_doc,
                   const std::vector<TermSpec> &terms, IndexOptions options = IndexOptions::DocsAndFreqs) {
    auto infos = postings_infos(options);
    SegmentInfo write_info = segment(name, max_doc);
    SegmentWriteState write_state(directory, infos, io_context_default());
    Lucene101PostingsFormat format;
    auto consumer = format.fields_consumer(write_state, write_info);
    ASSERT_TRUE(consumer.has_value()) << (consumer ? "" : consumer.error().to_string());
    ArrayFields fields("f", &terms, options);
    ASSERT_TRUE(consumer.value()->write(write_state, write_info, fields, nullptr).has_value());
    ASSERT_TRUE(consumer.value()->close().has_value());
}

std::unique_ptr<FieldsProducer> open_producer(ByteBuffersDirectory &directory, const std::string &name,
                                              int32_t max_doc, IndexOptions options = IndexOptions::DocsAndFreqs) {
    auto infos = postings_infos(options);
    SegmentInfo read_info = segment(name, max_doc);
    SegmentReadState read_state(directory, infos, io_context_default());
    Lucene101PostingsFormat format;
    auto producer = format.fields_producer(read_state, read_info);
    EXPECT_TRUE(producer.has_value()) << (producer ? "" : producer.error().to_string());
    return producer.has_value() ? std::move(producer.value()) : nullptr;
}

/// The documents of every term of field `f`, in term order.
using TermsOfField = std::vector<std::pair<std::string, std::vector<int32_t>>>;

/// Reads every term of field `f` with the documents it occurs in.
TermsOfField read_terms(FieldsProducer &producer) {
    TermsOfField result;
    auto terms = producer.terms("f");
    EXPECT_TRUE(terms.has_value()) << (terms ? "" : terms.error().to_string());
    if (!terms.has_value() || terms.value() == nullptr) {
        return result;
    }
    auto iterator = terms.value()->iterator();
    EXPECT_TRUE(iterator.has_value());
    if (!iterator.has_value()) {
        return result;
    }
    while (true) {
        auto term = iterator.value()->next();
        EXPECT_TRUE(term.has_value()) << (term ? "" : term.error().to_string());
        if (!term.has_value() || term.value() == nullptr) {
            break;
        }
        const std::string text = term.value()->utf8_to_string().value();
        auto postings = iterator.value()->postings(nullptr);
        EXPECT_TRUE(postings.has_value());
        std::vector<int32_t> docs;
        if (postings.has_value()) {
            while (true) {
                auto doc = postings.value()->next_doc();
                EXPECT_TRUE(doc.has_value());
                if (!doc.has_value() || doc.value() == NO_MORE_DOCS) {
                    break;
                }
                docs.push_back(doc.value());
            }
        }
        result.emplace_back(text, std::move(docs));
    }
    return result;
}

/// Reads the positions of field `f` as `term@doc:pos,pos,` strings, in term and
/// then doc order.
std::vector<std::string> read_positions(FieldsProducer &producer) {
    std::vector<std::string> result;
    auto terms = producer.terms("f");
    EXPECT_TRUE(terms.has_value());
    if (!terms.has_value() || terms.value() == nullptr) {
        return result;
    }
    auto iterator = terms.value()->iterator();
    EXPECT_TRUE(iterator.has_value());
    if (!iterator.has_value()) {
        return result;
    }
    while (true) {
        auto term = iterator.value()->next();
        EXPECT_TRUE(term.has_value());
        if (!term.has_value() || term.value() == nullptr) {
            break;
        }
        const std::string text = term.value()->utf8_to_string().value();
        auto postings = iterator.value()->postings_with_flags(nullptr, POSITIONS);
        EXPECT_TRUE(postings.has_value()) << (postings ? "" : postings.error().to_string());
        if (!postings.has_value()) {
            break;
        }
        while (true) {
            auto doc = postings.value()->next_doc();
            EXPECT_TRUE(doc.has_value());
            if (!doc.has_value() || doc.value() == NO_MORE_DOCS) {
                break;
            }
            auto freq = postings.value()->freq();
            EXPECT_TRUE(freq.has_value());
            std::string entry = text + "@" + std::to_string(doc.value()) + ":";
            for (int32_t i = 0; i < freq.value(); ++i) {
                auto position = postings.value()->next_position();
                EXPECT_TRUE(position.has_value());
                entry += std::to_string(position.value()) + ",";
            }
            result.push_back(std::move(entry));
        }
    }
    return result;
}

/// Minimal [`CodecReader`] exposing only a postings producer.
class PostingsCodecReader final : public CodecReader {
public:
    PostingsCodecReader(FieldsProducer *postings, int32_t max_doc, int32_t num_docs, std::shared_ptr<const Bits> live,
                        std::shared_ptr<const FieldInfos> infos)
            : postings_(postings), max_doc_(max_doc), num_docs_(num_docs), live_(std::move(live)),
              infos_(std::move(infos)) {}

    [[nodiscard]] StoredFieldsReader *get_fields_reader() override { return nullptr; }
    [[nodiscard]] TermVectorsReader *get_term_vectors_reader() override { return nullptr; }
    [[nodiscard]] NormsProducer *get_norms_reader() override { return nullptr; }
    [[nodiscard]] DocValuesProducer *get_doc_values_reader() override { return nullptr; }
    [[nodiscard]] FieldsProducer *get_postings_reader() override { return postings_; }
    [[nodiscard]] PointsReader *get_points_reader() override { return nullptr; }

    [[nodiscard]] Result<int32_t> max_doc() const override { return max_doc_; }
    [[nodiscard]] Result<int32_t> num_docs() const override { return num_docs_; }
    [[nodiscard]] Result<const FieldInfos *> field_infos() const override { return infos_.get(); }
    [[nodiscard]] Result<const Bits *> live_docs() const override { return live_.get(); }

private:
    FieldsProducer *postings_;
    int32_t max_doc_;
    int32_t num_docs_;
    std::shared_ptr<const Bits> live_;
    std::shared_ptr<const FieldInfos> infos_;
};

} // namespace

TEST(PostingsMergeTest, MergesTermsAndRebasesDocIds) {
    ByteBuffersDirectory directory;
    // Segment _0: three documents.
    write_segment(directory, "_0", 3, {{"a", {0, 2}}, {"b", {1}}});
    // Segment _1: three documents, the first one deleted.
    write_segment(directory, "_1", 3, {{"a", {0, 2}}, {"c", {1, 2}}});

    auto first = open_producer(directory, "_0", 3);
    ASSERT_TRUE(first != nullptr);
    auto second = open_producer(directory, "_1", 3);
    ASSERT_TRUE(second != nullptr);
    ASSERT_TRUE(first->check_integrity().has_value());

    auto live = std::make_shared<FixedBitSet>(3);
    live->set(1);
    live->set(2);

    auto infos = postings_infos();
    PostingsCodecReader reader_a(first.get(), 3, 3, nullptr, infos);
    PostingsCodecReader reader_b(second.get(), 3, 2, live, infos);
    std::vector<CodecReader *> readers{&reader_a, &reader_b};

    SegmentInfo merged_info = segment("_2", 0);
    auto merge_state = MergeState::create(readers, merged_info, std::make_shared<InfoStreamEnum>());
    ASSERT_TRUE(merge_state.has_value()) << merge_state.error().to_string();
    merge_state.value()->merge_field_infos = infos;
    // The merged segment has one document per live input document: deletions are
    // compacted away, so `max_doc` is the sum of the inputs' `num_docs`.
    ASSERT_EQ(merge_state.value()->segment_info.max_doc, 5);

    SegmentInfo write_info = segment("_2", 5);
    SegmentWriteState write_state(directory, infos, io_context_default());
    Lucene101PostingsFormat format;
    auto consumer = format.fields_consumer(write_state, write_info);
    ASSERT_TRUE(consumer.has_value()) << (consumer ? "" : consumer.error().to_string());
    auto merged = consumer.value()->merge(write_state, write_info, *merge_state.value(), nullptr);
    ASSERT_TRUE(merged.has_value()) << merged.error().to_string();
    ASSERT_TRUE(consumer.value()->close().has_value());

    auto producer = open_producer(directory, "_2", 5);
    ASSERT_TRUE(producer != nullptr);
    ASSERT_TRUE(producer->check_integrity().has_value());

    // The second segment's live documents are compacted and rebased to 3 and 4:
    // its deleted doc 0 is dropped, doc 1 (only "c") becomes 3 and doc 2 ("a"
    // and "c") becomes 4.
    EXPECT_EQ(read_terms(*producer),
              (TermsOfField{{"a", {0, 2, 4}}, {"b", {1}}, {"c", {3, 4}}}));
}

TEST(PostingsMergeTest, MergesPositionsAcrossSegments) {
    ByteBuffersDirectory directory;
    constexpr auto options = IndexOptions::DocsAndFreqsAndPositions;
    // Segment _0: "a" twice in doc 0 and once in doc 2.
    write_segment(directory, "_0", 3, {{"a", {0, 2}, {{0, 3}, {4}}}}, options);
    // Segment _1: "a" once in doc 0 and twice in doc 1, "b" once in doc 1.
    write_segment(directory, "_1", 2, {{"a", {0, 1}, {{2}, {0, 1}}}, {"b", {1}, {{5}}}}, options);

    auto first = open_producer(directory, "_0", 3, options);
    ASSERT_TRUE(first != nullptr);
    auto second = open_producer(directory, "_1", 2, options);
    ASSERT_TRUE(second != nullptr);

    auto infos = postings_infos(options);
    PostingsCodecReader reader_a(first.get(), 3, 3, nullptr, infos);
    PostingsCodecReader reader_b(second.get(), 2, 2, nullptr, infos);
    std::vector<CodecReader *> readers{&reader_a, &reader_b};

    SegmentInfo merged_info = segment("_2", 0);
    auto merge_state = MergeState::create(readers, merged_info, std::make_shared<InfoStreamEnum>());
    ASSERT_TRUE(merge_state.has_value()) << merge_state.error().to_string();
    merge_state.value()->merge_field_infos = infos;
    ASSERT_EQ(merge_state.value()->segment_info.max_doc, 5);

    SegmentInfo write_info = segment("_2", 5);
    SegmentWriteState write_state(directory, infos, io_context_default());
    Lucene101PostingsFormat format;
    auto consumer = format.fields_consumer(write_state, write_info);
    ASSERT_TRUE(consumer.has_value()) << (consumer ? "" : consumer.error().to_string());
    auto merged = consumer.value()->merge(write_state, write_info, *merge_state.value(), nullptr);
    ASSERT_TRUE(merged.has_value()) << merged.error().to_string();
    ASSERT_TRUE(consumer.value()->close().has_value());

    auto producer = open_producer(directory, "_2", 5, options);
    ASSERT_TRUE(producer != nullptr);
    ASSERT_TRUE(producer->check_integrity().has_value());

    // Segment _1's documents keep their ids (no deletions) but are rebased by
    // segment _0's 3 documents, and the positions of every occurrence survive.
    EXPECT_EQ(read_positions(*producer),
              (std::vector<std::string>{"a@0:0,3,", "a@2:4,", "a@3:2,", "a@4:0,1,", "b@4:5,"}));
}

TEST(PostingsMergeTest, SegmentWithoutPostingsIsSkipped) {
    ByteBuffersDirectory directory;
    write_segment(directory, "_0", 2, {{"a", {0, 1}}});

    auto first = open_producer(directory, "_0", 2);
    ASSERT_TRUE(first != nullptr);

    auto infos = postings_infos();
    PostingsCodecReader reader(first.get(), 2, 2, nullptr, infos);
    std::vector<CodecReader *> readers{&reader};

    SegmentInfo merged_info = segment("_1", 0);
    auto merge_state = MergeState::create(readers, merged_info, std::make_shared<InfoStreamEnum>());
    ASSERT_TRUE(merge_state.has_value());
    merge_state.value()->fields_producers[0] = nullptr;
    merge_state.value()->merge_field_infos = infos;

    SegmentInfo write_info = segment("_1", 2);
    SegmentWriteState write_state(directory, infos, io_context_default());
    Lucene101PostingsFormat format;
    auto consumer = format.fields_consumer(write_state, write_info);
    ASSERT_TRUE(consumer.has_value());
    auto merged = consumer.value()->merge(write_state, write_info, *merge_state.value(), nullptr);
    ASSERT_TRUE(merged.has_value()) << merged.error().to_string();
    ASSERT_TRUE(consumer.value()->close().has_value());

    auto producer = open_producer(directory, "_1", 2);
    ASSERT_TRUE(producer != nullptr);
    EXPECT_TRUE(read_terms(*producer).empty());
}
