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

module lucene_core:block_packed_reader_iterator.impl;

import :block_packed_reader_iterator;

import :bit_util;
import :block_packed_writer;
import :data_input;
import :error;
import :packed_stream;

import std;

namespace lucene {

BlockPackedReaderIterator::BlockPackedReaderIterator(int32_t packed_ints_version, size_t block_size)
    : values_(block_size, 0),
      block_size_(block_size),
      off_(block_size),
      packed_ints_version_(packed_ints_version) {}

Result<BlockPackedReaderIterator> BlockPackedReaderIterator::create(int32_t packed_ints_version, size_t block_size,
                                                                    size_t value_count) {
    if (block_size < static_cast<size_t>(BlockPackedWriter::MIN_BLOCK_SIZE) ||
        block_size > static_cast<size_t>(BlockPackedWriter::MAX_BLOCK_SIZE)) {
        return err(LuceneError::illegal_argument("block_size must be in [" +
                                                 std::to_string(BlockPackedWriter::MIN_BLOCK_SIZE) + ", " +
                                                 std::to_string(BlockPackedWriter::MAX_BLOCK_SIZE) + "] (got " +
                                                 std::to_string(block_size) + ")"));
    }
    BlockPackedReaderIterator reader(packed_ints_version, block_size);
    reader.value_count_ = value_count;
    return reader;
}

void BlockPackedReaderIterator::reset(size_t value_count) noexcept {
    value_count_ = value_count;
    off_ = block_size_;
    ord_ = 0;
}

Result<int64_t> BlockPackedReaderIterator::read_vlong(DataInput &in) {
    uint64_t value = 0;
    for (int32_t shift = 0; shift < 56; shift += 7) {
        auto byte = in.read_byte();
        if (!byte.has_value()) {
            return std::unexpected(std::move(byte.error()));
        }
        value |= static_cast<uint64_t>(byte.value() & 0x7F) << shift;
        if (static_cast<int8_t>(byte.value()) >= 0) {
            return static_cast<int64_t>(value);
        }
    }
    auto last_byte = in.read_byte();
    if (!last_byte.has_value()) {
        return std::unexpected(std::move(last_byte.error()));
    }
    return static_cast<int64_t>(value | (static_cast<uint64_t>(last_byte.value()) << 56));
}

VoidResult BlockPackedReaderIterator::skip(size_t count, DataInput &in) {
    if (ord_ + count > value_count_) {
        return err(LuceneError::eof("Attempt to skip past end of file"));
    }

    // 1. Consume what is already buffered.
    const size_t buffered = std::min(count, block_size_ - off_);
    off_ += buffered;
    ord_ += buffered;
    count -= buffered;
    if (count == 0) {
        return {};
    }

    // 2. Skip whole blocks without decoding them.
    while (count >= block_size_) {
        auto token = in.read_byte();
        if (!token.has_value()) {
            return std::unexpected(std::move(token.error()));
        }
        const int32_t bits_per_value = static_cast<int32_t>(token.value()) >> BlockPackedWriter::BPV_SHIFT;
        if (bits_per_value > 64) {
            return err(LuceneError::corrupt_index("Corrupted: bits_per_value > 64"));
        }
        if ((token.value() & BlockPackedWriter::MIN_VALUE_EQUALS_0) == 0) {
            auto min = read_vlong(in);
            if (!min.has_value()) {
                return std::unexpected(std::move(min.error()));
            }
        }
        const size_t block_bytes = packed_stream_byte_count(block_size_, bits_per_value);
        if (auto skipped = in.skip_bytes(static_cast<int64_t>(block_bytes)); !skipped.has_value()) {
            return skipped;
        }
        ord_ += block_size_;
        count -= block_size_;
    }

    if (count == 0) {
        return {};
    }

    // 3. Refill the buffer and consume the rest.
    if (auto filled = refill(in); !filled.has_value()) {
        return filled;
    }
    ord_ += count;
    off_ += count;
    return {};
}

Result<int64_t> BlockPackedReaderIterator::next_value(DataInput &in) {
    if (ord_ == value_count_) {
        return err(LuceneError::eof("Reached end of value stream"));
    }
    if (off_ == block_size_) {
        if (auto filled = refill(in); !filled.has_value()) {
            return std::unexpected(std::move(filled.error()));
        }
    }
    const int64_t value = values_[off_];
    ++off_;
    ++ord_;
    return value;
}

Result<std::span<const int64_t>> BlockPackedReaderIterator::next_batch(size_t count, DataInput &in) {
    if (count == 0) {
        return err(LuceneError::illegal_argument("count must be positive"));
    }
    if (ord_ == value_count_) {
        return err(LuceneError::eof("Reached end of value stream"));
    }
    if (off_ == block_size_) {
        if (auto filled = refill(in); !filled.has_value()) {
            return std::unexpected(std::move(filled.error()));
        }
    }
    count = std::min(count, block_size_ - off_);
    count = std::min(count, value_count_ - ord_);
    const std::span<const int64_t> batch(values_.data() + off_, count);
    off_ += count;
    ord_ += count;
    return batch;
}

VoidResult BlockPackedReaderIterator::refill(DataInput &in) {
    auto token = in.read_byte();
    if (!token.has_value()) {
        return std::unexpected(std::move(token.error()));
    }
    const int32_t token_value = static_cast<int32_t>(token.value());
    const bool min_equals_0 = (token_value & BlockPackedWriter::MIN_VALUE_EQUALS_0) != 0;
    const int32_t bits_per_value = token_value >> BlockPackedWriter::BPV_SHIFT;
    if (bits_per_value > 64) {
        return err(LuceneError::corrupt_index("Corrupted: bits_per_value > 64"));
    }

    int64_t min_value = 0;
    if (!min_equals_0) {
        auto encoded = read_vlong(in);
        if (!encoded.has_value()) {
            return std::unexpected(std::move(encoded.error()));
        }
        min_value = BitUtil::zig_zag_decode_i64(static_cast<uint64_t>(encoded.value()) + 1ULL);
    }

    const size_t count = std::min(value_count_ - ord_, block_size_);
    if (bits_per_value == 0) {
        std::fill_n(values_.begin(), count, min_value);
        off_ = 0;
        return {};
    }

    auto decoded = read_packed_stream(in, count, bits_per_value);
    if (!decoded.has_value()) {
        return std::unexpected(std::move(decoded.error()));
    }
    for (size_t i = 0; i < count; ++i) {
        const int64_t value = (*decoded)[i];
        values_[i] = min_value == 0 ? value
                                    : static_cast<int64_t>(static_cast<uint64_t>(value) +
                                                           static_cast<uint64_t>(min_value));
    }
    off_ = 0;
    return {};
}

} // namespace lucene
