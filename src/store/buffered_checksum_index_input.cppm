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

export module lucene_core:buffered_checksum_index_input;

import :types;
import std;

import :buffered_checksum;
import :check_sum_index_input;
import :checksum;
import :error;
import :index_input;

namespace lucene {

/// A [`CheckSumIndexInput`] over another [`IndexInput`], computing the CRC32 of
/// everything it reads through a [`BufferedChecksum`].
///
/// Port of `BufferedChecksumIndexInput`
/// (`core/store/buffered_checksum_index_input.rs`).
export class BufferedChecksumIndexInput : public CheckSumIndexInput {
public:
    /// Takes ownership of `main`, which must stay positioned by this wrapper
    /// only.
    explicit BufferedChecksumIndexInput(std::unique_ptr<IndexInput> main);

    [[nodiscard]] Result<size_t> get_file_pointer() const override;
    [[nodiscard]] Result<size_t> length() const override;

    [[nodiscard]] Result<uint8_t> read_byte() override;
    [[nodiscard]] VoidResult read_bytes(std::span<uint8_t> b) override;
    [[nodiscard]] VoidResult skip_bytes(int64_t num_bytes) override;

    [[nodiscard]] Result<int16_t> read_short() override;
    [[nodiscard]] Result<int32_t> read_int() override;
    [[nodiscard]] Result<int64_t> read_long() override;

    [[nodiscard]] Result<std::unique_ptr<IndexInput>> clone() const override;
    [[nodiscard]] Result<std::unique_ptr<RandomAccessInput>> random_access_slice(size_t offset,
                                                                                size_t length) const override;
    [[nodiscard]] VoidResult prefetch(size_t pos, size_t len) override;
    [[nodiscard]] VoidResult close() override;

    [[nodiscard]] int64_t get_checksum() override;
    [[nodiscard]] VoidResult skip_by_reading(size_t num_bytes) override;

    [[nodiscard]] std::string to_string() const override;

private:
    std::unique_ptr<IndexInput> main_;
    BufferedChecksum digest_;
};

} // namespace lucene
