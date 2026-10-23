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

module lucene_core:req_excl_bulk_scorer.impl;

import :req_excl_bulk_scorer;

import std;

import :bulk_scorer;
import :error;
import :leaf_collector;
import :scorer;
import :types;

namespace lucene {

ReqExclBulkScorer::ReqExclBulkScorer(std::unique_ptr<BulkScorer> required, std::unique_ptr<Scorer> excluded)
        : required_(std::move(required)), excluded_(std::move(excluded)) {}

Result<int32_t> ReqExclBulkScorer::score(LeafCollector &collector, const Bits *accept_docs, int32_t min,
                                         int32_t max) {
    int32_t upto = min;
    int32_t excluded_doc = excluded_->approx_doc_id();

    while (upto < max) {
        if (excluded_doc < upto) {
            auto advanced = excluded_->approx_advance(upto);
            if (!advanced.has_value()) {
                return std::unexpected(std::move(advanced.error()));
            }
            excluded_doc = advanced.value();
        }
        if (excluded_doc == upto) {
            auto excluded = excluded_->matches();
            if (!excluded.has_value()) {
                return std::unexpected(std::move(excluded.error()));
            }
            if (excluded.value()) {
                ++upto;
            }
            auto next = excluded_->approx_next_doc();
            if (!next.has_value()) {
                return std::unexpected(std::move(next.error()));
            }
            excluded_doc = next.value();
        } else {
            // Everything before the next excluded document may be scored
            // straight away.
            const int32_t limit = std::min(excluded_doc, max);
            auto scored = required_->score(collector, accept_docs, upto, limit);
            if (!scored.has_value()) {
                return std::unexpected(std::move(scored.error()));
            }
            upto = scored.value();
        }
    }

    if (upto == max) {
        // Let the required scorer finish the documents it still owns exactly at
        // the boundary.
        auto scored = required_->score(collector, accept_docs, upto, upto);
        if (!scored.has_value()) {
            return std::unexpected(std::move(scored.error()));
        }
        upto = scored.value();
    }
    return upto;
}

Result<int64_t> ReqExclBulkScorer::cost() { return required_->cost(); }

} // namespace lucene
