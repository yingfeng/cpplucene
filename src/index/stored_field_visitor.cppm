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

export module lucene_core:stored_field_visitor;

import :types;
import std;

import :error;
import :field_infos;

namespace lucene {

/// Whether a visitor wants a field it was asked about.
export enum class Status : uint8_t {
    /// Load the field.
    Yes = 0,
    /// Skip the field but keep visiting the document.
    No = 1,
    /// Stop visiting the document.
    Stop = 2,
};

/// Receives the stored fields of a document one at a time.
///
/// Port of `StoredFieldVisitor` (`index/stored_field_visitor.rs`).  A collector
/// is handed the fields it asked for in [`needs_field`] order, and can stop the
/// traversal early with [`Status::Stop`], which is what makes it cheap to load a
/// handful of fields out of a large document.
///
/// **Deviation:** the callbacks take no `StoredFieldsWriter`.  The reference implementation threads
/// the writer through so a merge can re-serialize fields without an intermediate
/// `Document`; this port has no merge path yet, so the parameter is gone.
export class StoredFieldVisitor {
public:
    virtual ~StoredFieldVisitor() = default;

    /// Whether the field is wanted.
    [[nodiscard]] virtual Status needs_field(const FieldInfo &field_info) = 0;

    [[nodiscard]] virtual VoidResult binary_field(const FieldInfo &field_info,
                                                 std::span<const uint8_t> value) = 0;
    [[nodiscard]] virtual VoidResult string_field(const FieldInfo &field_info, std::string_view value) = 0;
    [[nodiscard]] virtual VoidResult int_field(const FieldInfo &field_info, int32_t value) = 0;
    [[nodiscard]] virtual VoidResult long_field(const FieldInfo &field_info, int64_t value) = 0;
    [[nodiscard]] virtual VoidResult float_field(const FieldInfo &field_info, float value) = 0;
    [[nodiscard]] virtual VoidResult double_field(const FieldInfo &field_info, double value) = 0;
};

} // namespace lucene
