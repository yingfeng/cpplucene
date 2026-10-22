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

export module lucene_core:segment_infos;

import :types;
import :error;

import std;

import :directory;
import :index_file_names;
import :index_input;
import :index_output;
import :io_context;
import :segment_commit_info;
import :segment_info;
import :version;

namespace lucene {

/// The commit point of an index: the list of live segments plus the counters a
/// writer continues from.
///
/// Port of `SegmentInfos` (`index/segment_infos.rs`), minus the directory and
/// codec ownership rlucene keeps inside the type: here the file is read and
/// written explicitly, so `SegmentInfos` stays plain data.  The file is
/// `segments_<base36 generation>`:
///
/// ```text
/// header   // CodecUtil::write_index_header("segments", 10, id, base36(gen))
/// vint     lucene_version.major / minor / bugFix   // version that wrote the file
/// vint     index_created_version_major
/// long     version        // index version, bumped by every commit (big endian)
/// vlong    counter        // segment name counter
/// int      segment count  // big endian
/// vint     minSegmentLuceneVersion (only when the count is non-zero)
/// per segment:
///   string  name
///   16 byte id
///   string  codec name
///   long    delGen, int delCount, long fieldInfosGen, long docValuesGen
///   int     softDelCount
///   byte    hasCommitId; [16 byte commit id]
///   set     field infos files
///   int     doc values update count; per entry: int generation, set files
/// map      user data
/// footer
/// ```
///
/// Only [`VERSION_CURRENT`] is written and read: the format keeps the per-commit
/// id branch unconditionally and there is no legacy decoding path.
export class SegmentInfos {
public:
    /// Format written and read by this port: the version that records
    /// `SegmentCommitInfo` ids.  Older files are rejected as
    /// `index_format_too_old` rather than decoded, so there is no second code
    /// path for the pre-8.6 layout.
    static constexpr int32_t VERSION_CURRENT = 10;

    /// Highest document count of one index (`IndexWriter::MAX_DOCS`).
    static constexpr int32_t MAX_DOCS = std::numeric_limits<int32_t>::max() - 128;

    SegmentInfos() = default;

    /// Index version, incremented on every commit.
    int64_t version{0};
    /// Counter used to name new segments (`_<counter>`).
    int64_t counter{0};
    /// Generation of the file this commit lives in.
    int64_t generation{0};
    /// Highest generation seen when reading, so the next commit writes `+1`.
    int64_t last_generation{0};
    /// Index identity, also written into the header of the commit file.
    std::vector<uint8_t> id;
    int32_t index_created_version_major{LUCENE_CURRENT_VERSION.major};
    /// Version of the code that wrote this commit file.
    Version lucene_version{LUCENE_CURRENT_VERSION};
    /// Oldest version among the live segments.
    std::optional<Version> min_segment_lucene_version;
    std::vector<SegmentCommitInfo> segments;
    /// User data committed alongside the segments (`IndexWriter.setLiveCommitData`).
    std::map<std::string, std::string> user_data;

    [[nodiscard]] size_t size() const noexcept { return segments.size(); }
    [[nodiscard]] bool empty() const noexcept { return segments.empty(); }
    [[nodiscard]] int64_t total_max_doc() const;

    /// Appends `segment`, after validating it.
    [[nodiscard]] VoidResult add(SegmentCommitInfo segment);

    /// `segments_<base36 generation>`.
    [[nodiscard]] std::string generation_file_name() const;

    /// Writes the commit into `out` (header, payload and footer).
    [[nodiscard]] VoidResult write(IndexOutput &out) const;

    /// Creates `segments_<generation>` in `dir` and writes it.
    [[nodiscard]] VoidResult write_to(Directory &dir, const IOContext &context) const;

    /// Reads the commit named by `generation`.
    [[nodiscard]] static Result<SegmentInfos> read(const Directory &dir, int64_t generation,
                                                   const IOContext &context = io_context_default());

    /// Reads `segments_<generation>` by file name.
    [[nodiscard]] static Result<SegmentInfos> read_file(const Directory &dir, std::string_view file_name,
                                                        int64_t generation,
                                                        const IOContext &context = io_context_default());

    /// Finds the newest `segments_N` and reads it.
    [[nodiscard]] static Result<SegmentInfos> read_latest(const Directory &dir,
                                                          const IOContext &context = io_context_default());

    /// Generations of every commit file in `dir`, ascending.
    [[nodiscard]] static Result<std::vector<int64_t>> list_generations(const Directory &dir);
};

} // namespace lucene
