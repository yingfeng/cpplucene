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
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module lucene_core:ut.segment_reader;

import lucene_core;

using namespace lucene;

namespace {

// ---------------------------------------------------------------------------
// A tiny in-memory index that the tests flush to a real directory, so that the
// readers are exercised against actual codec files instead of fakes.
// ---------------------------------------------------------------------------

/// One term's postings inside the in-memory index.
struct TermPostings {
    std::string term;
    /// `(doc, freq)` pairs, in document order.
    std::vector<std::pair<int32_t, int32_t>> docs;
};

/// The postings of one field.
struct FieldPostings {
    std::vector<TermPostings> terms;
};

/// What one test document holds.
struct DocSpec {
    std::string id;
    /// Space separated; each word becomes a separate term of `title`.
    std::string title;
    int64_t num{0};
};

/// A segment to flush: the documents plus the deletions to record.
struct SegmentSpec {
    std::string name{"_0"};
    std::vector<DocSpec> docs;
    /// Deleted documents, by local document number.
    std::vector<int32_t> deleted;
};

constexpr std::string_view kIdField = "id";
constexpr std::string_view kTitleField = "title";
constexpr std::string_view kNumField = "num";

std::vector<std::string> split_words(std::string_view text) {
    std::vector<std::string> words;
    std::string current;
    for (const char ch : text) {
        if (ch == ' ') {
            if (!current.empty()) {
                words.push_back(current);
                current.clear();
            }
        } else {
            current.push_back(ch);
        }
    }
    if (!current.empty()) {
        words.push_back(current);
    }
    return words;
}

/// Builds the postings of `title` from the documents.
FieldPostings title_postings(const std::vector<DocSpec> &docs) {
    std::map<std::string, std::vector<std::pair<int32_t, int32_t>>> by_term;
    for (size_t doc = 0; doc < docs.size(); ++doc) {
        std::map<std::string, int32_t> freqs;
        for (const auto &word : split_words(docs[doc].title)) {
            ++freqs[word];
        }
        for (const auto &[word, freq] : freqs) {
            by_term[word].emplace_back(static_cast<int32_t>(doc), freq);
        }
    }
    FieldPostings postings;
    for (auto &[term, docs_with_term] : by_term) {
        postings.terms.push_back(TermPostings{term, std::move(docs_with_term)});
    }
    return postings;
}

/// `NumericDocValues` over a fixed array, one value per document.
class ArrayNumericDocValues final : public NumericDocValues {
public:
    explicit ArrayNumericDocValues(std::vector<int64_t> values) : values_(std::move(values)) {}

    [[nodiscard]] int32_t doc_id() const override { return doc_; }

    [[nodiscard]] Result<int32_t> next_doc() override {
        ++doc_;
        if (doc_ >= static_cast<int32_t>(values_.size())) {
            doc_ = NO_MORE_DOCS;
            return NO_MORE_DOCS;
        }
        return doc_;
    }

    [[nodiscard]] Result<int32_t> advance(int32_t target) override {
        while (doc_ < target) {
            auto next = next_doc();
            if (!next.has_value()) {
                return next;
            }
            if (next.value() == NO_MORE_DOCS) {
                return NO_MORE_DOCS;
            }
        }
        return doc_;
    }

    [[nodiscard]] Result<int64_t> cost() const override { return static_cast<int64_t>(values_.size()); }

    [[nodiscard]] Result<bool> advance_exact(int32_t target) override {
        doc_ = target;
        return target >= 0 && target < static_cast<int32_t>(values_.size());
    }

    [[nodiscard]] Result<int64_t> long_value() override {
        if (doc_ < 0 || doc_ >= static_cast<int32_t>(values_.size())) {
            return err(LuceneError::illegal_state("doc values cursor is not positioned on a document"));
        }
        return values_[static_cast<size_t>(doc_)];
    }

private:
    std::vector<int64_t> values_;
    int32_t doc_{-1};
};

/// The `NormsProducer` half of the norms consumer.
class ArrayNormsProducer final : public NormsProducer {
public:
    explicit ArrayNormsProducer(std::vector<int64_t> values) : values_(std::move(values)) {}

    [[nodiscard]] Result<std::unique_ptr<NumericDocValues>> get_norms(const FieldInfo & /*field*/) const override {
        return std::unique_ptr<NumericDocValues>(new ArrayNumericDocValues(values_));
    }

    [[nodiscard]] VoidResult check_integrity() const override { return {}; }

private:
    std::vector<int64_t> values_;
};

/// The `DocValuesProducer` half of the doc values consumer.
class ArrayDocValuesProducer final : public DocValuesProducer {
public:
    explicit ArrayDocValuesProducer(std::vector<int64_t> values) : values_(std::move(values)) {}

    [[nodiscard]] Result<std::unique_ptr<NumericDocValues>> get_numeric(const FieldInfo & /*field*/) const override {
        return std::unique_ptr<NumericDocValues>(new ArrayNumericDocValues(values_));
    }

    [[nodiscard]] VoidResult check_integrity() const override { return {}; }

private:
    std::vector<int64_t> values_;
};

/// `PostingsEnum` over one term's `(doc, freq)` pairs, as the postings writer
/// pulls it.
class ArrayPostingsEnum final : public PostingsEnum {
public:
    explicit ArrayPostingsEnum(const TermPostings *postings) : postings_(postings) {}

    [[nodiscard]] int32_t doc_id() const override { return doc_; }

    [[nodiscard]] Result<int32_t> next_doc() override {
        position_ = -1;
        if (index_ >= postings_->docs.size()) {
            doc_ = NO_MORE_DOCS;
            return NO_MORE_DOCS;
        }
        doc_ = postings_->docs[index_].first;
        ++index_;
        return doc_;
    }

    [[nodiscard]] Result<int32_t> advance(int32_t target) override {
        while (doc_ < target) {
            auto next = next_doc();
            if (!next.has_value()) {
                return next;
            }
            if (next.value() == NO_MORE_DOCS) {
                return NO_MORE_DOCS;
            }
        }
        return doc_;
    }

    [[nodiscard]] Result<int64_t> cost() const override { return static_cast<int64_t>(postings_->docs.size()); }

    [[nodiscard]] Result<int32_t> freq() override {
        if (index_ == 0 || index_ > postings_->docs.size()) {
            return 1;
        }
        return postings_->docs[index_ - 1].second;
    }

    [[nodiscard]] Result<int32_t> next_position() override { return ++position_; }
    [[nodiscard]] Result<int32_t> start_offset() const override { return position_ * 10; }
    [[nodiscard]] Result<int32_t> end_offset() const override { return position_ * 10 + 5; }

    [[nodiscard]] Result<std::optional<std::span<const uint8_t>>> get_payload() const override {
        return std::optional<std::span<const uint8_t>>{};
    }

private:
    const TermPostings *postings_;
    size_t index_{0};
    int32_t doc_{-1};
    int32_t position_{-1};
};

/// `TermsEnum` over one field's sorted terms.
class ArrayTermsEnum final : public EmptyTermsEnum {
public:
    explicit ArrayTermsEnum(const FieldPostings *postings) : postings_(postings) {}

    [[nodiscard]] Result<const BytesRef *> next() override {
        if (index_ >= postings_->terms.size()) {
            return static_cast<const BytesRef *>(nullptr);
        }
        const TermPostings &term = postings_->terms[index_++];
        current_ = BytesRef::from_string(term.term);
        current_postings_ = &term;
        return &current_;
    }

    [[nodiscard]] Result<SeekStatus> seek_ceil(std::span<const uint8_t> term) override {
        const std::string_view target(reinterpret_cast<const char *>(term.data()), term.size());
        for (size_t i = 0; i < postings_->terms.size(); ++i) {
            if (postings_->terms[i].term >= target) {
                index_ = i;
                current_ = BytesRef::from_string(postings_->terms[i].term);
                current_postings_ = &postings_->terms[i];
                index_ = i + 1;
                return postings_->terms[i].term == target ? SeekStatus::Found : SeekStatus::NotFound;
            }
        }
        index_ = postings_->terms.size();
        current_postings_ = nullptr;
        return SeekStatus::End;
    }

    [[nodiscard]] Result<const BytesRef *> term() const override { return &current_; }

    [[nodiscard]] Result<int32_t> doc_freq() override {
        return static_cast<int32_t>(current_postings_->docs.size());
    }

    [[nodiscard]] Result<int64_t> total_term_freq() override {
        int64_t total = 0;
        for (const auto &[doc, freq] : current_postings_->docs) {
            total += freq;
        }
        return total;
    }

    [[nodiscard]] Result<std::unique_ptr<PostingsEnum>>
    postings_with_flags(std::unique_ptr<PostingsEnum> /*reuse*/, int32_t /*flags*/) override {
        return std::unique_ptr<PostingsEnum>(new ArrayPostingsEnum(current_postings_));
    }

private:
    const FieldPostings *postings_;
    size_t index_{0};
    BytesRef current_{};
    const TermPostings *current_postings_{nullptr};
};

/// `Terms` over one field's postings.
class ArrayTerms final : public Terms {
public:
    explicit ArrayTerms(const FieldPostings *postings) : postings_(postings) {}

    [[nodiscard]] Result<std::unique_ptr<TermsEnum>> iterator() const override {
        return std::unique_ptr<TermsEnum>(new ArrayTermsEnum(postings_));
    }

    [[nodiscard]] Result<int64_t> size() const override { return static_cast<int64_t>(postings_->terms.size()); }

    [[nodiscard]] Result<int64_t> sum_total_term_freq() const override {
        int64_t total = 0;
        for (const auto &term : postings_->terms) {
            for (const auto &[doc, freq] : term.docs) {
                total += freq;
            }
        }
        return total;
    }

    [[nodiscard]] Result<int64_t> sum_doc_freq() const override {
        int64_t total = 0;
        for (const auto &term : postings_->terms) {
            total += static_cast<int64_t>(term.docs.size());
        }
        return total;
    }

    [[nodiscard]] Result<int32_t> doc_count() const override {
        int32_t max_doc = -1;
        for (const auto &term : postings_->terms) {
            for (const auto &[doc, freq] : term.docs) {
                max_doc = std::max(max_doc, doc);
            }
        }
        return max_doc + 1;
    }

    [[nodiscard]] bool has_freqs() const override { return true; }
    [[nodiscard]] bool has_offsets() const override { return false; }
    [[nodiscard]] bool has_positions() const override { return false; }
    [[nodiscard]] bool has_payloads() const override { return false; }

private:
    const FieldPostings *postings_;
};

/// `FieldNameIterator` over the names of the fields being flushed.
class ArrayFieldNames final : public FieldNameIterator {
public:
    explicit ArrayFieldNames(std::vector<std::string> names) : names_(std::move(names)) {}

    [[nodiscard]] Result<const std::string *> next() override {
        if (index_ >= names_.size()) {
            return static_cast<const std::string *>(nullptr);
        }
        return &names_[index_++];
    }

private:
    std::vector<std::string> names_;
    size_t index_{0};
};

/// `Fields` over the postings of one document batch.
class ArrayFields final : public Fields {
public:
    explicit ArrayFields(const FieldPostings *title) : title_(title) {}

    [[nodiscard]] Result<std::unique_ptr<FieldNameIterator>> iterator() const override {
        return std::unique_ptr<FieldNameIterator>(new ArrayFieldNames({std::string(kTitleField)}));
    }

    [[nodiscard]] Result<std::unique_ptr<Terms>> terms(std::string_view field) const override {
        if (field != kTitleField) {
            return std::unique_ptr<Terms>{};
        }
        return std::unique_ptr<Terms>(new ArrayTerms(title_));
    }

    [[nodiscard]] Result<int32_t> size() const override { return 1; }

private:
    const FieldPostings *title_;
};

FieldInfo text_field(int32_t number, std::string name, IndexOptions options) {
    FieldInfo info;
    info.number = number;
    info.name = std::move(name);
    info.index_options = options;
    info.omit_norms = false;
    return info;
}

FieldInfo stored_only_field(int32_t number, std::string name) {
    FieldInfo info;
    info.number = number;
    info.name = std::move(name);
    info.index_options = IndexOptions::None;
    info.omit_norms = true;
    return info;
}

std::vector<uint8_t> segment_id(uint8_t seed) {
    std::vector<uint8_t> id(ID_LENGTH, 0);
    for (size_t i = 0; i < id.size(); ++i) {
        id[i] = static_cast<uint8_t>(seed + i);
    }
    return id;
}

using FieldInfosPtr = std::shared_ptr<const FieldInfos>;

/// Flushes `spec` into `dir` and returns the commit that references it.
///
/// This plays the role `IndexWriter` plays in Lucene: it writes every codec
/// file of one segment (stored fields, norms, doc values, postings), then the
/// `.fnm` and `.si` metadata.  The writer is not ported yet, so the tests drive
/// the codecs directly.
SegmentCommitInfo write_segment(ByteBuffersDirectory &dir, const SegmentSpec &spec) {
    const Codec &codec = CodecFormats::default_codec();
    const IOContext &context = io_context_default();

    const FieldInfo id_field = stored_only_field(0, std::string(kIdField));

    FieldInfo title_field = text_field(1, std::string(kTitleField), IndexOptions::DocsAndFreqs);
    title_field.store_term_vector = false;

    FieldInfo num_field = stored_only_field(2, std::string(kNumField));
    num_field.doc_values_type = DocValuesType::Numeric;

    auto field_infos = FieldInfos::of({id_field, title_field, num_field});
    EXPECT_TRUE(field_infos.has_value()) << (field_infos ? "" : field_infos.error().to_string());
    FieldInfosPtr infos = std::make_shared<const FieldInfos>(std::move(field_infos.value()));

    SegmentInfo info;
    info.name = spec.name;
    info.id = segment_id(static_cast<uint8_t>(spec.name.size() + 7));
    info.version = Version{10, 1, 0};
    info.min_version = Version{10, 0, 0};
    info.max_doc = static_cast<int32_t>(spec.docs.size());
    info.codec_name = std::string(Lucene101Codec::CODEC_NAME);
    info.diagnostics.emplace("source", "flush");

    // 1. Stored fields.  This runs first because the format records its
    //    compression mode as a segment attribute, and `.si` has to include it.
    auto stored = codec.stored_fields_format()->fields_writer(dir, info, context);
    EXPECT_TRUE(stored.has_value()) << (stored ? "" : stored.error().to_string());
    for (const DocSpec &doc : spec.docs) {
        EXPECT_TRUE(stored.value()->start_document().has_value());
        EXPECT_TRUE(stored.value()->write_field_str(id_field, doc.id).has_value());
        EXPECT_TRUE(stored.value()->write_field_str(title_field, doc.title).has_value());
        EXPECT_TRUE(stored.value()->finish_document().has_value());
    }
    EXPECT_TRUE(stored.value()->finish(static_cast<int32_t>(spec.docs.size())).has_value());
    EXPECT_TRUE(stored.value()->close().has_value());

    SegmentWriteState state(get_default_info_stream(), dir, infos, context, "");

    // 2. Norms: one value per document.  The readers hand them back verbatim.
    std::vector<int64_t> norms(spec.docs.size(), 1);
    auto norms_consumer = codec.norms_format()->norms_consumer(state, info);
    EXPECT_TRUE(norms_consumer.has_value()) << (norms_consumer ? "" : norms_consumer.error().to_string());
    ArrayNormsProducer norms_producer(norms);
    EXPECT_TRUE(norms_consumer.value()->add_norms_field(title_field, norms_producer).has_value());
    EXPECT_TRUE(norms_consumer.value()->close().has_value());

    // 3. Doc values for `num`.
    std::vector<int64_t> numbers;
    numbers.reserve(spec.docs.size());
    for (const DocSpec &doc : spec.docs) {
        numbers.push_back(doc.num);
    }
    auto doc_values = codec.doc_values_format()->fields_consumer(state, info);
    EXPECT_TRUE(doc_values.has_value()) << (doc_values ? "" : doc_values.error().to_string());
    ArrayDocValuesProducer doc_values_producer(numbers);
    EXPECT_TRUE(doc_values.value()->add_numeric_field(num_field, doc_values_producer).has_value());
    EXPECT_TRUE(doc_values.value()->close().has_value());

    // 4. Postings, through the block tree terms dictionary.
    FieldPostings postings = title_postings(spec.docs);
    ArrayFields fields(&postings);
    auto postings_consumer = codec.postings_format()->fields_consumer(state, info);
    EXPECT_TRUE(postings_consumer.has_value()) << (postings_consumer ? "" : postings_consumer.error().to_string());
    auto written = postings_consumer.value()->write(state, info, fields, &norms_producer);
    EXPECT_TRUE(written.has_value()) << (written ? "" : written.error().to_string());
    EXPECT_TRUE(postings_consumer.value()->close().has_value());

    // 5. Segment metadata.
    EXPECT_TRUE(codec.field_infos_format().write(dir, info, "", *infos, context).has_value());
    EXPECT_TRUE(codec.segment_info_format().write(dir, info, context).has_value());

    SegmentCommitInfo commit;
    commit.info = std::move(info);

    // 6. Deletions.
    if (!spec.deleted.empty()) {
        FixedBitSet live_docs(static_cast<size_t>(commit.info.max_doc));
        for (int32_t doc = 0; doc < commit.info.max_doc; ++doc) {
            live_docs.set(static_cast<size_t>(doc));
        }
        for (const int32_t doc : spec.deleted) {
            live_docs.clear(static_cast<size_t>(doc));
        }
        const int32_t deleted = static_cast<int32_t>(spec.deleted.size());
        auto written = codec.live_docs_format().write_live_docs(live_docs, dir, commit, deleted, context);
        EXPECT_TRUE(written.has_value()) << (written ? "" : written.error().to_string());
        commit.del_gen = commit.next_del_gen();
        commit.del_count = deleted;
    }

    return commit;
}

/// Writes a commit that references `commits`.
void commit_segments(ByteBuffersDirectory &dir, std::vector<SegmentCommitInfo> commits, int64_t version,
                     int64_t generation) {
    SegmentInfos infos;
    infos.version = version;
    infos.counter = static_cast<int64_t>(commits.size());
    infos.generation = generation;
    infos.id = segment_id(1);
    for (auto &commit : commits) {
        ASSERT_TRUE(infos.add(std::move(commit)).has_value());
    }
    ASSERT_TRUE(infos.write_to(dir, io_context_default()).has_value());
}

std::shared_ptr<DirectoryReader> open_reader(ByteBuffersDirectory &dir) {
    auto reader = DirectoryReader::open(dir);
    EXPECT_TRUE(reader.has_value()) << (reader ? "" : reader.error().to_string());
    return reader.has_value() ? reader.value() : nullptr;
}

std::vector<int32_t> docs_of(const TopDocs &top_docs) {
    std::vector<int32_t> docs;
    for (const ScoreDoc &hit : top_docs.score_docs) {
        docs.push_back(hit.doc);
    }
    return docs;
}

Term title_term(std::string_view text) {
    return Term(std::string(kTitleField), BytesRef::from_string(text));
}

} // namespace

TEST(SegmentReaderTest, OpensEveryCodecReaderOfASegment) {
    ByteBuffersDirectory dir;
    SegmentSpec spec;
    spec.docs = {{"a", "alpha beta", 10}, {"b", "beta gamma", 20}, {"c", "beta", 30}};
    auto commit = write_segment(dir, spec);

    auto reader = SegmentReader::open(dir, commit, 10, io_context_default());
    ASSERT_TRUE(reader.has_value()) << (reader ? "" : reader.error().to_string());
    EXPECT_EQ(reader.value()->max_doc().value(), 3);
    EXPECT_EQ(reader.value()->num_docs().value(), 3);
    EXPECT_EQ(reader.value()->segment_name(), "_0");
    EXPECT_FALSE(reader.value()->has_deletions().value());
    EXPECT_EQ(reader.value()->live_docs().value(), nullptr);
    EXPECT_EQ(reader.value()->to_string(), "SegmentReader(_0 max_doc=3 num_docs=3)");

    // Every reader family the segment has is wired up.
    EXPECT_NE(reader.value()->get_fields_reader(), nullptr);
    EXPECT_NE(reader.value()->get_postings_reader(), nullptr);
    EXPECT_NE(reader.value()->get_norms_reader(), nullptr);
    EXPECT_NE(reader.value()->get_doc_values_reader(), nullptr);
    // This segment stores no term vectors and no points.
    EXPECT_EQ(reader.value()->get_term_vectors_reader(), nullptr);
    EXPECT_EQ(reader.value()->get_points_reader(), nullptr);

    // Field infos come from `.fnm`.
    auto infos = reader.value()->field_infos();
    ASSERT_TRUE(infos.has_value());
    ASSERT_NE(infos.value(), nullptr);
    EXPECT_EQ(infos.value()->size(), 3U);
    EXPECT_NE(infos.value()->by_name(kTitleField), nullptr);

    // A leaf reader is its own single leaf.
    auto leaves = reader.value()->leaves();
    ASSERT_TRUE(leaves.has_value());
    ASSERT_EQ(leaves.value().size(), 1U);
    EXPECT_EQ(leaves.value()[0].doc_base(), 0);
    EXPECT_EQ(leaves.value()[0].reader(), reader.value().get());
    EXPECT_EQ(leaves.value()[0].top_parent().leaves_num, 1);
    EXPECT_EQ(leaves.value()[0].top_parent().max_doc, 3);

    EXPECT_TRUE(reader.value()->check_integrity().has_value());
}

TEST(SegmentReaderTest, ReadsStoredFieldsNormsAndDocValues) {
    ByteBuffersDirectory dir;
    SegmentSpec spec;
    spec.docs = {{"a", "alpha beta", 10}, {"b", "beta gamma", 20}};
    auto commit = write_segment(dir, spec);

    auto reader = SegmentReader::open(dir, commit, 10, io_context_default());
    ASSERT_TRUE(reader.has_value()) << (reader ? "" : reader.error().to_string());

    auto fields = reader.value()->stored_fields();
    ASSERT_TRUE(fields.has_value()) << (fields ? "" : fields.error().to_string());
    auto doc = fields.value()->document(1);
    ASSERT_TRUE(doc.has_value()) << (doc ? "" : doc.error().to_string());
    auto id = doc.value().get(kIdField);
    ASSERT_TRUE(id.has_value());
    ASSERT_TRUE(id.value().has_value());
    EXPECT_EQ(id.value().value(), "b");
    auto title = doc.value().get(kTitleField);
    ASSERT_TRUE(title.has_value());
    ASSERT_TRUE(title.value().has_value());
    EXPECT_EQ(title.value().value(), "beta gamma");

    // The document id range is checked before it reaches the codec.
    EXPECT_FALSE(fields.value()->document(2).has_value());
    EXPECT_FALSE(fields.value()->document(-1).has_value());

    // Norms: one value per document, read through the norms producer.
    auto norms = reader.value()->get_norm_values(kTitleField);
    ASSERT_TRUE(norms.has_value()) << (norms ? "" : norms.error().to_string());
    ASSERT_NE(norms.value(), nullptr);
    for (int32_t doc_id = 0; doc_id < 2; ++doc_id) {
        ASSERT_EQ(norms.value()->advance(doc_id).value(), doc_id);
        EXPECT_EQ(norms.value()->long_value().value(), 1);
    }

    // Doc values of `num`.
    auto values = reader.value()->get_numeric_doc_values(kNumField);
    ASSERT_TRUE(values.has_value()) << (values ? "" : values.error().to_string());
    ASSERT_NE(values.value(), nullptr);
    ASSERT_EQ(values.value()->advance(0).value(), 0);
    EXPECT_EQ(values.value()->long_value().value(), 10);
    ASSERT_EQ(values.value()->advance(1).value(), 1);
    EXPECT_EQ(values.value()->long_value().value(), 20);

    // A field without that doc values type answers "absent", not an error.
    EXPECT_EQ(reader.value()->get_numeric_doc_values(kTitleField).value(), nullptr);
    EXPECT_EQ(reader.value()->get_numeric_doc_values("missing").value(), nullptr);
    // The same holds for norms of a field that has none.
    EXPECT_EQ(reader.value()->get_norm_values(kIdField).value(), nullptr);
}

TEST(SegmentReaderTest, ExposesTermsAndStatistics) {
    ByteBuffersDirectory dir;
    SegmentSpec spec;
    spec.docs = {{"a", "alpha beta", 10}, {"b", "beta gamma", 20}, {"c", "beta", 30}};
    auto commit = write_segment(dir, spec);

    auto reader = SegmentReader::open(dir, commit, 10, io_context_default());
    ASSERT_TRUE(reader.has_value()) << (reader ? "" : reader.error().to_string());

    // "beta" is in all three documents, "alpha" only in the first.
    EXPECT_EQ(reader.value()->doc_freq(title_term("beta")).value(), 3);
    EXPECT_EQ(reader.value()->doc_freq(title_term("alpha")).value(), 1);
    EXPECT_EQ(reader.value()->doc_freq(title_term("missing")).value(), 0);
    EXPECT_EQ(reader.value()->total_term_freq(title_term("beta")).value(), 3);
    EXPECT_EQ(reader.value()->doc_count(kTitleField).value(), 3);
    EXPECT_EQ(reader.value()->sum_doc_freq(kTitleField).value(), 5);
    EXPECT_EQ(reader.value()->sum_total_term_freq(kTitleField).value(), 5);
    // A field that is not indexed has no terms at all.
    EXPECT_EQ(reader.value()->terms(kIdField).value(), nullptr);
    EXPECT_EQ(reader.value()->terms("missing").value(), nullptr);
}

TEST(SegmentReaderTest, AppliesLiveDocs) {
    ByteBuffersDirectory dir;
    SegmentSpec spec;
    spec.name = "_0";
    spec.docs = {{"a", "alpha", 10}, {"b", "beta", 20}, {"c", "gamma", 30}};
    spec.deleted = {1};
    auto commit = write_segment(dir, spec);
    commit_segments(dir, {commit}, 1, 1);

    auto reader = open_reader(dir);
    ASSERT_NE(reader, nullptr);
    EXPECT_EQ(reader->max_doc().value(), 3);
    EXPECT_EQ(reader->num_docs().value(), 2);
    EXPECT_EQ(reader->num_deleted_docs().value(), 1);
    EXPECT_TRUE(reader->has_deletions().value());

    const auto &segment = reader->segment_readers()[0];
    auto live = segment->live_docs();
    ASSERT_TRUE(live.has_value());
    ASSERT_NE(live.value(), nullptr);
    EXPECT_TRUE(live.value()->get(0).value());
    EXPECT_FALSE(live.value()->get(1).value());
    EXPECT_TRUE(live.value()->get(2).value());
}

TEST(DirectoryReaderTest, OpensTheLatestCommit) {
    ByteBuffersDirectory dir;
    SegmentSpec first;
    first.name = "_0";
    first.docs = {{"a", "alpha", 1}, {"b", "beta", 2}};
    SegmentSpec second;
    second.name = "_1";
    second.docs = {{"c", "gamma", 3}};
    commit_segments(dir, {write_segment(dir, first), write_segment(dir, second)}, 7, 1);

    auto reader = open_reader(dir);
    ASSERT_NE(reader, nullptr);
    EXPECT_EQ(reader->max_doc().value(), 3);
    EXPECT_EQ(reader->num_docs().value(), 3);
    EXPECT_EQ(reader->version(), 7);
    EXPECT_EQ(reader->segment_infos().size(), 2U);
    ASSERT_EQ(reader->segment_readers().size(), 2U);

    // Document bases follow the segment order.
    auto leaves = reader->leaves();
    ASSERT_TRUE(leaves.has_value());
    ASSERT_EQ(leaves.value().size(), 2U);
    EXPECT_EQ(leaves.value()[0].doc_base(), 0);
    EXPECT_EQ(leaves.value()[0].ord(), 0);
    EXPECT_EQ(leaves.value()[1].doc_base(), 2);
    EXPECT_EQ(leaves.value()[1].ord(), 1);
    EXPECT_EQ(leaves.value()[0].top_parent().leaves_num, 2);
    EXPECT_EQ(leaves.value()[0].top_parent().max_doc, 3);
    EXPECT_EQ(leaves.value()[1].top_parent().id.to_string(), reader->identity().to_string());

    // Statistics aggregate over the segments.
    EXPECT_EQ(reader->doc_count(kTitleField).value(), 3);
    EXPECT_EQ(reader->doc_freq(title_term("beta")).value(), 1);

    // Stored fields dispatch to the right segment.
    auto fields = reader->stored_fields();
    ASSERT_TRUE(fields.has_value()) << (fields ? "" : fields.error().to_string());
    auto doc = fields.value()->document(2);
    ASSERT_TRUE(doc.has_value()) << (doc ? "" : doc.error().to_string());
    auto id = doc.value().get(kIdField);
    ASSERT_TRUE(id.has_value());
    ASSERT_TRUE(id.value().has_value());
    EXPECT_EQ(id.value().value(), "c");
    EXPECT_FALSE(fields.value()->document(3).has_value());

    // Term vectors are absent everywhere but still usable.
    auto vectors = reader->term_vectors();
    ASSERT_TRUE(vectors.has_value()) << (vectors ? "" : vectors.error().to_string());
    EXPECT_EQ(vectors.value()->get(0).value(), nullptr);
}

TEST(DirectoryReaderTest, SearchesRealSegments) {
    ByteBuffersDirectory dir;
    SegmentSpec first;
    first.name = "_0";
    first.docs = {{"a", "alpha beta", 1}, {"b", "beta", 2}};
    SegmentSpec second;
    second.name = "_1";
    second.docs = {{"c", "beta gamma", 3}};
    commit_segments(dir, {write_segment(dir, first), write_segment(dir, second)}, 3, 1);

    auto reader = open_reader(dir);
    ASSERT_NE(reader, nullptr);
    auto searcher = std::make_shared<IndexSearcher>(reader);

    TermQuery query(title_term("beta"));
    auto top_docs = searcher->search(query, 10);
    ASSERT_TRUE(top_docs.has_value()) << (top_docs ? "" : top_docs.error().to_string());
    ASSERT_EQ(top_docs.value().total_hits.value, 3U);
    // Document numbers are global: the second segment's doc is 2, not 0.
    std::vector<int32_t> docs = docs_of(top_docs.value());
    std::sort(docs.begin(), docs.end());
    EXPECT_EQ(docs, (std::vector<int32_t>{0, 1, 2}));

    // A term only present in the first segment.
    TermQuery single(title_term("alpha"));
    auto single_docs = searcher->search(single, 10);
    ASSERT_TRUE(single_docs.has_value()) << (single_docs ? "" : single_docs.error().to_string());
    EXPECT_EQ(docs_of(single_docs.value()), (std::vector<int32_t>{0}));

    // BooleanQuery across the segment boundary.
    BooleanQuery::Builder builder;
    ASSERT_TRUE(builder.add_clause(BooleanClause::create(std::make_unique<TermQuery>(title_term("beta")),
                                                         Occur::Must))
                        .has_value());
    ASSERT_TRUE(builder.add_clause(BooleanClause::create(std::make_unique<TermQuery>(title_term("gamma")),
                                                         Occur::MustNot))
                        .has_value());
    auto built = builder.build();
    ASSERT_TRUE(built.has_value()) << (built ? "" : built.error().to_string());
    auto boolean_docs = searcher->search(built.value(), 10);
    ASSERT_TRUE(boolean_docs.has_value()) << (boolean_docs ? "" : boolean_docs.error().to_string());
    docs = docs_of(boolean_docs.value());
    std::sort(docs.begin(), docs.end());
    EXPECT_EQ(docs, (std::vector<int32_t>{0, 1}));
}

TEST(DirectoryReaderTest, HandlesAnEmptyIndex) {
    ByteBuffersDirectory dir;
    commit_segments(dir, {}, 1, 1);

    auto reader = open_reader(dir);
    ASSERT_NE(reader, nullptr);
    EXPECT_EQ(reader->max_doc().value(), 0);
    EXPECT_EQ(reader->num_docs().value(), 0);
    EXPECT_TRUE(reader->segment_readers().empty());
    auto leaves = reader->leaves();
    ASSERT_TRUE(leaves.has_value());
    EXPECT_TRUE(leaves.value().empty());

    auto searcher = std::make_shared<IndexSearcher>(reader);
    TermQuery query(title_term("anything"));
    auto top_docs = searcher->search(query, 10);
    ASSERT_TRUE(top_docs.has_value()) << (top_docs ? "" : top_docs.error().to_string());
    EXPECT_TRUE(top_docs.value().score_docs.empty());
    EXPECT_EQ(top_docs.value().total_hits.value, 0U);
}

TEST(DirectoryReaderTest, ReopensOnlyWhenTheCommitMoved) {
    ByteBuffersDirectory dir;
    SegmentSpec first;
    first.docs = {{"a", "alpha", 1}};
    commit_segments(dir, {write_segment(dir, first)}, 1, 1);

    auto reader = open_reader(dir);
    ASSERT_NE(reader, nullptr);
    EXPECT_TRUE(reader->is_current().value());
    // Same commit: nothing to reopen.
    auto same = reader->open_if_changed();
    ASSERT_TRUE(same.has_value()) << (same ? "" : same.error().to_string());
    EXPECT_EQ(same.value(), nullptr);

    SegmentSpec second;
    second.name = "_1";
    second.docs = {{"b", "beta", 2}};
    commit_segments(dir, {write_segment(dir, second)}, 2, 2);

    EXPECT_FALSE(reader->is_current().value());
    auto reopened = reader->open_if_changed();
    ASSERT_TRUE(reopened.has_value()) << (reopened ? "" : reopened.error().to_string());
    ASSERT_NE(reopened.value(), nullptr);
    EXPECT_EQ(reopened.value()->max_doc().value(), 1);
    EXPECT_EQ(reopened.value()->version(), 2);
    EXPECT_EQ(reopened.value()->segment_readers()[0]->segment_name(), "_1");
    EXPECT_TRUE(reopened.value()->is_current().value());
}

TEST(DirectoryReaderTest, RejectsAMissingCommit) {
    ByteBuffersDirectory dir;
    auto reader = DirectoryReader::open(dir);
    ASSERT_FALSE(reader.has_value());
    EXPECT_EQ(reader.error().kind(), ErrorKind::kIndexNotFound);
}
