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

module lucene_core:bulk_operation.impl;

import :bulk_operation;

import :types;
import std;

import :error;
import :packed_ints;

namespace lucene {

namespace {

template <size_t Index>
[[nodiscard]] const BulkOperation *packed_operation_at() noexcept {
    static const BulkOperationPacked<static_cast<int32_t>(Index + 1)> operation;
    return &operation;
}

template <size_t... Index>
[[nodiscard]] std::array<const BulkOperation *, sizeof...(Index)> make_packed_table(
        std::index_sequence<Index...>) noexcept {
    return {packed_operation_at<Index>()...};
}

/// One operation per bit width, exactly like the reference implementation's
/// `PACKED_BULK_OPS` table; the entries are built on first use.
[[nodiscard]] const std::array<const BulkOperation *, 64> &packed_table() noexcept {
    static const auto table = make_packed_table(std::make_index_sequence<64>{});
    return table;
}

/// The widths `PACKED_SINGLE_BLOCK` supports: those where one 64-bit block holds
/// a whole number of values without wasting bits.  `None` elsewhere.
[[nodiscard]] const std::array<std::unique_ptr<const BulkOperation>, 32> &single_block_table() {
    static const auto table = [] {
        std::array<std::unique_ptr<const BulkOperation>, 32> operations{};
        for (const int32_t bits : {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 12, 16, 21, 32}) {
            operations[static_cast<size_t>(bits - 1)] = std::make_unique<BulkOperationPackedSingleBlock>(bits);
        }
        return operations;
    }();
    return table;
}

} // namespace

size_t BulkOperation::write_long(uint64_t block, std::span<uint8_t> blocks, size_t blocks_offset) {
    for (size_t j = 1; j <= 8; ++j) {
        blocks[blocks_offset] = static_cast<uint8_t>(block >> static_cast<uint32_t>(64 - (j << 3)));
        ++blocks_offset;
    }
    return blocks_offset;
}

int32_t BulkOperation::compute_iterations(int32_t value_count, int32_t ram_budget) const {
    const auto values_per_block = static_cast<int32_t>(byte_value_count());
    const int32_t iterations = ram_budget / (static_cast<int32_t>(byte_block_count()) + 8 * values_per_block);
    if (iterations == 0) {
        return 1;
    }
    if ((iterations - 1) * values_per_block >= value_count) {
        // Do not allocate for more values than the reader holds.
        return static_cast<int32_t>(std::ceil(static_cast<double>(value_count) / static_cast<double>(values_per_block)));
    }
    return iterations;
}

VoidResult PackedBulkStream::decode(std::span<const uint8_t> blocks,
                                    size_t blocks_offset,
                                    std::span<int64_t> values) const {
    const size_t values_per_group = operation_.byte_value_count();
    const size_t bytes_per_group = operation_.byte_block_count();
    if (values_per_group == 0) {
        return err(LuceneError::illegal_state("the bulk operation declares no values per block"));
    }
    const auto bits = static_cast<size_t>(operation_.bits_per_value());
    const size_t full_groups = values.size() / values_per_group;
    const size_t remainder = values.size() % values_per_group;
    const size_t full_bytes = full_groups * bytes_per_group;
    if (blocks_offset + full_bytes > blocks.size()) {
        return err(LuceneError::eof("packed stream is truncated"));
    }
    if (full_groups != 0) {
        operation_.decode_u8_to_i64(blocks, blocks_offset, values, 0, full_groups);
    }
    if (remainder != 0) {
        const size_t needed = (remainder * bits + 7) / 8;
        if (blocks_offset + full_bytes + needed > blocks.size()) {
            return err(LuceneError::eof("packed stream is truncated"));
        }
        std::vector<uint8_t> tail(bytes_per_group, 0);
        std::copy_n(blocks.begin() + static_cast<std::ptrdiff_t>(blocks_offset + full_bytes), needed, tail.begin());
        std::vector<int64_t> scratch(values_per_group, 0);
        operation_.decode_u8_to_i64(tail, 0, scratch, 0, 1);
        std::copy_n(scratch.begin(), remainder, values.begin() + static_cast<std::ptrdiff_t>(full_groups * values_per_group));
    }
    return {};
}

VoidResult PackedBulkStream::encode(std::span<const int64_t> values,
                                    size_t values_offset,
                                    std::span<uint8_t> blocks,
                                    size_t blocks_offset) const {
    const size_t values_per_group = operation_.byte_value_count();
    const size_t bytes_per_group = operation_.byte_block_count();
    if (values_per_group == 0) {
        return err(LuceneError::illegal_state("the bulk operation declares no values per block"));
    }
    if (values_offset > values.size()) {
        return err(LuceneError::illegal_argument("values_offset is outside the value array"));
    }
    const auto bits = static_cast<size_t>(operation_.bits_per_value());
    const size_t count = values.size() - values_offset;
    const size_t full_groups = count / values_per_group;
    const size_t remainder = count % values_per_group;
    const size_t full_bytes = full_groups * bytes_per_group;
    const size_t tail_bytes = remainder == 0 ? 0 : (remainder * bits + 7) / 8;
    if (blocks_offset + full_bytes + tail_bytes > blocks.size()) {
        return err(LuceneError::illegal_argument("the block buffer is too small for the encoded stream"));
    }
    if (full_groups != 0) {
        operation_.encode_i64_to_u8(values.subspan(values_offset), 0, blocks, blocks_offset, full_groups);
    }
    if (remainder != 0) {
        // Pad the last group with zeros and copy only the bytes the values occupy.
        std::vector<int64_t> scratch(values_per_group, 0);
        std::copy_n(values.begin() + static_cast<std::ptrdiff_t>(values_offset + full_groups * values_per_group), remainder,
                    scratch.begin());
        std::vector<uint8_t> tail(bytes_per_group, 0);
        operation_.encode_i64_to_u8(scratch, 0, tail, 0, 1);
        std::copy_n(tail.begin(), tail_bytes, blocks.begin() + static_cast<std::ptrdiff_t>(blocks_offset + full_bytes));
    }
    return {};
}

Result<PackedBulkStream> bulk_operation(int32_t bits_per_value) {
    auto operation = bulk_operation_of(PackedFormat::kPacked, bits_per_value);
    if (!operation) {
        return std::unexpected(std::move(operation.error()));
    }
    return PackedBulkStream(*operation.value());
}

Result<const BulkOperation *> bulk_operation_of(PackedFormat format, int32_t bits_per_value) {
    switch (format) {
        case PackedFormat::kPacked: {
            if (bits_per_value < 1 || bits_per_value > 64) {
                return err(LuceneError::array_index_out_of_bounds(
                        "invalid bits_per_value for PACKED: " + std::to_string(bits_per_value)));
            }
            return packed_table()[static_cast<size_t>(bits_per_value - 1)];
        }
        case PackedFormat::kPackedSingleBlock: {
            if (bits_per_value < 1 || bits_per_value > 32) {
                return err(LuceneError::array_index_out_of_bounds(
                        "invalid bits_per_value for PACKED_SINGLE_BLOCK: " + std::to_string(bits_per_value)));
            }
            const auto &operation = single_block_table()[static_cast<size_t>(bits_per_value - 1)];
            if (operation == nullptr) {
                return err(LuceneError::illegal_argument(
                        "unsupported bits_per_value for PACKED_SINGLE_BLOCK: " + std::to_string(bits_per_value)));
            }
            return operation.get();
        }
    }
    return err(LuceneError::illegal_argument("unknown packed format"));
}

} // namespace lucene
