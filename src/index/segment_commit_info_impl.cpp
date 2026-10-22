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

module lucene_core:segment_commit_info.impl;

import :segment_commit_info;

import :error;
import :types;

import std;

namespace lucene {

VoidResult SegmentCommitInfo::validate() const {
    if (del_count < 0 || del_count > info.max_doc) {
        return err(LuceneError::illegal_state("invalid deletion count " + std::to_string(del_count) +
                                              " for segment " + info.name + " with maxDoc=" +
                                              std::to_string(info.max_doc)));
    }
    if (soft_del_count < 0 || soft_del_count > info.max_doc) {
        return err(LuceneError::illegal_state("invalid soft deletion count " + std::to_string(soft_del_count) +
                                              " for segment " + info.name + " with maxDoc=" +
                                              std::to_string(info.max_doc)));
    }
    if (total_del_count() > info.max_doc) {
        return err(LuceneError::illegal_state("combined deletions " + std::to_string(total_del_count()) +
                                              " exceed maxDoc=" + std::to_string(info.max_doc) + " for segment " +
                                              info.name));
    }
    if (id.has_value() && id->size() != 16) {
        return err(LuceneError::illegal_state("the segment commit id must be 16 bytes long for segment " + info.name));
    }
    return info.validate();
}

} // namespace lucene
