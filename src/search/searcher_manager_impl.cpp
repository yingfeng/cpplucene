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

module lucene_core:searcher_manager.impl;

import :searcher_manager;

import :directory;
import :error;
import :index_reader;
import :index_searcher;
import :reference_manager;
import :standard_directory_reader;
import :types;

import std;

namespace lucene {

// ---- SearcherFactory ----

Result<std::shared_ptr<IndexSearcher>> SearcherFactory::new_searcher(std::shared_ptr<IndexReader> reader) const {
    if (reader == nullptr) {
        return err(LuceneError::illegal_argument("a searcher needs a reader"));
    }
    return std::make_shared<IndexSearcher>(std::move(reader));
}

// ---- SearcherManager ----

Result<std::shared_ptr<SearcherManager>> SearcherManager::open(std::shared_ptr<IndexReader> reader,
                                                              std::shared_ptr<const SearcherFactory> factory) {
    if (reader == nullptr) {
        return err(LuceneError::illegal_argument("a searcher manager needs a reader"));
    }
    std::shared_ptr<const SearcherFactory> effective = factory == nullptr
                                                              ? std::make_shared<const SearcherFactory>()
                                                              : std::move(factory);
    auto searcher = effective->new_searcher(std::move(reader));
    if (!searcher.has_value()) {
        return std::unexpected(std::move(searcher.error()));
    }
    return std::shared_ptr<SearcherManager>(new SearcherManager(std::move(searcher.value()), std::move(effective)));
}

Result<std::shared_ptr<SearcherManager>> SearcherManager::open(Directory &directory,
                                                              std::shared_ptr<const SearcherFactory> factory) {
    auto reader = DirectoryReader::open(directory);
    if (!reader.has_value()) {
        return std::unexpected(std::move(reader.error()));
    }
    std::shared_ptr<IndexReader> index_reader = reader.value();
    return open(std::move(index_reader), std::move(factory));
}

Result<std::shared_ptr<IndexSearcher>> SearcherManager::refresh_if_needed(
        std::shared_ptr<IndexSearcher> current) {
    if (current == nullptr) {
        return err(LuceneError::illegal_argument("there is no searcher to refresh"));
    }
    // Only a directory reader can be reopened; a searcher over a hand-built
    // composite reader stays where it is, like in the reference implementation.
    auto *directory_reader = dynamic_cast<DirectoryReader *>(&current->reader());
    if (directory_reader == nullptr) {
        return std::shared_ptr<IndexSearcher>{};
    }
    auto reopened = directory_reader->open_if_changed();
    if (!reopened.has_value()) {
        return std::unexpected(std::move(reopened.error()));
    }
    if (reopened.value() == nullptr) {
        // Nothing changed.
        return std::shared_ptr<IndexSearcher>{};
    }
    std::shared_ptr<IndexReader> reader = reopened.value();
    return factory_->new_searcher(std::move(reader));
}

Result<bool> SearcherManager::is_current(const std::shared_ptr<IndexSearcher> &current) const {
    if (current == nullptr) {
        return err(LuceneError::illegal_argument("there is no searcher to compare"));
    }
    auto *directory_reader = dynamic_cast<DirectoryReader *>(&current->reader());
    if (directory_reader == nullptr) {
        // A reader the manager did not open from a directory never changes.
        return true;
    }
    auto reopened = directory_reader->open_if_changed();
    if (!reopened.has_value()) {
        return std::unexpected(std::move(reopened.error()));
    }
    return reopened.value() == nullptr;
}

} // namespace lucene
