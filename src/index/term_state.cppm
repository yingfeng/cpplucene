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

export module lucene_core:term_state;

import :types;
import std;

import :error;

namespace lucene {

/// Encapsulates all the internal state required to position the associated
/// `TermsEnum` without re-seeking.  Port of the `TermState` trait
/// (`core/index/term_state.rs`, `Display + Clone`).
export class TermState {
public:
    virtual ~TermState() = default;

    /// The reference implementation's `Clone` bound.
    [[nodiscard]] virtual std::unique_ptr<TermState> clone() const = 0;

    /// Copies the content of `other` into this instance.
    [[nodiscard]] virtual VoidResult copy_from(const TermState &other) = 0;

    /// The reference implementation's `Display` bound.
    [[nodiscard]] virtual std::string to_string() const = 0;
};

/// An ordinal-based [`TermState`].  Port of `OrdTermState`
/// (`core/index/ord_term_state.rs`).
export class OrdTermState : public TermState {
public:
    OrdTermState() = default;
    explicit OrdTermState(int64_t term_ord) : ord(term_ord) {}

    /// Term ordinal, i.e. its position in the full list of sorted terms.
    int64_t ord{0};

    [[nodiscard]] std::unique_ptr<TermState> clone() const override {
        return std::make_unique<OrdTermState>(*this);
    }

    [[nodiscard]] VoidResult copy_from(const TermState &other) override {
        if (const auto *ord_state = dynamic_cast<const OrdTermState *>(&other)) {
            ord = ord_state->ord;
            return {};
        }
        return err(LuceneError::illegal_argument("OrdTermState::copy_from requires another OrdTermState"));
    }

    [[nodiscard]] std::string to_string() const override { return std::format("OrdTermState ord={}", ord); }
};

} // namespace lucene
