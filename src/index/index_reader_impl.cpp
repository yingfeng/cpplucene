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

module lucene_core:index_reader.impl;

import :index_reader;

import :error;
import :types;

import std;

namespace lucene {

Result<int32_t> IndexReader::num_deleted_docs() const {
    auto max = max_doc();
    if (!max.has_value()) {
        return std::unexpected(std::move(max.error()));
    }
    auto num = num_docs();
    if (!num.has_value()) {
        return std::unexpected(std::move(num.error()));
    }
    return max.value() - num.value();
}

Result<bool> IndexReader::has_deletions() const {
    auto deleted = num_deleted_docs();
    if (!deleted.has_value()) {
        return std::unexpected(std::move(deleted.error()));
    }
    return deleted.value() > 0;
}

} // namespace lucene
