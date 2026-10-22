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

export module lucene_core:block_term_state;

import :types;
import std;

import :error;
import :term_state;

namespace lucene {

/// Holds all state required for a `PostingsReaderBase` to produce a
/// `PostingsEnum` without re-seeking the terms dict.
///
/// Port of `BlockTermState` (`core/codecs/block_term_state.rs`).
export class BlockTermState : public TermState {
public:
    /// How many docs have this term.
    int32_t doc_freq{0};
    /// Total number of occurrences of this term.
    int64_t total_term_freq{0};
    /// The term's ordinal in the current block.
    int32_t term_block_ord{0};
    /// File pointer into the terms dict primary file (`_X.tim`) that holds this
    /// term.
    int64_t block_file_pointer{0};

    /// The term's ordinal in the full list of sorted terms (rlucene's private
    /// `ord: OrdTermState`).
    [[nodiscard]] int64_t ord() const noexcept { return ord_.ord; }
    void set_ord(int64_t ord) noexcept { ord_.ord = ord; }

    [[nodiscard]] std::unique_ptr<TermState> clone() const override {
        return std::make_unique<BlockTermState>(*this);
    }

    [[nodiscard]] VoidResult copy_from(const TermState &other) override {
        const auto *state = dynamic_cast<const BlockTermState *>(&other);
        if (state == nullptr) {
            return err(LuceneError::illegal_argument("BlockTermState::copy_from requires another BlockTermState"));
        }
        doc_freq = state->doc_freq;
        total_term_freq = state->total_term_freq;
        term_block_ord = state->term_block_ord;
        block_file_pointer = state->block_file_pointer;
        ord_ = state->ord_;
        return {};
    }

    [[nodiscard]] std::string to_string() const override {
        return std::format("{} docFreq={} totalTermFreq={} termBlockOrd={} blockFP={}", ord_.to_string(), doc_freq,
                           total_term_freq, term_block_ord, block_file_pointer);
    }

private:
    OrdTermState ord_{};
};

/// Base block term state plus the file pointers the Lucene101 postings reader
/// needs.  Port of `IntBlockTermState`
/// (`core/codecs/lucene101/lucene101_postings_format.rs`).
///
/// **Deviation:** rlucene declares this type inside the Lucene101 postings
/// format, which makes `block_term_state` and the format mutually dependent.  A
/// C++20 module cycle cannot be expressed, so the type lives here with the rest
/// of the term states.
///
/// Like rlucene this keeps the shared statistics in a [`BlockTermState`]
/// member rather than inheriting from it, so the two variants stay distinct.
export class IntBlockTermState : public TermState {
public:
    /// File pointer to the start of the doc ids enumeration in the `.doc` file.
    int64_t doc_start_fp{0};
    /// File pointer to the start of the positions enumeration in the `.pos` file.
    int64_t pos_start_fp{0};
    /// File pointer to the start of the payloads enumeration in the `.pay` file.
    int64_t pay_start_fp{0};
    /// File offset of the last position block, or `-1` when the term has at most
    /// one block of positions.
    int64_t last_pos_block_offset{-1};
    /// Doc id of the single-document posting, or `-1` when the term is not
    /// "pulsed".
    int32_t singleton_doc_id{-1};
    /// Base state.
    BlockTermState base{};

    [[nodiscard]] std::unique_ptr<TermState> clone() const override {
        return std::make_unique<IntBlockTermState>(*this);
    }

    [[nodiscard]] VoidResult copy_from(const TermState &other) override {
        const auto *state = dynamic_cast<const IntBlockTermState *>(&other);
        if (state == nullptr) {
            return err(LuceneError::illegal_argument("IntBlockTermState::copy_from requires another IntBlockTermState"));
        }
        doc_start_fp = state->doc_start_fp;
        pos_start_fp = state->pos_start_fp;
        pay_start_fp = state->pay_start_fp;
        last_pos_block_offset = state->last_pos_block_offset;
        singleton_doc_id = state->singleton_doc_id;
        return base.copy_from(state->base);
    }

    [[nodiscard]] std::string to_string() const override {
        return std::format("{} docStartFP={} posStartFP={} payStartFP={} lastPosBlockOffset={} singletonDocID={}",
                           base.to_string(), doc_start_fp, pos_start_fp, pay_start_fp, last_pos_block_offset,
                           singleton_doc_id);
    }
};

/// The term state variants the codecs exchange.  Port of `TermStateEnum`
/// (`core/codecs/block_term_state.rs`).
///
/// **Deviation:** the `Empty` (`EmptyTermState`) and `BaseTermsEnum`
/// (`BaseTermsEnumTermStateImpl`) variants of rlucene are not ported yet,
/// because the term dictionaries that own them do not exist in this port.
export class TermStateEnum {
public:
    using Variant = std::variant<BlockTermState, IntBlockTermState, OrdTermState>;

    template <class T>
        requires std::constructible_from<Variant, T>
    explicit TermStateEnum(T state) : value_(std::move(state)) {}

    [[nodiscard]] const Variant &value() const noexcept { return value_; }
    [[nodiscard]] Variant &value_mut() noexcept { return value_; }

    /// The wrapped [`BlockTermState`], also for the `Int` variant (which embeds
    /// it as [`IntBlockTermState::base`]).
    [[nodiscard]] Result<BlockTermState *> get_block_term_state_mut() {
        if (auto *state = std::get_if<BlockTermState>(&value_)) {
            return state;
        }
        if (auto *state = std::get_if<IntBlockTermState>(&value_)) {
            return &state->base;
        }
        return err(LuceneError::unsupported_operation("term state has no block term state"));
    }

    [[nodiscard]] Result<const BlockTermState *> get_block_term_state() const {
        if (const auto *state = std::get_if<BlockTermState>(&value_)) {
            return state;
        }
        if (const auto *state = std::get_if<IntBlockTermState>(&value_)) {
            return &state->base;
        }
        return err(LuceneError::unsupported_operation("term state has no block term state"));
    }

    [[nodiscard]] Result<int64_t> ord() const {
        if (const auto *state = std::get_if<BlockTermState>(&value_)) {
            return state->ord();
        }
        if (const auto *state = std::get_if<IntBlockTermState>(&value_)) {
            return state->base.ord();
        }
        if (const auto *state = std::get_if<OrdTermState>(&value_)) {
            return state->ord;
        }
        return err(LuceneError::unsupported_operation("term state has no ordinal"));
    }

    [[nodiscard]] VoidResult set_ord(int64_t ord) {
        if (auto *state = std::get_if<BlockTermState>(&value_)) {
            state->set_ord(ord);
        } else if (auto *state = std::get_if<IntBlockTermState>(&value_)) {
            state->base.set_ord(ord);
        } else if (auto *state = std::get_if<OrdTermState>(&value_)) {
            state->ord = ord;
        } else {
            return err(LuceneError::unsupported_operation("term state has no ordinal"));
        }
        return {};
    }

    [[nodiscard]] std::string to_string() const {
        return std::visit([](const auto &state) { return state.to_string(); }, value_);
    }

    /// Copies `other` into this state; the variants must match, like the `match`
    /// in rlucene.
    [[nodiscard]] VoidResult copy_from(const TermStateEnum &other) {
        if (value_.index() != other.value_.index()) {
            return err(LuceneError::illegal_state("TermState variants must match when copying"));
        }
        return std::visit(
                [&other]<class T>(T &state) -> VoidResult { return state.copy_from(std::get<T>(other.value_)); },
                value_);
    }

private:
    Variant value_;
};

} // namespace lucene
