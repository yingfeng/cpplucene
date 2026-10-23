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

export module lucene_core:sort_field_provider;

import :types;
import std;

import :data_input;
import :data_output;
import :error;
import :sort_field;

namespace lucene {

/// Provider name of a scalar [`SortField`] (`SortField`'s registry name).
export constexpr std::string_view SORT_FIELD_PROVIDER = "SortField";
/// Provider name of a multi-valued numeric [`SortField`].
export constexpr std::string_view SORTED_NUMERIC_SORT_FIELD_PROVIDER = "SortedNumericSortField";
/// Provider name of a [`SortField`] over `SortedSetDocValues`.
export constexpr std::string_view SORTED_SET_SORT_FIELD_PROVIDER = "SortedSetSortField";

/// The provider that knows how to (de)serialise `field`.
///
/// Port of `sort_field_provider::for_name` (`core/index/sort_field_provider.rs`),
/// in the other direction: the reference implementation looks the provider up
/// from a name to read, and the port also needs the name to write.
[[nodiscard]] Result<std::string_view> sort_field_provider_name(const SortField &field);

/// Writes the payload of `field`, the part that follows its provider name in the
/// segment metadata.
///
/// Port of `SortFieldProvider::write_sort_field`
/// (`core/index/sort_field_provider.rs`).
///
/// **Deviation:** the port's [`SortField`] has no missing value, so the
/// "has missing value" flag is always written as `0` and a field that has one
/// reads back without it.
[[nodiscard]] VoidResult write_sort_field(const SortField &field, DataOutput &output);

/// Reads the payload written by [`write_sort_field`] for `provider_name`.
///
/// Port of `SortFieldProvider::read_sort_field`
/// (`core/index/sort_field_provider.rs`).
[[nodiscard]] Result<SortField> read_sort_field(std::string_view provider_name, DataInput &input);

} // namespace lucene
