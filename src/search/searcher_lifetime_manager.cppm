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

export module lucene_core:searcher_lifetime_manager;

import :types;
import std;

import :error;
import :index_searcher;

namespace lucene {

/// Keeps older searchers usable so a long-running query can finish.
///
/// Port of `SearcherLifetimeManager` (`search/searcher_lifetime_manager.rs`).
/// A searcher is recorded under a token (Lucene uses the index version as a
/// `long`); [`prune`] later drops the ones nothing should use any more.
///
/// **Deviation:** the reference implementation ref-counts the searchers it
/// hands out; the port returns `std::shared_ptr`, so a searcher stays open for
/// as long as the caller holds it, and [`prune`] only affects what future
/// [`acquire`] calls can find.
export class SearcherLifetimeManager {
public:
    SearcherLifetimeManager() = default;
    ~SearcherLifetimeManager();

    SearcherLifetimeManager(const SearcherLifetimeManager &) = delete;
    SearcherLifetimeManager &operator=(const SearcherLifetimeManager &) = delete;

    /// Records `searcher` under `version` and returns `version`.
    ///
    /// Recording the same version twice replaces the searcher.
    [[nodiscard]] Result<int64_t> record(int64_t version, std::shared_ptr<IndexSearcher> searcher);

    /// The searcher recorded under `version`, or null when it was pruned.
    [[nodiscard]] std::shared_ptr<IndexSearcher> acquire(int64_t version) const;

    /// Drops every recorded searcher `keep` rejects; returns how many went.
    ///
    /// `keep` receives the recorded version, newest first.
    [[nodiscard]] size_t prune(const std::function<bool(int64_t version)> &keep);

    /// Drops every recorded searcher.
    void close();

    /// How many searchers are recorded.
    [[nodiscard]] size_t size() const;

private:
    mutable std::mutex mutex_;
    std::map<int64_t, std::shared_ptr<IndexSearcher>> searchers_;
    bool closed_{false};
};

} // namespace lucene
