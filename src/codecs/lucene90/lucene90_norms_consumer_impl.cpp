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

module lucene_core:lucene90_norms_consumer.impl;

import :lucene90_norms_consumer;

import :codec_util;
import :directory;
import :error;
import :field_infos;
import :index_output;
import :indexed_disi;
import :io_context;
import :lucene90_norms_format;
import :norms_consumer;
import :norms_producer;
import :numeric_doc_values;
import :segment_info;
import :segment_write_state;
import :types;

import std;

namespace lucene {

Lucene90NormsConsumer::Lucene90NormsConsumer(std::unique_ptr<IndexOutput> data, std::unique_ptr<IndexOutput> meta,
                                             int32_t max_doc)
    : data_(std::move(data)), meta_(std::move(meta)), max_doc_(max_doc) {}

Lucene90NormsConsumer::~Lucene90NormsConsumer() {
    static_cast<void>(close());
}

Result<std::unique_ptr<Lucene90NormsConsumer>> Lucene90NormsConsumer::create(const SegmentWriteState &state,
                                                                             const SegmentInfo &segment_info) {
    const std::span<const uint8_t> id(segment_info.id);
    const std::string &suffix = state.segment_suffix;

    const std::string data_file = segment_file_name(segment_info.name, suffix, Lucene90NormsFormat::DATA_EXTENSION);
    auto data = state.directory.create_output(data_file, state.context);
    if (!data.has_value()) {
        return std::unexpected(std::move(data.error()));
    }
    if (auto written = CodecUtil::write_index_header(*data.value(), Lucene90NormsFormat::DATA_CODEC,
                                                      Lucene90NormsFormat::VERSION_CURRENT, id, suffix);
        !written.has_value()) {
        return std::unexpected(std::move(written.error()));
    }

    const std::string meta_file =
            segment_file_name(segment_info.name, suffix, Lucene90NormsFormat::METADATA_EXTENSION);
    auto meta = state.directory.create_output(meta_file, state.context);
    if (!meta.has_value()) {
        return std::unexpected(std::move(meta.error()));
    }
    if (auto written = CodecUtil::write_index_header(*meta.value(), Lucene90NormsFormat::METADATA_CODEC,
                                                      Lucene90NormsFormat::VERSION_CURRENT, id, suffix);
        !written.has_value()) {
        return std::unexpected(std::move(written.error()));
    }

    std::unique_ptr<Lucene90NormsConsumer> consumer(
            new Lucene90NormsConsumer(std::move(data.value()), std::move(meta.value()), segment_info.max_doc));
    return consumer;
}

uint8_t Lucene90NormsConsumer::num_bytes_per_value(int64_t min, int64_t max) {
    // Every width is read back sign extended, so a negative value only needs the
    // bytes of its range; a single value needs none at all.
    if (min >= max) {
        return 0;
    }
    if (min >= std::numeric_limits<int8_t>::min() && max <= std::numeric_limits<int8_t>::max()) {
        return 1;
    }
    if (min >= std::numeric_limits<int16_t>::min() && max <= std::numeric_limits<int16_t>::max()) {
        return 2;
    }
    if (min >= std::numeric_limits<int32_t>::min() && max <= std::numeric_limits<int32_t>::max()) {
        return 4;
    }
    return 8;
}

VoidResult Lucene90NormsConsumer::write_values(NumericDocValues &values, uint8_t num_bytes_per_value,
                                               IndexOutput &out) {
    auto doc = values.next_doc();
    if (!doc.has_value()) {
        return std::unexpected(std::move(doc.error()));
    }
    while (doc.value() != NO_MORE_DOCS) {
        auto value = values.long_value();
        if (!value.has_value()) {
            return std::unexpected(std::move(value.error()));
        }
        switch (num_bytes_per_value) {
            case 1:
                if (auto written = out.write_byte(static_cast<uint8_t>(static_cast<int8_t>(value.value())));
                    !written.has_value()) {
                    return written;
                }
                break;
            case 2:
                if (auto written = out.write_short(static_cast<int16_t>(value.value())); !written.has_value()) {
                    return written;
                }
                break;
            case 4:
                if (auto written = out.write_int(static_cast<int32_t>(value.value())); !written.has_value()) {
                    return written;
                }
                break;
            case 8:
                if (auto written = out.write_long(value.value()); !written.has_value()) {
                    return written;
                }
                break;
            default:
                return err(LuceneError::illegal_state("unexpected bytes per norm: " +
                                                      std::to_string(num_bytes_per_value)));
        }
        doc = values.next_doc();
        if (!doc.has_value()) {
            return std::unexpected(std::move(doc.error()));
        }
    }
    return {};
}

VoidResult Lucene90NormsConsumer::add_norms_field(const FieldInfo &field, NormsProducer &norms_producer) {
    // Pass 1: the value range decides the width of the values.
    auto values = norms_producer.get_norms(field);
    if (!values.has_value()) {
        return std::unexpected(std::move(values.error()));
    }
    int64_t min = std::numeric_limits<int64_t>::max();
    int64_t max = std::numeric_limits<int64_t>::min();
    int32_t num_docs_with_field = 0;
    auto doc = values.value()->next_doc();
    if (!doc.has_value()) {
        return std::unexpected(std::move(doc.error()));
    }
    while (doc.value() != NO_MORE_DOCS) {
        auto value = values.value()->long_value();
        if (!value.has_value()) {
            return std::unexpected(std::move(value.error()));
        }
        min = std::min(min, value.value());
        max = std::max(max, value.value());
        ++num_docs_with_field;
        doc = values.value()->next_doc();
        if (!doc.has_value()) {
            return std::unexpected(std::move(doc.error()));
        }
    }

    if (auto written = meta_->write_int(field.number); !written.has_value()) {
        return written;
    }

    if (num_docs_with_field == 0) {
        // No document has a norm: no doc-id set, no values.
        if (auto written = meta_->write_long(-2); !written.has_value()) {
            return written;
        }
        if (auto written = meta_->write_long(0); !written.has_value()) {
            return written;
        }
        if (auto written = meta_->write_short(-1); !written.has_value()) {
            return written;
        }
        if (auto written = meta_->write_byte(static_cast<uint8_t>(-1)); !written.has_value()) {
            return written;
        }
    } else if (num_docs_with_field == max_doc_) {
        // Every document has a norm, so the doc-id set is implied.
        if (auto written = meta_->write_long(-1); !written.has_value()) {
            return written;
        }
        if (auto written = meta_->write_long(0); !written.has_value()) {
            return written;
        }
        if (auto written = meta_->write_short(-1); !written.has_value()) {
            return written;
        }
        if (auto written = meta_->write_byte(static_cast<uint8_t>(-1)); !written.has_value()) {
            return written;
        }
    } else {
        auto docs_with_field_address = data_->get_file_pointer();
        if (!docs_with_field_address.has_value()) {
            return std::unexpected(std::move(docs_with_field_address.error()));
        }
        if (auto written = meta_->write_long(static_cast<int64_t>(docs_with_field_address.value()));
            !written.has_value()) {
            return written;
        }

        // Pass 2: the doc ids of the documents that have a norm.
        auto doc_ids = norms_producer.get_norms(field);
        if (!doc_ids.has_value()) {
            return std::unexpected(std::move(doc_ids.error()));
        }
        auto bitset = IndexedDISI::write_bitset(*doc_ids.value(), *data_);
        if (!bitset.has_value()) {
            return std::unexpected(std::move(bitset.error()));
        }

        auto docs_end = data_->get_file_pointer();
        if (!docs_end.has_value()) {
            return std::unexpected(std::move(docs_end.error()));
        }
        if (auto written = meta_->write_long(static_cast<int64_t>(docs_end.value() - docs_with_field_address.value()));
            !written.has_value()) {
            return written;
        }
        if (auto written = meta_->write_short(static_cast<int16_t>(bitset.value().jump_table_entry_count));
            !written.has_value()) {
            return written;
        }
        if (auto written = meta_->write_byte(static_cast<uint8_t>(bitset.value().dense_rank_power));
            !written.has_value()) {
            return written;
        }
    }

    if (auto written = meta_->write_int(num_docs_with_field); !written.has_value()) {
        return written;
    }

    const uint8_t bytes_per_norm = num_bytes_per_value(min, max);
    if (auto written = meta_->write_byte(bytes_per_norm); !written.has_value()) {
        return written;
    }

    if (bytes_per_norm == 0) {
        // Every document has the same norm: store it once, in the metadata.
        return meta_->write_long(min);
    }

    auto norms_address = data_->get_file_pointer();
    if (!norms_address.has_value()) {
        return std::unexpected(std::move(norms_address.error()));
    }
    if (auto written = meta_->write_long(static_cast<int64_t>(norms_address.value())); !written.has_value()) {
        return written;
    }

    // Pass 3: the values themselves.
    auto all_values = norms_producer.get_norms(field);
    if (!all_values.has_value()) {
        return std::unexpected(std::move(all_values.error()));
    }
    return write_values(*all_values.value(), bytes_per_norm, *data_);
}

VoidResult Lucene90NormsConsumer::close() {
    if (closed_) {
        return {};
    }
    closed_ = true;

    if (meta_ != nullptr) {
        if (auto written = meta_->write_int(-1); !written.has_value()) {
            return written;
        }
        if (auto written = CodecUtil::write_footer(*meta_); !written.has_value()) {
            return written;
        }
        if (auto written = CodecUtil::write_footer(*data_); !written.has_value()) {
            return written;
        }
    }

    VoidResult result{};
    if (data_ != nullptr) {
        if (auto closed = data_->close(); !closed.has_value()) {
            result = closed;
        }
        data_.reset();
    }
    if (meta_ != nullptr) {
        if (auto closed = meta_->close(); !closed.has_value() && result.has_value()) {
            result = closed;
        }
        meta_.reset();
    }
    return result;
}

} // namespace lucene
