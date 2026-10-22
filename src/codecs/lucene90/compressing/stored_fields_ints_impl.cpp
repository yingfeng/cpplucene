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

module lucene_core:stored_fields_ints.impl;

import :stored_fields_ints;

import :data_input;
import :data_output;
import :error;
import :index_input;
import :types;

import std;

namespace lucene {

VoidResult StoredFieldsInts::write_ints(const std::vector<int32_t> &values, size_t start, size_t count,
                                        DataOutput &out) {
    if (start + count > values.size()) {
        return err(LuceneError::illegal_argument("the values array is shorter than the range to write"));
    }
    bool all_equal = true;
    if (count > 1) {
        const int32_t first = values[start];
        if (values[start + 1] != first) {
            all_equal = false;
        } else {
            for (size_t i = 2; i < count; ++i) {
                if (values[start + i] != first) {
                    all_equal = false;
                    break;
                }
            }
        }
    }

    if (all_equal) {
        if (auto written = out.write_byte(0); !written.has_value()) {
            return written;
        }
        return out.write_vint(values[start]);
    }

    uint32_t max = 0;
    for (size_t i = 0; i < count; ++i) {
        max |= static_cast<uint32_t>(values[start + i]);
    }
    if (max <= 0xFF) {
        if (auto written = out.write_byte(8); !written.has_value()) {
            return written;
        }
        return write_ints8(out, count, values, start);
    }
    if (max <= 0xFFFF) {
        if (auto written = out.write_byte(16); !written.has_value()) {
            return written;
        }
        return write_ints16(out, count, values, start);
    }
    if (auto written = out.write_byte(32); !written.has_value()) {
        return written;
    }
    return write_ints32(out, count, values, start);
}

VoidResult StoredFieldsInts::write_ints8(DataOutput &out, size_t count, const std::vector<int32_t> &values,
                                         size_t offset) {
    size_t k = 0;
    while (count - k >= BLOCK_SIZE) {
        const size_t step = offset + k;
        for (size_t i = 0; i < 16; ++i) {
            const auto l = (static_cast<int64_t>(values[step + i]) << 56) |
                           (static_cast<int64_t>(values[step + 16 + i]) << 48) |
                           (static_cast<int64_t>(values[step + 32 + i]) << 40) |
                           (static_cast<int64_t>(values[step + 48 + i]) << 32) |
                           (static_cast<int64_t>(values[step + 64 + i]) << 24) |
                           (static_cast<int64_t>(values[step + 80 + i]) << 16) |
                           (static_cast<int64_t>(values[step + 96 + i]) << 8) |
                           static_cast<int64_t>(values[step + 112 + i]);
            if (auto written = out.write_long(l); !written.has_value()) {
                return written;
            }
        }
        k += BLOCK_SIZE;
    }
    for (size_t i = k; i < count; ++i) {
        if (auto written = out.write_byte(static_cast<uint8_t>(values[offset + i])); !written.has_value()) {
            return written;
        }
    }
    return {};
}

VoidResult StoredFieldsInts::write_ints16(DataOutput &out, size_t count, const std::vector<int32_t> &values,
                                          size_t offset) {
    size_t k = 0;
    while (count - k >= BLOCK_SIZE) {
        const size_t step = offset + k;
        for (size_t i = 0; i < 32; ++i) {
            const auto l = (static_cast<int64_t>(values[step + i]) << 48) |
                           (static_cast<int64_t>(values[step + 32 + i]) << 32) |
                           (static_cast<int64_t>(values[step + 64 + i]) << 16) |
                           static_cast<int64_t>(values[step + 96 + i]);
            if (auto written = out.write_long(l); !written.has_value()) {
                return written;
            }
        }
        k += BLOCK_SIZE;
    }
    for (size_t i = k; i < count; ++i) {
        if (auto written = out.write_short(static_cast<int16_t>(values[offset + i])); !written.has_value()) {
            return written;
        }
    }
    return {};
}

VoidResult StoredFieldsInts::write_ints32(DataOutput &out, size_t count, const std::vector<int32_t> &values,
                                          size_t offset) {
    size_t k = 0;
    while (count - k >= BLOCK_SIZE) {
        const size_t step = offset + k;
        for (size_t i = 0; i < 64; ++i) {
            const auto l = (static_cast<int64_t>(values[step + i]) << 32) |
                           static_cast<int64_t>(values[step + 64 + i]);
            if (auto written = out.write_long(l); !written.has_value()) {
                return written;
            }
        }
        k += BLOCK_SIZE;
    }
    for (size_t i = k; i < count; ++i) {
        if (auto written = out.write_int(values[offset + i]); !written.has_value()) {
            return written;
        }
    }
    return {};
}

VoidResult StoredFieldsInts::read_ints(IndexInput &input, size_t count, std::vector<int64_t> &values,
                                       size_t offset) {
    if (offset + count > values.size()) {
        return err(LuceneError::illegal_argument("the values array is shorter than the range to read"));
    }
    auto bpv = input.read_byte();
    if (!bpv.has_value()) {
        return std::unexpected(std::move(bpv.error()));
    }
    switch (bpv.value()) {
        case 0: {
            auto value = input.read_vint();
            if (!value.has_value()) {
                return std::unexpected(std::move(value.error()));
            }
            std::fill(values.begin() + static_cast<std::ptrdiff_t>(offset),
                      values.begin() + static_cast<std::ptrdiff_t>(offset + count), value.value());
            return {};
        }
        case 8:
            return read_ints8(input, count, values, offset);
        case 16:
            return read_ints16(input, count, values, offset);
        case 32:
            return read_ints32(input, count, values, offset);
        default:
            return err(LuceneError::illegal_state("unsupported number of bits per value: " +
                                                  std::to_string(bpv.value())));
    }
}

VoidResult StoredFieldsInts::read_ints8(IndexInput &input, size_t count, std::vector<int64_t> &values,
                                        size_t offset) {
    size_t k = 0;
    while (count - k >= BLOCK_SIZE) {
        const size_t step = offset + k;
        for (size_t i = 0; i < 16; ++i) {
            auto l = input.read_long();
            if (!l.has_value()) {
                return std::unexpected(std::move(l.error()));
            }
            const int64_t packed = l.value();
            values[step + i] = (packed >> 56) & 0xFF;
            values[step + 16 + i] = (packed >> 48) & 0xFF;
            values[step + 32 + i] = (packed >> 40) & 0xFF;
            values[step + 48 + i] = (packed >> 32) & 0xFF;
            values[step + 64 + i] = (packed >> 24) & 0xFF;
            values[step + 80 + i] = (packed >> 16) & 0xFF;
            values[step + 96 + i] = (packed >> 8) & 0xFF;
            values[step + 112 + i] = packed & 0xFF;
        }
        k += BLOCK_SIZE;
    }
    for (size_t i = k; i < count; ++i) {
        auto value = input.read_byte();
        if (!value.has_value()) {
            return std::unexpected(std::move(value.error()));
        }
        values[offset + i] = value.value();
    }
    return {};
}

VoidResult StoredFieldsInts::read_ints16(IndexInput &input, size_t count, std::vector<int64_t> &values,
                                         size_t offset) {
    size_t k = 0;
    while (count - k >= BLOCK_SIZE) {
        const size_t step = offset + k;
        for (size_t i = 0; i < 32; ++i) {
            auto l = input.read_long();
            if (!l.has_value()) {
                return std::unexpected(std::move(l.error()));
            }
            const int64_t packed = l.value();
            values[step + i] = (packed >> 48) & 0xFFFF;
            values[step + 32 + i] = (packed >> 32) & 0xFFFF;
            values[step + 64 + i] = (packed >> 16) & 0xFFFF;
            values[step + 96 + i] = packed & 0xFFFF;
        }
        k += BLOCK_SIZE;
    }
    for (size_t i = k; i < count; ++i) {
        auto value = input.read_short();
        if (!value.has_value()) {
            return std::unexpected(std::move(value.error()));
        }
        values[offset + i] = static_cast<uint16_t>(value.value());
    }
    return {};
}

VoidResult StoredFieldsInts::read_ints32(IndexInput &input, size_t count, std::vector<int64_t> &values,
                                         size_t offset) {
    size_t k = 0;
    while (count - k >= BLOCK_SIZE) {
        const size_t step = offset + k;
        for (size_t i = 0; i < 64; ++i) {
            auto l = input.read_long();
            if (!l.has_value()) {
                return std::unexpected(std::move(l.error()));
            }
            const int64_t packed = l.value();
            values[step + i] = (packed >> 32) & 0xFFFFFFFF;
            values[step + 64 + i] = packed & 0xFFFFFFFF;
        }
        k += BLOCK_SIZE;
    }
    for (size_t i = k; i < count; ++i) {
        auto value = input.read_int();
        if (!value.has_value()) {
            return std::unexpected(std::move(value.error()));
        }
        values[offset + i] = value.value();
    }
    return {};
}

} // namespace lucene
