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
#include <format>
#include <map>
#include <memory>
#include <string>
#include <vector>

module lucene_core:ut.block_tree_terms_writer;

import lucene_core;

using namespace lucene;

namespace {

/// One term of the fake field, with the documents it occurs in.
struct TermSpec {
    std::string text;
    std::vector<int32_t> docs;
    int64_t total_term_freq{0};
};

std::string term_text(const BytesRef &term) {
    const auto bytes = term.as_slice();
    return std::string(reinterpret_cast<const char *>(bytes.data()), bytes.size());
}

/// The `Fields` a flush hands to the terms writer: one field over a sorted list
/// of terms.  `FieldsProducer` and the real segment readers are not ported yet,
/// so the writer test drives the pull API with this.
class FakeTermsEnum : public TermsEnum {
public:
    explicit FakeTermsEnum(const std::vector<TermSpec> *terms) : terms_(terms) {}

    Result<const BytesRef *> next() override {
        if (next_index_ >= terms_->size()) {
            return static_cast<const BytesRef *>(nullptr);
        }
        const TermSpec &spec = (*terms_)[next_index_++];
        current_.copy_from_slice(std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(spec.text.data()),
                                                         spec.text.size()));
        current_spec_ = &spec;
        return &current_;
    }

    Result<AttributeSource *> attributes() override {
        return err(LuceneError::need_implemented("FakeTermsEnum::attributes"));
    }
    Result<AttributeSource *> attributes_mut() override {
        return err(LuceneError::need_implemented("FakeTermsEnum::attributes_mut"));
    }
    Result<bool> seek_exact(std::span<const uint8_t> term) override {
        return err(LuceneError::need_implemented("FakeTermsEnum::seek_exact"));
    }
    Result<bool> prepare_seek_exact(std::span<const uint8_t> term) override {
        return err(LuceneError::need_implemented("FakeTermsEnum::prepare_seek_exact"));
    }
    Result<bool> get_prepare_seek_exact_status(std::span<const uint8_t> target) override {
        return err(LuceneError::need_implemented("FakeTermsEnum::get_prepare_seek_exact_status"));
    }
    Result<SeekStatus> seek_ceil(std::span<const uint8_t> term) override {
        return err(LuceneError::need_implemented("FakeTermsEnum::seek_ceil"));
    }
    VoidResult seek_exact_with_ord(int64_t ord) override {
        return err(LuceneError::need_implemented("FakeTermsEnum::seek_exact_with_ord"));
    }
    VoidResult seek_exact_with_state(std::span<const uint8_t> term, const TermStateEnum &state) override {
        return err(LuceneError::need_implemented("FakeTermsEnum::seek_exact_with_state"));
    }
    Result<const BytesRef *> term() const override { return &current_; }
    Result<int64_t> ord() const override { return err(LuceneError::need_implemented("FakeTermsEnum::ord")); }
    Result<int32_t> doc_freq() override { return static_cast<int32_t>(current_spec_->docs.size()); }
    Result<int64_t> total_term_freq() override { return current_spec_->total_term_freq; }
    Result<std::unique_ptr<PostingsEnum>> postings_with_flags(std::unique_ptr<PostingsEnum> reuse,
                                                             int32_t flags) override {
        (void)flags;
        return std::move(reuse);
    }
    Result<std::unique_ptr<ImpactsEnum>> impacts(int32_t flags) override {
        (void)flags;
        return std::unique_ptr<ImpactsEnum>(nullptr);
    }
    Result<TermStateEnum> term_state() override {
        return err(LuceneError::need_implemented("FakeTermsEnum::term_state"));
    }

private:
    const std::vector<TermSpec> *terms_;
    size_t next_index_{0};
    BytesRef current_{};
    const TermSpec *current_spec_{nullptr};
};

class FakeTerms : public Terms {
public:
    explicit FakeTerms(const std::vector<TermSpec> *terms) : terms_(terms) {}

    Result<std::unique_ptr<TermsEnum>> iterator() const override {
        return std::make_unique<FakeTermsEnum>(terms_);
    }
    Result<int64_t> size() const override { return static_cast<int64_t>(terms_->size()); }
    Result<int64_t> sum_total_term_freq() const override { return 0; }
    Result<int64_t> sum_doc_freq() const override { return 0; }
    Result<int32_t> doc_count() const override { return 0; }
    bool has_freqs() const override { return true; }
    bool has_offsets() const override { return false; }
    bool has_positions() const override { return false; }
    bool has_payloads() const override { return false; }

private:
    const std::vector<TermSpec> *terms_;
};

class FakeFieldNames : public FieldNameIterator {
public:
    explicit FakeFieldNames(std::vector<std::string> names) : names_(std::move(names)) {}

    Result<const std::string *> next() override {
        if (next_index_ >= names_.size()) {
            return static_cast<const std::string *>(nullptr);
        }
        return &names_[next_index_++];
    }

private:
    std::vector<std::string> names_;
    size_t next_index_{0};
};

class FakeFields : public Fields {
public:
    FakeFields(std::vector<std::string> names, std::map<std::string, const std::vector<TermSpec> *> terms)
        : names_(std::move(names)), terms_(std::move(terms)) {}

    Result<std::unique_ptr<FieldNameIterator>> iterator() const override {
        return std::make_unique<FakeFieldNames>(names_);
    }
    Result<std::unique_ptr<Terms>> terms(std::string_view field) const override {
        const auto found = terms_.find(std::string(field));
        if (found == terms_.end()) {
            return std::unique_ptr<Terms>(nullptr);
        }
        return std::unique_ptr<Terms>(std::make_unique<FakeTerms>(found->second));
    }
    Result<int32_t> size() const override { return static_cast<int32_t>(names_.size()); }

private:
    std::vector<std::string> names_;
    std::map<std::string, const std::vector<TermSpec> *> terms_;
};

/// `PostingsWriterBase` that encodes nothing but the two statistics, so the
/// terms dictionary can be exercised without the postings files.
class FakePostingsWriter : public PostingsWriterBase {
public:
    explicit FakePostingsWriter(const std::vector<TermSpec> *terms) : terms_(terms) {}

    VoidResult init(IndexOutput &terms_out, const SegmentWriteState &state, const SegmentInfo &segment_info) override {
        (void)terms_out;
        (void)state;
        (void)segment_info;
        return {};
    }

    Result<WriteTermResult> write_term(const BytesRef &term, TermsEnum &terms_enum, FixedBitSet &docs_seen,
                                       const NormsProducer *norms, std::unique_ptr<PostingsEnum> postings_enum) override {
        (void)terms_enum;
        (void)norms;
        WriteTermResult result;
        result.postings = std::move(postings_enum);

        const std::string text = term_text(term);
        const TermSpec *spec = nullptr;
        for (const TermSpec &candidate : *terms_) {
            if (candidate.text == text) {
                spec = &candidate;
                break;
            }
        }
        if (spec == nullptr) {
            return err(LuceneError::illegal_state(std::format("unknown term {}", text)));
        }
        if (spec->docs.empty()) {
            return result;
        }
        for (const int32_t doc : spec->docs) {
            docs_seen.set(static_cast<size_t>(doc));
        }
        IntBlockTermState state;
        state.base.doc_freq = static_cast<int32_t>(spec->docs.size());
        state.base.total_term_freq = spec->total_term_freq;
        result.state = TermStateEnum(std::move(state));
        return result;
    }

    VoidResult encode_term(DataOutput &out, const FieldInfo &field_info, const TermStateEnum &state,
                           bool absolute) override {
        (void)field_info;
        (void)absolute;
        const auto *int_state = std::get_if<IntBlockTermState>(&state.value());
        if (int_state == nullptr) {
            return err(LuceneError::illegal_state("FakePostingsWriter expects an IntBlockTermState"));
        }
        if (auto written = out.write_vlong(int_state->base.doc_freq); !written) {
            return written;
        }
        return out.write_vlong(int_state->base.total_term_freq);
    }

    void set_field(const FieldInfo &field_info) override { (void)field_info; }

private:
    const std::vector<TermSpec> *terms_;
};

/// Reads back what [`FakePostingsWriter`] encoded.
class FakePostingsReader : public PostingsReaderBase {
public:
    VoidResult init(IndexInput &terms_in, const SegmentReadState &state, const SegmentInfo &segment_info) override {
        (void)terms_in;
        (void)state;
        (void)segment_info;
        return {};
    }
    Result<TermStateEnum> new_term_state() const override { return TermStateEnum(IntBlockTermState{}); }
    VoidResult decode_term(DataInput &input, const FieldInfo &field_info, TermStateEnum &state,
                           bool absolute) const override {
        (void)field_info;
        (void)absolute;
        auto *int_state = std::get_if<IntBlockTermState>(&state.value_mut());
        if (int_state == nullptr) {
            return err(LuceneError::illegal_state("FakePostingsReader expects an IntBlockTermState"));
        }
        auto doc_freq = input.read_vlong();
        if (!doc_freq) {
            return err(std::move(doc_freq.error()));
        }
        auto total_term_freq = input.read_vlong();
        if (!total_term_freq) {
            return err(std::move(total_term_freq.error()));
        }
        int_state->base.doc_freq = static_cast<int32_t>(doc_freq.value());
        int_state->base.total_term_freq = total_term_freq.value();
        return {};
    }
    Result<std::unique_ptr<PostingsEnum>> postings(const FieldInfo &field_info, const TermStateEnum &state,
                                                   std::unique_ptr<PostingsEnum> reuse, int32_t flags) const override {
        (void)field_info;
        (void)state;
        (void)flags;
        return std::move(reuse);
    }
    Result<std::unique_ptr<ImpactsEnum>> impacts(const FieldInfo &field_info, const TermStateEnum &state,
                                                 int32_t flags) const override {
        (void)field_info;
        (void)state;
        (void)flags;
        return std::unique_ptr<ImpactsEnum>(nullptr);
    }
    VoidResult check_integrity() const override { return {}; }
};

FieldInfo make_field(std::string name = "f", IndexOptions options = IndexOptions::DocsAndFreqs) {
    FieldInfo info;
    info.number = 0;
    info.name = std::move(name);
    info.index_options = options;
    info.omit_norms = true;
    return info;
}

std::shared_ptr<const FieldInfos> field_infos_with(std::vector<FieldInfo> fields) {
    auto infos = FieldInfos::of(std::move(fields));
    EXPECT_TRUE(infos.has_value());
    return std::make_shared<const FieldInfos>(std::move(infos.value()));
}

SegmentInfo segment_info_for(std::string name = "_0", uint8_t id_byte = 0xAB, int32_t max_doc = 10) {
    SegmentInfo info;
    info.name = std::move(name);
    info.id.assign(ID_LENGTH, id_byte);
    info.max_doc = max_doc;
    return info;
}

std::vector<TermSpec> sample_terms() {
    return std::vector<TermSpec>{
            TermSpec{"apple", {0}, 1},
            TermSpec{"banana", {1, 2}, 3},
            TermSpec{"cherry", {2}, 2},
    };
}

/// Writes `terms` into `directory` and returns the writer, closed.
void write_terms(ByteBuffersDirectory &directory, const std::shared_ptr<const FieldInfos> &infos,
                 const SegmentInfo &segment, const std::vector<TermSpec> &terms, int32_t min_items,
                 int32_t max_items) {
    SegmentWriteState state{get_default_info_stream(), directory, infos, io_context_default(), {}};
    auto writer = BlockTreeTermsWriter::create(state, std::make_unique<FakePostingsWriter>(&terms), min_items,
                                               max_items, segment);
    ASSERT_TRUE(writer.has_value()) << writer.error().to_string();

    FakeFields fields{{infos->fields().front().name}, {{infos->fields().front().name, &terms}}};
    ASSERT_TRUE(writer.value()->write(state, segment, fields, nullptr).has_value());
    ASSERT_TRUE(writer.value()->close().has_value());
}

} // namespace

TEST(BlockTreeTermsWriterTest, RoundTripsTermsAndStatistics) {
    ByteBuffersDirectory directory;
    auto infos = field_infos_with({make_field()});
    auto segment = segment_info_for();
    const std::vector<TermSpec> terms = sample_terms();

    write_terms(directory, infos, segment, terms, BlockTreeTermsWriter::DEFAULT_MIN_BLOCK_SIZE,
                BlockTreeTermsWriter::DEFAULT_MAX_BLOCK_SIZE);

    SegmentReadState read_state{directory, infos, io_context_default()};
    auto reader = Lucene90BlockTreeTermsReader::create(std::make_unique<FakePostingsReader>(), read_state, segment);
    ASSERT_TRUE(reader.has_value()) << reader.error().to_string();
    ASSERT_EQ(reader.value()->size(), 1u);
    ASSERT_TRUE(reader.value()->check_integrity().has_value());

    auto field_reader = reader.value()->terms("f");
    ASSERT_TRUE(field_reader.has_value());
    ASSERT_NE(field_reader.value(), nullptr);

    EXPECT_EQ(field_reader.value()->size().value(), 3);
    EXPECT_EQ(field_reader.value()->sum_doc_freq().value(), 1 + 2 + 1);
    EXPECT_EQ(field_reader.value()->sum_total_term_freq().value(), 1 + 3 + 2);
    EXPECT_EQ(field_reader.value()->doc_count().value(), 3);
    EXPECT_EQ(term_text(*field_reader.value()->get_min().value()), "apple");
    EXPECT_EQ(term_text(*field_reader.value()->get_max().value()), "cherry");

    auto terms_enum = field_reader.value()->iterator();
    ASSERT_TRUE(terms_enum.has_value());
    for (const TermSpec &spec : terms) {
        auto term = terms_enum.value()->next();
        ASSERT_TRUE(term.has_value());
        ASSERT_NE(term.value(), nullptr);
        EXPECT_EQ(term_text(*term.value()), spec.text);
        EXPECT_EQ(terms_enum.value()->doc_freq().value(), static_cast<int32_t>(spec.docs.size()));
        EXPECT_EQ(terms_enum.value()->total_term_freq().value(), spec.total_term_freq);
    }
    auto end = terms_enum.value()->next();
    ASSERT_TRUE(end.has_value());
    EXPECT_EQ(end.value(), nullptr);

    auto seeked = field_reader.value()->iterator();
    ASSERT_TRUE(seeked.has_value());
    EXPECT_TRUE(seeked.value()->seek_exact(std::span<const uint8_t>(reinterpret_cast<const uint8_t *>("banana"), 6))
                        .value());
    EXPECT_FALSE(seeked.value()->seek_exact(std::span<const uint8_t>(reinterpret_cast<const uint8_t *>("blue"), 4))
                         .value());
    auto ceil = seeked.value()->seek_ceil(std::span<const uint8_t>(reinterpret_cast<const uint8_t *>("b"), 1));
    ASSERT_TRUE(ceil.has_value());
    EXPECT_EQ(ceil.value(), SeekStatus::NotFound);
    auto ceil_term = seeked.value()->term();
    ASSERT_TRUE(ceil_term.has_value());
    EXPECT_EQ(term_text(*ceil_term.value()), "banana");
}

TEST(BlockTreeTermsWriterTest, WritesManyTermsAcrossBlocksAndFloorBlocks) {
    ByteBuffersDirectory directory;
    auto infos = field_infos_with({make_field()});
    auto segment = segment_info_for("_0", 0xCD, 1000);

    std::vector<TermSpec> terms;
    // A long run sharing the "aa" prefix, which `max_items = 4` turns into a set
    // of floor blocks.
    for (int32_t i = 0; i < 100; ++i) {
        terms.push_back(TermSpec{std::format("aa{:02d}", i), {i % 7}, 1});
    }
    terms.push_back(TermSpec{"b", {0}, 1});
    terms.push_back(TermSpec{"ba", {1}, 2});
    terms.push_back(TermSpec{"bzz", {2}, 3});
    std::sort(terms.begin(), terms.end(), [](const TermSpec &left, const TermSpec &right) {
        return left.text < right.text;
    });

    write_terms(directory, infos, segment, terms, 2, 4);

    SegmentReadState read_state{directory, infos, io_context_default()};
    auto reader = Lucene90BlockTreeTermsReader::create(std::make_unique<FakePostingsReader>(), read_state, segment);
    ASSERT_TRUE(reader.has_value()) << reader.error().to_string();
    auto field_reader = reader.value()->terms("f");
    ASSERT_TRUE(field_reader.has_value());
    ASSERT_NE(field_reader.value(), nullptr);
    EXPECT_EQ(field_reader.value()->size().value(), static_cast<int64_t>(terms.size()));
    EXPECT_EQ(field_reader.value()->sum_doc_freq().value(), static_cast<int64_t>(terms.size()));

    auto terms_enum = field_reader.value()->iterator();
    ASSERT_TRUE(terms_enum.has_value());
    size_t seen = 0;
    while (true) {
        auto term = terms_enum.value()->next();
        ASSERT_TRUE(term.has_value()) << term.error().to_string();
        if (term.value() == nullptr) {
            break;
        }
        ASSERT_LT(seen, terms.size());
        EXPECT_EQ(term_text(*term.value()), terms[seen].text) << "at index " << seen;
        ++seen;
    }
    EXPECT_EQ(seen, terms.size());

    // Every term must still be found by an exact seek.
    auto seeked = field_reader.value()->iterator();
    ASSERT_TRUE(seeked.has_value());
    for (const TermSpec &spec : terms) {
        EXPECT_TRUE(seeked.value()
                            ->seek_exact(std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(spec.text.data()),
                                                                  spec.text.size()))
                            .value())
                << spec.text;
    }
}

/// Long, highly redundant suffixes make the writer compress the suffix block,
/// first with LZ4 (6+ bytes per term) and then with lowercase-ASCII (otherwise).
TEST(BlockTreeTermsWriterTest, RoundTripsCompressedSuffixBlocks) {
    for (const size_t suffix_width : {size_t{6}, size_t{8}, size_t{16}}) {
        ByteBuffersDirectory directory;
        auto infos = field_infos_with({make_field()});
        auto segment = segment_info_for("_0", 0xEE, 500);

        // A 3 byte block prefix leaves 3 suffix bytes per term (too short for
        // LZ4), a 2 byte one leaves 7 (long enough), so the two widths exercise
        // the lowercase-ASCII and the LZ4 path respectively.
        const std::string prefix = suffix_width == 6 ? "ccc" : "dd";
        // `16` byte suffixes repeat so heavily that LZ4 beats the lowercase
        // compressor, which is what covers the second compression path.
        std::vector<TermSpec> terms;
        for (int32_t i = 0; i < 200; ++i) {
            std::string suffix(suffix_width, 'a');
            int32_t value = i;
            for (size_t digit = 0; digit < suffix_width; ++digit) {
                // Lowercase only: that is what the lowercase-ASCII compressor
                // accepts, and LZ4 handles the same bytes for long suffixes.
                suffix[digit] = static_cast<char>('a' + value % 4);
                value /= 4;
            }
            terms.push_back(TermSpec{prefix + suffix, {i % 5}, 1});
        }
        std::sort(terms.begin(), terms.end(),
                  [](const TermSpec &left, const TermSpec &right) { return left.text < right.text; });

        write_terms(directory, infos, segment, terms, 4, 10);

        SegmentReadState read_state{directory, infos, io_context_default()};
        auto reader = Lucene90BlockTreeTermsReader::create(std::make_unique<FakePostingsReader>(), read_state, segment);
        ASSERT_TRUE(reader.has_value()) << reader.error().to_string();
        auto field_reader = reader.value()->terms("f");
        ASSERT_TRUE(field_reader.has_value());
        ASSERT_NE(field_reader.value(), nullptr);
        EXPECT_EQ(field_reader.value()->size().value(), static_cast<int64_t>(terms.size()));

        auto terms_enum = field_reader.value()->iterator();
        ASSERT_TRUE(terms_enum.has_value());
        size_t seen = 0;
        while (true) {
            auto term = terms_enum.value()->next();
            ASSERT_TRUE(term.has_value()) << term.error().to_string();
            if (term.value() == nullptr) {
                break;
            }
            ASSERT_LT(seen, terms.size());
            EXPECT_EQ(term_text(*term.value()), terms[seen].text) << "suffix width " << suffix_width;
            ++seen;
        }
        EXPECT_EQ(seen, terms.size());

        auto seeked = field_reader.value()->iterator();
        ASSERT_TRUE(seeked.has_value());
        for (const TermSpec &spec : terms) {
            EXPECT_TRUE(seeked.value()
                                ->seek_exact(std::span<const uint8_t>(
                                        reinterpret_cast<const uint8_t *>(spec.text.data()), spec.text.size()))
                                .value())
                    << spec.text;
        }
    }
}

TEST(BlockTreeTermsWriterTest, SkipsFieldsWithoutTerms) {
    ByteBuffersDirectory directory;
    auto infos = field_infos_with({make_field()});
    auto segment = segment_info_for();
    const std::vector<TermSpec> no_terms;

    write_terms(directory, infos, segment, no_terms, BlockTreeTermsWriter::DEFAULT_MIN_BLOCK_SIZE,
                BlockTreeTermsWriter::DEFAULT_MAX_BLOCK_SIZE);

    SegmentReadState read_state{directory, infos, io_context_default()};
    auto reader = Lucene90BlockTreeTermsReader::create(std::make_unique<FakePostingsReader>(), read_state, segment);
    ASSERT_TRUE(reader.has_value()) << reader.error().to_string();
    // A field with no terms leaves no `.tmd` record behind.
    EXPECT_EQ(reader.value()->size(), 0u);
    auto field_reader = reader.value()->terms("f");
    ASSERT_TRUE(field_reader.has_value());
    EXPECT_EQ(field_reader.value(), nullptr);
}

TEST(BlockTreeTermsWriterTest, ValidatesBlockSettings) {
    EXPECT_TRUE(BlockTreeTermsWriter::validate_settings(2, 2).has_value());
    EXPECT_TRUE(BlockTreeTermsWriter::validate_settings(25, 48).has_value());
    EXPECT_FALSE(BlockTreeTermsWriter::validate_settings(1, 10).has_value());
    EXPECT_FALSE(BlockTreeTermsWriter::validate_settings(10, 9).has_value());
    // max_items must be at least 2*(min_items-1).
    EXPECT_FALSE(BlockTreeTermsWriter::validate_settings(10, 17).has_value());
    EXPECT_TRUE(BlockTreeTermsWriter::validate_settings(10, 18).has_value());
}

TEST(BlockTreeTermsWriterTest, RejectsMissingPostingsWriter) {
    ByteBuffersDirectory directory;
    auto infos = field_infos_with({make_field()});
    auto segment = segment_info_for();
    SegmentWriteState state{get_default_info_stream(), directory, infos, io_context_default(), {}};

    auto writer = BlockTreeTermsWriter::create(state, nullptr, 2, 4, segment);
    ASSERT_FALSE(writer.has_value());
    EXPECT_EQ(writer.error().kind(), ErrorKind::kIllegalArgument);
}

TEST(BlockTreeTermsWriterTest, CloseIsIdempotent) {
    ByteBuffersDirectory directory;
    auto infos = field_infos_with({make_field()});
    auto segment = segment_info_for();
    const std::vector<TermSpec> terms = sample_terms();
    SegmentWriteState state{get_default_info_stream(), directory, infos, io_context_default(), {}};

    auto writer = BlockTreeTermsWriter::create_default(state, std::make_unique<FakePostingsWriter>(&terms), segment);
    ASSERT_TRUE(writer.has_value());
    EXPECT_FALSE(writer.value()->is_closed());
    EXPECT_EQ(writer.value()->version(), BLOCK_TREE_VERSION_CURRENT);

    FakeFields fields{{"f"}, {{"f", &terms}}};
    ASSERT_TRUE(writer.value()->write(state, segment, fields, nullptr).has_value());
    EXPECT_EQ(writer.value()->field_count(), 1u);

    ASSERT_TRUE(writer.value()->close().has_value());
    EXPECT_TRUE(writer.value()->is_closed());
    EXPECT_TRUE(writer.value()->close().has_value());
    // Writing after close is an error, not a crash.
    EXPECT_FALSE(writer.value()->write(state, segment, fields, nullptr).has_value());
}

namespace {

/// Compiles `automaton` into the "normal" form the block tree intersects.
CompiledAutomaton compile_normal(const Automaton &automaton, bool finite) {
    auto compiled = CompiledAutomaton::create(automaton, finite, false);
    EXPECT_TRUE(compiled.has_value());
    EXPECT_EQ(compiled->type(), AutomatonType::kNormal);
    return std::move(compiled.value());
}

/// Drains the terms of `field` accepted by `compiled`.
std::vector<std::string> intersect_texts(FieldReader &field, const CompiledAutomaton &compiled,
                                         const BytesRef *start_term = nullptr) {
    auto tenum = field.intersect(compiled, start_term);
    EXPECT_TRUE(tenum.has_value());
    std::vector<std::string> texts;
    while (true) {
        auto term = tenum.value()->next();
        EXPECT_TRUE(term.has_value());
        if (term.value() == nullptr) {
            break;
        }
        texts.push_back(term_text(*term.value()));
    }
    return texts;
}

/// A terms reader and one of its fields; the field's parent lives in the reader,
/// so both are handed back together.
struct OpenedTerms {
    std::unique_ptr<Lucene90BlockTreeTermsReader> reader;
    std::unique_ptr<FieldReader> field;
};

/// Writes `terms` and opens the field reader for them.
OpenedTerms write_and_open(ByteBuffersDirectory &directory, const std::vector<TermSpec> &terms, int32_t min_items,
                          int32_t max_items) {
    auto infos = field_infos_with({make_field()});
    auto segment = segment_info_for("_0", 0xEF, static_cast<int32_t>(terms.size()));
    write_terms(directory, infos, segment, terms, min_items, max_items);

    SegmentReadState read_state{directory, infos, io_context_default()};
    auto reader = Lucene90BlockTreeTermsReader::create(std::make_unique<FakePostingsReader>(), read_state, segment);
    EXPECT_TRUE(reader.has_value());
    OpenedTerms opened;
    opened.reader = std::move(reader.value());
    auto field = opened.reader->terms("f");
    EXPECT_TRUE(field.has_value());
    EXPECT_TRUE(field.value() != nullptr);
    // `Fields::terms` hands out the type-erased `Terms`; these tests need the
    // block tree field reader's `intersect` API, and the concrete type is
    // `FieldReader`.
    auto *concrete = dynamic_cast<FieldReader *>(field.value().get());
    EXPECT_TRUE(concrete != nullptr);
    if (concrete != nullptr) {
        field.value().release();
        opened.field.reset(concrete);
    }
    return opened;
}

} // namespace

TEST(BlockTreeIntersectTest, EnumeratesEveryAcceptedTerm) {
    ByteBuffersDirectory directory;
    const std::vector<TermSpec> terms = sample_terms();
    auto opened = write_and_open(directory, terms, BlockTreeTermsWriter::DEFAULT_MIN_BLOCK_SIZE,
                                BlockTreeTermsWriter::DEFAULT_MAX_BLOCK_SIZE);

    auto any_string = Automata::make_any_string();
    ASSERT_TRUE(any_string.has_value());
    auto compiled = compile_normal(any_string.value(), false);

    const auto texts = intersect_texts(*opened.field, compiled);
    EXPECT_EQ(texts, (std::vector<std::string>{"apple", "banana", "cherry"}));
}

TEST(BlockTreeIntersectTest, FiltersByAutomaton) {
    ByteBuffersDirectory directory;
    const std::vector<TermSpec> terms = sample_terms();
    auto opened = write_and_open(directory, terms, BlockTreeTermsWriter::DEFAULT_MIN_BLOCK_SIZE,
                                BlockTreeTermsWriter::DEFAULT_MAX_BLOCK_SIZE);

    // "ba.*"
    auto prefix = Automata::make_string("ba");
    auto any_string = Automata::make_any_string();
    ASSERT_TRUE(prefix.has_value());
    ASSERT_TRUE(any_string.has_value());
    auto automaton = Operations::concatenate(prefix.value(), any_string.value());
    ASSERT_TRUE(automaton.has_value());

    const auto texts = intersect_texts(*opened.field, compile_normal(automaton.value(), false));
    EXPECT_EQ(texts, (std::vector<std::string>{"banana"}));
}

TEST(BlockTreeIntersectTest, SkipsStartTermLikeRlucene) {
    ByteBuffersDirectory directory;
    const std::vector<TermSpec> terms = sample_terms();
    auto opened = write_and_open(directory, terms, BlockTreeTermsWriter::DEFAULT_MIN_BLOCK_SIZE,
                                BlockTreeTermsWriter::DEFAULT_MAX_BLOCK_SIZE);

    auto any_string = Automata::make_any_string();
    ASSERT_TRUE(any_string.has_value());
    auto compiled = compile_normal(any_string.value(), false);

    const BytesRef start = BytesRef::from_string("apple");
    EXPECT_EQ(intersect_texts(*opened.field, compiled, &start), (std::vector<std::string>{"banana", "cherry"}));

    const BytesRef past_end = BytesRef::from_string("zzz");
    EXPECT_EQ(intersect_texts(*opened.field, compiled, &past_end), (std::vector<std::string>{}));
}

TEST(BlockTreeIntersectTest, ReadsMetadataOfAcceptedTerms) {
    ByteBuffersDirectory directory;
    const std::vector<TermSpec> terms = sample_terms();
    auto opened = write_and_open(directory, terms, BlockTreeTermsWriter::DEFAULT_MIN_BLOCK_SIZE,
                                BlockTreeTermsWriter::DEFAULT_MAX_BLOCK_SIZE);

    auto any_string = Automata::make_any_string();
    ASSERT_TRUE(any_string.has_value());
    auto compiled = compile_normal(any_string.value(), false);

    auto tenum = opened.field->intersect(compiled, nullptr);
    ASSERT_TRUE(tenum.has_value());
    for (const TermSpec &spec : terms) {
        auto term = tenum.value()->next();
        ASSERT_TRUE(term.has_value());
        ASSERT_NE(term.value(), nullptr);
        EXPECT_EQ(term_text(*term.value()), spec.text);
        EXPECT_EQ(tenum.value()->doc_freq().value(), static_cast<int32_t>(spec.docs.size()));
        EXPECT_EQ(tenum.value()->total_term_freq().value(), spec.total_term_freq);
    }
    auto end = tenum.value()->next();
    ASSERT_TRUE(end.has_value());
    EXPECT_EQ(end.value(), nullptr);

    // The intersection enum cannot seek.
    EXPECT_FALSE(tenum.value()->seek_exact(BytesRef::from_string("apple").as_slice()).has_value());
}

TEST(BlockTreeIntersectTest, WalksFloorBlocks) {
    ByteBuffersDirectory directory;
    std::vector<TermSpec> terms;
    // A long run sharing the "aa" prefix: `max_items = 4` turns it into floor
    // blocks, which is what the frame stack has to follow.
    for (int32_t i = 0; i < 100; ++i) {
        terms.push_back(TermSpec{std::format("aa{:02d}", i), {i % 7}, 1});
    }
    terms.push_back(TermSpec{"b", {0}, 1});
    auto opened = write_and_open(directory, terms, 2, 4);

    auto prefix = Automata::make_string("aa");
    auto any_string = Automata::make_any_string();
    ASSERT_TRUE(prefix.has_value());
    ASSERT_TRUE(any_string.has_value());
    auto automaton = Operations::concatenate(prefix.value(), any_string.value());
    ASSERT_TRUE(automaton.has_value());

    const auto texts = intersect_texts(*opened.field, compile_normal(automaton.value(), false));
    ASSERT_EQ(texts.size(), 100u);
    for (int32_t i = 0; i < 100; ++i) {
        EXPECT_EQ(texts[static_cast<size_t>(i)], std::format("aa{:02d}", i));
    }
}

TEST(BlockTreeIntersectTest, RejectsSimplifiedAutomata) {
    ByteBuffersDirectory directory;
    const std::vector<TermSpec> terms = sample_terms();
    auto opened = write_and_open(directory, terms, BlockTreeTermsWriter::DEFAULT_MIN_BLOCK_SIZE,
                                BlockTreeTermsWriter::DEFAULT_MAX_BLOCK_SIZE);

    auto simplified = CompiledAutomaton::from_automaton(Automata::make_empty().value());
    ASSERT_TRUE(simplified.has_value());
    ASSERT_EQ(simplified->type(), AutomatonType::kNone);

    auto tenum = opened.field->intersect(simplified.value(), nullptr);
    ASSERT_FALSE(tenum.has_value());
    EXPECT_EQ(tenum.error().kind(), ErrorKind::kIllegalArgument);
}
