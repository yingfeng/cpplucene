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

export module lucene_core:term_states;

import :types;
import std;

import :block_term_state;
import :error;
import :identity;
import :index_reader;
import :index_searcher;
import :leaf_reader_context;
import :term;
import :terms_enum;

namespace lucene {

/// A term lookup for one leaf, started by [`TermStates::get`] and completed by
/// [`TermStates::resolve`].
///
/// Port of `PrepareState` (`core/index/term_states.rs`): either the state is
/// already at hand (`Ready`), or the term dictionary is mid-seek (`Pending`).
export class PrepareState {
public:
    /// The state of leaf `ord` is already registered.
    [[nodiscard]] static PrepareState ready(int32_t ord) {
        PrepareState state;
        state.ord_ = ord;
        return state;
    }

    /// The seek of `term` in leaf `ord` is pending on `terms_enum`.
    [[nodiscard]] static PrepareState pending(Term term, int32_t ord, std::unique_ptr<TermsEnum> terms_enum) {
        PrepareState state;
        state.ord_ = ord;
        state.term_ = std::move(term);
        state.terms_enum_ = std::move(terms_enum);
        return state;
    }

    [[nodiscard]] bool is_ready() const noexcept { return terms_enum_ == nullptr; }
    [[nodiscard]] int32_t ord() const noexcept { return ord_; }
    [[nodiscard]] const Term &term() const noexcept { return term_.value(); }
    [[nodiscard]] TermsEnum *terms_enum() const noexcept { return terms_enum_.get(); }

private:
    PrepareState() = default;

    int32_t ord_{0};
    std::optional<Term> term_{};
    std::unique_ptr<TermsEnum> terms_enum_{};
};

/// A `TermState` view over the leaves of one reader.
///
/// Port of `TermStates` (`core/index/term_states.rs`).  A `Weight` builds one of
/// these when it is created and hands it to every leaf; each leaf then either
/// already has its state registered (statistics were requested) or resolves it
/// lazily from the term dictionary.
///
/// **Deviations** (C++ mechanics only):
///  * the per-leaf `OnceLock<Arc<TermStateEnum>>` becomes
///    `std::optional<TermStateEnum>` plus a "lookup finished" flag: rlucene marks
///    a leaf whose term is absent with an `EmptyTermState` to avoid redoing the
///    I/O, and this port keeps that memoization without inventing a state variant;
///  * slot publication is guarded by one mutex instead of per-slot atomics
///    (`OnceLock`); the observable behavior is the same ("publish only after a
///    successful lookup");
///  * `resolve` hands out a borrowed pointer into this object instead of an
///    `Arc<TermStateEnum>`.
export class TermStates {
public:
    /// An empty view over `reader`'s leaves, remembering `term` for lazy
    /// lookups.  `reader` must be a top-level reader.
    [[nodiscard]] static Result<TermStates> with_term(std::optional<Term> term, const IndexReader &reader);

    /// [`with_term`] with no term, i.e. every state is registered explicitly.
    [[nodiscard]] static Result<TermStates> create(const IndexReader &reader);

    /// A view with a single state already registered for leaf `ord`.
    [[nodiscard]] static Result<TermStates> with_state_and_stats(const IndexReader &reader, TermStateEnum state,
                                                                int32_t ord, int32_t doc_freq,
                                                                int64_t total_term_freq);

    /// Whether this view was built for the reader `meta` describes.
    [[nodiscard]] bool was_built_for(const TopParentMeta &meta) const { return was_built_for_id(meta.id); }

    /// Whether this view was built for the reader with identity `id`.
    [[nodiscard]] bool was_built_for_id(const Identity &id) const { return top_reader_context_identity_ == id; }

    /// Drops every registered state and the accumulated statistics.
    void clear();

    /// Registers `state` for leaf `ord` and accumulates its statistics.
    void register_with_stats(TermStateEnum state, int32_t ord, int32_t doc_freq, int64_t total_term_freq);

    /// Registers `state` for leaf `ord` without touching the statistics.
    void register_state(TermStateEnum state, int32_t ord);

    /// Adds to the accumulated term statistics.
    void accumulate_statistics(int32_t doc_freq, int64_t total_term_freq);

    /// Prepares the state of the leaf `ctx` describes.
    ///
    /// `std::nullopt` means the term is known to be absent from that leaf.
    /// A returned [`PrepareState`] is either already resolved or pending on an
    /// I/O; either way it must be passed to [`resolve`].
    [[nodiscard]] Result<std::optional<PrepareState>> get(const LeafReaderContext &ctx);

    /// Completes the prepared lookup and returns the state of that leaf, or
    /// `std::nullopt` when the term does not occur there.
    [[nodiscard]] Result<std::optional<const TermStateEnum *>> resolve(PrepareState &state);

    /// The accumulated document frequency; only valid when no term was given to
    /// [`with_term`] (i.e. statistics were requested when building).
    [[nodiscard]] Result<int32_t> doc_freq() const;

    /// The accumulated total term frequency; same restriction as [`doc_freq`].
    [[nodiscard]] Result<int64_t> total_term_freq() const;

    [[nodiscard]] size_t size() const noexcept { return states_.size(); }

    [[nodiscard]] std::string to_string() const;

    [[nodiscard]] bool operator==(const TermStates &other) const { return identity_ == other.identity_; }

private:
    TermStates(Identity identity, Identity top_reader_context_identity, std::optional<Term> term, size_t num_leaves);

    Identity identity_{};
    Identity top_reader_context_identity_{};
    std::optional<Term> term_{};
    /// Registered state per leaf ord; empty when the term is absent or not
    /// resolved yet.
    std::vector<std::optional<TermStateEnum>> states_{};
    /// `1` once the lookup for that leaf finished (found or not).
    std::vector<uint8_t> resolved_{};
    /// Heap allocated so that `TermStates` stays movable (it is returned
    /// through `Result`).
    mutable std::unique_ptr<std::mutex> mutex_{};
    int32_t doc_freq_{0};
    int64_t total_term_freq_{0};
};

/// Builds the [`TermStates`] of `term` over `searcher`'s reader.
///
/// Port of `term_states::build` (`core/index/term_states.rs`).  When
/// `needs_stats` holds, every leaf is asked for the term eagerly so that the
/// document frequency and total term frequency can be accumulated; otherwise the
/// states are resolved lazily per leaf.
[[nodiscard]] Result<TermStates> build_term_states(const IndexSearcher &searcher, Term term, bool needs_stats);

} // namespace lucene
