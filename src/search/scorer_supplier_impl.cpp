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

module lucene_core:scorer_supplier.impl;

import :scorer_supplier;

import :bulk_scorer;
import :error;
import :leaf_reader_context;
import :scorer;
import :types;

import std;

namespace lucene {

Result<std::unique_ptr<BulkScorer>> ScorerSupplier::bulk_scorer(const LeafReaderContext &context,
                                                                const IndexSearcher &searcher) {
    auto scorer = get(std::numeric_limits<int64_t>::max(), context, searcher);
    if (!scorer.has_value()) {
        return std::unexpected(std::move(scorer.error()));
    }
    std::unique_ptr<BulkScorer> bulk = std::make_unique<DefaultBulkScorer>(std::move(scorer.value()));
    return bulk;
}

} // namespace lucene
