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

module lucene_core:reference_manager.impl;

import :reference_manager;

import :error;
import :index_searcher;
import :types;

import std;

namespace lucene {

std::shared_ptr<IndexSearcher> ReferenceManager::acquire() const {
    const std::scoped_lock lock(mutex_);
    return current_;
}

Result<bool> ReferenceManager::maybe_refresh() {
    std::shared_ptr<IndexSearcher> current;
    {
        const std::scoped_lock lock(mutex_);
        if (closed_) {
            return err(LuceneError::already_closed("this ReferenceManager is closed"));
        }
        current = current_;
    }

    auto is_current = this->is_current(current);
    if (!is_current.has_value()) {
        return std::unexpected(std::move(is_current.error()));
    }
    if (is_current.value()) {
        return false;
    }

    // A concurrent refresh may have won the race; the searcher it published is
    // just as good, so keep it instead of opening a third one.
    auto refreshed = refresh_if_needed(std::move(current));
    if (!refreshed.has_value()) {
        return std::unexpected(std::move(refreshed.error()));
    }
    if (refreshed.value() == nullptr) {
        return false;
    }

    const std::scoped_lock lock(mutex_);
    if (closed_) {
        return err(LuceneError::already_closed("this ReferenceManager is closed"));
    }
    current_ = std::move(refreshed.value());
    return true;
}

VoidResult ReferenceManager::close() {
    const std::scoped_lock lock(mutex_);
    if (closed_) {
        return {};
    }
    closed_ = true;
    current_.reset();
    return {};
}

} // namespace lucene
