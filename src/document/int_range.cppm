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

export module lucene_core:int_range;

import :types;
import std;

import :binary_range_field_range_query;
import :error;
import :field_type;
import :indexable_field;
import :range_field_query;
import :token_stream;

namespace lucene {

/// An indexed integer range field: up to four `[min, max]` dimensions, indexed as
/// `2 * dimensions` sortable integer points.
///
/// Port of `IntRange` (`core/document/int_range.rs`).
///
/// Multiple values for the same field in one document are supported, and open
/// ended ranges are spelled with `INT32_MIN` / `INT32_MAX`.
///
/// **Deviation:** `numeric_value()` reports `nullopt` instead of failing, because
/// the port's [`IndexableField`] returns an optional without an error channel.
export class IntRange final : public IndexableField {
public:
    /// Bytes of one dimension.
    static constexpr size_t BYTES = sizeof(int32_t);
    /// The most dimensions a range field can have.
    static constexpr size_t MAX_DIMENSIONS = 4;

    /// Lower and upper bounds must have the same length, between 1 and 4.
    [[nodiscard]] static Result<IntRange> create(std::string name, std::span<const int32_t> min,
                                                 std::span<const int32_t> max);

    [[nodiscard]] Result<int32_t> get_min(size_t dimension) const;
    [[nodiscard]] Result<int32_t> get_max(size_t dimension) const;

    /// Re-encodes the range; the dimension count cannot change.
    [[nodiscard]] VoidResult set_range_values(std::span<const int32_t> min, std::span<const int32_t> max);

    // ---- queries ----

    /// Matches the indexed ranges that intersect the given range.
    [[nodiscard]] static Result<RangeFieldQuery> new_intersects_query(std::string field, std::span<const int32_t> min,
                                                                     std::span<const int32_t> max);
    /// Matches the indexed ranges that contain the given range.
    [[nodiscard]] static Result<RangeFieldQuery> new_contains_query(std::string field, std::span<const int32_t> min,
                                                                   std::span<const int32_t> max);
    /// Matches the indexed ranges that lie within the given range.
    [[nodiscard]] static Result<RangeFieldQuery> new_within_query(std::string field, std::span<const int32_t> min,
                                                                 std::span<const int32_t> max);
    /// Matches the indexed ranges that cross the given range.
    [[nodiscard]] static Result<RangeFieldQuery> new_crosses_query(std::string field, std::span<const int32_t> min,
                                                                  std::span<const int32_t> max);

    /// Decodes the lower bound of `dimension` from an encoded range, as written by
    /// `create` and as the doc values range fields store it.
    [[nodiscard]] static Result<int32_t> decode_min(std::span<const uint8_t> bytes, size_t dimension);
    /// Decodes the upper bound of `dimension` from an encoded range.
    [[nodiscard]] static Result<int32_t> decode_max(std::span<const uint8_t> bytes, size_t dimension);

    /// Encodes `min`/`max` the way the point index and the doc values range
    /// fields store them.
    [[nodiscard]] static Result<std::vector<uint8_t>> encode(std::span<const int32_t> min,
                                                             std::span<const int32_t> max);

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
    explicit IntRange(Field field) : field_(std::move(field)) {}

    [[nodiscard]] static Result<FieldType> type_for(size_t dimensions);
    [[nodiscard]] static VoidResult check_args(std::span<const int32_t> min, std::span<const int32_t> max);
    [[nodiscard]] static VoidResult verify_and_encode(std::span<const int32_t> min, std::span<const int32_t> max,
                                                      std::span<uint8_t> bytes);
    [[nodiscard]] static Result<RangeFieldQuery> new_relation_query(std::string field, std::span<const int32_t> min,
                                                                    std::span<const int32_t> max, QueryType relation);

    Field field_;
};


/// A binary doc values field holding the encoded range, for the "slow" range
/// queries that do not use a point index.
///
/// Port of `IntRangeDocValuesField`
/// (`core/document/int_range_doc_values_field.rs`).
///
/// **Deviation:** the reference implementation's field object also caches
/// `get_min`/`get_max`; the port's field factories return a plain [`Field`], so the
/// bounds are decoded from the stored bytes with [`IntRange::decode_min`] /
/// [`IntRange::decode_max`].
export struct IntRangeDocValuesField {
    [[nodiscard]] static Result<Field> create(std::string name, std::span<const int32_t> min,
                                             std::span<const int32_t> max);
    /// Re-encodes the range; the dimension count cannot change.
    [[nodiscard]] static VoidResult set_range_values(Field &field, std::span<const int32_t> min,
                                                     std::span<const int32_t> max);
    /// Matches the encoded ranges that intersect the given range, reading doc
    /// values only.  This does not use the point index and may be slow.
    [[nodiscard]] static Result<BinaryRangeFieldRangeQuery>
    new_slow_intersects_query(std::string field, std::span<const int32_t> min, std::span<const int32_t> max);
};

} // namespace lucene
