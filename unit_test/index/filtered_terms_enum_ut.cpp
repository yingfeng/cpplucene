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
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

module lucene_core:ut.filtered_terms_enum;

import lucene_core;

using namespace lucene;

namespace {

/// A [`TermsEnum`] over a sorted vector of terms; enough to drive the filtered
/// enums without a real codec.
class VectorTermsEnum final : public TermsEnum {
public:
    explicit VectorTermsEnum(std::vector<BytesRef> terms) : terms_(std::move(terms)) {}

    [[nodiscard]] Result<const BytesRef *> next() override {
        if (!pos_.has_value()) {
            if (terms_.empty()) {
                return static_cast<const BytesRef *>(nullptr);
            }
            pos_ = 0;
            return &terms_[0];
        }
        if (pos_.value() + 1 >= terms_.size()) {
            pos_.reset();
            return static_cast<const BytesRef *>(nullptr);
        }
        pos_ = pos_.value() + 1;
        return &terms_[pos_.value()];
    }

    [[nodiscard]] Result<AttributeSource *> attributes() override {
        return err(LuceneError::unsupported_operation("VectorTermsEnum has no attributes"));
    }
    [[nodiscard]] Result<AttributeSource *> attributes_mut() override {
        return err(LuceneError::unsupported_operation("VectorTermsEnum has no attributes"));
    }

    [[nodiscard]] Result<bool> seek_exact(std::span<const uint8_t> term) override {
        auto status = seek_ceil(term);
        if (!status) {
            return std::unexpected(std::move(status.error()));
        }
        return status.value() == SeekStatus::Found;
    }

    [[nodiscard]] Result<bool> prepare_seek_exact(std::span<const uint8_t> /*term*/) override {
        return err(LuceneError::unsupported_operation("VectorTermsEnum::prepare_seek_exact"));
    }

    [[nodiscard]] Result<bool> get_prepare_seek_exact_status(std::span<const uint8_t> /*target*/) override {
        return err(LuceneError::unsupported_operation("VectorTermsEnum::get_prepare_seek_exact_status"));
    }

    [[nodiscard]] Result<SeekStatus> seek_ceil(std::span<const uint8_t> term) override {
        pos_.reset();
        for (size_t i = 0; i < terms_.size(); ++i) {
            const auto comparison = terms_[i].as_slice();
            if (std::ranges::lexicographical_compare(comparison, term)) {
                continue;
            }
            pos_ = i;
            return std::ranges::equal(comparison, term) ? SeekStatus::Found : SeekStatus::NotFound;
        }
        return SeekStatus::End;
    }

    [[nodiscard]] VoidResult seek_exact_with_ord(int64_t ord) override {
        if (ord < 0 || static_cast<size_t>(ord) >= terms_.size()) {
            return err(LuceneError::illegal_argument("ord out of range"));
        }
        pos_ = static_cast<size_t>(ord);
        return {};
    }

    [[nodiscard]] VoidResult seek_exact_with_state(std::span<const uint8_t> /*term*/,
                                                   const TermStateEnum & /*state*/) override {
        return err(LuceneError::unsupported_operation("VectorTermsEnum::seek_exact_with_state"));
    }

    [[nodiscard]] Result<const BytesRef *> term() const override {
        if (!pos_.has_value()) {
            return err(LuceneError::illegal_state("unpositioned"));
        }
        return &terms_[pos_.value()];
    }

    [[nodiscard]] Result<int64_t> ord() const override {
        if (!pos_.has_value()) {
            return err(LuceneError::illegal_state("unpositioned"));
        }
        return static_cast<int64_t>(pos_.value());
    }

    [[nodiscard]] Result<int32_t> doc_freq() override { return 1; }
    [[nodiscard]] Result<int64_t> total_term_freq() override { return 1; }

    [[nodiscard]] Result<std::unique_ptr<PostingsEnum>> postings_with_flags(std::unique_ptr<PostingsEnum> /*reuse*/,
                                                                           int32_t /*flags*/) override {
        return err(LuceneError::unsupported_operation("VectorTermsEnum has no postings"));
    }

    [[nodiscard]] Result<std::unique_ptr<ImpactsEnum>> impacts(int32_t /*flags*/) override {
        return err(LuceneError::unsupported_operation("VectorTermsEnum has no impacts"));
    }

    [[nodiscard]] Result<TermStateEnum> term_state() override {
        return err(LuceneError::unsupported_operation("VectorTermsEnum has no term state"));
    }

private:
    std::vector<BytesRef> terms_;
    std::optional<size_t> pos_{};
};

class VectorTerms final : public Terms {
public:
    explicit VectorTerms(std::vector<BytesRef> terms) : terms_(std::move(terms)) {}

    [[nodiscard]] Result<std::unique_ptr<TermsEnum>> iterator() const override {
        return std::unique_ptr<TermsEnum>(std::make_unique<VectorTermsEnum>(terms_));
    }
    [[nodiscard]] Result<int64_t> size() const override { return static_cast<int64_t>(terms_.size()); }
    [[nodiscard]] Result<int64_t> sum_total_term_freq() const override {
        return static_cast<int64_t>(terms_.size());
    }
    [[nodiscard]] Result<int64_t> sum_doc_freq() const override { return static_cast<int64_t>(terms_.size()); }
    [[nodiscard]] Result<int32_t> doc_count() const override { return static_cast<int32_t>(terms_.size()); }
    [[nodiscard]] bool has_freqs() const override { return false; }
    [[nodiscard]] bool has_offsets() const override { return false; }
    [[nodiscard]] bool has_positions() const override { return false; }
    [[nodiscard]] bool has_payloads() const override { return false; }

private:
    std::vector<BytesRef> terms_;
};

std::vector<BytesRef> terms_of(std::initializer_list<std::string_view> texts) {
    std::vector<BytesRef> terms;
    terms.reserve(texts.size());
    for (const auto text : texts) {
        terms.push_back(BytesRef::from_string(text));
    }
    std::ranges::sort(terms);
    return terms;
}

/// Drains an enum into a vector of strings.
std::vector<std::string> drain(TermsEnum &tenum) {
    std::vector<std::string> collected;
    while (true) {
        auto term = tenum.next();
        EXPECT_TRUE(term.has_value());
        if (term.value() == nullptr) {
            break;
        }
        auto text = term.value()->utf8_to_string();
        EXPECT_TRUE(text.has_value());
        collected.push_back(text.value());
    }
    return collected;
}

std::vector<std::string> to_strings(std::initializer_list<std::string_view> texts) {
    std::vector<std::string> result;
    result.reserve(texts.size());
    for (const auto text : texts) {
        result.push_back(std::string(text));
    }
    std::ranges::sort(result);
    return result;
}

} // namespace

TEST(FilteredTermsEnumTest, SingleTermsEnumYieldsOnlyItsTerm) {
    VectorTerms terms(terms_of({"bar", "foo", "zoo"}));

    auto compiled = CompiledAutomaton::create(Automata::make_string("foo").value(), true, true);
    ASSERT_TRUE(compiled.has_value());
    ASSERT_EQ(compiled->type(), AutomatonType::kSingle);

    auto tenum = terms.terms_enum_for(compiled.value());
    ASSERT_TRUE(tenum.has_value());
    EXPECT_EQ(drain(*tenum.value()), to_strings({"foo"}));
}

TEST(FilteredTermsEnumTest, SingleTermsEnumOfMissingTermYieldsNothing) {
    VectorTerms terms(terms_of({"bar", "zoo"}));
    auto tenum = SingleTermsEnum::create(terms.iterator().value(), BytesRef::from_string("foo"));
    EXPECT_EQ(drain(*tenum), (std::vector<std::string>{}));
}

TEST(FilteredTermsEnumTest, TermsEnumForNoneYieldsNothing) {
    VectorTerms terms(terms_of({"bar", "foo"}));
    auto compiled = CompiledAutomaton::from_automaton(Automata::make_empty().value());
    ASSERT_TRUE(compiled.has_value());
    ASSERT_EQ(compiled->type(), AutomatonType::kNone);

    auto tenum = terms.terms_enum_for(compiled.value());
    ASSERT_TRUE(tenum.has_value());
    EXPECT_EQ(drain(*tenum.value()), (std::vector<std::string>{}));
}

TEST(FilteredTermsEnumTest, TermsEnumForAllYieldsEveryTerm) {
    VectorTerms terms(terms_of({"bar", "foo", "zoo"}));
    auto compiled = CompiledAutomaton::from_automaton(Automata::make_any_string().value());
    ASSERT_TRUE(compiled.has_value());
    ASSERT_EQ(compiled->type(), AutomatonType::kAll);

    auto tenum = terms.terms_enum_for(compiled.value());
    ASSERT_TRUE(tenum.has_value());
    EXPECT_EQ(drain(*tenum.value()), to_strings({"bar", "foo", "zoo"}));
}

TEST(FilteredTermsEnumTest, AutomatonFiltersTerms) {
    VectorTerms terms(terms_of({"bar", "baz", "foo", "foobar", "quux"}));
    auto automaton = Automata::make_string_union(terms_of({"baz", "foo"}));
    ASSERT_TRUE(automaton.has_value());

    auto compiled = CompiledAutomaton::from_automaton(std::move(automaton.value()));
    ASSERT_TRUE(compiled.has_value());
    ASSERT_EQ(compiled->type(), AutomatonType::kNormal);

    auto tenum = terms.terms_enum_for(compiled.value());
    ASSERT_TRUE(tenum.has_value());
    EXPECT_EQ(drain(*tenum.value()), to_strings({"baz", "foo"}));
}

TEST(FilteredTermsEnumTest, AutomatonWithInfiniteLanguageUsesSeeking) {
    VectorTerms terms(terms_of({"b", "ba", "baa", "bar", "baz", "bb", "bzzz", "foo"}));
    auto automaton = Operations::concatenate(Automata::make_string("ba").value(),
                                             Automata::make_any_string().value());
    ASSERT_TRUE(automaton.has_value());

    // `finite` is false: the enum detects the loop and switches to linear mode.
    auto compiled = CompiledAutomaton::create(std::move(automaton.value()), false, false);
    ASSERT_TRUE(compiled.has_value());

    auto tenum = terms.terms_enum_for(compiled.value());
    ASSERT_TRUE(tenum.has_value());
    EXPECT_EQ(drain(*tenum.value()), to_strings({"ba", "baa", "bar", "baz"}));
}

TEST(FilteredTermsEnumTest, AutomatonWithCommonSuffix) {
    // "*.bar": the common suffix shortcut skips terms that cannot match.
    VectorTerms terms(terms_of({"a.bar", "a.baz", "b.bar", "bar", "x.y"}));
    auto automaton = Operations::concatenate(Automata::make_any_string().value(),
                                             Automata::make_string(".bar").value());
    ASSERT_TRUE(automaton.has_value());

    auto compiled = CompiledAutomaton::create(std::move(automaton.value()), false, false);
    ASSERT_TRUE(compiled.has_value());
    ASSERT_TRUE(compiled->common_suffix_ref().has_value());

    auto tenum = terms.terms_enum_for(compiled.value());
    ASSERT_TRUE(tenum.has_value());
    EXPECT_EQ(drain(*tenum.value()), to_strings({"a.bar", "b.bar"}));
}

TEST(FilteredTermsEnumTest, IntersectStartTermIsExclusive) {
    // Mirrors the reference implementation's `test_intersect`: the start term itself is skipped, even
    // when it is accepted by the automaton.
    VectorTerms terms(terms_of({"aaa", "bbb", "ccc"}));
    auto compiled = CompiledAutomaton::create(Automata::make_any_string().value(), false, false);
    ASSERT_TRUE(compiled.has_value());
    ASSERT_EQ(compiled->type(), AutomatonType::kNormal);

    {
        BytesRef start = BytesRef::from_string("aaa");
        auto tenum = terms.intersect(compiled.value(), &start);
        ASSERT_TRUE(tenum.has_value());
        EXPECT_EQ(drain(*tenum.value()), to_strings({"bbb", "ccc"}));
    }
    {
        BytesRef start = BytesRef::from_string("aad");
        auto tenum = terms.intersect(compiled.value(), &start);
        ASSERT_TRUE(tenum.has_value());
        EXPECT_EQ(drain(*tenum.value()), to_strings({"bbb", "ccc"}));
    }
    {
        // Nothing sorts after the last term.
        BytesRef start = BytesRef::from_string("ccc");
        auto tenum = terms.intersect(compiled.value(), &start);
        ASSERT_TRUE(tenum.has_value());
        EXPECT_EQ(drain(*tenum.value()), (std::vector<std::string>{}));
    }
    {
        auto tenum = terms.intersect(compiled.value(), nullptr);
        ASSERT_TRUE(tenum.has_value());
        EXPECT_EQ(drain(*tenum.value()), to_strings({"aaa", "bbb", "ccc"}));
    }
}

TEST(FilteredTermsEnumTest, FilteredEnumRejectsSeeking) {
    VectorTerms terms(terms_of({"bar", "foo"}));
    auto automaton = Automata::make_string_union(terms_of({"bar", "foo"}));
    ASSERT_TRUE(automaton.has_value());
    auto compiled = CompiledAutomaton::create(std::move(automaton.value()), false, false);
    ASSERT_TRUE(compiled.has_value());
    ASSERT_EQ(compiled->type(), AutomatonType::kNormal);

    auto tenum = terms.intersect(compiled.value(), nullptr);
    ASSERT_TRUE(tenum.has_value());

    BytesRef foo = BytesRef::from_string("foo");
    auto seek = tenum.value()->seek_exact(foo.as_slice());
    ASSERT_FALSE(seek.has_value());
    EXPECT_EQ(seek.error().kind(), ErrorKind::kUnsupportedOperation);
}

TEST(FilteredTermsEnumTest, UnfilteredEnumDelegatesEverything) {
    auto inner = std::make_unique<VectorTermsEnum>(terms_of({"bar", "foo"}));
    auto tenum = FilteredTermsEnum::unfiltered(std::move(inner));
    EXPECT_FALSE(tenum->is_filtered());
    EXPECT_EQ(drain(*tenum), to_strings({"bar", "foo"}));
}

TEST(FilteredTermsEnumTest, AutomatonRequiresNormalAutomaton) {
    VectorTerms terms(terms_of({"bar"}));
    auto compiled = CompiledAutomaton::from_automaton(Automata::make_empty().value());
    ASSERT_TRUE(compiled.has_value());

    auto tenum = AutomatonTermsEnum::create(terms.iterator().value(), compiled.value(), nullptr);
    ASSERT_FALSE(tenum.has_value());
    EXPECT_EQ(tenum.error().kind(), ErrorKind::kIllegalArgument);
}
