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

module lucene_core:postings_util.impl;

import :postings_util;

import :types;
import std;

import :error;
import :data_input;
import :data_output;
import :group_vint_util;

namespace lucene {

VoidResult PostingsUtil::read_vint_block(DataInput &doc_in,
                                        std::span<int32_t> doc_buffer,
                                        std::span<int32_t> freq_buffer,
                                        size_t num,
                                        bool index_has_freq,
                                        bool decode_freq) {
    if (doc_buffer.size() < num) {
        return err(LuceneError::illegal_argument("doc_buffer is too small"));
    }
    auto read = GroupVIntUtil::read_group_vints_i32(doc_in, doc_buffer, num);
    if (!read) {
        return read;
    }

    if (index_has_freq && decode_freq) {
        if (freq_buffer.size() < num) {
            return err(LuceneError::illegal_argument("freq_buffer is too small"));
        }
        for (size_t i = 0; i < num; ++i) {
            freq_buffer[i] = doc_buffer[i] & 1;
            doc_buffer[i] = static_cast<int32_t>(static_cast<uint32_t>(doc_buffer[i]) >> 1);
            if (freq_buffer[i] == 0) {
                auto freq = doc_in.read_vint();
                if (!freq) {
                    return std::unexpected(std::move(freq.error()));
                }
                freq_buffer[i] = freq.value();
            }
        }
    } else if (index_has_freq) {
        for (size_t i = 0; i < num; ++i) {
            doc_buffer[i] = static_cast<int32_t>(static_cast<uint32_t>(doc_buffer[i]) >> 1);
        }
    }
    return {};
}

VoidResult PostingsUtil::write_vint_block(DataOutput &doc_out,
                                         std::span<int32_t> doc_buffer,
                                         std::span<const int32_t> freq_buffer,
                                         size_t num,
                                         bool write_freqs) {
    if (doc_buffer.size() < num) {
        return err(LuceneError::illegal_argument("doc_buffer is too small"));
    }
    if (write_freqs && freq_buffer.size() < num) {
        return err(LuceneError::illegal_argument("freq_buffer is too small"));
    }

    if (write_freqs) {
        // Fold `freq == 1` (by far the most common value) into the low bit of
        // the doc id; the remaining frequencies are written afterwards.
        for (size_t i = 0; i < num; ++i) {
            const uint32_t flag = freq_buffer[i] == 1 ? 1u : 0u;
            doc_buffer[i] = static_cast<int32_t>((static_cast<uint32_t>(doc_buffer[i]) << 1) | flag);
        }
    }

    std::array<uint8_t, GroupVIntUtil::MAX_LENGTH_PER_GROUP> scratch{};
    auto written = GroupVIntUtil::write_group_vints_i32(doc_out, scratch, doc_buffer, num);
    if (!written) {
        return written;
    }

    if (write_freqs) {
        for (size_t i = 0; i < num; ++i) {
            if (freq_buffer[i] != 1) {
                auto freq = doc_out.write_vint(freq_buffer[i]);
                if (!freq) {
                    return freq;
                }
            }
        }
    }
    return {};
}

} // namespace lucene
