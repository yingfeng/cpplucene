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

export module lucene_core:per_field_merge_state;

import :types;
import std;

import :error;
import :field_infos;
import :fields_producer;
import :merge_state;

namespace lucene {

/// A merge-state view restricted to a set of field names.
///
/// Port of `PerFieldMergeState` (`codecs/perfield/per_field_merge_state.rs`).
/// A per-field codec uses it to hand one format a merge state that looks like
/// the segment contained only that format's fields: the postings readers are
/// wrapped so they only list (and only accept) those fields, and the field
/// infos become [`FieldInfos::with_filter`] views, so the `has_*` predicates and
/// the field iteration of the writer follow the group.
///
/// **Deviation:** the reference implementation returns a `Result` because
/// rebuilding `FieldInfos` can fail; the port's [`FieldInfos::with_filter`] is
/// total, so this cannot fail.  The returned state borrows every reader from
/// `state`, which must therefore outlive it (the `shared_ptr`s keep the readers
/// themselves alive).
///
/// `fields` must be sorted and free of duplicates, the way the per-field
/// writers produce it.
[[nodiscard]] std::unique_ptr<MergeState> restrict_merge_state_fields(const MergeState &state,
                                                                      const std::vector<std::string> &fields);

} // namespace lucene
