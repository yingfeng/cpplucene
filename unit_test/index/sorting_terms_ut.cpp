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
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module lucene_core:ut.sorting_terms;

import lucene_core;

using namespace lucene;

namespace {

/// The postings of one document in the fake below.
struct FakeDocs {
    int32_t doc{0};
    std::vector<int32_t> positions;
    std::vector<int32_t> starts;
    std::vector<int32_t> ends;
    std::vector<std::vector<uint8_t>> payloads;
};

/// A postings enum over a fixed list of documents.
class FakePostingsEnum final : public PostingsEnum {
public:
    explicit FakePostingsEnum(std::vector<FakeDocs> docs) : docs_(std::move(docs)) {}

    [[nodiscard]] int32_t doc_id() const override { return doc_; }

    [[nodiscard]] Result<int32_t> next_doc() override {
        index_ += 1;
        if (index_ >= docs_.size()) {
            doc_ = NO_MORE_DOCS;
            return doc_;
        }
        doc_ = docs_[index_].doc;
        position_index_ = 0;
        return doc_;
    }

    [[nodiscard]] Result<int32_t> advance(int32_t target) override {
        while (true) {
            auto doc = next_doc();
            if (!doc.has_value() || doc.value() == NO_MORE_DOCS || doc.value() >= target) {
                return doc;
            }
        }
    }

    [[nodiscard]] Result<int64_t> cost() const override { return static_cast<int64_t>(docs_.size()); }

    [[nodiscard]] Result<int32_t> freq() override { return static_cast<int32_t>(docs_[index_].positions.size()); }

    [[nodiscard]] Result<int32_t> next_position() override {
        return docs_[index_].positions[position_index_++];
    }

    [[nodiscard]] Result<int32_t> start_offset() const override { return docs_[index_].starts[position_index_ - 1]; }
    [[nodiscard]] Result<int32_t> end_offset() const override { return docs_[index_].ends[position_index_ - 1]; }

    [[nodiscard]] Result<std::optional<std::span<const uint8_t>>> get_payload() const override {
        const std::vector<uint8_t> &payload = docs_[index_].payloads[position_index_ - 1];
        if (payload.empty()) {
            return std::optional<std::span<const uint8_t>>{};
        }
        return std::optional<std::span<const uint8_t>>{std::span<const uint8_t>(payload)};
    }

private:
    std::vector<FakeDocs> docs_;
    size_t index_{static_cast<size_t>(-1)};
    size_t position_index_{0};
    int32_t doc_{-1};
};

/// One term of the fake dictionary: its text and the postings it has.
struct FakeTerm {
    std::string text;
    std::vector<FakeDocs> postings;
};

/// A [`TermsEnum`] over a fixed, sorted list of terms.
class FakeTermsEnum final : public TermsEnum {
public:
    FakeTermsEnum(std::vector<FakeTerm> terms, bool impacts_fail)
            : terms_(std::move(terms)), impacts_fail_(impacts_fail) {}

    [[nodiscard]] Result<const BytesRef *> next() override {
        if (!pos_.has_value()) {
            if (terms_.empty()) {
                return static_cast<const BytesRef *>(nullptr);
            }
            pos_ = 0;
            set_term();
            return &term_;
        }
        if (pos_.value() + 1 >= terms_.size()) {
            pos_.reset();
            return static_cast<const BytesRef *>(nullptr);
        }
        pos_ = pos_.value() + 1;
        set_term();
        return &term_;
    }

    [[nodiscard]] Result<AttributeSource *> attributes() override {
        return err(LuceneError::unsupported_operation("FakeTermsEnum has no attributes"));
    }
    [[nodiscard]] Result<AttributeSource *> attributes_mut() override {
        return err(LuceneError::unsupported_operation("FakeTermsEnum has no attributes"));
    }

    [[nodiscard]] Result<bool> seek_exact(std::span<const uint8_t> term) override {
        auto status = seek_ceil(term);
        if (!status) {
            return std::unexpected(std::move(status.error()));
        }
        return status.value() == SeekStatus::Found;
    }

    [[nodiscard]] Result<bool> prepare_seek_exact(std::span<const uint8_t> /*term*/) override {
        return err(LuceneError::unsupported_operation("FakeTermsEnum::prepare_seek_exact"));
    }

    [[nodiscard]] Result<bool> get_prepare_seek_exact_status(std::span<const uint8_t> /*target*/) override {
        return err(LuceneError::unsupported_operation("FakeTermsEnum::get_prepare_seek_exact_status"));
    }

    [[nodiscard]] Result<SeekStatus> seek_ceil(std::span<const uint8_t> term) override {
        for (size_t i = 0; i < terms_.size(); ++i) {
            const std::span<const uint8_t> candidate(
                    reinterpret_cast<const uint8_t *>(terms_[i].text.data()), terms_[i].text.size());
            if (std::ranges::lexicographical_compare(candidate, term)) {
                continue;
            }
            pos_ = i;
            set_term();
            return std::ranges::equal(candidate, term) ? SeekStatus::Found : SeekStatus::NotFound;
        }
        pos_.reset();
        return SeekStatus::End;
    }

    [[nodiscard]] VoidResult seek_exact_with_ord(int64_t ord) override {
        if (ord < 0 || static_cast<size_t>(ord) >= terms_.size()) {
            return err(LuceneError::illegal_argument("ord out of range"));
        }
        pos_ = static_cast<size_t>(ord);
        set_term();
        return {};
    }

    [[nodiscard]] VoidResult seek_exact_with_state(std::span<const uint8_t> /*term*/,
                                                   const TermStateEnum & /*state*/) override {
        return err(LuceneError::unsupported_operation("FakeTermsEnum::seek_exact_with_state"));
    }

    [[nodiscard]] Result<const BytesRef *> term() const override {
        if (!pos_.has_value()) {
            return err(LuceneError::illegal_state("unpositioned"));
        }
        return &term_;
    }

    [[nodiscard]] Result<int64_t> ord() const override {
        if (!pos_.has_value()) {
            return err(LuceneError::illegal_state("unpositioned"));
        }
        return static_cast<int64_t>(pos_.value());
    }

    [[nodiscard]] Result<int32_t> doc_freq() override {
        if (!pos_.has_value()) {
            return err(LuceneError::illegal_state("unpositioned"));
        }
        return static_cast<int32_t>(terms_[pos_.value()].postings.size());
    }

    [[nodiscard]] Result<int64_t> total_term_freq() override {
        auto freq = doc_freq();
        if (!freq) {
            return std::unexpected(std::move(freq.error()));
        }
        return static_cast<int64_t>(freq.value());
    }

    [[nodiscard]] Result<std::unique_ptr<PostingsEnum>> postings_with_flags(std::unique_ptr<PostingsEnum> /*reuse*/,
                                                                           int32_t /*flags*/) override {
        if (!pos_.has_value()) {
            return err(LuceneError::illegal_state("unpositioned"));
        }
        return std::unique_ptr<PostingsEnum>(std::make_unique<FakePostingsEnum>(terms_[pos_.value()].postings));
    }

    [[nodiscard]] Result<std::unique_ptr<ImpactsEnum>> impacts(int32_t /*flags*/) override {
        if (impacts_fail_) {
            return err(LuceneError::unsupported_operation("FakeTermsEnum has no impacts"));
        }
        return err(LuceneError::unsupported_operation("FakeTermsEnum has no impacts"));
    }

    [[nodiscard]] Result<TermStateEnum> term_state() override {
        return err(LuceneError::unsupported_operation("FakeTermsEnum has no term state"));
    }

private:
    void set_term() { term_ = BytesRef::from_string(terms_[pos_.value()].text); }

    std::vector<FakeTerm> terms_;
    bool impacts_fail_{false};
    std::optional<size_t> pos_{};
    BytesRef term_;
};

/// A [`Terms`] over a fixed dictionary.
class FakeTerms final : public Terms {
public:
    explicit FakeTerms(std::vector<FakeTerm> terms) : terms_(std::move(terms)) {}

    [[nodiscard]] Result<std::unique_ptr<TermsEnum>> iterator() const override {
        return std::unique_ptr<TermsEnum>(std::make_unique<FakeTermsEnum>(terms_, false));
    }

    /// Ignores the automaton: enough to check that the wrapper renumbers the
    /// postings of an intersected enum.
    [[nodiscard]] Result<std::unique_ptr<TermsEnum>> intersect(const CompiledAutomaton & /*compiled*/,
                                                              const BytesRef * /*start_term*/) const override {
        return iterator();
    }

    [[nodiscard]] Result<int64_t> size() const override { return static_cast<int64_t>(terms_.size()); }
    [[nodiscard]] Result<int64_t> sum_total_term_freq() const override { return 7; }
    [[nodiscard]] Result<int64_t> sum_doc_freq() const override { return 5; }
    [[nodiscard]] Result<int32_t> doc_count() const override { return 3; }
    [[nodiscard]] bool has_freqs() const override { return true; }
    [[nodiscard]] bool has_offsets() const override { return true; }
    [[nodiscard]] bool has_positions() const override { return true; }
    [[nodiscard]] bool has_payloads() const override { return true; }

private:
    std::vector<FakeTerm> terms_;
};

/// Three documents, "a" in all of them and "b" in the first one.
[[nodiscard]] std::vector<FakeTerm> sample_terms() {
    std::vector<FakeTerm> terms;
    terms.push_back(FakeTerm{"a",
                             {FakeDocs{0, {1}, {10}, {14}, {{0xAA}}},
                              FakeDocs{1, {2, 5}, {20, 40}, {24, 44}, {{0xBB}, {}}},
                              FakeDocs{2, {7}, {70}, {74}, {{0xCC}}}}});
    terms.push_back(FakeTerm{"b", {FakeDocs{0, {3}, {30}, {34}, {{0xDD}}}}});
    return terms;
}

/// Drains an enum into a list of term texts.
[[nodiscard]] std::vector<std::string> drain(TermsEnum &tenum) {
    std::vector<std::string> texts;
    while (true) {
        auto term = tenum.next();
        EXPECT_TRUE(term.has_value());
        if (term.value() == nullptr) {
            break;
        }
        auto text = term.value()->utf8_to_string();
        EXPECT_TRUE(text.has_value());
        texts.push_back(text.value());
    }
    return texts;
}

} // namespace

TEST(SortingTermsTest, ForwardsDictionaryStatistics) {
    auto terms = SortingTerms::create(std::make_unique<FakeTerms>(sample_terms()), IndexOptions::DocsAndFreqs,
                                     std::make_shared<IndexSortDocMapImpl>(std::vector<int32_t>{2, 1, 0}));
    ASSERT_TRUE(terms.has_value()) << (terms ? "" : terms.error().to_string());

    EXPECT_EQ(terms.value()->size().value(), 2);
    EXPECT_EQ(terms.value()->sum_total_term_freq().value(), 7);
    EXPECT_EQ(terms.value()->sum_doc_freq().value(), 5);
    EXPECT_EQ(terms.value()->doc_count().value(), 3);
    EXPECT_TRUE(terms.value()->has_freqs());
    EXPECT_TRUE(terms.value()->has_offsets());
    EXPECT_TRUE(terms.value()->has_positions());
    EXPECT_TRUE(terms.value()->has_payloads());

    auto min_term = terms.value()->get_min();
    ASSERT_TRUE(min_term.has_value());
    ASSERT_TRUE(min_term.value().has_value());
    EXPECT_EQ(min_term.value().value().utf8_to_string().value(), "a");
    auto max_term = terms.value()->get_max();
    ASSERT_TRUE(max_term.has_value());
    ASSERT_TRUE(max_term.value().has_value());
    EXPECT_EQ(max_term.value().value().utf8_to_string().value(), "b");

    EXPECT_TRUE(terms.value()->get_stats().has_value());
}

TEST(SortingTermsTest, IteratesTheTermsOfTheDictionary) {
    auto terms = SortingTerms::create(std::make_unique<FakeTerms>(sample_terms()), IndexOptions::DocsAndFreqs,
                                     std::make_shared<IndexSortDocMapImpl>(std::vector<int32_t>{2, 1, 0}));
    ASSERT_TRUE(terms.has_value());
    auto tenum = terms.value()->iterator();
    ASSERT_TRUE(tenum.has_value()) << (tenum ? "" : tenum.error().to_string());

    // The sort does not touch the term order nor the document frequencies.
    EXPECT_EQ(drain(*tenum.value()), (std::vector<std::string>{"a", "b"}));

    ASSERT_TRUE(tenum.value()->seek_exact(std::span<const uint8_t>(
            reinterpret_cast<const uint8_t *>("b"), 1)).value());
    EXPECT_EQ(tenum.value()->doc_freq().value(), 1);
    EXPECT_EQ(tenum.value()->ord().value(), 1);
    auto term = tenum.value()->term();
    ASSERT_TRUE(term.has_value());
    ASSERT_NE(term.value(), nullptr);
    EXPECT_EQ(term.value()->utf8_to_string().value(), "b");
}

TEST(SortingTermsTest, RenumbersPostingsWithFrequencies) {
    auto terms = SortingTerms::create(std::make_unique<FakeTerms>(sample_terms()), IndexOptions::DocsAndFreqs,
                                     std::make_shared<IndexSortDocMapImpl>(std::vector<int32_t>{2, 1, 0}));
    ASSERT_TRUE(terms.has_value());
    auto tenum = terms.value()->iterator();
    ASSERT_TRUE(tenum.has_value());

    auto first = tenum.value()->next();
    ASSERT_TRUE(first.has_value());
    ASSERT_NE(first.value(), nullptr);

    auto postings = tenum.value()->postings_with_flags(nullptr, ALL);
    ASSERT_TRUE(postings.has_value()) << (postings ? "" : postings.error().to_string());

    // Sorted document 0 is old document 2, with a single position at 7.
    ASSERT_EQ(postings.value()->next_doc().value(), 0);
    EXPECT_EQ(postings.value()->freq().value(), 1);
    EXPECT_EQ(postings.value()->next_position().value(), 7);
    EXPECT_EQ(postings.value()->start_offset().value(), 70);
    EXPECT_EQ(postings.value()->end_offset().value(), 74);
    auto payload = postings.value()->get_payload();
    ASSERT_TRUE(payload.has_value());
    ASSERT_TRUE(payload.value().has_value());
    EXPECT_EQ(payload.value().value()[0], 0xCC);

    // Sorted document 1 is old document 1, with two positions.
    ASSERT_EQ(postings.value()->next_doc().value(), 1);
    EXPECT_EQ(postings.value()->freq().value(), 2);
    EXPECT_EQ(postings.value()->next_position().value(), 2);
    EXPECT_EQ(postings.value()->next_position().value(), 5);

    ASSERT_EQ(postings.value()->next_doc().value(), 2);
    EXPECT_EQ(postings.value()->next_doc().value(), NO_MORE_DOCS);
    EXPECT_EQ(postings.value()->cost().value(), 3);
}

TEST(SortingTermsTest, RenumbersDocsOnlyPostings) {
    // A field indexed without frequencies: only the documents are renumbered.
    auto terms = SortingTerms::create(std::make_unique<FakeTerms>(sample_terms()), IndexOptions::Docs,
                                     std::make_shared<IndexSortDocMapImpl>(std::vector<int32_t>{2, 1, 0}));
    ASSERT_TRUE(terms.has_value());
    auto tenum = terms.value()->iterator();
    ASSERT_TRUE(tenum.has_value());

    auto first = tenum.value()->next();
    ASSERT_TRUE(first.has_value());
    ASSERT_NE(first.value(), nullptr);

    auto postings = tenum.value()->postings_with_flags(nullptr, ALL);
    ASSERT_TRUE(postings.has_value()) << (postings ? "" : postings.error().to_string());

    ASSERT_EQ(postings.value()->advance(1).value(), 1);
    EXPECT_EQ(postings.value()->freq().value(), 1);
    EXPECT_EQ(postings.value()->next_position().value(), -1);
    EXPECT_EQ(postings.value()->start_offset().value(), -1);
    EXPECT_EQ(postings.value()->end_offset().value(), -1);
    auto payload = postings.value()->get_payload();
    ASSERT_TRUE(payload.has_value());
    EXPECT_FALSE(payload.value().has_value());

    EXPECT_EQ(postings.value()->advance(5).value(), NO_MORE_DOCS);
    EXPECT_EQ(postings.value()->cost().value(), 3);
}

TEST(SortingTermsTest, DropsPostingsOfDeletedDocuments) {
    // Old document 1 was deleted, so the sorted view only has two documents.
    auto terms = SortingTerms::create(std::make_unique<FakeTerms>(sample_terms()), IndexOptions::DocsAndFreqs,
                                     std::make_shared<IndexSortDocMapImpl>(std::vector<int32_t>{2, 0}, 3));
    ASSERT_TRUE(terms.has_value());
    auto tenum = terms.value()->iterator();
    ASSERT_TRUE(tenum.has_value());

    auto first = tenum.value()->next();
    ASSERT_TRUE(first.has_value());
    ASSERT_NE(first.value(), nullptr);

    auto postings = tenum.value()->postings_with_flags(nullptr, FREQS);
    ASSERT_TRUE(postings.has_value()) << (postings ? "" : postings.error().to_string());

    ASSERT_EQ(postings.value()->next_doc().value(), 0);
    ASSERT_EQ(postings.value()->next_doc().value(), 1);
    EXPECT_EQ(postings.value()->doc_id(), 1);
    EXPECT_EQ(postings.value()->next_doc().value(), NO_MORE_DOCS);
    EXPECT_EQ(postings.value()->cost().value(), 2);
}

TEST(SortingTermsTest, WrapsTheEnumOfAnIntersection) {
    auto terms = SortingTerms::create(std::make_unique<FakeTerms>(sample_terms()), IndexOptions::DocsAndFreqs,
                                     std::make_shared<IndexSortDocMapImpl>(std::vector<int32_t>{2, 1, 0}));
    ASSERT_TRUE(terms.has_value());

    auto automaton = Automata::make_empty();
    ASSERT_TRUE(automaton.has_value()) << (automaton ? "" : automaton.error().to_string());
    auto compiled = CompiledAutomaton::from_automaton(std::move(automaton.value()));
    ASSERT_TRUE(compiled.has_value()) << (compiled ? "" : compiled.error().to_string());

    auto tenum = terms.value()->intersect(compiled.value(), nullptr);
    ASSERT_TRUE(tenum.has_value()) << (tenum ? "" : tenum.error().to_string());
    EXPECT_EQ(drain(*tenum.value()), (std::vector<std::string>{"a", "b"}));

    ASSERT_TRUE(tenum.value()->seek_exact_with_ord(0).has_value());
    auto postings = tenum.value()->postings_with_flags(nullptr, FREQS);
    ASSERT_TRUE(postings.has_value());
    ASSERT_EQ(postings.value()->next_doc().value(), 0);
    EXPECT_EQ(postings.value()->freq().value(), 1);
}

TEST(SortingTermsTest, RejectsMissingArguments) {
    auto doc_map = std::make_shared<IndexSortDocMapImpl>(std::vector<int32_t>{0});

    auto no_terms = SortingTerms::create(nullptr, IndexOptions::DocsAndFreqs, doc_map);
    ASSERT_FALSE(no_terms.has_value());
    EXPECT_EQ(no_terms.error().kind(), ErrorKind::kIllegalArgument);

    auto no_map = SortingTerms::create(std::make_unique<FakeTerms>(sample_terms()), IndexOptions::DocsAndFreqs,
                                       nullptr);
    ASSERT_FALSE(no_map.has_value());
    EXPECT_EQ(no_map.error().kind(), ErrorKind::kIllegalArgument);

    auto no_enum = SortingTermsEnum::create(nullptr, IndexOptions::DocsAndFreqs, doc_map);
    ASSERT_FALSE(no_enum.has_value());
    EXPECT_EQ(no_enum.error().kind(), ErrorKind::kIllegalArgument);

    auto enum_no_map = SortingTermsEnum::create(std::make_unique<FakeTermsEnum>(sample_terms(), false),
                                                IndexOptions::DocsAndFreqs, nullptr);
    ASSERT_FALSE(enum_no_map.has_value());
    EXPECT_EQ(enum_no_map.error().kind(), ErrorKind::kIllegalArgument);

    auto no_postings = SortingDocsEnum::create(nullptr, *doc_map);
    ASSERT_FALSE(no_postings.has_value());
    EXPECT_EQ(no_postings.error().kind(), ErrorKind::kIllegalArgument);
}

TEST(SortingDocsEnumTest, ReplaysDocumentsInTheNewOrder) {
    auto fake = std::make_unique<FakePostingsEnum>(sample_terms()[0].postings);
    IndexSortDocMapImpl doc_map(std::vector<int32_t>{2, 1, 0});
    auto postings = SortingDocsEnum::create(std::move(fake), doc_map);
    ASSERT_TRUE(postings.has_value()) << (postings ? "" : postings.error().to_string());

    EXPECT_EQ(postings.value()->doc_id(), -1);
    EXPECT_EQ(postings.value()->next_doc().value(), 0);
    EXPECT_EQ(postings.value()->next_doc().value(), 1);
    EXPECT_EQ(postings.value()->advance(2).value(), 2);
    EXPECT_EQ(postings.value()->next_doc().value(), NO_MORE_DOCS);
    EXPECT_EQ(postings.value()->doc_id(), NO_MORE_DOCS);
    EXPECT_EQ(postings.value()->cost().value(), 3);
}
