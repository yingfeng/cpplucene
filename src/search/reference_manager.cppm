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

export module lucene_core:reference_manager;

import :types;
import std;

import :error;
import :index_searcher;

namespace lucene {

/// Keeps one [`IndexSearcher`] current with the index it searches.
///
/// Port of `ReferenceManager` (`search/referece_manager.rs` of the reference
/// implementation, which is Lucene's `ReferenceManager<Searcher>`).
///
/// **Deviation:** Lucene's manager hands out reference-counted handles that the
/// caller has to release; the port hands out `std::shared_ptr`, so dropping the
/// last reference is the release.  A caller must therefore keep the searcher it
/// acquired for as long as it uses it — which is exactly what the reference
/// counting bought, with none of the bookkeeping.
///
/// The value is refreshed only when it is no longer current
/// ([`refresh_if_needed`]), so a reader that does not see a new commit keeps
/// working undisturbed.
export class ReferenceManager {
public:
    virtual ~ReferenceManager() = default;

    ReferenceManager(const ReferenceManager &) = delete;
    ReferenceManager &operator=(const ReferenceManager &) = delete;

    /// The current searcher.
    ///
    /// The result is never null: the manager always holds one after
    /// construction.
    [[nodiscard]] std::shared_ptr<IndexSearcher> acquire() const;

    /// Refreshes the searcher when the index moved on; `true` when it did.
    ///
    /// Port of `ReferenceManager::maybe_refresh`.  The new searcher is created
    /// before the old one is dropped, so a concurrent [`acquire`] never sees a
    /// hole.
    [[nodiscard]] Result<bool> maybe_refresh();

    /// Drops the current searcher.
    [[nodiscard]] VoidResult close();

    [[nodiscard]] bool is_closed() const noexcept { return closed_; }

    /// A searcher for the current reader, or null when it is still current.
    [[nodiscard]] virtual Result<std::shared_ptr<IndexSearcher>> refresh_if_needed(
            std::shared_ptr<IndexSearcher> current) = 0;

    /// Whether `current` still describes the index.
    [[nodiscard]] virtual Result<bool> is_current(const std::shared_ptr<IndexSearcher> &current) const = 0;

protected:
    /// A manager holding `current`.
    explicit ReferenceManager(std::shared_ptr<IndexSearcher> current) : current_(std::move(current)) {}

private:
    std::shared_ptr<IndexSearcher> current_;
    mutable std::mutex mutex_;
    bool closed_{false};
};

} // namespace lucene
