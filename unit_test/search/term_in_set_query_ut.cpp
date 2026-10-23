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
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

module lucene_core:ut.term_in_set_query;

import lucene_core;

using namespace lucene;

namespace {

constexpr std::string_view kField = "body";

/// Builds term data from the given term texts of `field`.
std::shared_ptr<const PrefixCodedTerms> make_term_data(std::string_view field,
                                                       const std::vector<std::string> &texts) {
    PrefixCodedTerms::Builder builder;
    for (const std::string &text : texts) {
        EXPECT_TRUE(builder.add(field, BytesRef::from_string(text)).has_value());
    }
    return std::make_shared<const PrefixCodedTerms>(builder.build());
}

std::vector<std::string> drain(const PrefixCodedTerms &terms) {
    std::vector<std::string> texts;
    auto iterator = terms.iterator();
    while (true) {
        auto next = iterator.next();
        EXPECT_TRUE(next.has_value()) << (next.has_value() ? "" : next.error().to_string());
        if (!next.has_value() || next.value() == nullptr) {
            break;
        }
        texts.push_back(next.value()->utf8_to_string().value());
    }
    return texts;
}

/// [`TermsEnum`] over a sorted list of term texts.
class DictionaryTermsEnum final : public TermsEnum {
public:
    explicit DictionaryTermsEnum(std::shared_ptr<const std::vector<std::string>> terms)
            : terms_(std::move(terms)) {}

    [[nodiscard]] Result<const BytesRef *> next() override {
        if (!position_.has_value()) {
            if (terms_->empty()) {
                return static_cast<const BytesRef *>(nullptr);
            }
            position_ = 0;
            return &buffer(0);
        }
        if (position_.value() + 1 >= terms_->size()) {
            position_.reset();
            return static_cast<const BytesRef *>(nullptr);
        }
        position_ = position_.value() + 1;
        return &buffer(position_.value());
    }

    [[nodiscard]] Result<const BytesRef *> term() const override {
        if (!position_.has_value()) {
            return err(LuceneError::illegal_state("unpositioned"));
        }
        return &buffer(position_.value());
    }

    [[nodiscard]] Result<int64_t> ord() const override { return static_cast<int64_t>(position_.value_or(0)); }

    [[nodiscard]] Result<int32_t> doc_freq() override { return 1; }
    [[nodiscard]] Result<int64_t> total_term_freq() override { return 1; }

    [[nodiscard]] Result<std::unique_ptr<PostingsEnum>> postings_with_flags(std::unique_ptr<PostingsEnum> /*reuse*/,
                                                                           int32_t /*flags*/) override {
        return err(LuceneError::unsupported_operation("the dictionary has no postings"));
    }

    [[nodiscard]] Result<std::unique_ptr<ImpactsEnum>> impacts(int32_t /*flags*/) override {
        return err(LuceneError::unsupported_operation("no impacts"));
    }

    [[nodiscard]] Result<AttributeSource *> attributes() override {
        return err(LuceneError::unsupported_operation("no attributes"));
    }
    [[nodiscard]] Result<AttributeSource *> attributes_mut() override {
        return err(LuceneError::unsupported_operation("no attributes"));
    }

    [[nodiscard]] Result<SeekStatus> seek_ceil(std::span<const uint8_t> target) override {
        const std::string text(reinterpret_cast<const char *>(target.data()), target.size());
        const auto found = std::lower_bound(terms_->begin(), terms_->end(), text);
        if (found == terms_->end()) {
            position_.reset();
            return SeekStatus::End;
        }
        position_ = static_cast<size_t>(std::distance(terms_->begin(), found));
        return *found == text ? SeekStatus::Found : SeekStatus::NotFound;
    }

    [[nodiscard]] Result<bool> seek_exact(std::span<const uint8_t> target) override {
        auto status = seek_ceil(target);
        if (!status.has_value()) {
            return std::unexpected(std::move(status.error()));
        }
        return status.value() == SeekStatus::Found;
    }

    [[nodiscard]] Result<bool> prepare_seek_exact(std::span<const uint8_t> target) override {
        return seek_exact(target);
    }

    [[nodiscard]] Result<bool> get_prepare_seek_exact_status(std::span<const uint8_t> target) override {
        return seek_exact(target);
    }

    [[nodiscard]] VoidResult seek_exact_with_ord(int64_t ord) override {
        if (ord < 0 || static_cast<size_t>(ord) >= terms_->size()) {
            return err(LuceneError::illegal_argument("ord out of range"));
        }
        position_ = static_cast<size_t>(ord);
        return {};
    }

    [[nodiscard]] VoidResult seek_exact_with_state(std::span<const uint8_t> /*term*/,
                                                   const TermStateEnum &state) override {
        auto ord = state.ord();
        if (!ord.has_value()) {
            return std::unexpected(std::move(ord.error()));
        }
        return seek_exact_with_ord(ord.value());
    }

    [[nodiscard]] Result<TermStateEnum> term_state() override { return TermStateEnum(OrdTermState{ord().value()}); }

private:
    [[nodiscard]] const BytesRef &buffer(size_t index) const {
        cache_ = BytesRef::from_string((*terms_)[index]);
        return cache_;
    }

    std::shared_ptr<const std::vector<std::string>> terms_;
    std::optional<size_t> position_{};
    mutable BytesRef cache_{};
};

/// [`Terms`] over a sorted dictionary of term texts.
class DictionaryTerms final : public Terms {
public:
    explicit DictionaryTerms(std::vector<std::string> texts)
            : terms_(std::make_shared<const std::vector<std::string>>(std::move(texts))) {}

    [[nodiscard]] Result<std::unique_ptr<TermsEnum>> iterator() const override {
        return std::unique_ptr<TermsEnum>(new DictionaryTermsEnum(terms_));
    }

    [[nodiscard]] Result<int64_t> size() const override { return static_cast<int64_t>(terms_->size()); }
    [[nodiscard]] Result<int64_t> sum_total_term_freq() const override {
        return static_cast<int64_t>(terms_->size());
    }
    [[nodiscard]] Result<int64_t> sum_doc_freq() const override { return static_cast<int64_t>(terms_->size()); }
    [[nodiscard]] Result<int32_t> doc_count() const override { return static_cast<int32_t>(terms_->size()); }
    [[nodiscard]] bool has_freqs() const override { return false; }
    [[nodiscard]] bool has_offsets() const override { return false; }
    [[nodiscard]] bool has_positions() const override { return false; }
    [[nodiscard]] bool has_payloads() const override { return false; }

private:
    std::shared_ptr<const std::vector<std::string>> terms_;
};

/// An [`IndexReader`] without leaves, enough for rewrites that never touch a
/// reader.
class EmptyIndexReader final : public IndexReader {
public:
    [[nodiscard]] Result<int32_t> max_doc() const override { return 0; }
    [[nodiscard]] Result<int32_t> num_docs() const override { return 0; }
    [[nodiscard]] Result<std::unique_ptr<StoredFields>> stored_fields() override { return nullptr; }
    [[nodiscard]] Result<std::unique_ptr<TermVectors>> term_vectors() override { return nullptr; }
    [[nodiscard]] Result<int32_t> doc_freq(const Term & /*term*/) override { return 0; }
    [[nodiscard]] Result<int64_t> total_term_freq(const Term & /*term*/) override { return 0; }
    [[nodiscard]] Result<int64_t> sum_doc_freq(std::string_view /*field*/) override { return 0; }
    [[nodiscard]] Result<int32_t> doc_count(std::string_view /*field*/) override { return 0; }
    [[nodiscard]] Result<int64_t> sum_total_term_freq(std::string_view /*field*/) override { return 0; }
    [[nodiscard]] Result<std::span<const LeafReaderContext>> leaves() const override {
        return std::span<const LeafReaderContext>(empty_);
    }
    [[nodiscard]] const Identity &identity() const override { return identity_; }

private:
    Identity identity_{};
    std::vector<LeafReaderContext> empty_{};
};

/// The terms of `query` that the dictionary contains, in dictionary order.
std::vector<std::string> intersect(const MultiTermQuery &query, DictionaryTerms &terms) {
    std::vector<std::string> texts;
    auto iterator = query.terms_enum_for(terms);
    EXPECT_TRUE(iterator.has_value()) << (iterator.has_value() ? "" : iterator.error().to_string());
    if (!iterator.has_value()) {
        return texts;
    }
    while (true) {
        auto next = iterator.value()->next();
        EXPECT_TRUE(next.has_value()) << (next.has_value() ? "" : next.error().to_string());
        if (!next.has_value() || next.value() == nullptr) {
            break;
        }
        texts.push_back(next.value()->utf8_to_string().value());
    }
    return texts;
}

} // namespace

// ------------------------------------------------------- PrefixCodedTerms

TEST(PrefixCodedTermsTest, EmptyTermsHaveNoContent) {
    PrefixCodedTerms terms;
    EXPECT_EQ(terms.size(), 0);
    EXPECT_TRUE(terms.empty());
    EXPECT_TRUE(drain(terms).empty());
    const PrefixCodedTerms built = PrefixCodedTerms::Builder{}.build();
    EXPECT_TRUE(built.empty());
    EXPECT_EQ(built, terms);
}

TEST(PrefixCodedTermsTest, RoundTripsTheTerms) {
    const auto terms = make_term_data(kField, {"apple", "applesauce", "banana", "banana split"});
    EXPECT_EQ(terms->size(), 4);
    EXPECT_FALSE(terms->empty());
    EXPECT_EQ(drain(*terms), (std::vector<std::string>{"apple", "applesauce", "banana", "banana split"}));
}

TEST(PrefixCodedTermsTest, IteratorReportsTheField) {
    PrefixCodedTerms::Builder builder;
    EXPECT_TRUE(builder.add("body", BytesRef::from_string("a")).has_value());
    EXPECT_TRUE(builder.add("body", BytesRef::from_string("b")).has_value());
    EXPECT_TRUE(builder.add("title", BytesRef::from_string("c")).has_value());
    const PrefixCodedTerms terms = builder.build();

    auto iterator = terms.iterator();
    std::vector<std::pair<std::string, std::string>> seen;
    while (true) {
        auto next = iterator.next();
        ASSERT_TRUE(next.has_value());
        if (next.value() == nullptr) {
            break;
        }
        seen.emplace_back(iterator.field(), next.value()->utf8_to_string().value());
    }
    EXPECT_EQ(seen, (std::vector<std::pair<std::string, std::string>>{
                            {"body", "a"}, {"body", "b"}, {"title", "c"}}));
}

TEST(PrefixCodedTermsTest, RejectsTermsOutOfOrder) {
    PrefixCodedTerms::Builder builder;
    EXPECT_TRUE(builder.add("body", BytesRef::from_string("b")).has_value());
    auto duplicate = builder.add("body", BytesRef::from_string("b"));
    EXPECT_FALSE(duplicate.has_value());
    EXPECT_EQ(duplicate.error().kind(), ErrorKind::kIllegalArgument);
    auto backwards = builder.add("body", BytesRef::from_string("a"));
    EXPECT_FALSE(backwards.has_value());
    auto previous_field = builder.add("alpha", BytesRef::from_string("c"));
    EXPECT_FALSE(previous_field.has_value());
}

TEST(PrefixCodedTermsTest, EqualityAndHashFollowTheTerms) {
    const auto first = make_term_data(kField, {"a", "b"});
    const auto second = make_term_data(kField, {"a", "b"});
    const auto different = make_term_data(kField, {"a", "c"});
    EXPECT_EQ(*first, *second);
    EXPECT_EQ(first->hash(), second->hash());
    EXPECT_FALSE(*first == *different);

    const PrefixCodedTerms empty;
    EXPECT_NE(*first, empty);
}

TEST(PrefixCodedTermsTest, DelGenIsMetadata) {
    PrefixCodedTerms terms = *make_term_data(kField, {"a"});
    EXPECT_EQ(terms.del_gen(), 0);
    terms.set_del_gen(7);
    EXPECT_EQ(terms.del_gen(), 7);
    PrefixCodedTerms other = *make_term_data(kField, {"a"});
    other.set_del_gen(9);
    EXPECT_EQ(terms, other);
}

TEST(PrefixCodedTermsTest, CompressesSharedPrefixes) {
    PrefixCodedTerms::Builder builder;
    EXPECT_TRUE(builder.add(kField, BytesRef::from_string("abcdefgh")).has_value());
    EXPECT_TRUE(builder.add(kField, BytesRef::from_string("abcdefghi")).has_value());
    const PrefixCodedTerms terms = builder.build();
    // The second term shares eight bytes with the first, so it only stores the
    // one byte that differs.
    auto iterator = terms.iterator();
    ASSERT_TRUE(iterator.next().has_value());
    ASSERT_TRUE(iterator.next().has_value());
    auto third = iterator.next();
    ASSERT_TRUE(third.has_value());
    EXPECT_EQ(third.value(), nullptr);
}

// -------------------------------------------------------- TermInSetQuery

TEST(TermInSetQueryTest, EnumeratesOnlyTheTermsTheDictionaryHas) {
    auto query = TermInSetQuery::create(std::string(kField),
                                        make_term_data(kField, {"apple", "apricot", "avocado"}));
    ASSERT_TRUE(query.has_value()) << query.error().to_string();
    EXPECT_EQ(query.value()->terms_count(), 3);

    // "apricot" is missing from the dictionary and "avocado" is only there.
    DictionaryTerms terms({"apple", "avocado", "banana"});
    EXPECT_EQ(intersect(*query.value(), terms), (std::vector<std::string>{"apple", "avocado"}));

    DictionaryTerms empty({});
    EXPECT_TRUE(intersect(*query.value(), empty).empty());
}

TEST(TermInSetQueryTest, EnumeratesNothingWhenNoTermMatches) {
    auto query = TermInSetQuery::create(std::string(kField), make_term_data(kField, {"zzz"}));
    ASSERT_TRUE(query.has_value());
    DictionaryTerms terms({"apple", "banana"});
    EXPECT_TRUE(intersect(*query.value(), terms).empty());
}

TEST(TermInSetQueryTest, DefaultRewriteIsTheConstantScoreWrapper) {
    auto query = TermInSetQuery::create(std::string(kField), make_term_data(kField, {"a", "b"}));
    ASSERT_TRUE(query.has_value());
    EXPECT_EQ(query.value()->rewrite_method().kind(), MultiTermRewriteMethod::Kind::kConstantScoreBlended);

    // Rewriting does not touch the reader, so a searcher over an empty reader is
    // enough.
    IndexSearcher searcher(std::make_shared<EmptyIndexReader>());
    auto rewritten = query.value()->rewrite(searcher);
    ASSERT_TRUE(rewritten.has_value()) << rewritten.error().to_string();
    ASSERT_NE(rewritten.value(), nullptr);
    const auto *wrapper = dynamic_cast<const MultiTermQueryConstantScoreWrapper *>(rewritten.value().get());
    ASSERT_NE(wrapper, nullptr);
    EXPECT_EQ(wrapper->query()->field(), kField);
}

TEST(TermInSetQueryTest, TextFormListsTheTerms) {
    auto query = TermInSetQuery::create(std::string(kField), make_term_data(kField, {"a", "b"}));
    ASSERT_TRUE(query.has_value());
    auto text = query.value()->to_string("");
    ASSERT_TRUE(text.has_value()) << text.error().to_string();
    EXPECT_EQ(text.value(), "body:(a b)");
}

TEST(TermInSetQueryTest, EqualityNeedsTheSameTerms) {
    auto first = TermInSetQuery::create(std::string(kField), make_term_data(kField, {"a", "b"}));
    auto second = TermInSetQuery::create(std::string(kField), make_term_data(kField, {"a", "b"}));
    auto other_terms = TermInSetQuery::create(std::string(kField), make_term_data(kField, {"a", "c"}));
    auto other_field = TermInSetQuery::create("title", make_term_data(kField, {"a", "b"}));
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    ASSERT_TRUE(other_terms.has_value());
    ASSERT_TRUE(other_field.has_value());
    EXPECT_TRUE(first.value()->equals(*second.value()));
    EXPECT_EQ(first.value()->hash(), second.value()->hash());
    EXPECT_FALSE(first.value()->equals(*other_terms.value()));
    EXPECT_FALSE(first.value()->equals(*other_field.value()));
}

TEST(TermInSetQueryTest, NeedsTermData) {
    auto query = TermInSetQuery::create(std::string(kField), nullptr);
    EXPECT_FALSE(query.has_value());
    EXPECT_EQ(query.error().kind(), ErrorKind::kIllegalArgument);
}
