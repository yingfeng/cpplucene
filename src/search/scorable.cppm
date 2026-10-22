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

export module lucene_core:scorable;

import :types;
import std;

import :error;

namespace lucene {

export class Scorable;

/// A sub-scorer and its relationship to the parent, e.g. `"MUST"`.
///
/// Port of `ChildScorable` (`core/search/scorable.rs`).  The reference implementation ties the child
/// type to the parent through a generic associated type; here the child is a
/// plain non-owning pointer, since every child in the ported code is a scorer
/// owned by its parent.
export struct ChildScorable {
    Scorable *child{nullptr};
    std::string relationship;
};

/// Allows access to the score of a query.
///
/// Port of `Scorable` (`core/search/scorable.rs`).
///
/// **Deviation:** the `cost()` method of the Rust trait is not repeated here —
/// every `Scorable` in the ported code is also a `DocIdSetIterator`, whose
/// `cost()` already carries the same signature and meaning.
export class Scorable {
public:
    virtual ~Scorable() = default;

    /// The score of the document the scorer is positioned on.
    [[nodiscard]] virtual Result<float> score() = 0;

    /// The score used when the term does not appear in the document, behaving
    /// like an IDF.
    [[nodiscard]] virtual Result<float> smoothing_score(int32_t /*doc_id*/) { return 0.0F; }

    /// Tells the scorer that documents scoring below `min_score` may be
    /// skipped.  Only meaningful for `ScoreMode::TopScores`.
    [[nodiscard]] virtual VoidResult set_min_competitive_score(float /*min_score*/) { return {}; }

    /// The sub-scorers positioned on the current document.
    [[nodiscard]] virtual Result<std::vector<ChildScorable>> get_children() {
        return std::vector<ChildScorable>{};
    }

    /// Overwrites the score of the current document, for scorers that allow
    /// their score to be fixed.
    [[nodiscard]] virtual VoidResult set_score(float /*score*/) {
        return err(LuceneError::need_implemented("set_score"));
    }
};

} // namespace lucene
