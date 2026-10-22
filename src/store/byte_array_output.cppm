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

export module lucene_core:byte_array_output;

import :types;
import std;

import :data_output;
import :error;
import :index_output;

namespace lucene {

/// An [`IndexOutput`] that keeps everything in memory.
///
/// The stand-in for the reference implementation's `ByteBuffersIndexOutput` / `ByteArrayDataOutput`
/// and for Lucene's `ByteBuffersDataOutput`: the doc values format buffers a
/// block of terms or of packed values, then measures it and copies it to the real
/// file, and the monotonic writers need a temporary data output they can drain
/// afterwards.
///
/// The checksum is always `0`; nothing that writes through this output records one.
export class ByteArrayOutput final : public IndexOutput {
public:
    explicit ByteArrayOutput(std::string name = "temp") : name_(std::move(name)) {}

    [[nodiscard]] VoidResult write_byte(uint8_t b) override {
        if (position_ == bytes_.size()) {
            bytes_.push_back(b);
        } else {
            bytes_[position_] = b;
        }
        ++position_;
        return {};
    }

    [[nodiscard]] VoidResult write_bytes(std::span<const uint8_t> b, size_t offset, size_t length) override {
        if (offset > b.size() || length > b.size() - offset) {
            return err(LuceneError::illegal_argument("writing past the end of the source buffer"));
        }
        if (bytes_.size() < position_ + length) {
            bytes_.resize(position_ + length, 0);
        }
        std::copy_n(b.begin() + static_cast<std::ptrdiff_t>(offset), length,
                    bytes_.begin() + static_cast<std::ptrdiff_t>(position_));
        position_ += length;
        return {};
    }

    [[nodiscard]] Result<size_t> get_file_pointer() const override { return position_; }

    [[nodiscard]] uint64_t get_checksum() override { return 0; }

    [[nodiscard]] std::string_view get_name() const override { return name_; }

    [[nodiscard]] VoidResult close() override {
        closed_ = true;
        return {};
    }

    /// Conversational alias: appends the whole of `bytes`.
    [[nodiscard]] VoidResult write_all(std::span<const uint8_t> bytes) {
        return write_bytes(bytes, 0, bytes.size());
    }

    /// Bytes written so far.
    [[nodiscard]] std::span<const uint8_t> bytes() const { return bytes_; }

    /// Number of bytes written; the same value as [`get_file_pointer`].
    [[nodiscard]] size_t size() const noexcept { return position_; }

    /// Drops everything written and starts again at position `0`.
    void reset() noexcept {
        bytes_.clear();
        position_ = 0;
    }

    /// Forgets the content without releasing the buffer, keeping the capacity.
    void clear() noexcept {
        bytes_.clear();
        position_ = 0;
    }

    /// Appends everything buffered to `out`.
    [[nodiscard]] VoidResult copy_to(DataOutput &out) const {
        return out.write_bytes(std::span<const uint8_t>(bytes_), 0, position_);
    }

    [[nodiscard]] bool closed() const noexcept { return closed_; }

private:
    std::string name_;
    std::vector<uint8_t> bytes_;
    size_t position_{0};
    bool closed_{false};
};

} // namespace lucene
