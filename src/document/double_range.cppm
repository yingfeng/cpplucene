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

export module lucene_core:double_range;

import :types;
import std;

import :binary_range_field_range_query;
import :error;
import :field_type;
import :indexable_field;
import :range_field_query;
import :token_stream;

namespace lucene {

/// An indexed double range field; `NaN` endpoints are rejected.
///
/// Port of `DoubleRange` (`core/document/double_range.rs`).
export class DoubleRange final : public IndexableField {
public:
    /// Bytes of one dimension.
    static constexpr size_t BYTES = sizeof(double);
    /// The most dimensions a range field can have.
    static constexpr size_t MAX_DIMENSIONS = 4;

    [[nodiscard]] static Result<DoubleRange> create(std::string name, std::span<const double> min,
                                                    std::span<const double> max);

    [[nodiscard]] Result<double> get_min(size_t dimension) const;
    [[nodiscard]] Result<double> get_max(size_t dimension) const;

    /// Re-encodes the range; the dimension count cannot change.
    [[nodiscard]] VoidResult set_range_values(std::span<const double> min, std::span<const double> max);

    // ---- queries ----

    [[nodiscard]] static Result<RangeFieldQuery> new_intersects_query(std::string field, std::span<const double> min,
                                                                     std::span<const double> max);
    [[nodiscard]] static Result<RangeFieldQuery> new_contains_query(std::string field, std::span<const double> min,
                                                                   std::span<const double> max);
    [[nodiscard]] static Result<RangeFieldQuery> new_within_query(std::string field, std::span<const double> min,
                                                                 std::span<const double> max);
    [[nodiscard]] static Result<RangeFieldQuery> new_crosses_query(std::string field, std::span<const double> min,
                                                                  std::span<const double> max);

    /// Decodes the lower bound of `dimension` from an encoded range, as written by
    /// `create` and as the doc values range fields store it.
    [[nodiscard]] static Result<double> decode_min(std::span<const uint8_t> bytes, size_t dimension);
    /// Decodes the upper bound of `dimension` from an encoded range.
    [[nodiscard]] static Result<double> decode_max(std::span<const uint8_t> bytes, size_t dimension);

    /// Encodes `min`/`max` the way the point index and the doc values range
    /// fields store them.
    [[nodiscard]] static Result<std::vector<uint8_t>> encode(std::span<const double> min,
                                                             std::span<const double> max);

    // ---- IndexableField ----

    [[nodiscard]] std::string_view name() const override { return field_.name(); }
    [[nodiscard]] const FieldType &field_type() const override { return field_.field_type(); }
    [[nodiscard]] std::optional<std::string_view> string_value() const override { return field_.string_value(); }
    [[nodiscard]] std::optional<std::vector<uint8_t>> binary_value() const override { return field_.binary_value(); }
    [[nodiscard]] std::optional<Number> numeric_value() const override { return std::nullopt; }
    [[nodiscard]] InvertableType invertable_type() const override { return field_.invertable_type(); }
    [[nodiscard]] Result<TokenStream *> token_stream(Analyzer &analyzer) const override {
        return field_.token_stream(analyzer);
    }
    [[nodiscard]] std::string to_string() const override;

private:
    explicit DoubleRange(Field field) : field_(std::move(field)) {}

    [[nodiscard]] static Result<FieldType> type_for(size_t dimensions);
    [[nodiscard]] static VoidResult check_args(std::span<const double> min, std::span<const double> max);
    [[nodiscard]] static VoidResult verify_and_encode(std::span<const double> min, std::span<const double> max,
                                                      std::span<uint8_t> bytes);
    [[nodiscard]] static Result<RangeFieldQuery> new_relation_query(std::string field, std::span<const double> min,
                                                                    std::span<const double> max, QueryType relation);

    Field field_;
};


/// A binary doc values field holding the encoded range, for the "slow" range
/// queries that do not use a point index.
///
/// Port of `DoubleRangeDocValuesField`
/// (`core/document/double_range_doc_values_field.rs`).
///
/// **Deviation:** the reference implementation's field object also caches
/// `get_min`/`get_max`; the port's field factories return a plain [`Field`], so the
/// bounds are decoded from the stored bytes with [`DoubleRange::decode_min`] /
/// [`DoubleRange::decode_max`].
export struct DoubleRangeDocValuesField {
    [[nodiscard]] static Result<Field> create(std::string name, std::span<const double> min,
                                             std::span<const double> max);
    /// Re-encodes the range; the dimension count cannot change.
    [[nodiscard]] static VoidResult set_range_values(Field &field, std::span<const double> min,
                                                     std::span<const double> max);
    /// Matches the encoded ranges that intersect the given range, reading doc
    /// values only.  This does not use the point index and may be slow.
    [[nodiscard]] static Result<BinaryRangeFieldRangeQuery>
    new_slow_intersects_query(std::string field, std::span<const double> min, std::span<const double> max);
};

} // namespace lucene
