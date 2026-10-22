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

export module lucene_core:merge_trigger;

import :types;
import std;

namespace lucene {

/// The event that triggered a call to
/// [`MergePolicy::find_merges`](lucene_core:merge_policy).
///
/// Port of `MergeTrigger` (`index/merge_trigger.rs`).
export enum class MergeTrigger : uint8_t {
    /// A segment flush.
    SegmentFlush = 0,
    /// A full flush: a commit, an NRT reopen or a `close` on the writer.
    FullFlush = 1,
    /// The user asked for a merge explicitly.
    Explicit = 2,
    /// A merge finished successfully.
    MergeFinished = 3,
    /// The `IndexWriter` is closing.
    Closing = 4,
    /// A commit.
    Commit = 5,
    /// An NRT reader is being opened.
    GetReader = 6,
    /// `IndexWriter::add_indexes`.
    AddIndexes = 7,
};

} // namespace lucene
