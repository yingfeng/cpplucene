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

export module lucene_core:searcher_manager;

import :types;
import std;

import :directory;
import :error;
import :index_reader;
import :index_searcher;
import :reference_manager;
import :standard_directory_reader;

namespace lucene {

/// Builds the [`IndexSearcher`] a [`SearcherManager`] hands out.
///
/// Port of `SearcherFactory` (`search/searcher_factory.rs`).  Subclass it to
/// configure the searcher — its similarity, its query cache — or to wrap the
/// reader (a soft deletes or field cache wrapper, for instance).
export class SearcherFactory {
public:
    virtual ~SearcherFactory() = default;

    /// A searcher over `reader`; the default reuses the reader as is.
    [[nodiscard]] virtual Result<std::shared_ptr<IndexSearcher>> new_searcher(
            std::shared_ptr<IndexReader> reader) const;
};

/// Keeps an [`IndexSearcher`] current with a directory.
///
/// Port of `SearcherManager` (`search/searcher_manager.rs`): call
/// [`maybe_refresh`] after a commit and [`acquire`] to search.  An acquired
/// searcher keeps its reader open until the last reference is dropped, so a
/// refresh never pulls the rug from under a running query.
export class SearcherManager final : public ReferenceManager {
public:
    /// Manages a searcher over `reader`.
    [[nodiscard]] static Result<std::shared_ptr<SearcherManager>> open(
            std::shared_ptr<IndexReader> reader, std::shared_ptr<const SearcherFactory> factory = nullptr);

    /// Opens the index in `directory` and manages a searcher over it.
    [[nodiscard]] static Result<std::shared_ptr<SearcherManager>> open(
            Directory &directory, std::shared_ptr<const SearcherFactory> factory = nullptr);

    /// A searcher over the current commit, or null when `current` still is it.
    [[nodiscard]] Result<std::shared_ptr<IndexSearcher>> refresh_if_needed(
            std::shared_ptr<IndexSearcher> current) override;

    /// Whether the directory still holds the commit `current` searches.
    [[nodiscard]] Result<bool> is_current(const std::shared_ptr<IndexSearcher> &current) const override;

private:
    SearcherManager(std::shared_ptr<IndexSearcher> current, std::shared_ptr<const SearcherFactory> factory)
        : ReferenceManager(std::move(current)), factory_(std::move(factory)) {}

    std::shared_ptr<const SearcherFactory> factory_;
};

} // namespace lucene
