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

module lucene_core:segment_merger.impl;

import :segment_merger;

import std;

import :codec;
import :codec_formats;
import :codec_reader;
import :directory;
import :doc_values_consumer;
import :doc_values_format;
import :error;
import :fields_consumer;
import :field_infos;
import :field_infos_builder;
import :field_infos_format;
import :info_stream;
import :io_context;
import :merge_state;
import :multi_sorter;
import :norms_consumer;
import :norms_format;
import :norms_producer;
import :points_format;
import :points_writer;
import :postings_format;
import :segment_info;
import :segment_read_state;
import :segment_write_state;
import :stored_fields_format;
import :stored_fields_writer;
import :string_helper;
import :term_vectors_format;
import :term_vectors_writer;

namespace lucene {

namespace {

/// Runs `merge`, then closes `closeable`, reporting the merge error first.
///
/// This is the port's replacement for the reference implementation's
/// `IOUtils::use_or_suppress_caught_result`.
template <class T, class Merger, class Closer>
auto merge_then_close(T &closeable, Merger merge, Closer close) -> decltype(merge()) {
    auto merged = merge();
    auto closed = close(closeable);
    if (!merged.has_value()) {
        return std::unexpected(std::move(merged.error()));
    }
    if (!closed.has_value()) {
        using ResultType = decltype(merge());
        return ResultType(std::unexpected(std::move(closed.error())));
    }
    return merged;
}

/// Wraps a step that returns no value so that it can be timed and logged.
[[nodiscard]] VoidResult log_step(const InfoStreamMT &info_stream, std::string_view format_name, int32_t num_merged,
                                 std::chrono::steady_clock::time_point start) {
    if (info_stream == nullptr || !info_stream->is_enabled("SM")) {
        return {};
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
    return info_stream->message("SM", std::format("{} ms to merge {} [{} docs]", elapsed.count(), format_name,
                                                  num_merged));
}

} // namespace

SegmentMerger::SegmentMerger(std::unique_ptr<MergeState> merge_state, Directory &directory, const IOContext &context,
                             std::vector<uint8_t> id)
        : merge_state_(std::move(merge_state)), directory_(directory), context_(context), id_(std::move(id)) {}

SegmentMerger::~SegmentMerger() = default;

Result<std::unique_ptr<SegmentMerger>> SegmentMerger::create(std::span<CodecReader *const> readers,
                                                             SegmentInfo &segment_info, InfoStreamMT info_stream,
                                                             Directory &directory, const IOContext &context) {
    if (context.context() != IOContextType::Merge) {
        return err(LuceneError::illegal_argument(
                std::format("IOContext.context should be MERGE; got: {}", static_cast<int32_t>(context.context()))));
    }

    auto merge_state = MergeState::create(readers, segment_info, std::move(info_stream));
    if (!merge_state.has_value()) {
        return std::unexpected(std::move(merge_state.error()));
    }

    // The segment is being written with an index sort: the documents of the
    // inputs have to interleave in that order, so the doc maps come from the
    // multi sorter instead of the deletion-only mapping.
    //
    // This lives here rather than in `MergeState::create` because the multi
    // sorter depends on the merge state, so the two modules would form an import
    // cycle.
    if (segment_info.sort.has_value()) {
        std::vector<std::shared_ptr<CodecReader>> shared_readers;
        shared_readers.reserve(readers.size());
        for (CodecReader *reader : readers) {
            shared_readers.push_back(merge_state_borrow(reader));
        }
        auto sorted = MultiSorter::sort(segment_info.sort.value(), shared_readers);
        if (!sorted.has_value()) {
            return std::unexpected(std::move(sorted.error()));
        }
        if (sorted.value().has_value()) {
            merge_state.value()->doc_maps =
                    std::make_shared<const MergeState::DocMaps>(std::move(sorted.value().value()));
            merge_state.value()->needs_index_sort = true;
        }
    }

    const auto random = StringHelper::random_id();
    std::vector<uint8_t> id(random.begin(), random.end());
    return std::unique_ptr<SegmentMerger>(
            new SegmentMerger(std::move(merge_state.value()), directory, context, std::move(id)));
}

Result<const Codec *> SegmentMerger::codec() const {
    const Codec *found = CodecFormats::for_name(merge_state_->segment_info.codec_name);
    if (found == nullptr) {
        return err(LuceneError::illegal_argument("unknown codec: " + merge_state_->segment_info.codec_name));
    }
    return found;
}

bool SegmentMerger::should_merge() const { return merge_state_->segment_info.max_doc > 0; }

VoidResult SegmentMerger::merge_field_infos() {
    for (const auto &reader_field_infos : *merge_state_->field_infos) {
        if (reader_field_infos == nullptr) {
            continue;
        }
        for (const FieldInfo &field_info : reader_field_infos->fields()) {
            if (auto added = field_infos_builder_.add_existing(field_info); !added.has_value()) {
                return std::unexpected(std::move(added.error()));
            }
        }
    }

    auto finished = field_infos_builder_.finish();
    if (!finished.has_value()) {
        return std::unexpected(std::move(finished.error()));
    }
    merge_state_->merge_field_infos = std::make_shared<const FieldInfos>(std::move(finished.value()));
    return {};
}

VoidResult SegmentMerger::merge_field_infos_with_state() {
    auto found = codec();
    if (!found.has_value()) {
        return std::unexpected(std::move(found.error()));
    }
    return found.value()->field_infos_format().write(directory_, merge_state_->segment_info, "",
                                                     *merge_state_->merge_field_infos, context_);
}

Result<int32_t> SegmentMerger::merge_fields() {
    auto found = codec();
    if (!found.has_value()) {
        return std::unexpected(std::move(found.error()));
    }
    const StoredFieldsFormat *format = found.value()->stored_fields_format();
    if (format == nullptr) {
        return err(LuceneError::illegal_state("the codec does not provide a stored fields format"));
    }
    auto writer = format->fields_writer(directory_, merge_state_->segment_info, context_);
    if (!writer.has_value()) {
        return std::unexpected(std::move(writer.error()));
    }
    return merge_then_close(*writer.value(),
                            [this, &writer] { return writer.value()->merge(*merge_state_); },
                            [](StoredFieldsWriter &w) { return w.close(); });
}

VoidResult SegmentMerger::merge_norms() {
    auto found = codec();
    if (!found.has_value()) {
        return std::unexpected(std::move(found.error()));
    }
    const NormsFormat *format = found.value()->norms_format();
    if (format == nullptr) {
        return err(LuceneError::illegal_state("the codec does not provide a norms format"));
    }
    auto consumer = format->norms_consumer(SegmentWriteState(get_default_info_stream(), directory_,
                                                             merge_state_->merge_field_infos, context_, ""),
                                           merge_state_->segment_info);
    if (!consumer.has_value()) {
        return std::unexpected(std::move(consumer.error()));
    }
    return merge_then_close(*consumer.value(),
                            [this, &consumer] { return consumer.value()->merge(*merge_state_); },
                            [](NormsConsumer &c) { return c.close(); });
}

VoidResult SegmentMerger::merge_terms(const SegmentWriteState &write_state, const SegmentReadState &read_state) {
    auto found = codec();
    if (!found.has_value()) {
        return std::unexpected(std::move(found.error()));
    }
    const Codec *codec_ptr = found.value();

    // The postings writer needs the norms of the documents it writes; open a
    // producer over the norms file this merger just wrote, plus a merge instance
    // so the same `IndexInput` is reused for all terms.
    std::unique_ptr<NormsProducer> norms;
    std::unique_ptr<NormsProducer> norms_merge_instance;
    if (merge_state_->merge_field_infos->has_norms()) {
        const NormsFormat *norms_format = codec_ptr->norms_format();
        if (norms_format == nullptr) {
            return err(LuceneError::illegal_state("the codec does not provide a norms format"));
        }
        auto producer = norms_format->norms_producer(read_state, merge_state_->segment_info);
        if (!producer.has_value()) {
            return std::unexpected(std::move(producer.error()));
        }
        norms = std::move(producer.value());
        if (auto instance = norms->get_merge_instance(); instance.has_value()) {
            // A null merge instance means "use the producer itself".
            norms_merge_instance = std::move(instance.value());
        } else {
            return std::unexpected(std::move(instance.error()));
        }
    }

    VoidResult result;
    if (merge_state_->merge_field_infos->has_postings()) {
        const PostingsFormat *postings_format = codec_ptr->postings_format();
        if (postings_format == nullptr) {
            result = err(LuceneError::illegal_state("the codec does not provide a postings format"));
        } else {
            auto consumer = postings_format->fields_consumer(write_state, merge_state_->segment_info);
            if (!consumer.has_value()) {
                result = std::unexpected(std::move(consumer.error()));
            } else {
                NormsProducer *norms_to_use = norms_merge_instance != nullptr ? norms_merge_instance.get() : norms.get();
                result = merge_then_close(
                        *consumer.value(),
                        [this, &consumer, &write_state, norms_to_use] {
                            return consumer.value()->merge(write_state, merge_state_->segment_info, *merge_state_,
                                                           norms_to_use);
                        },
                        [](FieldsConsumer &c) { return c.close(); });
            }
        }
    }

    // Drop the merge instance before the producer it was cloned from.
    norms_merge_instance.reset();
    if (norms != nullptr) {
        auto closed = norms->close();
        if (result.has_value() && !closed.has_value()) {
            result = std::unexpected(std::move(closed.error()));
        }
    }
    return result;
}

VoidResult SegmentMerger::merge_doc_values(const SegmentWriteState &write_state) {
    auto found = codec();
    if (!found.has_value()) {
        return std::unexpected(std::move(found.error()));
    }
    const DocValuesFormat *format = found.value()->doc_values_format();
    if (format == nullptr) {
        return err(LuceneError::illegal_state("the codec does not provide a doc values format"));
    }
    auto consumer = format->fields_consumer(write_state, merge_state_->segment_info);
    if (!consumer.has_value()) {
        return std::unexpected(std::move(consumer.error()));
    }
    return merge_then_close(*consumer.value(),
                            [this, &consumer] { return consumer.value()->merge(*merge_state_); },
                            [](DocValuesConsumer &c) { return c.close(); });
}

VoidResult SegmentMerger::merge_points(SegmentWriteState &write_state) {
    auto found = codec();
    if (!found.has_value()) {
        return std::unexpected(std::move(found.error()));
    }
    const PointsFormat *format = found.value()->points_format();
    if (format == nullptr) {
        return err(LuceneError::illegal_state("the codec does not provide a points format"));
    }
    auto writer = format->fields_writer(write_state, merge_state_->segment_info);
    if (!writer.has_value()) {
        return std::unexpected(std::move(writer.error()));
    }
    return merge_then_close(*writer.value(),
                            [this, &writer] { return writer.value()->merge(*merge_state_); },
                            [](PointsWriter &w) { return w.close(); });
}

Result<int32_t> SegmentMerger::merge_term_vectors() {
    auto found = codec();
    if (!found.has_value()) {
        return std::unexpected(std::move(found.error()));
    }
    const TermVectorsFormat *format = found.value()->term_vectors_format();
    if (format == nullptr) {
        return err(LuceneError::illegal_state("the codec does not provide a term vectors format"));
    }
    auto writer = format->vectors_writer(directory_, merge_state_->segment_info, context_);
    if (!writer.has_value()) {
        return std::unexpected(std::move(writer.error()));
    }
    auto merged = merge_then_close(*writer.value(),
                                   [this, &writer] { return writer.value()->merge(*merge_state_); },
                                   [](TermVectorsWriter &w) { return w.close(); });
    if (!merged.has_value()) {
        return std::unexpected(std::move(merged.error()));
    }
    debug_check(merged.value() == merge_state_->segment_info.max_doc,
                "the term vectors writer wrote a different number of documents");
    return merged;
}

VoidResult SegmentMerger::merge() {
    if (!should_merge()) {
        return err(LuceneError::illegal_state("Merge would result in 0 document segment"));
    }

    if (auto merged = merge_field_infos(); !merged.has_value()) {
        return merged;
    }
    const InfoStreamMT info_stream = merge_state_->info_stream;

    auto start = std::chrono::steady_clock::now();
    auto fields_merged = merge_fields();
    if (!fields_merged.has_value()) {
        return std::unexpected(std::move(fields_merged.error()));
    }
    const int32_t num_merged = fields_merged.value();
    debug_check(num_merged == merge_state_->segment_info.max_doc,
                "the stored fields writer wrote a different number of documents");
    if (auto logged = log_step(info_stream, "stored fields", num_merged, start); !logged.has_value()) {
        return logged;
    }

    SegmentWriteState write_state(info_stream, directory_, merge_state_->merge_field_infos, context_, "");
    SegmentReadState read_state(directory_, merge_state_->merge_field_infos, io_context_default(),
                                write_state.segment_suffix);

    if (merge_state_->merge_field_infos->has_norms()) {
        start = std::chrono::steady_clock::now();
        if (auto merged = merge_norms(); !merged.has_value()) {
            return merged;
        }
        if (auto logged = log_step(info_stream, "norms", num_merged, start); !logged.has_value()) {
            return logged;
        }
    }

    start = std::chrono::steady_clock::now();
    if (auto merged = merge_terms(write_state, read_state); !merged.has_value()) {
        return merged;
    }
    if (auto logged = log_step(info_stream, "postings", num_merged, start); !logged.has_value()) {
        return logged;
    }

    if (merge_state_->merge_field_infos->has_doc_values()) {
        start = std::chrono::steady_clock::now();
        if (auto merged = merge_doc_values(write_state); !merged.has_value()) {
            return merged;
        }
        if (auto logged = log_step(info_stream, "doc values", num_merged, start); !logged.has_value()) {
            return logged;
        }
    }

    if (merge_state_->merge_field_infos->has_point_values()) {
        start = std::chrono::steady_clock::now();
        if (auto merged = merge_points(write_state); !merged.has_value()) {
            return merged;
        }
        if (auto logged = log_step(info_stream, "points", num_merged, start); !logged.has_value()) {
            return logged;
        }
    }

    // Vectors (HNSW) are not merged by this port.

    if (merge_state_->merge_field_infos->has_term_vectors()) {
        start = std::chrono::steady_clock::now();
        if (auto merged = merge_term_vectors(); !merged.has_value()) {
            return std::unexpected(std::move(merged.error()));
        }
        if (auto logged = log_step(info_stream, "term vectors", num_merged, start); !logged.has_value()) {
            return logged;
        }
    }

    start = std::chrono::steady_clock::now();
    if (auto written = merge_field_infos_with_state(); !written.has_value()) {
        return written;
    }
    return log_step(info_stream, "field infos", num_merged, start);
}

} // namespace lucene
