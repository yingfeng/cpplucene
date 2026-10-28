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

export module lucene_core:time_limiting_collector;

import :types;
import std;

import :collector;
import :collector_manager;
import :error;
import :leaf_collector;
import :query_timeout;
import :score_mode;
import :top_docs;
import :weight;

namespace lucene {

export class IndexSearcher;

/// Stops a search once the [`QueryTimeout`] says so.
///
/// Port of `TimeLimitingCollector` (`search/time_limiting_collector.rs`).  The
/// wrapped leaf collector asks the timeout every `ticks` documents; when it says
/// `should_exit`, `collect` reports `time_exceeded`, which the search loop
/// propagates to the caller — the same contract as Lucene's
/// `TimeExceededException`, so a timed-out search returns no documents rather
/// than an arbitrary partial page.
///
/// **Deviation:** Lucene checks a `Counter` clock (`Clock::now`) instead of the
/// timeout object, and it can be told to swallow the exception
/// (`TimeLimitingCollector.setCollector`); here the timeout is asked directly and
/// the error always surfaces.
export class TimeLimitingCollector final : public Collector {
public:
    /// Wraps `collector`; `ticks` documents are collected between two checks.
    TimeLimitingCollector(std::unique_ptr<Collector> collector, const QueryTimeout &timeout, int32_t ticks);

    [[nodiscard]] Result<std::unique_ptr<LeafCollector>>
    get_leaf_collector(const LeafReaderContext &context, const Weight *weight,
                       const IndexSearcher &searcher) override;
    [[nodiscard]] ScoreMode score_mode() const override;
    [[nodiscard]] VoidResult set_weight(const Weight *weight) override;

    /// Whether the timeout fired during the collection.
    [[nodiscard]] bool timed_out() const noexcept { return timed_out_; }

    /// Documents collected between two checks of the timeout.
    [[nodiscard]] int32_t ticks() const noexcept { return ticks_; }

    /// Asks the timeout whether the search has to stop, remembering the answer.
    [[nodiscard]] bool timeout_expired() noexcept {
        if (!timed_out_ && timeout_->should_exit()) {
            timed_out_ = true;
        }
        return timed_out_;
    }

    /// Releases the wrapped collector, for a manager that has to hand it back.
    [[nodiscard]] std::unique_ptr<Collector> take_collector();

private:
    std::unique_ptr<Collector> collector_;
    const QueryTimeout *timeout_;
    int32_t ticks_;
    bool timed_out_{false};
    /// The per-leaf collector counts the documents of its own leaf.
    int64_t collected_{0};
};

/// Applies a [`QueryTimeout`] to every collector of `inner`.
export class TimeLimitingCollectorManager final : public CollectorManager {
public:
    /// Wraps `inner`; `ticks` documents are collected between two checks.
    TimeLimitingCollectorManager(const CollectorManager &inner, const QueryTimeout &timeout,
                                 int32_t ticks = DEFAULT_TICKS);

    /// Documents collected between two checks of the timeout.
    static constexpr int32_t DEFAULT_TICKS = 1024;

    [[nodiscard]] Result<std::unique_ptr<Collector>> new_collector() const override;

    /// Merges through the wrapped manager, with the collectors unwrapped again.
    [[nodiscard]] Result<std::vector<TopDocs>>
    reduce(std::vector<std::unique_ptr<Collector>> collectors) const override;

private:
    const CollectorManager *inner_;
    const QueryTimeout *timeout_;
    int32_t ticks_;
};

} // namespace lucene
