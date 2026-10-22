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

export module lucene_core:field_infos_builder;

import :types;
import std;

import :error;
import :field_infos;

namespace lucene {

/// Accumulates the [`FieldInfo`]s of the segment being written.
///
/// Port of `FieldInfosBuilder` (`index/field_infos.rs`, the part the indexing
/// chain uses).  Adding a field whose name is already known merges the two
/// descriptions: `index_options` / doc-values-skip-index take the maximum, the
/// store flags are OR-ed, and incompatible doc values or point/vector
/// dimensions are rejected, exactly like Lucene's builder.
///
/// **Deviation:** the port hands out one shared, **mutated in place**
/// `FieldInfo` per name, whereas the reference implementation replaces the `Arc` in the map (so a
/// writer that resolved the field earlier keeps the old value).  In-place
/// mutation keeps every per-field writer consistent with the final
/// `FieldInfos`, which is what the indexing chain relies on when a field is
/// first seen as `docs` and later upgraded to `docs+positions`.
export class FieldInfosBuilder {
public:
    FieldInfosBuilder() = default;

    /// Adds `fi` (its `number` is assigned by the builder) and returns the
    /// field, merging it with a same-named field that was added before.
    [[nodiscard]] Result<std::shared_ptr<FieldInfo>> add(FieldInfo fi);

    /// [`add`] with an explicit doc values generation.
    [[nodiscard]] Result<std::shared_ptr<FieldInfo>> add_with_dv_gen(FieldInfo fi, int64_t dv_gen);

    /// Adds a field that already belongs to another segment, for a merge: the
    /// incoming `number` is discarded and a fresh one assigned, because the
    /// merged segment numbers its fields from scratch.
    [[nodiscard]] Result<std::shared_ptr<FieldInfo>> add_existing(FieldInfo fi);

    /// The field with that name, or `nullptr`.
    [[nodiscard]] std::shared_ptr<FieldInfo> field_info(std::string_view name) const;

    /// Builds the immutable collection (empty when no field was added).
    [[nodiscard]] Result<FieldInfos> finish() const;

    [[nodiscard]] std::set<std::string> get_field_names() const;
    [[nodiscard]] size_t size() const noexcept { return fields_.size(); }

    void clear() noexcept;

    [[nodiscard]] std::optional<std::string> soft_deletes_field() const;
    [[nodiscard]] std::optional<std::string> parent_field() const;
    [[nodiscard]] bool is_soft_deletes_field_name(std::string_view field_name) const;
    [[nodiscard]] bool is_parent_field_name(std::string_view field_name) const;

private:
    std::map<std::string, std::shared_ptr<FieldInfo>, std::less<>> by_name_;
    /// Ordered by field number, which is the order `.fnm` stores.
    std::vector<std::shared_ptr<FieldInfo>> fields_;
};

} // namespace lucene
