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
#include <variant>

module lucene_core:ut.postings_interfaces;

import lucene_core;

using namespace lucene;

namespace {

/// A single-doc numeric doc values with a fixed value.
class FixedNumericDocValues : public NumericDocValues {
public:
    explicit FixedNumericDocValues(int64_t value) : value_(value) {}

    [[nodiscard]] int32_t doc_id() const override { return 0; }
    [[nodiscard]] Result<int32_t> next_doc() override { return NO_MORE_DOCS; }
    [[nodiscard]] Result<int64_t> long_value() override { return value_; }

private:
    int64_t value_;
};

/// Norms producer returning `value` for every field.
class FixedNormsProducer : public NormsProducer {
public:
    FixedNormsProducer(int64_t value, bool *integrity_checked)
        : value_(value), integrity_checked_(integrity_checked) {}

    [[nodiscard]] Result<std::unique_ptr<NumericDocValues>> get_norms(const FieldInfo & /*field*/) const override {
        return std::unique_ptr<NumericDocValues>(std::make_unique<FixedNumericDocValues>(value_));
    }

    [[nodiscard]] VoidResult check_integrity() const override {
        *integrity_checked_ = true;
        return {};
    }

private:
    int64_t value_;
    bool *integrity_checked_;
};

FieldInfo indexed_field() {
    FieldInfo info;
    info.name = "f";
    info.index_options = IndexOptions::DocsAndFreqs;
    return info;
}

/// Records what the terms dict passes to `write_term`.
class RecordingPostingsWriter : public PostingsWriterBase {
public:
    [[nodiscard]] VoidResult init(IndexOutput & /*terms_out*/, const SegmentWriteState & /*state*/,
                                  const SegmentInfo & /*segment_info*/) override {
        return {};
    }

    [[nodiscard]] Result<WriteTermResult> write_term(const BytesRef & /*term*/, TermsEnum & /*terms_enum*/,
                                                     FixedBitSet & /*docs_seen*/, const NormsProducer *norms,
                                                     std::unique_ptr<PostingsEnum> postings_enum) override {
        norms_seen = norms != nullptr;
        WriteTermResult result;
        result.postings = std::move(postings_enum);
        if (norms_seen) {
            auto values = norms->get_norms(field_);
            if (!values) {
                return err(std::move(values.error()));
            }
            auto value = values.value()->long_value();
            if (!value) {
                return err(std::move(value.error()));
            }
            result_state = value.value();
        }
        BlockTermState state;
        state.doc_freq = 2;
        result.state = TermStateEnum{state};
        return result;
    }

    [[nodiscard]] VoidResult encode_term(DataOutput & /*out*/, const FieldInfo & /*field_info*/,
                                         const TermStateEnum & /*state*/, bool /*absolute*/) override {
        return {};
    }

    void set_field(const FieldInfo &field_info) override {
        field_ = field_info;
        field_set = true;
    }

    bool field_set{false};
    bool norms_seen{false};
    int64_t result_state{0};
    FieldInfo field_;
};

} // namespace

TEST(NormsProducerEnumTest, DispatchesPerAlternative) {
    bool first_checked = false;
    bool second_checked = false;
    using Enum = NormsProducerEnum<FixedNormsProducer, FixedNormsProducer>;

    Enum first{std::variant<FixedNormsProducer, FixedNormsProducer>{
            std::in_place_index<0>, FixedNormsProducer{3, &first_checked}}};
    ASSERT_TRUE(first.check_integrity().has_value());
    EXPECT_TRUE(first_checked);
    EXPECT_FALSE(second_checked);

    auto norms = first.get_norms(indexed_field());
    ASSERT_TRUE(norms.has_value());
    EXPECT_EQ(norms.value()->long_value().value(), 3);

    Enum second{std::variant<FixedNormsProducer, FixedNormsProducer>{
            std::in_place_index<1>, FixedNormsProducer{9, &second_checked}}};
    ASSERT_TRUE(second.check_integrity().has_value());
    EXPECT_TRUE(second_checked);

    auto other = second.get_norms(indexed_field());
    ASSERT_TRUE(other.has_value());
    EXPECT_EQ(other.value()->long_value().value(), 9);

    // The reference implementation's default: no separate merge instance.
    auto merge = second.get_merge_instance();
    ASSERT_TRUE(merge.has_value());
    EXPECT_EQ(merge.value(), nullptr);
}

TEST(PostingsWriterBaseTest, WriteTermReportsStateAndPostings) {
    RecordingPostingsWriter writer;
    FieldInfo field = indexed_field();
    writer.set_field(field);
    EXPECT_TRUE(writer.field_set);
    EXPECT_EQ(writer.field_.name, "f");

    bool checked = false;
    FixedNormsProducer norms{11, &checked};
    FixedBitSet docs_seen{8};
    EmptyTermsEnum terms;

    auto result = writer.write_term(BytesRef::from_string("term"), terms, docs_seen, &norms, nullptr);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(writer.norms_seen);
    EXPECT_EQ(writer.result_state, 11);
    ASSERT_TRUE(result.value().state.has_value());
    auto state = result.value().state->get_block_term_state();
    ASSERT_TRUE(state.has_value());
    EXPECT_EQ((*state)->doc_freq, 2);
    EXPECT_EQ(result.value().postings, nullptr);

    // Without norms the writer must not touch the producer.
    auto second = writer.write_term(BytesRef::from_string("term"), terms, docs_seen, nullptr, nullptr);
    ASSERT_TRUE(second.has_value());
    EXPECT_FALSE(writer.norms_seen);
}
