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

module lucene_core:per_field_doc_values_format.impl;

import :per_field_doc_values_format;

import :types;
import std;

import :close;
import :directory;
import :doc_values_consumer;
import :doc_values_format;
import :doc_values_producer;
import :error;
import :field_infos;
import :index_types;
import :merge_state;
import :per_field_merge_state;
import :segment_info;
import :segment_read_state;
import :segment_write_state;

namespace lucene {

namespace {

[[nodiscard]] std::string make_segment_suffix(std::string_view format_name, int32_t suffix) {
    return std::string(format_name) + "_" + std::to_string(suffix);
}

/// `outer` + `_` + `inner`, or `inner` when there is no outer suffix.
[[nodiscard]] std::string full_segment_suffix(std::string_view outer, const std::string &inner) {
    if (outer.empty()) {
        return inner;
    }
    return std::string(outer) + "_" + inner;
}

/// Parses the recorded per-field suffix, which must be a plain integer.
[[nodiscard]] Result<int32_t> parse_suffix(const std::string &text, const std::string &field_name) {
    int32_t value = 0;
    const auto *start = text.data();
    const auto *end = start + text.size();
    const auto parsed = std::from_chars(start, end, value);
    if (parsed.ec != std::errc{} || parsed.ptr != end) {
        return err(LuceneError::number_format("invalid attribute: " + std::string(PER_FIELD_DV_SUFFIX_KEY) + "=" +
                                              text + " for field: " + field_name));
    }
    return value;
}

/// Writes the doc values of a segment, one format at a time.
class PerFieldDocValuesWriter final : public DocValuesConsumer {
public:
    PerFieldDocValuesWriter(const DocValuesFormat &format, const SegmentWriteState &state,
                            const SegmentInfo &segment_info)
            : format_(&format), state_(state), segment_info_(&segment_info) {}

    VoidResult add_numeric_field(const FieldInfo &field, DocValuesProducer &producer) override {
        auto consumer = get_instance(field, /*ignore_current_format=*/false);
        if (!consumer.has_value()) {
            return std::unexpected(std::move(consumer.error()));
        }
        return consumer.value()->add_numeric_field(field, producer);
    }

    VoidResult add_binary_field(const FieldInfo &field, DocValuesProducer &producer) override {
        auto consumer = get_instance(field, /*ignore_current_format=*/false);
        if (!consumer.has_value()) {
            return std::unexpected(std::move(consumer.error()));
        }
        return consumer.value()->add_binary_field(field, producer);
    }

    VoidResult add_sorted_field(const FieldInfo &field, DocValuesProducer &producer) override {
        auto consumer = get_instance(field, /*ignore_current_format=*/false);
        if (!consumer.has_value()) {
            return std::unexpected(std::move(consumer.error()));
        }
        return consumer.value()->add_sorted_field(field, producer);
    }

    VoidResult add_sorted_numeric_field(const FieldInfo &field, DocValuesProducer &producer) override {
        auto consumer = get_instance(field, /*ignore_current_format=*/false);
        if (!consumer.has_value()) {
            return std::unexpected(std::move(consumer.error()));
        }
        return consumer.value()->add_sorted_numeric_field(field, producer);
    }

    VoidResult add_sorted_set_field(const FieldInfo &field, DocValuesProducer &producer) override {
        auto consumer = get_instance(field, /*ignore_current_format=*/false);
        if (!consumer.has_value()) {
            return std::unexpected(std::move(consumer.error()));
        }
        return consumer.value()->add_sorted_set_field(field, producer);
    }

    VoidResult merge(const MergeState &merge_state) override {
        if (merge_state.merge_field_infos == nullptr) {
            return err(LuceneError::illegal_state("the merged field infos are not built"));
        }
        // Group the fields of the merged segment by the format that will write
        // them, and let each format merge its own group.
        std::map<std::string, std::vector<std::string>> groups;
        for (const auto &field : merge_state.merge_field_infos->fields()) {
            if (field.doc_values_type == DocValuesType::None) {
                continue;
            }
            auto consumer = get_instance(field, /*ignore_current_format=*/true);
            if (!consumer.has_value()) {
                return std::unexpected(std::move(consumer.error()));
            }
            groups[std::string(format_->name())].push_back(field.name);
        }

        for (const auto &[format_name, fields] : groups) {
            auto found = consumers_.find(format_name);
            if (found == consumers_.end()) {
                return err(LuceneError::illegal_state("missing doc values consumer for merge"));
            }
            auto restricted = restrict_merge_state_fields(merge_state, fields);
            if (auto merged = found->second.consumer->merge(*restricted); !merged.has_value()) {
                return merged;
            }
        }
        return {};
    }

    VoidResult close() override {
        for (auto &[format_name, entry] : consumers_) {
            if (entry.consumer == nullptr) {
                continue;
            }
            auto closed = entry.consumer->close();
            if (!closed.has_value()) {
                return closed;
            }
        }
        return {};
    }

private:
    /// One format's consumer, with the suffix it writes under.
    struct ConsumerAndSuffix {
        std::unique_ptr<DocValuesConsumer> consumer;
        int32_t suffix{0};
    };

    /// The consumer responsible for `field`, recording the format name and
    /// suffix in the field infos.
    Result<DocValuesConsumer *> get_instance(const FieldInfo &field, bool ignore_current_format) {
        // A field that was updated in place keeps the format it was originally
        // written with, at that format's recorded suffix.
        std::optional<std::string> recorded_format;
        if (field.doc_values_gen != -1 && !ignore_current_format) {
            const auto found = field.attributes.find(std::string(PER_FIELD_DV_FORMAT_KEY));
            if (found != field.attributes.end()) {
                recorded_format = found->second;
            }
        }
        if (recorded_format.has_value() && *recorded_format != format_->name()) {
            return err(LuceneError::illegal_argument("could not load doc values format named \"" +
                                                     *recorded_format + "\""));
        }

        const std::string format_name(format_->name());
        annotate(field.name, format_name, /*place_holder=*/std::nullopt);

        auto found = consumers_.find(format_name);
        if (found == consumers_.end()) {
            std::optional<int32_t> suffix;
            if (field.doc_values_gen != -1 && !ignore_current_format) {
                const auto recorded = field.attributes.find(std::string(PER_FIELD_DV_SUFFIX_KEY));
                if (recorded != field.attributes.end()) {
                    auto parsed = parse_suffix(recorded->second, field.name);
                    if (!parsed.has_value()) {
                        return std::unexpected(std::move(parsed.error()));
                    }
                    suffix = parsed.value();
                }
            }
            if (!suffix.has_value()) {
                suffix = 0;
            }
            const std::string segment_suffix = full_segment_suffix(state_.segment_suffix,
                                                                   make_segment_suffix(format_name, *suffix));
            auto state = SegmentWriteState::copy_with_suffix(state_, segment_suffix);
            auto created = format_->fields_consumer(state, *segment_info_);
            if (!created.has_value()) {
                return std::unexpected(std::move(created.error()));
            }
            found = consumers_.emplace(format_name, ConsumerAndSuffix{std::move(created.value()), *suffix}).first;
        }

        annotate(field.name, format_name, found->second.suffix);
        return found->second.consumer.get();
    }

    /// Records the format name (and, when known, the suffix) of `field_name`.
    void annotate(const std::string &field_name, const std::string &format_name,
                  std::optional<int32_t> place_holder) {
        auto *infos = const_cast<FieldInfos *>(state_.field_infos.get());
        if (infos == nullptr) {
            return;
        }
        FieldInfo *info = infos->field_info_mut(field_name);
        if (info == nullptr) {
            return;
        }
        info->attributes[std::string(PER_FIELD_DV_FORMAT_KEY)] = format_name;
        if (place_holder.has_value()) {
            info->attributes[std::string(PER_FIELD_DV_SUFFIX_KEY)] = std::to_string(*place_holder);
        }
    }

    const DocValuesFormat *format_;
    SegmentWriteState state_;
    const SegmentInfo *segment_info_;
    std::map<std::string, ConsumerAndSuffix> consumers_;
};

/// Reads a segment written by [`PerFieldDocValuesFormat`].
class PerFieldDocValuesReader final : public DocValuesProducer {
public:
    static Result<std::unique_ptr<PerFieldDocValuesReader>> open(const DocValuesFormat &format,
                                                                 const SegmentReadState &state,
                                                                 const SegmentInfo &segment_info) {
        std::unique_ptr<PerFieldDocValuesReader> reader{new PerFieldDocValuesReader()};
        if (state.field_infos == nullptr) {
            return err(LuceneError::illegal_state("the field infos of the segment are not available"));
        }
        for (const auto &field_info : state.field_infos->fields()) {
            if (field_info.doc_values_type == DocValuesType::None) {
                continue;
            }
            const auto format_entry = field_info.attributes.find(std::string(PER_FIELD_DV_FORMAT_KEY));
            if (format_entry == field_info.attributes.end()) {
                // A field listed without doc values: nothing to read.
                continue;
            }
            if (format_entry->second != format.name()) {
                return err(LuceneError::illegal_argument("could not load doc values format named \"" +
                                                         format_entry->second + "\""));
            }
            const auto suffix_entry = field_info.attributes.find(std::string(PER_FIELD_DV_SUFFIX_KEY));
            if (suffix_entry == field_info.attributes.end()) {
                return err(LuceneError::illegal_state("missing attribute: " + std::string(PER_FIELD_DV_SUFFIX_KEY) +
                                                      " for field: " + field_info.name));
            }
            auto suffix = parse_suffix(suffix_entry->second, field_info.name);
            if (!suffix.has_value()) {
                return std::unexpected(std::move(suffix.error()));
            }
            const std::string segment_suffix = full_segment_suffix(
                    state.segment_suffix, make_segment_suffix(format_entry->second, suffix.value()));

            auto found = reader->formats_.find(segment_suffix);
            std::shared_ptr<DocValuesProducer> producer;
            if (found != reader->formats_.end()) {
                producer = found->second;
            } else {
                auto read_state = SegmentReadState::copy_with_suffix(state, segment_suffix);
                auto opened = format.fields_producer(read_state, segment_info);
                if (!opened.has_value()) {
                    return std::unexpected(std::move(opened.error()));
                }
                producer = std::shared_ptr<DocValuesProducer>(std::move(opened.value()));
                reader->formats_.emplace(segment_suffix, producer);
            }
            reader->fields_.emplace(field_info.number, producer);
        }
        return reader;
    }

    Result<std::unique_ptr<NumericDocValues>> get_numeric(const FieldInfo &field) const override {
        auto producer = find(field);
        if (producer == nullptr) {
            return err(missing_error(field));
        }
        return producer->get_numeric(field);
    }

    Result<std::unique_ptr<BinaryDocValues>> get_binary(const FieldInfo &field) const override {
        auto producer = find(field);
        if (producer == nullptr) {
            return err(missing_error(field));
        }
        return producer->get_binary(field);
    }

    Result<std::unique_ptr<SortedDocValues>> get_sorted(const FieldInfo &field) const override {
        auto producer = find(field);
        if (producer == nullptr) {
            return err(missing_error(field));
        }
        return producer->get_sorted(field);
    }

    Result<std::unique_ptr<SortedNumericDocValues>> get_sorted_numeric(const FieldInfo &field) const override {
        auto producer = find(field);
        if (producer == nullptr) {
            return err(missing_error(field));
        }
        return producer->get_sorted_numeric(field);
    }

    Result<std::unique_ptr<SortedSetDocValues>> get_sorted_set(const FieldInfo &field) const override {
        auto producer = find(field);
        if (producer == nullptr) {
            return err(missing_error(field));
        }
        return producer->get_sorted_set(field);
    }

    Result<std::unique_ptr<DocValuesSkipper>> get_skipper(const FieldInfo &field) const override {
        auto producer = find(field);
        if (producer == nullptr) {
            return std::unique_ptr<DocValuesSkipper>(nullptr);
        }
        return producer->get_skipper(field);
    }

    VoidResult check_integrity() const override {
        for (const auto &[suffix, producer] : formats_) {
            if (auto checked = producer->check_integrity(); !checked.has_value()) {
                return checked;
            }
        }
        return {};
    }

    VoidResult close() const override {
        for (const auto &[suffix, producer] : formats_) {
            if (auto closed = producer->close(); !closed.has_value()) {
                return closed;
            }
        }
        return {};
    }

    Result<std::unique_ptr<DocValuesProducer>> get_merge_instance() const override {
        std::unique_ptr<PerFieldDocValuesReader> copy{new PerFieldDocValuesReader()};
        std::map<const DocValuesProducer *, std::shared_ptr<DocValuesProducer>> replacements;
        for (const auto &[suffix, producer] : formats_) {
            auto instance = producer->get_merge_instance();
            if (!instance.has_value()) {
                return std::unexpected(std::move(instance.error()));
            }
            std::shared_ptr<DocValuesProducer> replacement =
                    instance.value() != nullptr ? std::shared_ptr<DocValuesProducer>(std::move(instance.value()))
                                                : producer;
            replacements.emplace(producer.get(), replacement);
            copy->formats_.emplace(suffix, replacement);
        }
        for (const auto &[number, producer] : fields_) {
            auto found = replacements.find(producer.get());
            if (found == replacements.end()) {
                return err(LuceneError::illegal_state("missing merge instance for field number: " +
                                                      std::to_string(number)));
            }
            copy->fields_.emplace(number, found->second);
        }
        return std::unique_ptr<DocValuesProducer>(std::move(copy));
    }

private:
    PerFieldDocValuesReader() = default;

    [[nodiscard]] const DocValuesProducer *find(const FieldInfo &field) const {
        auto found = fields_.find(field.number);
        return found == fields_.end() ? nullptr : found->second.get();
    }

    [[nodiscard]] static LuceneError missing_error(const FieldInfo &field) {
        return LuceneError::illegal_state("missing doc values producer for field: " + field.name);
    }

    /// Field number to the producer of the format that wrote it.
    std::map<int32_t, std::shared_ptr<DocValuesProducer>> fields_;
    /// Segment suffix (`<format>_<n>`) to its producer.
    std::map<std::string, std::shared_ptr<DocValuesProducer>> formats_;
};

} // namespace

Result<std::unique_ptr<DocValuesConsumer>> PerFieldDocValuesFormat::fields_consumer(const SegmentWriteState &state,
                                                                                   const SegmentInfo &segment_info) const {
    return std::unique_ptr<DocValuesConsumer>(new PerFieldDocValuesWriter(*default_format_, state, segment_info));
}

Result<std::unique_ptr<DocValuesProducer>> PerFieldDocValuesFormat::fields_producer(const SegmentReadState &state,
                                                                                   const SegmentInfo &segment_info) const {
    auto reader = PerFieldDocValuesReader::open(*default_format_, state, segment_info);
    if (!reader.has_value()) {
        return std::unexpected(std::move(reader.error()));
    }
    return std::unique_ptr<DocValuesProducer>(std::move(reader.value()));
}

} // namespace lucene
