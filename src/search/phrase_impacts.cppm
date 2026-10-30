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

export module lucene_core:phrase_impacts;

import :types;
import std;

import :error;
import :impacts;
import :impacts_source;

namespace lucene {

/// Combines the impacts of the postings of a phrase into impacts of the phrase
/// itself, so a phrase scorer can skip blocks that cannot be competitive.
///
/// Port of `ExactPhraseMatcher.mergeImpacts` (the reference implementation has
/// no separate type for it).
///
/// The block layout (number of levels and their last document ids) is taken
/// from the source whose postings are cheapest to iterate, the way
/// `BlockMaxConjunctionScorer` picks its lead.  Inside a block the impacts of
/// all sources are walked in parallel by increasing frequency and merged into
/// the pair (smallest frequency still reachable, largest norm still reachable):
/// a phrase never occurs more often than any of its terms and all its terms
/// share the document's norm, so those pairs bound the phrase's score from
/// above.
export class PhraseImpacts final : public ImpactsSource {
public:
    /// `sources` are the impacts of the postings of each phrase slot; they must
    /// not be empty and must outlive this object.
    explicit PhraseImpacts(std::vector<ImpactsSource *> sources);

    [[nodiscard]] VoidResult advance_shallow(int32_t target) override;

    /// The merged view over the sources' current blocks.  It is owned by this
    /// object and re-pointed at every call.
    [[nodiscard]] Result<Impacts *> get_impacts() override;

private:
    std::vector<ImpactsSource *> sources_;
    /// Index of the source that decides the block layout.
    size_t lead_{0};
    /// The single merged view this source hands out; built on first use.
    std::unique_ptr<Impacts> merged_;
};

} // namespace lucene
