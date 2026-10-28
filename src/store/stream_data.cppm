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

export module lucene_core:stream_data;

import :types;
import std;

import :data_input;
import :data_output;
import :error;

namespace lucene {

/// A [`DataOutput`] that writes into a `std::ostream`.
///
/// Port of `OutputStreamDataOutput` (`store/output_stream_data_output.rs`).
/// Useful for exporting an index structure to a stream; the library itself does
/// not use it.
export class OutputStreamDataOutput final : public DataOutput {
public:
    using DataOutput::write_bytes;

    explicit OutputStreamDataOutput(std::ostream &out) : out_(&out) {}

    [[nodiscard]] VoidResult write_byte(uint8_t b) override;
    [[nodiscard]] VoidResult write_bytes(std::span<const uint8_t> b, size_t offset, size_t length) override;
    [[nodiscard]] std::string to_string() const override { return "OutputStreamDataOutput"; }

private:
    std::ostream *out_;
};

/// A [`DataInput`] that reads from a `std::istream`.
///
/// Port of `InputStreamDataInput` (`store/input_stream_data_input.rs`).  The
/// stream is not buffered here; wrap it in a `std::istreambuf_iterator` based
/// buffer upstream if that matters.
export class InputStreamDataInput final : public DataInput {
public:
    explicit InputStreamDataInput(std::istream &in) : in_(&in) {}

    [[nodiscard]] Result<uint8_t> read_byte() override;
    [[nodiscard]] VoidResult read_bytes(std::span<uint8_t> b) override;
    [[nodiscard]] VoidResult skip_bytes(int64_t num_bytes) override;
    [[nodiscard]] std::string to_string() const override { return "InputStreamDataInput"; }

private:
    std::istream *in_;
};

} // namespace lucene
