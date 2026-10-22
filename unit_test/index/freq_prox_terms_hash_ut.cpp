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
#include <vector>

module lucene_core:ut.freq_prox_terms_hash;

import lucene_core;

using namespace lucene;

namespace {

BytesRef term(std::string_view text) { return BytesRef::from_string(text); }

/// One inverted token of the synthetic documents below.
struct Token {
    std::string text;
    int32_t position{0};
    int32_t start{0};
    int32_t end{0};
    std::optional<std::string> payload;

    Token(std::string text_, int32_t position_, int32_t start_, int32_t end_,
          std::optional<std::string> payload_ = std::nullopt)
            : text(std::move(text_)),
              position(position_),
              start(start_),
              end(end_),
              payload(std::move(payload_)) {}
};

/// Drives `FreqProxTermsWriterPerField` the way the indexing chain does: one
/// `FieldInvertState` and one attribute source, stepped over the tokens.
class FieldHarness {
public:
    explicit FieldHarness(IndexOptions options, std::string name = "body") {
        info_ = std::make_shared<FieldInfo>();
        info_->number = 0;
        info_->name = std::move(name);
        info_->index_options = options;
        writer_ = std::make_shared<FreqProxTermsWriterPerField>(info_, nullptr);

        attributes_.add_attribute<OffsetAttribute>();
        attributes_.add_attribute<PositionIncrementAttribute>();
        attributes_.add_attribute<PayloadAttribute>();
        attributes_.add_attribute<TermFrequencyAttribute>();
    }

    void add_doc(int32_t doc_id, const std::vector<Token> &tokens) {
        FieldInvertState state(6, info_->name, info_->index_options);
        for (const auto &token : tokens) {
            state.set_position(token.position);
            state.set_offset(0);
            attributes_.get_attribute<OffsetAttribute>()->set_offset(static_cast<uint32_t>(token.start),
                                                                     static_cast<uint32_t>(token.end));
            auto *payload = attributes_.get_attribute<PayloadAttribute>();
            if (token.payload.has_value()) {
                payload->set_payload(term(token.payload.value()));
            } else {
                payload->set_payload(std::nullopt);
            }
            auto bytes = term(token.text);
            ASSERT_TRUE(writer_->add_with_bytes_ref(&bytes, doc_id, state, attributes_, context_).has_value());
        }
    }

    /// Sorts the terms and hands the buffered fields to a `FreqProxFields`.
    std::shared_ptr<FreqProxFields> fields() {
        EXPECT_TRUE(writer_->base().sort_terms(context_.byte_pool).has_value());
        std::vector<std::shared_ptr<FreqProxTermsWriterPerField>> list{writer_};
        return std::make_shared<FreqProxFields>(list,
                                                std::make_shared<IntBlockPool>(
                                                        std::move(context_.freq_prox_term_int_pool)),
                                                std::make_shared<ByteBlockPool>(std::move(context_.byte_pool)));
    }

    std::shared_ptr<FreqProxTermsWriterPerField> writer() const { return writer_; }
    [[nodiscard]] AttributeSource &attributes() noexcept { return attributes_; }
    [[nodiscard]] IndexContext &context() noexcept { return context_; }

private:
    std::shared_ptr<FieldInfo> info_;
    std::shared_ptr<FreqProxTermsWriterPerField> writer_;
    IndexContext context_;
    AttributeSource attributes_;
};

std::vector<std::string> term_list(const Terms &terms) {
    std::vector<std::string> result;
    auto enumeration = terms.iterator();
    EXPECT_TRUE(enumeration.has_value());
    while (true) {
        auto next = enumeration.value()->next();
        EXPECT_TRUE(next.has_value());
        if (next.value() == nullptr) {
            break;
        }
        result.push_back(std::string(reinterpret_cast<const char *>(next.value()->as_slice().data()),
                                     next.value()->length()));
    }
    return result;
}

} // namespace

TEST(FreqProxTermsHashTest, CollectsTermsInSortedOrder) {
    FieldHarness harness(IndexOptions::DocsAndFreqs);
    harness.add_doc(0, {{"delta", 0, 0, 5}, {"alpha", 1, 6, 11}, {"charlie", 2, 12, 19}});
    harness.add_doc(1, {{"bravo", 0, 0, 5}, {"alpha", 1, 6, 11}});

    auto fields = harness.fields();
    ASSERT_EQ(fields->size().has_value(), false); // statistics are not buffered

    auto terms = fields->terms("body");
    ASSERT_TRUE(terms.has_value());
    ASSERT_NE(terms.value(), nullptr);
    EXPECT_EQ(term_list(*terms.value()), (std::vector<std::string>{"alpha", "bravo", "charlie", "delta"}));

    // Unknown fields and the field names are reported.
    EXPECT_TRUE(fields->terms("missing").has_value());
    EXPECT_EQ(fields->terms("missing").value(), nullptr);

    auto names = fields->iterator();
    ASSERT_TRUE(names.has_value());
    auto first = names.value()->next();
    ASSERT_TRUE(first.has_value());
    ASSERT_NE(first.value(), nullptr);
    EXPECT_EQ(*first.value(), "body");
    EXPECT_EQ(names.value()->next().value(), nullptr);
}

TEST(FreqProxTermsHashTest, DocsOnlyPostingsReportDocumentIds) {
    FieldHarness harness(IndexOptions::Docs);
    harness.add_doc(0, {{"alpha", 0, 0, 5}, {"bravo", 1, 6, 11}});
    harness.add_doc(1, {{"alpha", 0, 0, 5}});
    harness.add_doc(3, {{"alpha", 0, 0, 5}});

    auto fields = harness.fields();
    auto terms = fields->terms("body");
    ASSERT_TRUE(terms.has_value());
    EXPECT_FALSE(terms.value()->has_freqs());
    EXPECT_FALSE(terms.value()->has_positions());

    auto enumeration = terms.value()->iterator();
    ASSERT_TRUE(enumeration.has_value());
    ASSERT_TRUE(enumeration.value()->seek_exact(term("alpha").as_slice()).value());

    // No frequency flags: the docs-only enum must not pretend to know freqs.
    auto postings = enumeration.value()->postings_with_flags(nullptr, 0);
    ASSERT_TRUE(postings.has_value()) << postings.error().to_string();
    std::vector<int32_t> docs;
    while (true) {
        auto doc = postings.value()->next_doc();
        ASSERT_TRUE(doc.has_value()) << doc.error().to_string();
        if (doc.value() == NO_MORE_DOCS) {
            break;
        }
        docs.push_back(doc.value());
    }
    EXPECT_EQ(docs, (std::vector<int32_t>{0, 1, 3}));
    EXPECT_FALSE(postings.value()->freq().has_value());
}

TEST(FreqProxTermsHashTest, FrequenciesAreAccumulatedPerDocument) {
    FieldHarness harness(IndexOptions::DocsAndFreqs);
    harness.add_doc(0, {{"alpha", 0, 0, 5}, {"alpha", 1, 6, 11}, {"bravo", 2, 12, 17}});
    harness.add_doc(1, {{"alpha", 0, 0, 5}});
    harness.add_doc(2, {{"alpha", 0, 0, 5}, {"alpha", 1, 6, 11}, {"alpha", 2, 12, 17}});

    auto fields = harness.fields();
    auto terms = fields->terms("body");
    ASSERT_TRUE(terms.has_value());
    EXPECT_TRUE(terms.value()->has_freqs());

    auto enumeration = terms.value()->iterator();
    ASSERT_TRUE(enumeration.has_value());
    ASSERT_TRUE(enumeration.value()->seek_exact(term("alpha").as_slice()).value());

    auto postings = enumeration.value()->postings_with_flags(nullptr, 8); // FREQS
    ASSERT_TRUE(postings.has_value()) << postings.error().to_string();

    std::vector<std::pair<int32_t, int32_t>> doc_freqs;
    while (true) {
        auto doc = postings.value()->next_doc();
        ASSERT_TRUE(doc.has_value()) << doc.error().to_string();
        if (doc.value() == NO_MORE_DOCS) {
            break;
        }
        auto freq = postings.value()->freq();
        ASSERT_TRUE(freq.has_value());
        doc_freqs.emplace_back(doc.value(), freq.value());
    }
    EXPECT_EQ(doc_freqs, (std::vector<std::pair<int32_t, int32_t>>{{0, 2}, {1, 1}, {2, 3}}));
}

TEST(FreqProxTermsHashTest, PositionsAndOffsetsRoundTrip) {
    FieldHarness harness(IndexOptions::DocsAndFreqsAndPositionsAndOffsets);
    harness.add_doc(0, {{"alpha", 1, 0, 5}, {"bravo", 2, 6, 11}, {"alpha", 3, 12, 17}});
    harness.add_doc(1, {{"bravo", 1, 0, 5}});

    auto fields = harness.fields();
    auto terms = fields->terms("body");
    ASSERT_TRUE(terms.has_value());
    EXPECT_TRUE(terms.value()->has_positions());
    EXPECT_TRUE(terms.value()->has_offsets());

    auto enumeration = terms.value()->iterator();
    ASSERT_TRUE(enumeration.has_value());
    ASSERT_TRUE(enumeration.value()->seek_exact(term("alpha").as_slice()).value());

    auto postings = enumeration.value()->postings_with_flags(nullptr, 24); // POSITIONS
    ASSERT_TRUE(postings.has_value()) << postings.error().to_string();

    auto doc = postings.value()->next_doc();
    ASSERT_TRUE(doc.has_value());
    EXPECT_EQ(doc.value(), 0);
    EXPECT_EQ(postings.value()->freq().value(), 2);

    auto first_position = postings.value()->next_position();
    ASSERT_TRUE(first_position.has_value());
    EXPECT_EQ(first_position.value(), 1);
    EXPECT_EQ(postings.value()->start_offset().value(), 0);
    EXPECT_EQ(postings.value()->end_offset().value(), 5);

    auto second_position = postings.value()->next_position();
    ASSERT_TRUE(second_position.has_value());
    EXPECT_EQ(second_position.value(), 3);
    EXPECT_EQ(postings.value()->start_offset().value(), 12);
    EXPECT_EQ(postings.value()->end_offset().value(), 17);

    EXPECT_EQ(postings.value()->next_doc().value(), NO_MORE_DOCS);
}

TEST(FreqProxTermsHashTest, PayloadsAreRecordedAndFlagged) {
    FieldHarness harness(IndexOptions::DocsAndFreqsAndPositionsAndOffsets);
    harness.add_doc(0, {{"alpha", 1, 0, 5, std::string{"P1"}}, {"bravo", 2, 6, 11}});

    EXPECT_TRUE(harness.writer()->saw_payloads());
    ASSERT_TRUE(harness.writer()->finish(0).has_value());
    EXPECT_TRUE(harness.writer()->field_info()->store_payloads);

    auto fields = harness.fields();
    auto terms = fields->terms("body");
    ASSERT_TRUE(terms.has_value());
    EXPECT_TRUE(terms.value()->has_payloads());

    auto enumeration = terms.value()->iterator();
    ASSERT_TRUE(enumeration.has_value());
    ASSERT_TRUE(enumeration.value()->seek_exact(term("alpha").as_slice()).value());

    auto postings = enumeration.value()->postings_with_flags(nullptr, 24);
    ASSERT_TRUE(postings.has_value());
    ASSERT_EQ(postings.value()->next_doc().value(), 0);
    ASSERT_EQ(postings.value()->next_position().value(), 1);

    auto payload = postings.value()->get_payload();
    ASSERT_TRUE(payload.has_value());
    ASSERT_TRUE(payload.value().has_value());
    const auto bytes = payload.value().value();
    EXPECT_EQ(std::string(reinterpret_cast<const char *>(bytes.data()), bytes.size()), "P1");

    // A term without a payload reports none.
    ASSERT_TRUE(enumeration.value()->seek_exact(term("bravo").as_slice()).value());
    auto bravo_postings = enumeration.value()->postings_with_flags(nullptr, 24);
    ASSERT_TRUE(bravo_postings.has_value());
    ASSERT_EQ(bravo_postings.value()->next_doc().value(), 0);
    ASSERT_EQ(bravo_postings.value()->next_position().value(), 2);
    EXPECT_FALSE(bravo_postings.value()->get_payload().value().has_value());
}

TEST(FreqProxTermsHashTest, SeeksExactAndCeiling) {
    FieldHarness harness(IndexOptions::DocsAndFreqs);
    harness.add_doc(0, {{"alpha", 0, 0, 5}, {"charlie", 1, 6, 13}, {"echo", 2, 14, 18}});

    auto fields = harness.fields();
    auto terms = fields->terms("body");
    ASSERT_TRUE(terms.has_value());
    auto enumeration = terms.value()->iterator();
    ASSERT_TRUE(enumeration.has_value());

    EXPECT_TRUE(enumeration.value()->seek_exact(term("charlie").as_slice()).value());
    EXPECT_EQ(enumeration.value()->ord().value(), 1);
    EXPECT_EQ(enumeration.value()->term().value()->utf8_to_string().value(), "charlie");

    EXPECT_FALSE(enumeration.value()->seek_exact(term("bravo").as_slice()).value());
    EXPECT_EQ(enumeration.value()->term().value()->utf8_to_string().value(), "charlie");

    auto status = enumeration.value()->seek_ceil(term("zzz").as_slice());
    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(status.value(), SeekStatus::End);

    // seek_exact_with_ord round-trips.
    ASSERT_TRUE(enumeration.value()->seek_exact_with_ord(2).has_value());
    EXPECT_EQ(enumeration.value()->term().value()->utf8_to_string().value(), "echo");

    // Terms expose no statistics while buffered.
    EXPECT_FALSE(enumeration.value()->doc_freq().has_value());
    EXPECT_FALSE(enumeration.value()->total_term_freq().has_value());
    EXPECT_FALSE(enumeration.value()->impacts(0).has_value());
}

TEST(FreqProxTermsHashTest, RejectsPositionFlagsWhenPositionsAreNotIndexed) {
    FieldHarness harness(IndexOptions::DocsAndFreqs);
    harness.add_doc(0, {{"alpha", 0, 0, 5}});

    auto fields = harness.fields();
    auto terms = fields->terms("body");
    ASSERT_TRUE(terms.has_value());
    auto enumeration = terms.value()->iterator();
    ASSERT_TRUE(enumeration.has_value());
    ASSERT_TRUE(enumeration.value()->seek_exact(term("alpha").as_slice()).value());

    auto postings = enumeration.value()->postings_with_flags(nullptr, 24);
    ASSERT_FALSE(postings.has_value());
    EXPECT_EQ(postings.error().kind(), ErrorKind::kIllegalArgument);
}

TEST(FreqProxTermsHashTest, MultipleFieldsAreListedSortedByName) {
    // The flush sorts the per-field writers by name before building the fields;
    // do the same here.
    auto body_info = std::make_shared<FieldInfo>();
    body_info->number = 0;
    body_info->name = "body";
    body_info->index_options = IndexOptions::DocsAndFreqs;
    auto title_info = std::make_shared<FieldInfo>();
    title_info->number = 1;
    title_info->name = "title";
    title_info->index_options = IndexOptions::DocsAndFreqs;

    auto body = std::make_shared<FreqProxTermsWriterPerField>(body_info, nullptr);
    auto title = std::make_shared<FreqProxTermsWriterPerField>(title_info, nullptr);
    ASSERT_EQ(body->compare(*title), -1);
    ASSERT_EQ(title->compare(*body), 1);
    ASSERT_EQ(body->compare(*body), 0);

    IndexContext context;
    AttributeSource attributes;
    attributes.add_attribute<OffsetAttribute>();
    attributes.add_attribute<PayloadAttribute>();
    attributes.add_attribute<TermFrequencyAttribute>();

    for (const auto &writer : {body, title}) {
        FieldInvertState state(6, writer->field_info()->name, writer->field_info()->index_options);
        attributes.get_attribute<OffsetAttribute>()->set_offset(0, 5);
        attributes.get_attribute<PayloadAttribute>()->set_payload(std::nullopt);
        auto bytes = term(writer->field_info()->name + "-term");
        ASSERT_TRUE(writer->add_with_bytes_ref(&bytes, 0, state, attributes, context).has_value());
    }

    ASSERT_TRUE(body->base().sort_terms(context.byte_pool).has_value());
    ASSERT_TRUE(title->base().sort_terms(context.byte_pool).has_value());

    FreqProxFields fields({body, title}, std::make_shared<IntBlockPool>(std::move(context.freq_prox_term_int_pool)),
                          std::make_shared<ByteBlockPool>(std::move(context.byte_pool)));

    std::vector<std::string> names;
    auto iterator = fields.iterator();
    ASSERT_TRUE(iterator.has_value());
    while (true) {
        auto name = iterator.value()->next();
        ASSERT_TRUE(name.has_value());
        if (name.value() == nullptr) {
            break;
        }
        names.push_back(*name.value());
    }
    EXPECT_EQ(names, (std::vector<std::string>{"body", "title"}));

    auto body_terms = fields.terms("body");
    ASSERT_TRUE(body_terms.has_value());
    EXPECT_EQ(term_list(*body_terms.value()), (std::vector<std::string>{"body-term"}));
    auto title_terms = fields.terms("title");
    ASSERT_TRUE(title_terms.has_value());
    EXPECT_EQ(term_list(*title_terms.value()), (std::vector<std::string>{"title-term"}));
}

TEST(FreqProxTermsHashTest, TermBytesComeFromTheAttributeSourceWhenNotGiven) {
    FieldHarness harness(IndexOptions::DocsAndFreqs);
    auto &attributes = harness.attributes();
    // The indexing chain passes no explicit `BytesRef`; the term then comes from
    // the attribute source (`BytesTermAttribute`, else `CharTermAttribute`).
    auto *char_term = attributes.add_attribute<CharTermAttribute>();
    char_term->set_text("gamma");
    attributes.get_attribute<OffsetAttribute>()->set_offset(0, 5);
    attributes.get_attribute<PayloadAttribute>()->set_payload(std::nullopt);

    FieldInvertState state(6, "body", IndexOptions::DocsAndFreqs);
    ASSERT_TRUE(harness.writer()
                        ->add_with_bytes_ref(nullptr, 0, state, attributes, harness.context())
                        .has_value());

    auto fields = harness.fields();
    auto terms = fields->terms("body");
    ASSERT_TRUE(terms.has_value());
    EXPECT_EQ(term_list(*terms.value()), (std::vector<std::string>{"gamma"}));
}
