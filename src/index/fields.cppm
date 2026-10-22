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

export module lucene_core:fields;

import :types;
import std;

import :error;
import :terms;

namespace lucene {

/// Iterates the field names of a [`Fields`], in sorted order.
///
/// Port of the `Fields::FieldIter` GAT (`core/index/fields.rs`).
///
/// **Deviation:** the reference implementation's `IteratorExt` yields `&String` borrowed from the
/// `Fields`; the type-erased version here yields a borrowed `const std::string *`
/// that stays valid as long as the owning `Fields` (the reference implementation's `Arc` keeps it
/// alive for the same reason).  A null pointer means "end of iteration", like
/// `BytesRefIterator::next`.
export class FieldNameIterator {
public:
    virtual ~FieldNameIterator() = default;

    /// The next field name, or a null pointer once the iteration is exhausted.
    [[nodiscard]] virtual Result<const std::string *> next() = 0;
};

/// Provides a [`Terms`] index for fields that have one, and lists which fields
/// do.
///
/// Port of the `Fields` trait (`core/index/fields.rs`).
///
/// **Deviations** (C++ mechanics only):
///  * the associated `FieldIter` / `Terms` types collapse into
///    `std::unique_ptr<FieldNameIterator>` / `std::unique_ptr<Terms>`, the same
///    type erasure the rest of the port uses for the `Terms` hierarchy;
///  * `terms` returns a null pointer instead of `None`.
export class Fields {
public:
    virtual ~Fields() = default;

    /// Steps through all field names; never null.
    [[nodiscard]] virtual Result<std::unique_ptr<FieldNameIterator>> iterator() const = 0;

    /// The [`Terms`] of `field`, or a null pointer when the field does not
    /// exist.
    [[nodiscard]] virtual Result<std::unique_ptr<Terms>> terms(std::string_view field) const = 0;

    /// Number of fields, or `-1` when the number of distinct field names is
    /// unknown.  When `>= 0`, [`iterator`] returns that many names.
    [[nodiscard]] virtual Result<int32_t> size() const = 0;
};

} // namespace lucene
