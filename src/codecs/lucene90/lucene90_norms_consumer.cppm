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

export module lucene_core:lucene90_norms_consumer;

import :types;
import :error;

import std;

import :field_infos;
import :index_output;
import :io_context;
import :lucene90_norms_format;
import :norms_consumer;
import :norms_producer;
import :numeric_doc_values;
import :segment_info;
import :segment_write_state;

namespace lucene {

/// Writes the `.nvd`/`.nvm` pair of one segment.
///
/// Port of `Lucene90NormsConsumer`
/// (`codecs/lucene90/lucene90_norms_consumer.rs`): both files are opened and
/// header-stamped up front, every field is appended by [`add_norms_field`], and
/// [`close`] writes the terminating `-1` field number plus both footers.
///
/// Each field is probed three times through fresh iterators — once for the value
/// range, once for the doc-id set and once for the values — because the pull API
/// of [`NormsProducer`] allows it and the range has to be known before the first
/// value is written.
export class Lucene90NormsConsumer final : public NormsConsumer {
public:
    /// Opens the two outputs for `state`.
    [[nodiscard]] static Result<std::unique_ptr<Lucene90NormsConsumer>> create(const SegmentWriteState &state,
                                                                              const SegmentInfo &segment_info);

    ~Lucene90NormsConsumer() override;

    Lucene90NormsConsumer(const Lucene90NormsConsumer &) = delete;
    Lucene90NormsConsumer &operator=(const Lucene90NormsConsumer &) = delete;

    [[nodiscard]] VoidResult add_norms_field(const FieldInfo &field, NormsProducer &norms_producer) override;

    [[nodiscard]] VoidResult close() override;

private:
    Lucene90NormsConsumer(std::unique_ptr<IndexOutput> data, std::unique_ptr<IndexOutput> meta, int32_t max_doc);

    /// Smallest width that holds `[min, max]`: 0, 1, 2, 4 or 8 bytes.
    [[nodiscard]] static uint8_t num_bytes_per_value(int64_t min, int64_t max);

    /// Copies every value of `values` with the given width.
    [[nodiscard]] static VoidResult write_values(NumericDocValues &values, uint8_t num_bytes_per_value,
                                                 IndexOutput &out);

    std::unique_ptr<IndexOutput> data_;
    std::unique_ptr<IndexOutput> meta_;
    int32_t max_doc_{0};
    bool closed_{false};
};

} // namespace lucene
