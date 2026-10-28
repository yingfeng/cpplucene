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

module lucene_core:per_field_postings_format.impl;

import :per_field_postings_format;

import :types;
import std;

import :close;
import :directory;
import :error;
import :field_infos;
import :fields;
import :fields_consumer;
import :fields_producer;
import :index_types;
import :merge_state;
import :norms_producer;
import :per_field_merge_state;
import :postings_format;
import :segment_info;
import :segment_read_state;
import :segment_write_state;
import :terms;

namespace lucene {

namespace {

/// Iterates a private copy of a list of field names.
class NameListIterator final : public FieldNameIterator {
public:
    explicit NameListIterator(std::vector<std::string> names) : names_(std::move(names)) {}

    Result<const std::string *> next() override {
        if (index_ >= names_.size()) {
            return static_cast<const std::string *>(nullptr);
        }
        return &names_[index_++];
    }

private:
    std::vector<std::string> names_;
    size_t index_{0};
};

/// Exposes only the fields of one group to a postings writer.
///
/// Port of `FilterFields`: `terms` is forwarded unchanged, the iteration and the
/// size report the group only.
class FilterFields final : public Fields {
public:
    FilterFields(const Fields &in, const std::vector<std::string> &fields) : in_(&in), fields_(&fields) {}

    Result<std::unique_ptr<FieldNameIterator>> iterator() const override {
        return std::unique_ptr<FieldNameIterator>(new NameListIterator(*fields_));
    }

    Result<std::unique_ptr<Terms>> terms(std::string_view field) const override { return in_->terms(field); }

    Result<int32_t> size() const override { return static_cast<int32_t>(fields_->size()); }

private:
    const Fields *in_;
    const std::vector<std::string> *fields_;
};

/// One postings format's share of a segment: its fields and its own write state.
struct Group {
    std::string format_name;
    int32_t suffix{0};
    SegmentWriteState state;
    std::vector<std::string> fields;
};

[[nodiscard]] std::string make_segment_suffix(std::string_view format_name, int32_t suffix) {
    return std::string(format_name) + "_" + std::to_string(suffix);
}

/// Appends the per-field suffix to the outer one, refusing to nest two
/// [`PerFieldPostingsFormat`]s.
[[nodiscard]] Result<std::string> full_segment_suffix(std::string_view outer, std::string inner,
                                                      std::string_view field_name) {
    if (outer.empty()) {
        return inner;
    }
    return err(LuceneError::illegal_state("cannot embed PerFieldPostingsFormat inside itself (field \"" +
                                          std::string(field_name) + "\" returned PerFieldPostingsFormat)"));
}

} // namespace

namespace {

/// Writes the postings of a segment, one format at a time.
class PerFieldFieldsWriter final : public FieldsConsumer {
public:
    explicit PerFieldFieldsWriter(const PostingsFormat &format) : format_(&format) {}

    VoidResult write(const SegmentWriteState &state, const SegmentInfo &segment_info, Fields &fields,
                     NormsProducer *norms) override {
        std::vector<Group> groups;
        if (auto built = build_groups(state, fields, groups); !built.has_value()) {
            return built;
        }
        for (auto &group : groups) {
            FilterFields masked(fields, group.fields);
            auto consumer = format_->fields_consumer(group.state, segment_info);
            if (!consumer.has_value()) {
                return std::unexpected(std::move(consumer.error()));
            }
            auto written = consumer.value()->write(group.state, segment_info, masked, norms);
            auto closed = consumer.value()->close();
            if (!closed.has_value()) {
                record_close_error(std::move(closed.error()));
            }
            if (!written.has_value()) {
                return written;
            }
        }
        return {};
    }

    VoidResult merge(const SegmentWriteState &state, const SegmentInfo &segment_info,
                     const MergeState &merge_state, NormsProducer *norms) override {
        // The union of the field names of every input segment, sorted like the
        // reference implementation's merged iterator.
        std::set<std::string> names;
        for (const auto &producer : merge_state.fields_producers) {
            if (producer == nullptr) {
                continue;
            }
            auto iter = producer->iterator();
            if (!iter.has_value()) {
                return std::unexpected(std::move(iter.error()));
            }
            while (true) {
                auto name = iter.value()->next();
                if (!name.has_value()) {
                    return std::unexpected(std::move(name.error()));
                }
                if (name.value() == nullptr) {
                    break;
                }
                names.insert(*name.value());
            }
        }

        std::vector<Group> groups;
        for (const auto &name : names) {
            if (auto added = add_to_group(state, name, groups); !added.has_value()) {
                return added;
            }
        }

        for (auto &group : groups) {
            auto consumer = format_->fields_consumer(group.state, segment_info);
            if (!consumer.has_value()) {
                return std::unexpected(std::move(consumer.error()));
            }
            auto restricted = restrict_merge_state_fields(merge_state, group.fields);
            auto merged = consumer.value()->merge(group.state, segment_info, *restricted, norms);
            auto closed = consumer.value()->close();
            if (!closed.has_value()) {
                record_close_error(std::move(closed.error()));
            }
            if (!merged.has_value()) {
                return merged;
            }
        }
        return {};
    }

    VoidResult close() override {
        if (close_error_.has_value()) {
            return err(std::move(close_error_.value()));
        }
        return {};
    }

private:
    void record_close_error(LuceneError error) {
        if (!close_error_.has_value()) {
            close_error_ = std::move(error);
        }
    }

    /// Assigns every indexed field of `fields` to the group of its format,
    /// recording the format name and suffix in the field infos.
    VoidResult build_groups(const SegmentWriteState &state, Fields &fields, std::vector<Group> &groups) {
        if (state.field_infos == nullptr) {
            return err(LuceneError::illegal_state("the field infos of the segment are not built"));
        }
        auto index = [&groups](std::string_view name) -> size_t {
            for (size_t i = 0; i < groups.size(); ++i) {
                if (groups[i].format_name == name) {
                    return i;
                }
            }
            return groups.size();
        };

        auto iter = fields.iterator();
        if (!iter.has_value()) {
            return std::unexpected(std::move(iter.error()));
        }
        while (true) {
            auto name = iter.value()->next();
            if (!name.has_value()) {
                return std::unexpected(std::move(name.error()));
            }
            if (name.value() == nullptr) {
                break;
            }
            const std::string &field_name = *name.value();
            if (state.field_infos->by_name(field_name) == nullptr) {
                return err(LuceneError::illegal_state("missing FieldInfo for field " + field_name));
            }
            size_t slot = index(format_->name());
            if (slot == groups.size()) {
                const std::string format_name(format_->name());
                const std::string suffix = make_segment_suffix(format_name, 0);
                auto full = full_segment_suffix(state.segment_suffix, suffix, field_name);
                if (!full.has_value()) {
                    return std::unexpected(std::move(full.error()));
                }
                groups.push_back(Group{format_name, 0, SegmentWriteState::copy_with_suffix(state, std::move(full.value())),
                                       {}});
            }
            groups[slot].fields.push_back(field_name);
            if (auto annotated = annotate(state, field_name, groups[slot].format_name, groups[slot].suffix);
                !annotated.has_value()) {
                return annotated;
            }
        }

        for (auto &group : groups) {
            std::sort(group.fields.begin(), group.fields.end());
        }
        return {};
    }

    /// Adds one field to its group, creating it on first sight.
    VoidResult add_to_group(const SegmentWriteState &state, const std::string &field_name, std::vector<Group> &groups) {
        if (state.field_infos == nullptr) {
            return err(LuceneError::illegal_state("the field infos of the segment are not built"));
        }
        if (state.field_infos->by_name(field_name) == nullptr) {
            return err(LuceneError::illegal_state("missing FieldInfo for field " + field_name));
        }
        for (auto &group : groups) {
            if (group.format_name == format_->name()) {
                group.fields.push_back(field_name);
                return annotate(state, field_name, group.format_name, group.suffix);
            }
        }
        const std::string format_name(format_->name());
        const std::string suffix = make_segment_suffix(format_name, 0);
        auto full = full_segment_suffix(state.segment_suffix, suffix, field_name);
        if (!full.has_value()) {
            return std::unexpected(std::move(full.error()));
        }
        groups.push_back(Group{format_name, 0, SegmentWriteState::copy_with_suffix(state, std::move(full.value())),
                               {field_name}});
        return annotate(state, field_name, format_name, 0);
    }

    /// Records the format name and suffix of `field_name` in the field infos.
    VoidResult annotate(const SegmentWriteState &state, const std::string &field_name,
                        const std::string &format_name, int32_t suffix) const {
        auto *infos = const_cast<FieldInfos *>(state.field_infos.get());
        FieldInfo *info = infos->field_info_mut(field_name);
        if (info == nullptr) {
            return err(LuceneError::illegal_state("missing FieldInfo for field " + field_name));
        }
        info->attributes[std::string(PER_FIELD_POSTINGS_FORMAT_KEY)] = format_name;
        info->attributes[std::string(PER_FIELD_POSTINGS_SUFFIX_KEY)] = std::to_string(suffix);
        return {};
    }

    const PostingsFormat *format_;
    std::optional<LuceneError> close_error_;
};

/// Reads a segment written by [`PerFieldPostingsFormat`].
class PerFieldFieldsReader final : public FieldsProducer {
public:
    static Result<std::unique_ptr<PerFieldFieldsReader>> open(const PostingsFormat &format,
                                                              const SegmentReadState &state,
                                                              const SegmentInfo &segment_info) {
        std::unique_ptr<PerFieldFieldsReader> reader{new PerFieldFieldsReader()};
        if (state.field_infos == nullptr) {
            return err(LuceneError::illegal_state("the field infos of the segment are not available"));
        }
        for (const auto &field_info : state.field_infos->fields()) {
            if (!field_info.is_indexed()) {
                continue;
            }
            auto format_entry = field_info.attributes.find(std::string(PER_FIELD_POSTINGS_FORMAT_KEY));
            if (format_entry == field_info.attributes.end()) {
                // A field without the attribute has no postings.
                continue;
            }
            auto suffix_entry = field_info.attributes.find(std::string(PER_FIELD_POSTINGS_SUFFIX_KEY));
            if (suffix_entry == field_info.attributes.end()) {
                return err(LuceneError::illegal_state("missing attribute: " +
                                                      std::string(PER_FIELD_POSTINGS_SUFFIX_KEY) + " for field: " +
                                                      field_info.name));
            }
            int32_t suffix_number = 0;
            const auto *start = suffix_entry->second.data();
            const auto *end = start + suffix_entry->second.size();
            const auto parsed = std::from_chars(start, end, suffix_number);
            if (parsed.ec != std::errc{} || parsed.ptr != end) {
                return err(LuceneError::number_format("invalid attribute: " +
                                                      std::string(PER_FIELD_POSTINGS_SUFFIX_KEY) + "=" +
                                                      suffix_entry->second + " for field: " + field_info.name));
            }
            // The reference implementation reads the postings of a group without
            // re-applying the outer suffix (only the doc values format does that);
            // the port's states never carry an outer suffix, so both agree.
            const std::string segment_suffix = make_segment_suffix(format_entry->second, suffix_number);

            auto found = reader->formats_.find(segment_suffix);
            std::shared_ptr<FieldsProducer> producer;
            if (found != reader->formats_.end()) {
                producer = found->second;
            } else {
                auto read_state = SegmentReadState::copy_with_suffix(state, segment_suffix);
                auto opened = format.fields_producer(read_state, segment_info);
                if (!opened.has_value()) {
                    return std::unexpected(std::move(opened.error()));
                }
                producer = std::shared_ptr<FieldsProducer>(std::move(opened.value()));
                reader->formats_.emplace(segment_suffix, producer);
            }
            reader->fields_.emplace(field_info.name, producer);
        }
        return reader;
    }

    Result<std::unique_ptr<FieldNameIterator>> iterator() const override {
        std::vector<std::string> names;
        names.reserve(fields_.size());
        for (const auto &[name, producer] : fields_) {
            names.push_back(name);
        }
        return std::unique_ptr<FieldNameIterator>(new NameListIterator(std::move(names)));
    }

    Result<std::unique_ptr<Terms>> terms(std::string_view field) const override {
        auto found = fields_.find(std::string(field));
        if (found == fields_.end()) {
            return std::unique_ptr<Terms>{};
        }
        return found->second->terms(field);
    }

    Result<int32_t> size() const override { return static_cast<int32_t>(fields_.size()); }

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

    Result<std::unique_ptr<FieldsProducer>> get_merge_instance() override {
        std::unique_ptr<PerFieldFieldsReader> copy{new PerFieldFieldsReader()};
        std::map<const FieldsProducer *, std::shared_ptr<FieldsProducer>> replacements;
        for (const auto &[suffix, producer] : formats_) {
            auto instance = producer->get_merge_instance();
            if (!instance.has_value()) {
                return std::unexpected(std::move(instance.error()));
            }
            std::shared_ptr<FieldsProducer> replacement = instance.value() != nullptr
                                                                  ? std::shared_ptr<FieldsProducer>(
                                                                            std::move(instance.value()))
                                                                  : producer;
            replacements.emplace(producer.get(), replacement);
            copy->formats_.emplace(suffix, replacement);
        }
        for (const auto &[name, producer] : fields_) {
            auto found = replacements.find(producer.get());
            if (found == replacements.end()) {
                return err(LuceneError::illegal_state("missing merge instance for field: " + name));
            }
            copy->fields_.emplace(name, found->second);
        }
        return std::unique_ptr<FieldsProducer>(std::move(copy));
    }

private:
    PerFieldFieldsReader() = default;

    /// Field name to the producer of the format that wrote it.
    std::map<std::string, std::shared_ptr<FieldsProducer>> fields_;
    /// Segment suffix (`<format>_<n>`) to its producer.
    std::map<std::string, std::shared_ptr<FieldsProducer>> formats_;
};

} // namespace

Result<std::unique_ptr<FieldsConsumer>> PerFieldPostingsFormat::fields_consumer(const SegmentWriteState &state,
                                                                               const SegmentInfo &segment_info) const {
    static_cast<void>(state);
    static_cast<void>(segment_info);
    return std::unique_ptr<FieldsConsumer>(new PerFieldFieldsWriter(*default_format_));
}

Result<std::unique_ptr<FieldsProducer>> PerFieldPostingsFormat::fields_producer(const SegmentReadState &state,
                                                                               const SegmentInfo &segment_info) const {
    auto reader = PerFieldFieldsReader::open(*default_format_, state, segment_info);
    if (!reader.has_value()) {
        return std::unexpected(std::move(reader.error()));
    }
    return std::unique_ptr<FieldsProducer>(std::move(reader.value()));
}

} // namespace lucene
