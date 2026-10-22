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

export module lucene_core:reverse_bytes_reader;

import :types;
import :error;

import std;

import :data_input;
import :fst_bytes_reader;

namespace lucene {

/// Reads a byte array **backwards**, which is how the FST reader walks the
/// nodes it just deserialized.
///
/// Port of `util/fst_impl/reverse_bytes_reader.rs`.  The caller keeps the
/// buffer alive; the reader only holds a span.
export class ReverseBytesReader final : public FstBytesReader {
public:
    explicit ReverseBytesReader(std::span<const uint8_t> bytes) : bytes_(bytes) {}

    [[nodiscard]] Result<uint8_t> read_byte() override;
    [[nodiscard]] VoidResult read_bytes(std::span<uint8_t> b) override;
    [[nodiscard]] VoidResult skip_bytes(int64_t num_bytes) override;

    [[nodiscard]] int64_t get_position() const override { return pos_; }
    void set_position(int64_t pos) override { pos_ = pos; }

    [[nodiscard]] std::string to_string() const override { return "ReverseBytesReader"; }

private:
    std::span<const uint8_t> bytes_;
    int64_t pos_{0};
};

} // namespace lucene
