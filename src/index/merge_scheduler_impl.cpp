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

module lucene_core:merge_scheduler.impl;

import :merge_scheduler;

import std;

import :error;
import :merge_policy;
import :merge_trigger;

namespace lucene {

std::shared_ptr<NoMergeScheduler> NoMergeScheduler::create() { return std::make_shared<NoMergeScheduler>(); }

std::shared_ptr<SerialMergeScheduler> SerialMergeScheduler::create() {
    return std::make_shared<SerialMergeScheduler>();
}

VoidResult SerialMergeScheduler::merge(MergeSource &source, MergeTrigger /*trigger*/) {
    while (true) {
        auto next = source.get_next_merge();
        if (!next.has_value()) {
            return std::unexpected(std::move(next.error()));
        }
        if (!next.value().has_value()) {
            return {};
        }
        if (auto merged = source.merge(std::move(next.value().value())); !merged.has_value()) {
            return merged;
        }
    }
}

} // namespace lucene
