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

module lucene_core:leaf_collector.impl;

import :leaf_collector;

import std;

import :doc_id_stream;
import :error;
import :scorable;
import :types;

namespace lucene {

namespace {

/// The consumer the default [`LeafCollector::collect_stream`] uses: every
/// document goes through [`LeafCollector::collect`].
class CollectConsumer final : public DocIdStreamConsumer {
public:
    CollectConsumer(LeafCollector &collector, Scorable &scorer) : collector_(&collector), scorer_(&scorer) {}

    [[nodiscard]] VoidResult accept(int32_t doc) override { return collector_->collect(doc, *scorer_); }

    [[nodiscard]] VoidResult accept_with_score(int32_t doc, float score) override {
        if (auto scored = scorer_->set_score(score); !scored.has_value()) {
            return scored;
        }
        return collector_->collect(doc, *scorer_);
    }

private:
    LeafCollector *collector_;
    Scorable *scorer_;
};

} // namespace

VoidResult LeafCollector::collect_stream(DocIdStream &stream, Scorable &scorer) {
    CollectConsumer consumer(*this, scorer);
    return stream.for_each(consumer);
}

} // namespace lucene
