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

export module lucene_core:segment_read_state;

import :types;
import std;

import :directory;
import :field_infos;
import :io_context;

namespace lucene {

/// Holder for the parameters shared by every reader of one segment.
///
/// Port of `SegmentReadState` (`core/index/segment_read_state.rs`).  The reference implementation
/// borrows the directory and the IO context and shares the field infos through
/// an `Arc`; the port keeps the same shape with references and a
/// `std::shared_ptr<const FieldInfos>`.
///
/// The members are references/`shared_ptr`s, so the type is neither assignable
/// nor default constructible; build it with one of the constructors.
export class SegmentReadState {
public:
    /// Creates a state with an empty segment suffix.
    SegmentReadState(Directory &directory, std::shared_ptr<const FieldInfos> field_infos,
                     const IOContext &context)
        : directory(directory), field_infos(std::move(field_infos)), context(context) {}

    /// Creates a state with a custom segment suffix.
    SegmentReadState(Directory &directory, std::shared_ptr<const FieldInfos> field_infos,
                     const IOContext &context, std::string segment_suffix)
        : directory(directory),
          field_infos(std::move(field_infos)),
          context(context),
          segment_suffix(std::move(segment_suffix)) {}

    /// Copy of `other` with a different segment suffix (the reference implementation's
    /// `copy_with_suffix`).
    [[nodiscard]] static SegmentReadState copy_with_suffix(const SegmentReadState &other,
                                                          std::string segment_suffix) {
        return SegmentReadState(other.directory, other.field_infos, other.context, std::move(segment_suffix));
    }

    /// Directory the segment is read from.
    ///
    /// Non-const because `Directory::open_input` is non-const in this port
    /// (the reference implementation gets the same effect through interior mutability).
    Directory &directory;
    /// Field infos describing all fields in this segment.
    std::shared_ptr<const FieldInfos> field_infos;
    /// IO context to pass to `Directory::open_input`.
    const IOContext &context;
    /// Unique suffix of any postings files read for this segment.
    std::string segment_suffix;
};

} // namespace lucene
