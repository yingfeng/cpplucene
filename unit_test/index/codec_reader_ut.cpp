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
#include <span>
#include <string>
#include <string_view>
#include <vector>

module lucene_core:ut.codec_reader;

import lucene_core;

using namespace lucene;

namespace {

// ---------------------------------------------------------------------------
// Fakes.  They record what `CodecReader` asked them for, so the tests can tell
// "the getter was gated by the field info" from "the getter was forwarded".
// ---------------------------------------------------------------------------

/// [`Terms`] over a single term; enough to see that the lookup was forwarded.
class SingleTermTerms final : public Terms {
public:
    explicit SingleTermTerms(std::string term) : term_(std::move(term)) {}

    [[nodiscard]] Result<std::unique_ptr<TermsEnum>> iterator() const override {
        return err(LuceneError::unsupported_operation("SingleTermTerms has no iterator"));
    }
    [[nodiscard]] Result<int64_t> size() const override { return 1; }
    [[nodiscard]] Result<int64_t> sum_total_term_freq() const override { return 1; }
    [[nodiscard]] Result<int64_t> sum_doc_freq() const override { return 1; }
    [[nodiscard]] Result<int32_t> doc_count() const override { return 1; }
    [[nodiscard]] bool has_freqs() const override { return true; }
    [[nodiscard]] bool has_offsets() const override { return false; }
    [[nodiscard]] bool has_positions() const override { return false; }
    [[nodiscard]] bool has_payloads() const override { return false; }

    [[nodiscard]] const std::string &term() const noexcept { return term_; }

private:
    std::string term_;
};

class RecordingFieldsProducer final : public FieldsProducer {
public:
    [[nodiscard]] Result<std::unique_ptr<FieldNameIterator>> iterator() const override {
        return err(LuceneError::unsupported_operation("RecordingFieldsProducer has no iterator"));
    }

    [[nodiscard]] Result<std::unique_ptr<Terms>> terms(std::string_view field) const override {
        ++terms_calls;
        last_field = std::string(field);
        return std::unique_ptr<Terms>(new SingleTermTerms(std::string(field) + ":term"));
    }

    [[nodiscard]] Result<int32_t> size() const override { return 1; }

    [[nodiscard]] VoidResult check_integrity() const override {
        ++integrity_calls;
        return {};
    }

    mutable int terms_calls{0};
    mutable int integrity_calls{0};
    mutable std::string last_field;
};

/// Shared between a reader and its clones: the cursors are handed out by clone,
/// so the assertions have to look at state both share.
struct StoredFieldsRecord {
    int clone_calls{0};
    int integrity_calls{0};
    std::vector<int32_t> visited;
    std::vector<int32_t> prefetched;
};

class RecordingStoredFields final : public StoredFieldsReader {
public:
    explicit RecordingStoredFields(std::shared_ptr<StoredFieldsRecord> record)
        : record_(std::move(record)) {}

    [[nodiscard]] VoidResult prefetch(int32_t doc_id) override {
        record_->prefetched.push_back(doc_id);
        return {};
    }

    [[nodiscard]] VoidResult document_with_visitor(int32_t doc_id, StoredFieldVisitor & /*visitor*/) override {
        record_->visited.push_back(doc_id);
        return {};
    }

    [[nodiscard]] Result<Document> document(int32_t doc_id) override {
        record_->visited.push_back(doc_id);
        return Document{};
    }

    [[nodiscard]] VoidResult check_integrity() const override {
        ++record_->integrity_calls;
        return {};
    }

    [[nodiscard]] Result<std::unique_ptr<StoredFieldsReader>> get_merge_instance() const override {
        return std::unique_ptr<StoredFieldsReader>{};
    }

    [[nodiscard]] Result<std::unique_ptr<StoredFieldsReader>> clone() const override {
        ++record_->clone_calls;
        return std::unique_ptr<StoredFieldsReader>(new RecordingStoredFields(record_));
    }

private:
    std::shared_ptr<StoredFieldsRecord> record_;
};

struct TermVectorsRecord {
    int clone_calls{0};
    int integrity_calls{0};
    std::vector<int32_t> read;
};

class RecordingTermVectors final : public TermVectorsReader {
public:
    explicit RecordingTermVectors(std::shared_ptr<TermVectorsRecord> record) : record_(std::move(record)) {}

    [[nodiscard]] Result<std::unique_ptr<Fields>> get(int32_t doc) override {
        record_->read.push_back(doc);
        return std::unique_ptr<Fields>{};
    }

    [[nodiscard]] VoidResult check_integrity() const override {
        ++record_->integrity_calls;
        return {};
    }

    [[nodiscard]] Result<std::unique_ptr<TermVectorsReader>> get_merge_instance() const override {
        return std::unique_ptr<TermVectorsReader>{};
    }

    [[nodiscard]] Result<std::unique_ptr<TermVectorsReader>> clone() const override {
        ++record_->clone_calls;
        return std::unique_ptr<TermVectorsReader>(new RecordingTermVectors(record_));
    }

private:
    std::shared_ptr<TermVectorsRecord> record_;
};

/// Doc values producer that answers every getter, so the test can tell which
/// one `CodecReader` picked.
class RecordingDocValuesProducer final : public DocValuesProducer {
public:
    [[nodiscard]] Result<std::unique_ptr<NumericDocValues>> get_numeric(const FieldInfo &field) const override {
        ++numeric_calls;
        last_field = field.name;
        return std::unique_ptr<NumericDocValues>(new EmptyNumericDocValues());
    }

    [[nodiscard]] Result<std::unique_ptr<BinaryDocValues>> get_binary(const FieldInfo &field) const override {
        ++binary_calls;
        last_field = field.name;
        return std::unique_ptr<BinaryDocValues>(new EmptyBinaryDocValues());
    }

    [[nodiscard]] Result<std::unique_ptr<SortedDocValues>> get_sorted(const FieldInfo &field) const override {
        ++sorted_calls;
        last_field = field.name;
        return std::unique_ptr<SortedDocValues>(new EmptySortedDocValues());
    }

    [[nodiscard]] Result<std::unique_ptr<SortedNumericDocValues>> get_sorted_numeric(
            const FieldInfo &field) const override {
        ++sorted_numeric_calls;
        last_field = field.name;
        return std::unique_ptr<SortedNumericDocValues>(new EmptySortedNumericDocValues());
    }

    [[nodiscard]] Result<std::unique_ptr<SortedSetDocValues>> get_sorted_set(const FieldInfo &field) const override {
        ++sorted_set_calls;
        last_field = field.name;
        return std::unique_ptr<SortedSetDocValues>(new EmptySortedSetDocValues());
    }

    [[nodiscard]] Result<std::unique_ptr<DocValuesSkipper>> get_skipper(const FieldInfo &field) const override {
        ++skipper_calls;
        last_field = field.name;
        return std::unique_ptr<DocValuesSkipper>{};
    }

    [[nodiscard]] VoidResult check_integrity() const override {
        ++integrity_calls;
        return {};
    }

    mutable int numeric_calls{0};
    mutable int binary_calls{0};
    mutable int sorted_calls{0};
    mutable int sorted_numeric_calls{0};
    mutable int sorted_set_calls{0};
    mutable int skipper_calls{0};
    mutable int integrity_calls{0};
    mutable std::string last_field;
};

class RecordingNormsProducer final : public NormsProducer {
public:
    [[nodiscard]] Result<std::unique_ptr<NumericDocValues>> get_norms(const FieldInfo &field) const override {
        ++calls;
        last_field = field.name;
        return std::unique_ptr<NumericDocValues>(new EmptyNumericDocValues());
    }

    [[nodiscard]] VoidResult check_integrity() const override {
        ++integrity_calls;
        return {};
    }

    mutable int calls{0};
    mutable int integrity_calls{0};
    mutable std::string last_field;
};

class RecordingPointsReader final : public PointsReader {
public:
    [[nodiscard]] VoidResult check_integrity() override {
        ++integrity_calls;
        return {};
    }

    [[nodiscard]] Result<std::shared_ptr<PointValues>> get_values(std::string_view field_name) const override {
        ++calls;
        last_field = std::string(field_name);
        // "The reader holds no points for it": valid, and enough to observe the
        // forwarding without a whole `PointValues` implementation.
        return std::shared_ptr<PointValues>{};
    }

    [[nodiscard]] VoidResult close() override { return {}; }

    mutable int calls{0};
    mutable int integrity_calls{0};
    mutable std::string last_field;
};

/// A [`CodecReader`] whose sub-readers are injected by the test.
class TestCodecReader final : public CodecReader {
public:
    TestCodecReader(FieldInfos infos, int32_t max_doc) : infos_(std::move(infos)), max_doc_(max_doc) {}

    // ---- injected readers ----
    StoredFieldsReader *fields{nullptr};
    TermVectorsReader *vectors{nullptr};
    NormsProducer *norms{nullptr};
    DocValuesProducer *doc_values{nullptr};
    FieldsProducer *postings{nullptr};
    PointsReader *points{nullptr};

    [[nodiscard]] StoredFieldsReader *get_fields_reader() override { return fields; }
    [[nodiscard]] TermVectorsReader *get_term_vectors_reader() override { return vectors; }
    [[nodiscard]] NormsProducer *get_norms_reader() override { return norms; }
    [[nodiscard]] DocValuesProducer *get_doc_values_reader() override { return doc_values; }
    [[nodiscard]] FieldsProducer *get_postings_reader() override { return postings; }
    [[nodiscard]] PointsReader *get_points_reader() override { return points; }

    // ---- IndexReader ----
    [[nodiscard]] Result<int32_t> max_doc() const override { return max_doc_; }
    [[nodiscard]] Result<int32_t> num_docs() const override { return max_doc_; }

    [[nodiscard]] Result<const FieldInfos *> field_infos() const override { return &infos_; }
    [[nodiscard]] Result<const Bits *> live_docs() const override { return static_cast<const Bits *>(nullptr); }

private:
    FieldInfos infos_;
    int32_t max_doc_;
};

/// Field infos covering every gate `CodecReader` applies.
///
///  * `body`       indexed with freqs (postings available)
///  * `untokenized` stored but not indexed
///  * `num`        numeric doc values
///  * `tags`       sorted set doc values, skip index enabled
///  * `plain`      doc values skipped entirely
///  * `no_norms`   indexed with `omit_norms`
///  * `location`   two-dimensional points
FieldInfos standard_infos() {
    std::vector<FieldInfo> fields;

    FieldInfo body;
    body.number = 0;
    body.name = "body";
    body.index_options = IndexOptions::DocsAndFreqs;
    fields.push_back(body);

    FieldInfo untokenized;
    untokenized.number = 1;
    untokenized.name = "untokenized";
    untokenized.index_options = IndexOptions::None;
    fields.push_back(untokenized);

    FieldInfo num;
    num.number = 2;
    num.name = "num";
    num.index_options = IndexOptions::Docs;
    num.doc_values_type = DocValuesType::Numeric;
    fields.push_back(num);

    FieldInfo tags;
    tags.number = 3;
    tags.name = "tags";
    tags.index_options = IndexOptions::Docs;
    tags.doc_values_type = DocValuesType::SortedSet;
    tags.doc_values_skip_index = DocValuesSkipIndexType::Range;
    fields.push_back(tags);

    FieldInfo plain;
    plain.number = 4;
    plain.name = "plain";
    plain.index_options = IndexOptions::Docs;
    fields.push_back(plain);

    FieldInfo no_norms;
    no_norms.number = 5;
    no_norms.name = "no_norms";
    no_norms.index_options = IndexOptions::DocsAndFreqs;
    no_norms.omit_norms = true;
    fields.push_back(no_norms);

    FieldInfo location;
    location.number = 6;
    location.name = "location";
    location.index_options = IndexOptions::Docs;
    location.point_dimension_count = 2;
    location.point_index_dimension_count = 2;
    location.point_num_bytes = 4;
    fields.push_back(location);

    auto infos = FieldInfos::of(std::move(fields));
    EXPECT_TRUE(infos.has_value());
    return infos.value();
}

} // namespace

TEST(CodecReaderTest, StoredFieldsRequireAReader) {
    TestCodecReader reader(standard_infos(), /*max_doc=*/3);

    auto fields = reader.stored_fields();
    ASSERT_FALSE(fields.has_value());
    EXPECT_EQ(fields.error().kind(), ErrorKind::kIllegalState);
}

TEST(CodecReaderTest, StoredFieldsCheckTheDocumentIdRange) {
    auto record = std::make_shared<StoredFieldsRecord>();
    RecordingStoredFields stored(record);
    TestCodecReader reader(standard_infos(), /*max_doc=*/3);
    reader.fields = &stored;

    auto cursor = reader.stored_fields();
    ASSERT_TRUE(cursor.has_value());
    ASSERT_NE(cursor.value(), nullptr);

    EXPECT_TRUE(cursor.value()->document(2).has_value());
    EXPECT_EQ(record->visited, (std::vector<int32_t>{2}));

    // Out of range, on both sides: the codec must never see these ids.
    auto too_big = cursor.value()->document(3);
    ASSERT_FALSE(too_big.has_value());
    EXPECT_EQ(too_big.error().kind(), ErrorKind::kArrayIndexOutOfBounds);

    auto negative = cursor.value()->prefetch(-1);
    ASSERT_FALSE(negative.has_value());
    EXPECT_EQ(negative.error().kind(), ErrorKind::kArrayIndexOutOfBounds);

    EXPECT_EQ(record->visited, (std::vector<int32_t>{2}));
    EXPECT_TRUE(record->prefetched.empty());
}

TEST(CodecReaderTest, StoredFieldsHandOutACheckedClonePerCursor) {
    auto record = std::make_shared<StoredFieldsRecord>();
    RecordingStoredFields stored(record);
    TestCodecReader reader(standard_infos(), /*max_doc=*/5);
    reader.fields = &stored;

    auto first = reader.stored_fields();
    auto second = reader.stored_fields();
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(record->clone_calls, 2);

    EXPECT_TRUE(first.value()->prefetch(0).has_value());
    EXPECT_TRUE(second.value()->prefetch(4).has_value());
    EXPECT_EQ(record->prefetched, (std::vector<int32_t>{0, 4}));
}

TEST(CodecReaderTest, TermVectorsAreEmptyWithoutAReader) {
    TestCodecReader reader(standard_infos(), /*max_doc=*/3);

    auto vectors = reader.term_vectors();
    ASSERT_TRUE(vectors.has_value());
    ASSERT_NE(vectors.value(), nullptr);

    auto fields = vectors.value()->get(0);
    ASSERT_TRUE(fields.has_value());
    EXPECT_EQ(fields.value(), nullptr);

    auto terms = vectors.value()->get_field_terms(1, "body");
    ASSERT_TRUE(terms.has_value());
    EXPECT_EQ(terms.value(), nullptr);

    EXPECT_TRUE(vectors.value()->prefetch(2).has_value());
}

TEST(CodecReaderTest, TermVectorsDelegateToACloneOfTheReader) {
    auto record = std::make_shared<TermVectorsRecord>();
    RecordingTermVectors vectors(record);
    TestCodecReader reader(standard_infos(), /*max_doc=*/3);
    reader.vectors = &vectors;

    auto cursor = reader.term_vectors();
    ASSERT_TRUE(cursor.has_value());
    EXPECT_EQ(record->clone_calls, 1);

    EXPECT_TRUE(cursor.value()->get(1).has_value());
    EXPECT_EQ(record->read, (std::vector<int32_t>{1}));
}

TEST(CodecReaderTest, TermsSkipFieldsWithoutPostings) {
    RecordingFieldsProducer postings;
    TestCodecReader reader(standard_infos(), /*max_doc=*/3);
    reader.postings = &postings;

    // Unknown field, and a stored-but-not-indexed one.
    for (const std::string_view field : {"missing", "untokenized"}) {
        auto terms = reader.terms(field);
        ASSERT_TRUE(terms.has_value()) << field;
        EXPECT_EQ(terms.value(), nullptr) << field;
    }
    EXPECT_EQ(postings.terms_calls, 0);
}

TEST(CodecReaderTest, TermsAreForwardedToThePostingsReader) {
    RecordingFieldsProducer postings;
    TestCodecReader reader(standard_infos(), /*max_doc=*/3);
    reader.postings = &postings;

    auto terms = reader.terms("body");
    ASSERT_TRUE(terms.has_value());
    ASSERT_NE(terms.value(), nullptr);
    EXPECT_EQ(postings.terms_calls, 1);
    EXPECT_EQ(postings.last_field, "body");
}

TEST(CodecReaderTest, TermsFailWhenAnIndexedFieldHasNoPostingsReader) {
    TestCodecReader reader(standard_infos(), /*max_doc=*/3);

    auto terms = reader.terms("body");
    ASSERT_FALSE(terms.has_value());
    EXPECT_EQ(terms.error().kind(), ErrorKind::kIllegalState);
}

TEST(CodecReaderTest, DocValuesAreGatedByTheFieldInfoType) {
    RecordingDocValuesProducer doc_values;
    TestCodecReader reader(standard_infos(), /*max_doc=*/3);
    reader.doc_values = &doc_values;

    // A field with a different doc values type answers "absent" rather than
    // failing, as rlucene's `get_dv_field` does.
    auto sorted = reader.get_sorted_doc_values("num");
    ASSERT_TRUE(sorted.has_value());
    EXPECT_EQ(sorted.value(), nullptr);

    // A field without doc values at all, and an unknown field.
    auto none = reader.get_numeric_doc_values("plain");
    ASSERT_TRUE(none.has_value());
    EXPECT_EQ(none.value(), nullptr);
    auto missing = reader.get_numeric_doc_values("missing");
    ASSERT_TRUE(missing.has_value());
    EXPECT_EQ(missing.value(), nullptr);
    EXPECT_EQ(doc_values.numeric_calls, 0);

    // The matching getter is forwarded to the producer.
    auto numeric = reader.get_numeric_doc_values("num");
    ASSERT_TRUE(numeric.has_value());
    ASSERT_NE(numeric.value(), nullptr);
    EXPECT_EQ(doc_values.numeric_calls, 1);
    EXPECT_EQ(doc_values.last_field, "num");
}

TEST(CodecReaderTest, EveryDocValuesGetterIsForwarded) {
    RecordingDocValuesProducer doc_values;
    TestCodecReader reader(standard_infos(), /*max_doc=*/3);
    reader.doc_values = &doc_values;

    // `tags` is a sorted set field, so only that getter can serve it; the others
    // are still forwarded once the field info matches.
    auto sets = reader.get_sorted_set_doc_values("tags");
    ASSERT_TRUE(sets.has_value());
    ASSERT_NE(sets.value(), nullptr);
    EXPECT_EQ(doc_values.sorted_set_calls, 1);

    auto skipper = reader.get_doc_values_skipper("tags");
    ASSERT_TRUE(skipper.has_value());
    EXPECT_EQ(doc_values.skipper_calls, 1);
    EXPECT_EQ(doc_values.last_field, "tags");

    // A field whose skip index type is `None` never reaches the producer.
    auto no_skipper = reader.get_doc_values_skipper("num");
    ASSERT_TRUE(no_skipper.has_value());
    EXPECT_EQ(no_skipper.value(), nullptr);
    EXPECT_EQ(doc_values.skipper_calls, 1);
}

TEST(CodecReaderTest, DocValuesFailWhenTheFieldNeedsAProducerThatIsMissing) {
    TestCodecReader reader(standard_infos(), /*max_doc=*/3);

    auto numeric = reader.get_numeric_doc_values("num");
    ASSERT_FALSE(numeric.has_value());
    EXPECT_EQ(numeric.error().kind(), ErrorKind::kIllegalState);

    auto skipper = reader.get_doc_values_skipper("tags");
    ASSERT_FALSE(skipper.has_value());
    EXPECT_EQ(skipper.error().kind(), ErrorKind::kIllegalState);
}

TEST(CodecReaderTest, NormsAreGatedOnTheFieldInfo) {
    RecordingNormsProducer norms;
    TestCodecReader reader(standard_infos(), /*max_doc=*/3);
    reader.norms = &norms;

    // `omit_norms` was set when the field was written.
    auto omitted = reader.get_norm_values("no_norms");
    ASSERT_TRUE(omitted.has_value());
    EXPECT_EQ(omitted.value(), nullptr);
    EXPECT_EQ(norms.calls, 0);

    // Not indexed at all: no norms either.
    auto unindexed = reader.get_norm_values("untokenized");
    ASSERT_TRUE(unindexed.has_value());
    EXPECT_EQ(unindexed.value(), nullptr);
    EXPECT_EQ(norms.calls, 0);

    auto body = reader.get_norm_values("body");
    ASSERT_TRUE(body.has_value());
    ASSERT_NE(body.value(), nullptr);
    EXPECT_EQ(norms.calls, 1);
    EXPECT_EQ(norms.last_field, "body");
}

TEST(CodecReaderTest, NormsFailWhenTheProducerIsMissing) {
    TestCodecReader reader(standard_infos(), /*max_doc=*/3);

    auto norms = reader.get_norm_values("body");
    ASSERT_FALSE(norms.has_value());
    EXPECT_EQ(norms.error().kind(), ErrorKind::kIllegalState);
}

TEST(CodecReaderTest, PointValuesAreGatedOnTheDimensionCount) {
    RecordingPointsReader points;
    TestCodecReader reader(standard_infos(), /*max_doc=*/3);
    reader.points = &points;

    auto no_points = reader.point_values("body");
    ASSERT_TRUE(no_points.has_value());
    EXPECT_EQ(no_points.value(), nullptr);
    EXPECT_EQ(points.calls, 0);

    auto location = reader.point_values("location");
    ASSERT_TRUE(location.has_value());
    EXPECT_EQ(location.value(), nullptr);
    EXPECT_EQ(points.calls, 1);
    EXPECT_EQ(points.last_field, "location");
}

TEST(CodecReaderTest, PointValuesFailWhenTheReaderIsMissing) {
    TestCodecReader reader(standard_infos(), /*max_doc=*/3);

    auto location = reader.point_values("location");
    ASSERT_FALSE(location.has_value());
    EXPECT_EQ(location.error().kind(), ErrorKind::kIllegalState);
}

TEST(CodecReaderTest, CheckIntegrityVisitsEveryReader) {
    auto stored_record = std::make_shared<StoredFieldsRecord>();
    auto vector_record = std::make_shared<TermVectorsRecord>();
    RecordingStoredFields stored(stored_record);
    RecordingTermVectors vectors(vector_record);
    RecordingNormsProducer norms;
    RecordingDocValuesProducer doc_values;
    RecordingFieldsProducer postings;
    RecordingPointsReader points;

    TestCodecReader reader(standard_infos(), /*max_doc=*/3);
    reader.fields = &stored;
    reader.vectors = &vectors;
    reader.norms = &norms;
    reader.doc_values = &doc_values;
    reader.postings = &postings;
    reader.points = &points;

    EXPECT_TRUE(reader.check_integrity().has_value());
    EXPECT_EQ(stored_record->integrity_calls, 1);
    EXPECT_EQ(vector_record->integrity_calls, 1);
    EXPECT_EQ(norms.integrity_calls, 1);
    EXPECT_EQ(doc_values.integrity_calls, 1);
    EXPECT_EQ(postings.integrity_calls, 1);
    EXPECT_EQ(points.integrity_calls, 1);
}

TEST(CodecReaderTest, CheckIntegrityToleratesAbsentReaders) {
    TestCodecReader reader(standard_infos(), /*max_doc=*/3);

    EXPECT_TRUE(reader.check_integrity().has_value());
}
