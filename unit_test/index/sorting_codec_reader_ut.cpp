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
#include <span>
#include <string>
#include <utility>
#include <vector>

module lucene_core:ut.sorting_codec_reader;

import lucene_core;

using namespace lucene;

namespace {

/// The map that reverses a three-document segment.
[[nodiscard]] std::shared_ptr<const IndexSortDocMap> reversed_map() {
    return std::make_shared<const IndexSortDocMapImpl>(std::vector<int32_t>{2, 1, 0});
}

/// Numeric doc values over `values`, one value per old document.
class FakeNumericDocValues final : public NumericDocValues {
public:
    explicit FakeNumericDocValues(std::vector<int64_t> values) : values_(std::move(values)) {}

    [[nodiscard]] int32_t doc_id() const override { return doc_; }

    [[nodiscard]] Result<int32_t> next_doc() override {
        doc_ += 1;
        if (static_cast<size_t>(doc_) >= values_.size()) {
            doc_ = NO_MORE_DOCS;
        }
        return doc_;
    }

    [[nodiscard]] Result<int32_t> advance(int32_t target) override {
        doc_ = target;
        if (doc_ < 0 || static_cast<size_t>(doc_) >= values_.size()) {
            doc_ = NO_MORE_DOCS;
        }
        return doc_;
    }

    [[nodiscard]] Result<int64_t> cost() const override { return static_cast<int64_t>(values_.size()); }

    [[nodiscard]] Result<bool> advance_exact(int32_t target) override {
        if (target < 0 || static_cast<size_t>(target) >= values_.size()) {
            return false;
        }
        doc_ = target;
        return true;
    }

    [[nodiscard]] Result<int64_t> long_value() override { return values_[static_cast<size_t>(doc_)]; }

private:
    std::vector<int64_t> values_;
    int32_t doc_{-1};
};

/// Norms over one field.
class FakeNormsProducer final : public NormsProducer {
public:
    explicit FakeNormsProducer(std::vector<int64_t> values) : values_(std::move(values)) {}

    [[nodiscard]] Result<std::unique_ptr<NumericDocValues>> get_norms(const FieldInfo & /*field*/) const override {
        return std::unique_ptr<NumericDocValues>(new FakeNumericDocValues(values_));
    }

    [[nodiscard]] VoidResult check_integrity() const override { return {}; }
    [[nodiscard]] VoidResult close() const override { return {}; }

private:
    std::vector<int64_t> values_;
};

/// Doc values with numeric values only.
class FakeDocValuesProducer final : public DocValuesProducer {
public:
    explicit FakeDocValuesProducer(std::vector<int64_t> numeric) : numeric_(std::move(numeric)) {}

    [[nodiscard]] Result<std::unique_ptr<NumericDocValues>> get_numeric(const FieldInfo & /*field*/) const override {
        return std::unique_ptr<NumericDocValues>(new FakeNumericDocValues(numeric_));
    }

    [[nodiscard]] VoidResult check_integrity() const override { return {}; }
    [[nodiscard]] VoidResult close() const override { return {}; }

private:
    std::vector<int64_t> numeric_;
};

/// Stored fields: document `i` holds one field named `doc` with `values[i]`.
class FakeStoredFieldsReader final : public StoredFieldsReader {
public:
    explicit FakeStoredFieldsReader(std::vector<std::string> values) : values_(std::move(values)) {}

    [[nodiscard]] VoidResult prefetch(int32_t /*doc_id*/) override { return {}; }
    [[nodiscard]] VoidResult document_with_visitor(int32_t /*doc_id*/, StoredFieldVisitor & /*visitor*/) override {
        return {};
    }

    [[nodiscard]] Result<Document> document(int32_t doc_id) override {
        if (doc_id < 0 || static_cast<size_t>(doc_id) >= values_.size()) {
            return err(LuceneError::illegal_argument("doc id out of range"));
        }
        Document doc;
        doc.add(std::make_unique<Field>(StoredField::from_string("doc", values_[static_cast<size_t>(doc_id)])));
        return doc;
    }

    [[nodiscard]] VoidResult check_integrity() const override { return {}; }
    [[nodiscard]] Result<std::unique_ptr<StoredFieldsReader>> get_merge_instance() const override {
        return std::unique_ptr<StoredFieldsReader>();
    }
    [[nodiscard]] Result<std::unique_ptr<StoredFieldsReader>> clone() const override {
        return std::unique_ptr<StoredFieldsReader>(new FakeStoredFieldsReader(values_));
    }
    [[nodiscard]] VoidResult close() const override { return {}; }

private:
    std::vector<std::string> values_;
};

/// Term vectors that only record which documents were asked for.
class FakeTermVectorsReader final : public TermVectorsReader {
public:
    explicit FakeTermVectorsReader(std::shared_ptr<std::vector<int32_t>> requested)
            : requested_(std::move(requested)) {}

    [[nodiscard]] VoidResult prefetch(int32_t doc_id) override {
        requested_->push_back(doc_id);
        return {};
    }

    [[nodiscard]] Result<std::unique_ptr<Fields>> get(int32_t doc) override {
        requested_->push_back(doc);
        return std::unique_ptr<Fields>();
    }

    [[nodiscard]] VoidResult check_integrity() const override { return {}; }
    [[nodiscard]] Result<std::unique_ptr<TermVectorsReader>> get_merge_instance() const override {
        return std::unique_ptr<TermVectorsReader>();
    }
    [[nodiscard]] Result<std::unique_ptr<TermVectorsReader>> clone() const override {
        return std::unique_ptr<TermVectorsReader>(new FakeTermVectorsReader(requested_));
    }
    [[nodiscard]] VoidResult close() const override { return {}; }

private:
    std::shared_ptr<std::vector<int32_t>> requested_;
};

/// Postings over documents `0 .. count-1`, one position each.
class FakePostingsEnum final : public PostingsEnum {
public:
    explicit FakePostingsEnum(int32_t count) : count_(count) {}

    [[nodiscard]] int32_t doc_id() const override { return doc_; }

    [[nodiscard]] Result<int32_t> next_doc() override {
        doc_ += 1;
        if (doc_ >= count_) {
            doc_ = NO_MORE_DOCS;
        }
        return doc_;
    }

    [[nodiscard]] Result<int32_t> advance(int32_t target) override {
        doc_ = target;
        if (doc_ >= count_) {
            doc_ = NO_MORE_DOCS;
        }
        return doc_;
    }

    [[nodiscard]] Result<int64_t> cost() const override { return static_cast<int64_t>(count_); }
    [[nodiscard]] Result<int32_t> freq() override { return 1; }
    [[nodiscard]] Result<int32_t> next_position() override { return 0; }
    [[nodiscard]] Result<int32_t> start_offset() const override { return -1; }
    [[nodiscard]] Result<int32_t> end_offset() const override { return -1; }
    [[nodiscard]] Result<std::optional<std::span<const uint8_t>>> get_payload() const override {
        return std::optional<std::span<const uint8_t>>{};
    }

private:
    int32_t count_{0};
    int32_t doc_{-1};
};

/// A [`TermsEnum`] over the single term `t` of the fake segment.
class FakeTermsEnum final : public TermsEnum {
public:
    explicit FakeTermsEnum(int32_t doc_count) : doc_count_(doc_count) {}

    [[nodiscard]] Result<const BytesRef *> next() override {
        if (positioned_) {
            return static_cast<const BytesRef *>(nullptr);
        }
        positioned_ = true;
        term_ = BytesRef::from_string("t");
        return &term_;
    }

    [[nodiscard]] Result<AttributeSource *> attributes() override {
        return err(LuceneError::unsupported_operation("FakeTermsEnum has no attributes"));
    }
    [[nodiscard]] Result<AttributeSource *> attributes_mut() override {
        return err(LuceneError::unsupported_operation("FakeTermsEnum has no attributes"));
    }
    [[nodiscard]] Result<bool> seek_exact(std::span<const uint8_t> /*term*/) override {
        positioned_ = true;
        term_ = BytesRef::from_string("t");
        return true;
    }
    [[nodiscard]] Result<bool> prepare_seek_exact(std::span<const uint8_t> /*term*/) override {
        return err(LuceneError::unsupported_operation("FakeTermsEnum::prepare_seek_exact"));
    }
    [[nodiscard]] Result<bool> get_prepare_seek_exact_status(std::span<const uint8_t> /*target*/) override {
        return err(LuceneError::unsupported_operation("FakeTermsEnum::get_prepare_seek_exact_status"));
    }
    [[nodiscard]] Result<SeekStatus> seek_ceil(std::span<const uint8_t> /*term*/) override {
        positioned_ = true;
        term_ = BytesRef::from_string("t");
        return SeekStatus::Found;
    }
    [[nodiscard]] VoidResult seek_exact_with_ord(int64_t ord) override {
        if (ord != 0) {
            return err(LuceneError::illegal_argument("ord out of range"));
        }
        positioned_ = true;
        term_ = BytesRef::from_string("t");
        return {};
    }
    [[nodiscard]] VoidResult seek_exact_with_state(std::span<const uint8_t> /*term*/,
                                                   const TermStateEnum & /*state*/) override {
        return err(LuceneError::unsupported_operation("FakeTermsEnum::seek_exact_with_state"));
    }
    [[nodiscard]] Result<const BytesRef *> term() const override {
        if (!positioned_) {
            return err(LuceneError::illegal_state("unpositioned"));
        }
        return &term_;
    }
    [[nodiscard]] Result<int64_t> ord() const override { return 0; }
    [[nodiscard]] Result<int32_t> doc_freq() override { return doc_count_; }
    [[nodiscard]] Result<int64_t> total_term_freq() override { return doc_count_; }
    [[nodiscard]] Result<std::unique_ptr<PostingsEnum>> postings_with_flags(std::unique_ptr<PostingsEnum> /*reuse*/,
                                                                           int32_t /*flags*/) override {
        return std::unique_ptr<PostingsEnum>(new FakePostingsEnum(doc_count_));
    }
    [[nodiscard]] Result<std::unique_ptr<ImpactsEnum>> impacts(int32_t /*flags*/) override {
        return err(LuceneError::unsupported_operation("FakeTermsEnum has no impacts"));
    }
    [[nodiscard]] Result<TermStateEnum> term_state() override {
        return err(LuceneError::unsupported_operation("FakeTermsEnum has no term state"));
    }

private:
    int32_t doc_count_{0};
    bool positioned_{false};
    BytesRef term_;
};

/// A [`Terms`] over the single term `t`.
class FakeTerms final : public Terms {
public:
    explicit FakeTerms(int32_t doc_count) : doc_count_(doc_count) {}

    [[nodiscard]] Result<std::unique_ptr<TermsEnum>> iterator() const override {
        return std::unique_ptr<TermsEnum>(new FakeTermsEnum(doc_count_));
    }
    [[nodiscard]] Result<int64_t> size() const override { return 1; }
    [[nodiscard]] Result<int64_t> sum_total_term_freq() const override { return doc_count_; }
    [[nodiscard]] Result<int64_t> sum_doc_freq() const override { return doc_count_; }
    [[nodiscard]] Result<int32_t> doc_count() const override { return doc_count_; }
    [[nodiscard]] bool has_freqs() const override { return true; }
    [[nodiscard]] bool has_offsets() const override { return false; }
    [[nodiscard]] bool has_positions() const override { return false; }
    [[nodiscard]] bool has_payloads() const override { return false; }

private:
    int32_t doc_count_{0};
};

/// A [`FieldNameIterator`] over a fixed list.
class FakeFieldNameIterator final : public FieldNameIterator {
public:
    explicit FakeFieldNameIterator(std::vector<std::string> names) : names_(std::move(names)) {}

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

/// A postings producer that only knows the field `t`.
class FakeFieldsProducer final : public FieldsProducer {
public:
    explicit FakeFieldsProducer(int32_t doc_count) : doc_count_(doc_count) {}

    [[nodiscard]] Result<std::unique_ptr<FieldNameIterator>> iterator() const override {
        return std::unique_ptr<FieldNameIterator>(new FakeFieldNameIterator({"t"}));
    }
    [[nodiscard]] Result<std::unique_ptr<Terms>> terms(std::string_view field) const override {
        if (field != "t") {
            return std::unique_ptr<Terms>();
        }
        return std::unique_ptr<Terms>(new FakeTerms(doc_count_));
    }
    [[nodiscard]] Result<int32_t> size() const override { return 1; }
    [[nodiscard]] VoidResult check_integrity() const override { return {}; }
    [[nodiscard]] VoidResult close() const override { return {}; }

private:
    int32_t doc_count_{0};
};

/// A point-tree cursor over the documents `0 .. count-1`, each with a
/// one-dimensional 4-byte value equal to its old document number.
class FakePointTree final : public PointTree {
public:
    explicit FakePointTree(int32_t count) : count_(count) {}

    [[nodiscard]] Result<bool> move_to_child() override { return false; }

    [[nodiscard]] Result<BytesRef> get_min_packed_value() override {
        std::vector<uint8_t> bytes(4, 0);
        return BytesRef(std::move(bytes));
    }
    [[nodiscard]] Result<BytesRef> get_max_packed_value() override {
        std::vector<uint8_t> bytes(4, 0);
        bytes[3] = static_cast<uint8_t>(count_);
        return BytesRef(std::move(bytes));
    }
    [[nodiscard]] Result<size_t> size() override { return static_cast<size_t>(count_); }

    [[nodiscard]] VoidResult visit_doc_ids(IntersectVisitor &visitor) override {
        for (int32_t doc = 0; doc < count_; ++doc) {
            if (auto visited = visitor.visit(doc); !visited.has_value()) {
                return visited;
            }
        }
        return {};
    }

    [[nodiscard]] VoidResult visit_doc_values(IntersectVisitor &visitor) override {
        for (int32_t doc = 0; doc < count_; ++doc) {
            const std::vector<uint8_t> value{0, 0, 0, static_cast<uint8_t>(doc)};
            if (auto visited = visitor.visit_with_packed_value(doc, value); !visited.has_value()) {
                return visited;
            }
        }
        return {};
    }

    [[nodiscard]] std::unique_ptr<PointTree> try_clone() const override {
        return std::unique_ptr<PointTree>(new FakePointTree(count_));
    }

private:
    int32_t count_{0};
};

/// Point values over one field.
class FakePointValues final : public PointValues {
public:
    explicit FakePointValues(int32_t count) : count_(count) {}

    [[nodiscard]] Result<std::optional<BytesRef>> get_min_packed_value() const override {
        return std::optional<BytesRef>{};
    }
    [[nodiscard]] Result<std::optional<BytesRef>> get_max_packed_value() const override {
        return std::optional<BytesRef>{};
    }
    [[nodiscard]] Result<size_t> get_num_dimensions() const override { return 1; }
    [[nodiscard]] Result<size_t> get_num_index_dimensions() const override { return 1; }
    [[nodiscard]] Result<size_t> get_bytes_per_dimension() const override { return 4; }
    [[nodiscard]] Result<size_t> size() const override { return static_cast<size_t>(count_); }
    [[nodiscard]] Result<int32_t> get_doc_count() const override { return count_; }
    [[nodiscard]] Result<std::unique_ptr<PointTree>> get_point_tree() const override {
        return std::unique_ptr<PointTree>(new FakePointTree(count_));
    }

private:
    int32_t count_{0};
};

/// A points reader over one field.
class FakePointsReader final : public PointsReader {
public:
    explicit FakePointsReader(int32_t count) : values_(std::make_shared<FakePointValues>(count)) {}

    [[nodiscard]] VoidResult check_integrity() override { return {}; }

    [[nodiscard]] Result<std::shared_ptr<PointValues>> get_values(std::string_view field_name) const override {
        if (field_name != "p") {
            return std::shared_ptr<PointValues>{};
        }
        return values_;
    }

    [[nodiscard]] bool is_lucene90_points_reader() const override { return true; }
    [[nodiscard]] VoidResult close() override { return {}; }

private:
    std::shared_ptr<FakePointValues> values_;
};

/// Live documents: all set except the ones listed in `deleted`.
class FakeLiveDocs final : public Bits {
public:
    explicit FakeLiveDocs(std::vector<bool> live) : live_(std::move(live)) {}

    [[nodiscard]] Result<bool> get(size_t index) const override {
        if (index >= live_.size()) {
            return err(LuceneError::array_index_out_of_bounds("live docs index out of range"));
        }
        return live_[index];
    }

    [[nodiscard]] size_t length() const override { return live_.size(); }

private:
    std::vector<bool> live_;
};

/// A codec reader with one indexed field `t`, numeric doc values on `dv` and
/// points on `p`.
class FakeCodecReader final : public CodecReader {
public:
    FakeCodecReader(std::shared_ptr<std::vector<int32_t>> requested)
            : stored_(std::make_shared<FakeStoredFieldsReader>(std::vector<std::string>{"first", "second", "third"})),
              term_vectors_(std::make_shared<FakeTermVectorsReader>(std::move(requested))),
              norms_(std::make_shared<FakeNormsProducer>(std::vector<int64_t>{100, 200, 300})),
              doc_values_(std::make_shared<FakeDocValuesProducer>(std::vector<int64_t>{10, 20, 30})),
              postings_(std::make_shared<FakeFieldsProducer>(3)),
              points_(std::make_shared<FakePointsReader>(3)),
              live_docs_(std::make_shared<FakeLiveDocs>(std::vector<bool>{true, false, true})) {
        std::vector<FieldInfo> fields;
        FieldInfo terms_field;
        terms_field.number = 0;
        terms_field.name = "t";
        terms_field.index_options = IndexOptions::DocsAndFreqs;
        fields.push_back(terms_field);

        FieldInfo dv_field;
        dv_field.number = 1;
        dv_field.name = "dv";
        dv_field.doc_values_type = DocValuesType::Numeric;
        fields.push_back(dv_field);

        FieldInfo point_field;
        point_field.number = 2;
        point_field.name = "p";
        point_field.point_dimension_count = 1;
        point_field.point_index_dimension_count = 1;
        point_field.point_num_bytes = 4;
        fields.push_back(point_field);

        auto built = FieldInfos::of(std::move(fields));
        EXPECT_TRUE(built.has_value());
        infos_ = std::make_shared<FieldInfos>(std::move(built.value()));
    }

    [[nodiscard]] StoredFieldsReader *get_fields_reader() override { return stored_.get(); }
    [[nodiscard]] TermVectorsReader *get_term_vectors_reader() override { return term_vectors_.get(); }
    [[nodiscard]] NormsProducer *get_norms_reader() override { return norms_.get(); }
    [[nodiscard]] DocValuesProducer *get_doc_values_reader() override { return doc_values_.get(); }
    [[nodiscard]] FieldsProducer *get_postings_reader() override { return postings_.get(); }
    [[nodiscard]] PointsReader *get_points_reader() override { return points_.get(); }

    [[nodiscard]] Result<const FieldInfos *> field_infos() const override { return infos_.get(); }
    [[nodiscard]] Result<const Bits *> live_docs() const override { return live_docs_.get(); }

    [[nodiscard]] Result<int32_t> max_doc() const override { return 3; }
    [[nodiscard]] Result<int32_t> num_docs() const override { return 2; }

    [[nodiscard]] std::string to_string() const override { return "FakeCodecReader"; }

private:
    std::shared_ptr<FakeStoredFieldsReader> stored_;
    std::shared_ptr<FakeTermVectorsReader> term_vectors_;
    std::shared_ptr<FakeNormsProducer> norms_;
    std::shared_ptr<FakeDocValuesProducer> doc_values_;
    std::shared_ptr<FakeFieldsProducer> postings_;
    std::shared_ptr<FakePointsReader> points_;
    std::shared_ptr<FakeLiveDocs> live_docs_;
    std::shared_ptr<FieldInfos> infos_;
};

/// A visitor that collects the document numbers it is handed.
class CollectingVisitor final : public IntersectVisitor {
public:
    std::vector<int32_t> docs;

    [[nodiscard]] VoidResult visit(int32_t doc_id) override {
        docs.push_back(doc_id);
        return {};
    }

    [[nodiscard]] VoidResult visit_with_packed_value(int32_t doc_id, std::span<const uint8_t> /*packed_value*/) override {
        docs.push_back(doc_id);
        return {};
    }

    [[nodiscard]] Result<Relation> compare(std::span<const uint8_t> /*min*/,
                                           std::span<const uint8_t> /*max*/) const override {
        return Relation::kCellCrossesQuery;
    }
};

} // namespace

TEST(SortingCodecReaderTest, ForwardsTheDocumentCountsAndTheFieldInfos) {
    auto delegate = std::make_shared<FakeCodecReader>(std::make_shared<std::vector<int32_t>>());
    auto reader = SortingCodecReader::create(delegate, reversed_map());
    ASSERT_TRUE(reader.has_value()) << (reader ? "" : reader.error().to_string());

    EXPECT_EQ(reader.value()->max_doc().value(), 3);
    EXPECT_EQ(reader.value()->num_docs().value(), 2);

    auto infos = reader.value()->field_infos();
    ASSERT_TRUE(infos.has_value());
    ASSERT_NE(infos.value(), nullptr);
    EXPECT_EQ(infos.value()->size(), 3U);
    EXPECT_EQ(reader.value()->to_string(), "SortingCodecReader(FakeCodecReader)");
}

TEST(SortingCodecReaderTest, ReadsStoredFieldsInTheSortedOrder) {
    auto delegate = std::make_shared<FakeCodecReader>(std::make_shared<std::vector<int32_t>>());
    auto reader = SortingCodecReader::create(delegate, reversed_map());
    ASSERT_TRUE(reader.has_value());

    auto fields = reader.value()->stored_fields();
    ASSERT_TRUE(fields.has_value()) << (fields ? "" : fields.error().to_string());
    auto doc = fields.value()->document(0);
    ASSERT_TRUE(doc.has_value()) << (doc ? "" : doc.error().to_string());
    auto values = doc.value().get_values("doc");
    ASSERT_TRUE(values.has_value()) << (values ? "" : values.error().to_string());
    ASSERT_EQ(values.value().size(), 1U);
    EXPECT_EQ(values.value()[0], "third");
}

TEST(SortingCodecReaderTest, RenumbersThePostings) {
    auto delegate = std::make_shared<FakeCodecReader>(std::make_shared<std::vector<int32_t>>());
    auto reader = SortingCodecReader::create(delegate, reversed_map());
    ASSERT_TRUE(reader.has_value());

    auto terms = reader.value()->terms("t");
    ASSERT_TRUE(terms.has_value()) << (terms ? "" : terms.error().to_string());
    ASSERT_NE(terms.value(), nullptr);

    auto tenum = terms.value()->iterator();
    ASSERT_TRUE(tenum.has_value());
    auto term = tenum.value()->next();
    ASSERT_TRUE(term.has_value());
    ASSERT_NE(term.value(), nullptr);

    // Old documents 0, 1, 2 become new documents 2, 1, 0.
    auto postings = tenum.value()->postings_with_flags(nullptr, FREQS);
    ASSERT_TRUE(postings.has_value()) << (postings ? "" : postings.error().to_string());
    EXPECT_EQ(postings.value()->next_doc().value(), 0);
    EXPECT_EQ(postings.value()->next_doc().value(), 1);
    EXPECT_EQ(postings.value()->next_doc().value(), 2);
    EXPECT_EQ(postings.value()->next_doc().value(), NO_MORE_DOCS);
}

TEST(SortingCodecReaderTest, RenumbersTheNumericDocValues) {
    auto delegate = std::make_shared<FakeCodecReader>(std::make_shared<std::vector<int32_t>>());
    auto reader = SortingCodecReader::create(delegate, reversed_map());
    ASSERT_TRUE(reader.has_value());

    auto values = reader.value()->get_numeric_doc_values("dv");
    ASSERT_TRUE(values.has_value()) << (values ? "" : values.error().to_string());
    ASSERT_NE(values.value(), nullptr);

    ASSERT_TRUE(values.value()->advance_exact(0).value());
    EXPECT_EQ(values.value()->long_value().value(), 30);
    ASSERT_TRUE(values.value()->advance_exact(1).value());
    EXPECT_EQ(values.value()->long_value().value(), 20);
    ASSERT_TRUE(values.value()->advance_exact(2).value());
    EXPECT_EQ(values.value()->long_value().value(), 10);
}

TEST(SortingCodecReaderTest, RenumbersTheNorms) {
    auto delegate = std::make_shared<FakeCodecReader>(std::make_shared<std::vector<int32_t>>());
    auto reader = SortingCodecReader::create(delegate, reversed_map());
    ASSERT_TRUE(reader.has_value());

    auto norms = reader.value()->get_norm_values("t");
    ASSERT_TRUE(norms.has_value()) << (norms ? "" : norms.error().to_string());
    ASSERT_NE(norms.value(), nullptr);

    ASSERT_TRUE(norms.value()->advance_exact(0).value());
    EXPECT_EQ(norms.value()->long_value().value(), 300);
    ASSERT_TRUE(norms.value()->advance_exact(2).value());
    EXPECT_EQ(norms.value()->long_value().value(), 100);
}

TEST(SortingCodecReaderTest, RenumbersTheLiveDocuments) {
    auto delegate = std::make_shared<FakeCodecReader>(std::make_shared<std::vector<int32_t>>());
    auto reader = SortingCodecReader::create(delegate, reversed_map());
    ASSERT_TRUE(reader.has_value());

    auto live = reader.value()->live_docs();
    ASSERT_TRUE(live.has_value()) << (live ? "" : live.error().to_string());
    ASSERT_NE(live.value(), nullptr);

    // Old document 1 is the deleted one, which sorts into position 1.
    EXPECT_TRUE(live.value()->get(0).value());
    EXPECT_FALSE(live.value()->get(1).value());
    EXPECT_TRUE(live.value()->get(2).value());
}

TEST(SortingCodecReaderTest, RenumbersThePoints) {
    auto delegate = std::make_shared<FakeCodecReader>(std::make_shared<std::vector<int32_t>>());
    auto reader = SortingCodecReader::create(delegate, reversed_map());
    ASSERT_TRUE(reader.has_value());

    auto values = reader.value()->point_values("p");
    ASSERT_TRUE(values.has_value()) << (values ? "" : values.error().to_string());
    ASSERT_NE(values.value(), nullptr);

    auto tree = values.value()->get_point_tree();
    ASSERT_TRUE(tree.has_value());
    ASSERT_NE(tree.value(), nullptr);

    CollectingVisitor visitor;
    ASSERT_TRUE(tree.value()->visit_doc_ids(visitor).has_value());
    EXPECT_EQ(visitor.docs, (std::vector<int32_t>{2, 1, 0}));

    CollectingVisitor values_visitor;
    ASSERT_TRUE(tree.value()->visit_doc_values(values_visitor).has_value());
    EXPECT_EQ(values_visitor.docs, (std::vector<int32_t>{2, 1, 0}));
}

TEST(SortingCodecReaderTest, SharesOnePointsWrapperPerField) {
    auto delegate = std::make_shared<FakeCodecReader>(std::make_shared<std::vector<int32_t>>());
    auto reader = SortingCodecReader::create(delegate, reversed_map());
    ASSERT_TRUE(reader.has_value());

    auto first = reader.value()->point_values("p");
    ASSERT_TRUE(first.has_value());
    auto second = reader.value()->point_values("p");
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(first.value().get(), second.value().get());
}

TEST(SortingCodecReaderTest, RenumbersTheTermVectorRequests) {
    auto requested = std::make_shared<std::vector<int32_t>>();
    auto delegate = std::make_shared<FakeCodecReader>(requested);
    auto reader = SortingCodecReader::create(delegate, reversed_map());
    ASSERT_TRUE(reader.has_value());

    auto vectors = reader.value()->term_vectors();
    ASSERT_TRUE(vectors.has_value()) << (vectors ? "" : vectors.error().to_string());
    auto doc = vectors.value()->get(0);
    ASSERT_TRUE(doc.has_value());
    ASSERT_EQ(requested->size(), 1U);
    EXPECT_EQ((*requested)[0], 2);
}

TEST(SortingCodecReaderTest, DropsThePointsOfDeletedDocuments) {
    auto delegate = std::make_shared<FakeCodecReader>(std::make_shared<std::vector<int32_t>>());
    // Old document 1 does not survive the sort.
    auto doc_map = std::make_shared<const IndexSortDocMapImpl>(std::vector<int32_t>{2, 0}, 3);
    auto reader = SortingCodecReader::create(delegate, doc_map);
    ASSERT_TRUE(reader.has_value());

    auto values = reader.value()->point_values("p");
    ASSERT_TRUE(values.has_value());
    auto tree = values.value()->get_point_tree();
    ASSERT_TRUE(tree.has_value());

    CollectingVisitor visitor;
    ASSERT_TRUE(tree.value()->visit_doc_ids(visitor).has_value());
    EXPECT_EQ(visitor.docs, (std::vector<int32_t>{1, 0}));
}

TEST(SortingCodecReaderTest, RejectsMissingArguments) {
    auto no_reader = SortingCodecReader::create(nullptr, reversed_map());
    ASSERT_FALSE(no_reader.has_value());
    EXPECT_EQ(no_reader.error().kind(), ErrorKind::kIllegalArgument);

    auto delegate = std::make_shared<FakeCodecReader>(std::make_shared<std::vector<int32_t>>());
    auto no_map = SortingCodecReader::create(delegate, nullptr);
    ASSERT_FALSE(no_map.has_value());
    EXPECT_EQ(no_map.error().kind(), ErrorKind::kIllegalArgument);

    auto infos = std::make_shared<FieldInfos>(FieldInfos::none());
    auto fields_no_producer = SortingFieldsProducer::create(nullptr, infos, reversed_map());
    ASSERT_FALSE(fields_no_producer.has_value());
    EXPECT_EQ(fields_no_producer.error().kind(), ErrorKind::kIllegalArgument);

    auto fields_no_map = SortingFieldsProducer::create(std::make_shared<FakeFieldsProducer>(3), infos, nullptr);
    ASSERT_FALSE(fields_no_map.has_value());
    EXPECT_EQ(fields_no_map.error().kind(), ErrorKind::kIllegalArgument);

    auto points_no_reader = SortingPointsReader::create(nullptr, reversed_map());
    ASSERT_FALSE(points_no_reader.has_value());
    EXPECT_EQ(points_no_reader.error().kind(), ErrorKind::kIllegalArgument);
}
