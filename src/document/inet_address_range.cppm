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

export module lucene_core:inet_address_range;

import :types;
import std;

import :error;
import :field_type;
import :indexable_field;
import :inet_address_point;
import :range_field_query;
import :token_stream;

namespace lucene {

/// An indexed IP address range field: one `[min, max]` pair of addresses,
/// indexed as two 16-byte sortable points.
///
/// Port of `InetAddressRange` (`core/document/inet_address_range.rs`).
///
/// **Deviation:** addresses are the 16-byte encoding of
/// [`InetAddressPoint`] (see [`InetAddressPoint::parse`] / [`to_string`]) instead
/// of `std::net::IpAddr`.
export class InetAddressRange final : public IndexableField {
public:
    /// Bytes of one dimension, synced with [`InetAddressPoint::BYTES`].
    static constexpr size_t BYTES = InetAddressPoint::BYTES;

    using Address = InetAddressPoint::Address;

    /// A range field over `[min, max]`; `min` must not sort after `max`.
    [[nodiscard]] static Result<InetAddressRange> create(std::string name, const Address &min, const Address &max);

    [[nodiscard]] Result<Address> get_min() const;
    [[nodiscard]] Result<Address> get_max() const;

    /// Re-encodes the range; the range has exactly one dimension.
    [[nodiscard]] VoidResult set_range_values(const Address &min, const Address &max);

    // ---- queries ----

    /// Matches the indexed ranges that intersect the given range.
    [[nodiscard]] static Result<RangeFieldQuery> new_intersects_query(std::string field, const Address &min,
                                                                     const Address &max);
    /// Matches the indexed ranges that contain the given range.
    [[nodiscard]] static Result<RangeFieldQuery> new_contains_query(std::string field, const Address &min,
                                                                   const Address &max);
    /// Matches the indexed ranges that lie within the given range.
    [[nodiscard]] static Result<RangeFieldQuery> new_within_query(std::string field, const Address &min,
                                                                 const Address &max);
    /// Matches the indexed ranges that cross the given range.
    [[nodiscard]] static Result<RangeFieldQuery> new_crosses_query(std::string field, const Address &min,
                                                                  const Address &max);

    /// The encoding of one `[min, max]` pair: `min` then `max`, 16 bytes each.
    [[nodiscard]] static Result<std::vector<uint8_t>> encode(const Address &min, const Address &max);

    /// Decodes the lower bound of an encoded range.
    [[nodiscard]] static Result<Address> decode_min(std::span<const uint8_t> bytes);
    /// Decodes the upper bound of an encoded range.
    [[nodiscard]] static Result<Address> decode_max(std::span<const uint8_t> bytes);

    /// Renders one encoded range the way the queries describe it: `[min : max]`.
    [[nodiscard]] static Result<std::string> format_range(std::span<const uint8_t> bytes);

    // ---- IndexableField ----

    [[nodiscard]] std::string_view name() const override { return field_.name(); }
    [[nodiscard]] const FieldType &field_type() const override { return field_.field_type(); }
    [[nodiscard]] std::optional<std::string_view> string_value() const override { return field_.string_value(); }
    [[nodiscard]] std::optional<std::vector<uint8_t>> binary_value() const override { return field_.binary_value(); }
    /// Always `nullopt`: a range cannot be read as one number.
    [[nodiscard]] std::optional<Number> numeric_value() const override { return std::nullopt; }
    [[nodiscard]] InvertableType invertable_type() const override { return field_.invertable_type(); }
    [[nodiscard]] Result<TokenStream *> token_stream(Analyzer &analyzer) const override {
        return field_.token_stream(analyzer);
    }
    [[nodiscard]] std::string to_string() const override;

private:
    explicit InetAddressRange(Field field) : field_(std::move(field)) {}

    [[nodiscard]] static Result<FieldType> type();
    [[nodiscard]] static Result<RangeFieldQuery> new_relation_query(std::string field, const Address &min,
                                                                   const Address &max, QueryType relation);

    Field field_;
};

} // namespace lucene
