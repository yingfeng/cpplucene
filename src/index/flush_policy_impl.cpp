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

module lucene_core:flush_policy.impl;

import :flush_policy;

import std;

import :types;

namespace lucene {

std::shared_ptr<const FlushByRamOrCountsPolicy> FlushByRamOrCountsPolicy::create() {
    static const auto instance = std::shared_ptr<const FlushByRamOrCountsPolicy>(new FlushByRamOrCountsPolicy());
    return instance;
}

bool FlushByRamOrCountsPolicy::flushes_on_doc_count(const FlushPolicyContext &context) noexcept {
    return context.max_buffered_docs > 0;
}

bool FlushByRamOrCountsPolicy::flushes_on_ram(const FlushPolicyContext &context) noexcept {
    return context.ram_buffer_size_bytes != std::numeric_limits<int64_t>::max();
}

bool FlushByRamOrCountsPolicy::should_flush(const FlushPolicyContext &context) const {
    // The document count is checked first, and it decides on its own: the
    // reference implementation returns as soon as it marks the buffer pending.
    if (flushes_on_doc_count(context) && context.num_docs_in_ram >= context.max_buffered_docs) {
        return true;
    }
    if (!flushes_on_ram(context)) {
        return false;
    }
    // Without buffered deletes of their own, the bytes the policy can see are the
    // bytes of the buffered documents alone.
    return context.bytes_used_in_ram >= context.ram_buffer_size_bytes;
}

} // namespace lucene
