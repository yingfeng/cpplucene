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

module lucene_core:searcher_lifetime_manager.impl;

import :searcher_lifetime_manager;

import :error;
import :index_searcher;
import :types;

import std;

namespace lucene {

SearcherLifetimeManager::~SearcherLifetimeManager() {
    close();
}

Result<int64_t> SearcherLifetimeManager::record(int64_t version, std::shared_ptr<IndexSearcher> searcher) {
    if (searcher == nullptr) {
        return err(LuceneError::illegal_argument("there is no searcher to record"));
    }
    const std::scoped_lock lock(mutex_);
    if (closed_) {
        return err(LuceneError::already_closed("this SearcherLifetimeManager is closed"));
    }
    searchers_[version] = std::move(searcher);
    return version;
}

std::shared_ptr<IndexSearcher> SearcherLifetimeManager::acquire(int64_t version) const {
    const std::scoped_lock lock(mutex_);
    const auto found = searchers_.find(version);
    return found == searchers_.end() ? nullptr : found->second;
}

size_t SearcherLifetimeManager::prune(const std::function<bool(int64_t version)> &keep) {
    if (!keep) {
        return 0;
    }
    const std::scoped_lock lock(mutex_);
    // Newest first, so a pruner that only looks at the age sees the newest one.
    std::vector<int64_t> doomed;
    for (auto it = searchers_.rbegin(); it != searchers_.rend(); ++it) {
        if (!keep(it->first)) {
            doomed.push_back(it->first);
        }
    }
    for (const int64_t version : doomed) {
        searchers_.erase(version);
    }
    return doomed.size();
}

void SearcherLifetimeManager::close() {
    const std::scoped_lock lock(mutex_);
    closed_ = true;
    searchers_.clear();
}

size_t SearcherLifetimeManager::size() const {
    const std::scoped_lock lock(mutex_);
    return searchers_.size();
}

} // namespace lucene
