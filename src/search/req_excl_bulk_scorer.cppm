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

export module lucene_core:req_excl_bulk_scorer;

import :types;
import std;

import :bulk_scorer;
import :error;
import :leaf_collector;
import :scorer;

namespace lucene {

/// The bulk scorer of a `MUST NOT` query: it scores the required documents and
/// asks the excluded scorer which of them really match.
///
/// Port of `ReqExclBulkScorer` (`core/search/req_excl_bulk_scorer.rs`).
export class ReqExclBulkScorer final : public BulkScorer {
public:
    /// Scores the documents of `required`, dropping the ones `excluded` matches.
    ReqExclBulkScorer(std::unique_ptr<BulkScorer> required, std::unique_ptr<Scorer> excluded);

    [[nodiscard]] Result<int32_t> score(LeafCollector &collector, const Bits *accept_docs, int32_t min,
                                        int32_t max) override;

    [[nodiscard]] Result<int64_t> cost() override;

private:
    std::unique_ptr<BulkScorer> required_;
    std::unique_ptr<Scorer> excluded_;
};

} // namespace lucene
