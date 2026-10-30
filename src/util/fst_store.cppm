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

export module lucene_core:fst_store;

import :types;
import :error;

import std;

import :data_input;
import :data_output;
import :fst_bytes_reader;

namespace lucene {

/// A [`FstBytesReader`] over a heap buffer it **shares ownership** of.
///
/// Port of `ReverseBytesReader`: the FST stores every node byte reversed and
/// addresses it at its **last** byte, so all reads walk backwards (a `position`
/// is a physical offset that decreases on every `read_byte`).  This is what the
/// on-disk format requires — a forward reader silently mis-decodes nodes, and
/// other implementations reject the index.
export class OwnedBytesReader final : public FstBytesReader {
public:
    OwnedBytesReader(std::shared_ptr<const std::vector<uint8_t>> bytes, size_t offset, size_t length);

    /// Both readers keep the raw pointer of their slice and read through it
    /// inline: seeking a term reads a handful of bytes per arc, and a virtual call
    /// that then chased `shared_ptr -> vector -> data` per byte dominated the
    /// profile of every lookup.
    [[nodiscard]] Result<uint8_t> read_byte() override {
        if (pos_ < 0 || static_cast<size_t>(pos_) >= length_) {
            return err(LuceneError::eof("read past the end of the FST data"));
        }
        return data_[static_cast<size_t>(pos_--)];
    }

    [[nodiscard]] VoidResult read_bytes(std::span<uint8_t> b) override {
        if (pos_ < 0 || static_cast<size_t>(pos_) >= length_ ||
            static_cast<size_t>(pos_) + 1 < b.size()) {
            return err(LuceneError::eof("read past the end of the FST data"));
        }
        for (auto &slot : b) {
            slot = data_[static_cast<size_t>(pos_--)];
        }
        return {};
    }

    /// Decoded from the slice directly, in reverse.  The default walks one virtual
    /// `read_byte` per byte, and walking the FST of a term dictionary pays that for
    /// every label, output and skip length on the seek path.
    [[gnu::always_inline]] [[nodiscard]] Result<int32_t> read_vint() override {
        int32_t value = 0;
        int32_t shift = 0;
        int64_t position = pos_;
        while (position >= 0 && shift < 35) {
            const int32_t byte = data_[static_cast<size_t>(position--)];
            value |= static_cast<int32_t>(static_cast<uint32_t>(byte & 0x7F) << shift);
            if ((byte & 0x80) == 0) {
                pos_ = position;
                return value;
            }
            shift += 7;
        }
        // Malformed or truncated: let the generic decoder report it.
        return DataInput::read_vint();
    }

    [[gnu::always_inline]] [[nodiscard]] Result<int64_t> read_vlong() override {
        int64_t value = 0;
        int64_t shift = 0;
        int64_t position = pos_;
        while (position >= 0 && shift < 70) {
            const int64_t byte = data_[static_cast<size_t>(position--)];
            value |= (byte & 0x7F) << shift;
            if ((byte & 0x80) == 0) {
                pos_ = position;
                return value;
            }
            shift += 7;
        }
        return DataInput::read_vlong();
    }

    [[nodiscard]] VoidResult skip_bytes(int64_t num_bytes) override;

    [[nodiscard]] int64_t get_position() const override { return pos_; }
    void set_position(int64_t pos) override { pos_ = pos; }

    [[nodiscard]] std::string to_string() const override { return "OwnedBytesReader"; }

private:
    std::shared_ptr<const std::vector<uint8_t>> bytes_;
    /// `bytes_->data() + offset`; the slice this reader walks.
    const uint8_t *data_{nullptr};
    size_t length_{0};
    int64_t pos_{0};
};

/// A [`FstBytesReader`] over a **borrowed** buffer, read backwards.
///
/// The caller guarantees the buffer outlives the reader; this is the C++
/// counterpart of `OffHeapFSTStore`'s `ReverseRandomAccessReader`, which reads
/// straight out of an `IndexInput` slice.
export class SliceBytesReader final : public FstBytesReader {
public:
    explicit SliceBytesReader(std::span<const uint8_t> bytes) : bytes_(bytes) {}

    [[nodiscard]] Result<uint8_t> read_byte() override {
        if (pos_ < 0 || static_cast<size_t>(pos_) >= bytes_.size()) {
            return err(LuceneError::eof("read past the end of the FST slice"));
        }
        return bytes_[static_cast<size_t>(pos_--)];
    }

    [[nodiscard]] VoidResult read_bytes(std::span<uint8_t> b) override {
        if (pos_ < 0 || static_cast<size_t>(pos_) >= bytes_.size() ||
            static_cast<size_t>(pos_) + 1 < b.size()) {
            return err(LuceneError::eof("read past the end of the FST slice"));
        }
        for (auto &slot : b) {
            slot = bytes_[static_cast<size_t>(pos_--)];
        }
        return {};
    }

    /// See [`OwnedBytesReader::read_vint`]: the same reverse decode over the slice.
    [[gnu::always_inline]] [[nodiscard]] Result<int32_t> read_vint() override {
        int32_t value = 0;
        int32_t shift = 0;
        int64_t position = pos_;
        while (position >= 0 && shift < 35) {
            const int32_t byte = bytes_[static_cast<size_t>(position--)];
            value |= static_cast<int32_t>(static_cast<uint32_t>(byte & 0x7F) << shift);
            if ((byte & 0x80) == 0) {
                pos_ = position;
                return value;
            }
            shift += 7;
        }
        return DataInput::read_vint();
    }

    [[gnu::always_inline]] [[nodiscard]] Result<int64_t> read_vlong() override {
        int64_t value = 0;
        int64_t shift = 0;
        int64_t position = pos_;
        while (position >= 0 && shift < 70) {
            const int64_t byte = bytes_[static_cast<size_t>(position--)];
            value |= (byte & 0x7F) << shift;
            if ((byte & 0x80) == 0) {
                pos_ = position;
                return value;
            }
            shift += 7;
        }
        return DataInput::read_vlong();
    }

    [[nodiscard]] VoidResult skip_bytes(int64_t num_bytes) override;

    [[nodiscard]] int64_t get_position() const override { return pos_; }
    void set_position(int64_t pos) override { pos_ = pos; }

    [[nodiscard]] std::string to_string() const override { return "SliceBytesReader"; }

private:
    std::span<const uint8_t> bytes_;
    int64_t pos_{0};
};

/// Where an [`FST`]'s bytes live.
///
/// Port of the `FstReader` trait (`util/fst_impl/fst_reader.rs`).  The Rust
/// trait has an associated reader type; with type erasure here it returns a
/// `unique_ptr<FstBytesReader>` instead.
export class FstStore {
public:
    virtual ~FstStore() = default;

    /// A new reader over the whole store, read backwards from `num_bytes() - 1`
    /// downwards; the store must outlive it unless it shares ownership (as
    /// [`OnHeapFstStore`] does).
    [[nodiscard]] virtual std::unique_ptr<FstBytesReader> make_reader() const = 0;

    /// Copies the FST bytes into `out`.
    [[nodiscard]] virtual VoidResult write_to(DataOutput &out) const = 0;

    [[nodiscard]] virtual int64_t ram_bytes_used() const = 0;
    [[nodiscard]] virtual int64_t num_bytes() const = 0;
};

/// An FST store that owns its bytes.
///
/// Port of `OnHeapFSTStore`.  The reference implementation additionally switches between a paged and
/// a contiguous reader based on `maxBlockBits`; since this port builds FSTs in
/// one contiguous buffer, that choice disappears and the bytes are simply kept
/// in a `shared_ptr<vector>`, so readers keep them alive on their own.
export class OnHeapFstStore final : public FstStore {
public:
    explicit OnHeapFstStore(std::shared_ptr<const std::vector<uint8_t>> bytes);

    /// Takes ownership of `bytes`.
    explicit OnHeapFstStore(std::vector<uint8_t> bytes);

    /// Reads exactly `num_bytes` bytes from `input`.
    [[nodiscard]] static Result<OnHeapFstStore> read_from(DataInput &input, int64_t num_bytes);

    [[nodiscard]] std::unique_ptr<FstBytesReader> make_reader() const override;
    [[nodiscard]] VoidResult write_to(DataOutput &out) const override;
    [[nodiscard]] int64_t ram_bytes_used() const override;
    [[nodiscard]] int64_t num_bytes() const override;

    [[nodiscard]] std::span<const uint8_t> bytes() const {
        return std::span<const uint8_t>(bytes_->data(), bytes_->size());
    }

    /// `true` when `address` is a readable offset inside the store.
    [[nodiscard]] bool contains(int64_t address) const { return address >= 0 && address < num_bytes(); }

private:
    std::shared_ptr<const std::vector<uint8_t>> bytes_;
};

/// An FST store over a borrowed buffer, e.g. an `IndexInput` slice.
///
/// Port of `OffHeapFSTStore`.  `write_to` is unsupported there and here:
/// the bytes already exist in the index file.
export class OffHeapFstStore final : public FstStore {
public:
    OffHeapFstStore(std::span<const uint8_t> bytes, size_t offset, size_t num_bytes);

    [[nodiscard]] std::unique_ptr<FstBytesReader> make_reader() const override;
    [[nodiscard]] VoidResult write_to(DataOutput &out) const override;
    [[nodiscard]] int64_t ram_bytes_used() const override;
    [[nodiscard]] int64_t num_bytes() const override;

private:
    std::span<const uint8_t> bytes_;
    size_t offset_{0};
    size_t length_{0};
};

} // namespace lucene
