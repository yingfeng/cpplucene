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

export module lucene_core:sorting_rewriter;

import :types;
import std;

import :codec_reader;
import :directory;
import :error;
import :segment_reader;
import :sort_field;

namespace lucene {

/// Writes the documents of `readers` into `directory` as one new segment, sorted
/// by `sort`.
///
/// Port of the sorted half of `IndexWriter::add_indexes`: every input segment is
/// sorted (see [`sort_documents`]) and wrapped in a [`SortingCodecReader`], and
/// the resulting view is then merged into a fresh segment, so all the formats of
/// the output (stored fields, norms, doc values, postings, term vectors, points)
/// already describe the sorted document order.
///
/// `segment_name` (`_7`, ...) and `segment_id` name the new segment; the codec
/// name and the version are filled in from the default codecs.  The merge runs
/// with a merge [`IOContext`], which the helper builds from the input sizes.
///
/// The new segment records `sort` as its index sort, so readers can tell that
/// its documents are already ordered.
///
/// The input segments must be free of deletions: the sort compacts the live
/// documents, so a deletion would make the reader's document range disagree with
/// the number of documents the merge sees.
///
/// **Deviation:** the merge runs single threaded, like every other merge in this
/// port.
[[nodiscard]] Result<std::shared_ptr<SegmentReader>> rewrite_sorted_segment(
        std::span<CodecReader *const> readers, const Sort &sort, Directory &directory, std::string segment_name,
        std::vector<uint8_t> segment_id);

} // namespace lucene
