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
/// Port of `ReverseBytesReader` usage in `read_write_data_output.rs`, except
/// that the direction is forward: this project's [`FstBuilder`] appends nodes
/// as it serializes them (children first, so a parent's address is always
/// greater than its children's), which means the FST data is read front to
/// back.  The reference implementation writes nodes backwards and therefore needs a reverse reader.
export class OwnedBytesReader final : public FstBytesReader {
public:
    OwnedBytesReader(std::shared_ptr<const std::vector<uint8_t>> bytes, size_t offset, size_t length);

    [[nodiscard]] Result<uint8_t> read_byte() override;
    [[nodiscard]] VoidResult read_bytes(std::span<uint8_t> b) override;
    [[nodiscard]] VoidResult skip_bytes(int64_t num_bytes) override;

    [[nodiscard]] int64_t get_position() const override { return pos_; }
    void set_position(int64_t pos) override { pos_ = pos; }

    [[nodiscard]] std::string to_string() const override { return "OwnedBytesReader"; }

private:
    std::shared_ptr<const std::vector<uint8_t>> bytes_;
    size_t offset_{0};
    size_t length_{0};
    int64_t pos_{0};
};

/// A [`FstBytesReader`] over a **borrowed** buffer.
///
/// The caller guarantees the buffer outlives the reader; this is the C++
/// counterpart of `OffHeapFSTStore`'s `ReverseRandomAccessReader`, which reads
/// straight out of an `IndexInput` slice.
export class SliceBytesReader final : public FstBytesReader {
public:
    explicit SliceBytesReader(std::span<const uint8_t> bytes) : bytes_(bytes) {}

    [[nodiscard]] Result<uint8_t> read_byte() override;
    [[nodiscard]] VoidResult read_bytes(std::span<uint8_t> b) override;
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

    /// A new reader positioned at the first byte; the store must outlive it
    /// unless it shares ownership (as [`OnHeapFstStore`] does).
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
