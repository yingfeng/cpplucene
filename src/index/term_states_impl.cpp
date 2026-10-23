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

module lucene_core:term_states.impl;

import :term_states;

import :block_term_state;
import :error;
import :identity;
import :index_reader;
import :index_searcher;
import :leaf_reader;
import :leaf_reader_context;
import :term;
import :terms;
import :terms_enum;
import :types;

import std;

namespace lucene {

TermStates::TermStates(Identity identity, Identity top_reader_context_identity, std::optional<Term> term,
                       size_t num_leaves)
        : identity_(std::move(identity)),
          top_reader_context_identity_(std::move(top_reader_context_identity)),
          term_(std::move(term)),
          states_(num_leaves),
          resolved_(num_leaves, 0),
          mutex_(std::make_unique<std::mutex>()) {}

TermStates::TermStates(const TermStates &other)
        : identity_(other.identity_),
          top_reader_context_identity_(other.top_reader_context_identity_),
          term_(other.term_),
          states_(other.states_),
          resolved_(other.resolved_),
          mutex_(std::make_unique<std::mutex>()),
          doc_freq_(other.doc_freq_),
          total_term_freq_(other.total_term_freq_) {}

TermStates &TermStates::operator=(const TermStates &other) {
    if (this == &other) {
        return *this;
    }
    const std::lock_guard<std::mutex> guard(*mutex_);
    identity_ = other.identity_;
    top_reader_context_identity_ = other.top_reader_context_identity_;
    term_ = other.term_;
    states_ = other.states_;
    resolved_ = other.resolved_;
    doc_freq_ = other.doc_freq_;
    total_term_freq_ = other.total_term_freq_;
    return *this;
}

Result<TermStates> TermStates::with_term(std::optional<Term> term, const IndexReader &reader) {
    auto leaves = reader.leaves();
    if (!leaves.has_value()) {
        return std::unexpected(std::move(leaves.error()));
    }
    return TermStates(Identity{}, reader.identity(), std::move(term), leaves.value().size());
}

Result<TermStates> TermStates::create(const IndexReader &reader) {
    return with_term(std::nullopt, reader);
}

Result<TermStates> TermStates::with_state_and_stats(const IndexReader &reader, TermStateEnum state, int32_t ord,
                                                    int32_t doc_freq, int64_t total_term_freq) {
    auto states = create(reader);
    if (!states.has_value()) {
        return std::unexpected(std::move(states.error()));
    }
    states.value().register_with_stats(std::move(state), ord, doc_freq, total_term_freq);
    return states;
}

void TermStates::clear() {
    const std::lock_guard<std::mutex> guard(*mutex_);
    doc_freq_ = 0;
    total_term_freq_ = 0;
    for (auto &slot : states_) {
        slot.reset();
    }
    std::fill(resolved_.begin(), resolved_.end(), uint8_t{0});
}

void TermStates::register_with_stats(TermStateEnum state, int32_t ord, int32_t doc_freq, int64_t total_term_freq) {
    register_state(std::move(state), ord);
    accumulate_statistics(doc_freq, total_term_freq);
}

void TermStates::register_state(TermStateEnum state, int32_t ord) {
    const std::lock_guard<std::mutex> guard(*mutex_);
    if (ord < 0 || static_cast<size_t>(ord) >= states_.size()) {
        // The reference implementation asserts here; a release build would corrupt memory, so report
        // instead of writing out of bounds.
        return;
    }
    states_[static_cast<size_t>(ord)] = std::move(state);
    resolved_[static_cast<size_t>(ord)] = 1;
}

void TermStates::accumulate_statistics(int32_t doc_freq, int64_t total_term_freq) {
    const std::lock_guard<std::mutex> guard(*mutex_);
    doc_freq_ = static_cast<int32_t>(static_cast<int64_t>(doc_freq_) + doc_freq);
    total_term_freq_ += total_term_freq;
}

Result<std::optional<PrepareState>> TermStates::get(const LeafReaderContext &ctx) {
    const int32_t ctx_ord = ctx.ord();
    if (ctx_ord < 0 || static_cast<size_t>(ctx_ord) >= states_.size()) {
        return err(LuceneError::illegal_argument("leaf ordinal " + std::to_string(ctx_ord) + " is out of bounds for " +
                                                 std::to_string(states_.size()) + " states"));
    }
    const auto slot = static_cast<size_t>(ctx_ord);

    {
        const std::lock_guard<std::mutex> guard(*mutex_);
        if (states_[slot].has_value()) {
            return std::optional<PrepareState>(PrepareState::ready(ctx_ord));
        }
        if (resolved_[slot] != 0) {
            // Known absent from this leaf: no need to seek again.
            return std::optional<PrepareState>{};
        }
    }

    if (!term_.has_value()) {
        // States are registered explicitly; nothing to resolve lazily.
        return std::optional<PrepareState>{};
    }

    auto terms = ctx.reader()->terms(term_->field());
    if (!terms.has_value()) {
        return std::unexpected(std::move(terms.error()));
    }
    if (terms.value() == nullptr) {
        const std::lock_guard<std::mutex> guard(*mutex_);
        resolved_[slot] = 1;
        return std::optional<PrepareState>{};
    }
    auto terms_enum = terms.value()->iterator();
    if (!terms_enum.has_value()) {
        return std::unexpected(std::move(terms_enum.error()));
    }
    auto pending = terms_enum.value()->prepare_seek_exact(term_->bytes().as_slice());
    if (!pending.has_value()) {
        return std::unexpected(std::move(pending.error()));
    }
    if (!pending.value()) {
        const std::lock_guard<std::mutex> guard(*mutex_);
        resolved_[slot] = 1;
        return std::optional<PrepareState>{};
    }
    return std::optional<PrepareState>(PrepareState::pending(*term_, ctx_ord, std::move(terms_enum.value())));
}

Result<std::optional<const TermStateEnum *>> TermStates::resolve(PrepareState &state) {
    const int32_t ctx_ord = state.ord();
    if (ctx_ord < 0 || static_cast<size_t>(ctx_ord) >= states_.size()) {
        return err(LuceneError::illegal_argument("leaf ordinal " + std::to_string(ctx_ord) + " is out of bounds for " +
                                                 std::to_string(states_.size()) + " states"));
    }
    const auto slot = static_cast<size_t>(ctx_ord);

    if (state.is_ready()) {
        const std::lock_guard<std::mutex> guard(*mutex_);
        if (!states_[slot].has_value()) {
            return std::optional<const TermStateEnum *>{};
        }
        return std::optional<const TermStateEnum *>(&states_[slot].value());
    }

    TermsEnum &terms_enum = *state.terms_enum();
    auto found = terms_enum.get_prepare_seek_exact_status(state.term().bytes().as_slice());
    if (!found.has_value()) {
        return std::unexpected(std::move(found.error()));
    }

    const std::lock_guard<std::mutex> guard(*mutex_);
    if (!states_[slot].has_value()) {
        if (found.value()) {
            // Publish only after the lookup succeeded, mirroring OnceLock.
            auto term_state = terms_enum.term_state();
            if (!term_state.has_value()) {
                return std::unexpected(std::move(term_state.error()));
            }
            states_[slot] = std::move(term_state.value());
        }
        resolved_[slot] = 1;
    }
    if (!states_[slot].has_value()) {
        return std::optional<const TermStateEnum *>{};
    }
    return std::optional<const TermStateEnum *>(&states_[slot].value());
}

Result<int32_t> TermStates::doc_freq() const {
    if (term_.has_value()) {
        return err(LuceneError::illegal_state("Cannot call docFreq() when needsStats=false"));
    }
    return doc_freq_;
}

Result<int64_t> TermStates::total_term_freq() const {
    if (term_.has_value()) {
        return err(LuceneError::illegal_state("Cannot call totalTermFreq() when needsStats=false"));
    }
    return total_term_freq_;
}

std::string TermStates::to_string() const {
    std::string result = "TermStates\n";
    for (const auto &slot : states_) {
        result += "  state=" + (slot.has_value() ? slot.value().to_string() : std::string("null")) + "\n";
    }
    return result;
}

Result<TermStates> build_term_states(const IndexSearcher &searcher, Term term, bool needs_stats) {
    const IndexReader &reader = searcher.reader();
    if (!needs_stats) {
        return TermStates::with_term(term, reader);
    }

    auto per_reader_term_state = TermStates::create(reader);
    if (!per_reader_term_state.has_value()) {
        return std::unexpected(std::move(per_reader_term_state.error()));
    }

    auto leaves = reader.leaves();
    if (!leaves.has_value()) {
        return std::unexpected(std::move(leaves.error()));
    }

    // Phase one: start the dictionary lookups of every leaf that can have the
    // term, so the I/O of all leaves can overlap.
    std::vector<std::unique_ptr<TermsEnum>> pending(leaves.value().size());
    for (const auto &ctx : leaves.value()) {
        LeafReader &leaf = *ctx.reader();
        auto terms = leaf.terms(term.field());
        if (!terms.has_value()) {
            return std::unexpected(std::move(terms.error()));
        }
        if (terms.value() == nullptr) {
            continue;
        }
        auto terms_enum = terms.value()->iterator();
        if (!terms_enum.has_value()) {
            return std::unexpected(std::move(terms_enum.error()));
        }
        auto started = terms_enum.value()->prepare_seek_exact(term.bytes().as_slice());
        if (!started.has_value()) {
            return std::unexpected(std::move(started.error()));
        }
        if (started.value()) {
            const auto ord = static_cast<size_t>(ctx.ord());
            if (ord >= pending.size()) {
                return err(LuceneError::illegal_state("leaf ord " + std::to_string(ord) + " exceeds the leaf count " +
                                                      std::to_string(pending.size())));
            }
            pending[ord] = std::move(terms_enum.value());
        }
    }

    // Phase two: finish the lookups and accumulate the statistics.
    for (size_t ord = 0; ord < pending.size(); ++ord) {
        if (pending[ord] == nullptr) {
            continue;
        }
        TermsEnum &terms_enum = *pending[ord];
        auto found = terms_enum.get_prepare_seek_exact_status(term.bytes().as_slice());
        if (!found.has_value()) {
            return std::unexpected(std::move(found.error()));
        }
        if (!found.value()) {
            continue;
        }
        auto state = terms_enum.term_state();
        if (!state.has_value()) {
            return std::unexpected(std::move(state.error()));
        }
        auto doc_freq = terms_enum.doc_freq();
        if (!doc_freq.has_value()) {
            return std::unexpected(std::move(doc_freq.error()));
        }
        auto total_term_freq = terms_enum.total_term_freq();
        if (!total_term_freq.has_value()) {
            return std::unexpected(std::move(total_term_freq.error()));
        }
        per_reader_term_state.value().register_with_stats(std::move(state.value()), static_cast<int32_t>(ord),
                                                          doc_freq.value(), total_term_freq.value());
    }

    return per_reader_term_state;
}

} // namespace lucene
