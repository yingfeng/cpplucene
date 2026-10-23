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

export module lucene_core:filter_leaf_collector;

import :types;
import std;

import :doc_id_set_iterator;
import :error;
import :leaf_collector;
import :scorable;

namespace lucene {

/// A [`LeafCollector`] that forwards to another one, for the wrappers that only
/// change some of the calls.
///
/// Port of `FilterLeafCollector` (`core/search/filter_leaf_collector.rs`).
///
/// **Deviation:** the reference implementation is a placeholder struct; the port
/// gives it the delegation the wrappers need, because every wrapper in the
/// reference implementation re-implements the forwarding by hand.
export class FilterLeafCollector : public LeafCollector {
public:
    explicit FilterLeafCollector(LeafCollector &inner) : inner_(&inner) {}

    [[nodiscard]] VoidResult set_scorer(Scorable &scorer) override { return inner_->set_scorer(scorer); }

    [[nodiscard]] VoidResult collect(int32_t doc, Scorable &scorer) override { return inner_->collect(doc, scorer); }

    [[nodiscard]] Result<std::unique_ptr<DocIdSetIterator>> competitive_iterator() override {
        return inner_->competitive_iterator();
    }

    [[nodiscard]] VoidResult finish() override { return inner_->finish(); }

    [[nodiscard]] std::string to_string() const override { return "FilterLeafCollector(" + inner_->to_string() + ")"; }

protected:
    /// The collector being filtered.
    [[nodiscard]] LeafCollector &inner() noexcept { return *inner_; }

private:
    LeafCollector *inner_{nullptr};
};

} // namespace lucene
