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

module lucene_core:soft_deletes_reader.impl;

import :soft_deletes_reader;
import :types;
import std;

import :bits;
import :codec_reader;
import :standard_directory_reader;
import :doc_id_set_iterator;
import :error;
import :filter_codec_reader;
import :filter_directory_reader;
import :fixed_bit_set;
import :leaf_reader;
import :numeric_doc_values;

namespace lucene {

namespace {

/// Copies `source` into a fresh bit set of `num_bits` bits; when `source` is
/// null every bit is set (nothing is deleted yet).
Result<FixedBitSet> copy_of_live_docs(const Bits *source, int32_t num_bits) {
    FixedBitSet bits(static_cast<size_t>(num_bits));
    if (num_bits <= 0) {
        return bits;
    }
    if (source == nullptr) {
        if (auto r = bits.set_with_range(0, static_cast<size_t>(num_bits)); !r) {
            return std::unexpected(std::move(r.error()));
        }
        return bits;
    }
    for (int32_t doc = 0; doc < num_bits; ++doc) {
        auto live = source->get(static_cast<size_t>(doc));
        if (!live) {
            return std::unexpected(std::move(live.error()));
        }
        if (live.value()) {
            bits.set(static_cast<size_t>(doc));
        }
    }
    return bits;
}

/// Wraps the leaves of a [`DirectoryReader`], applying soft deletes to each and
/// dropping the leaves that become empty.
class SoftDeletesSubReaderWrapper final : public SubReaderWrapper {
public:
    explicit SoftDeletesSubReaderWrapper(std::string field) : field_(std::move(field)) {}

    [[nodiscard]] Result<std::vector<std::shared_ptr<LeafReader>>>
    wrap_readers(std::vector<std::shared_ptr<LeafReader>> readers) const override {
        std::vector<std::shared_ptr<LeafReader>> wrapped;
        wrapped.reserve(readers.size());
        for (std::shared_ptr<LeafReader> &reader : readers) {
            auto wrapped_leaf = wrap(std::move(reader));
            if (!wrapped_leaf) {
                return std::unexpected(std::move(wrapped_leaf.error()));
            }
            auto num_docs = wrapped_leaf.value()->num_docs();
            if (!num_docs) {
                return std::unexpected(std::move(num_docs.error()));
            }
            // A leaf whose documents are all soft-deleted disappears.
            if (num_docs.value() != 0) {
                wrapped.push_back(std::move(wrapped_leaf.value()));
            }
        }
        return wrapped;
    }

    [[nodiscard]] Result<std::shared_ptr<LeafReader>> wrap(std::shared_ptr<LeafReader> reader) const override {
        auto codec = std::dynamic_pointer_cast<CodecReader>(reader);
        if (codec == nullptr) {
            return err(LuceneError::illegal_argument("soft deletes need codec readers"));
        }
        auto wrapped = wrap_soft_deletes(std::move(codec), field_);
        if (!wrapped) {
            return std::unexpected(std::move(wrapped.error()));
        }
        return std::shared_ptr<LeafReader>(std::move(wrapped.value()));
    }

private:
    std::string field_;
};

} // namespace

SoftDeletesFilterCodecReader::SoftDeletesFilterCodecReader(std::shared_ptr<CodecReader> delegate,
                                                           std::shared_ptr<const Bits> live_docs, int32_t num_docs)
    : FilterCodecReader(std::move(delegate)), live_docs_(std::move(live_docs)), num_docs_(num_docs) {}

Result<int32_t> SoftDeletesFilterCodecReader::num_deleted_docs() const {
    auto max_doc = delegate_->max_doc();
    if (!max_doc) {
        return std::unexpected(std::move(max_doc.error()));
    }
    return max_doc.value() - num_docs_;
}

std::string SoftDeletesFilterCodecReader::to_string() const {
    auto inner = delegate_->to_string();
    return "SoftDeletesFilterCodecReader(" + inner + ", numDocs=" + std::to_string(num_docs_) + ")";
}

Result<std::shared_ptr<CodecReader>> wrap_soft_deletes(std::shared_ptr<CodecReader> reader, std::string_view field) {
    if (reader == nullptr) {
        return err(LuceneError::illegal_argument("soft deletes need a reader"));
    }
    auto max_doc = reader->max_doc();
    if (!max_doc) {
        return std::unexpected(std::move(max_doc.error()));
    }

    // Without the soft-delete field there is nothing to hide.
    auto values = reader->get_numeric_doc_values(field);
    if (!values) {
        return std::unexpected(std::move(values.error()));
    }
    if (values.value() == nullptr) {
        return reader;
    }

    auto existing = reader->live_docs();
    if (!existing) {
        return std::unexpected(std::move(existing.error()));
    }
    auto bits = copy_of_live_docs(existing.value(), max_doc.value());
    if (!bits) {
        return std::unexpected(std::move(bits.error()));
    }
    FixedBitSet live = std::move(bits.value());

    int32_t num_soft_deletes = 0;
    NumericDocValues &dv = *values.value();
    while (true) {
        auto doc = dv.next_doc();
        if (!doc) {
            return std::unexpected(std::move(doc.error()));
        }
        if (doc.value() == NO_MORE_DOCS) {
            break;
        }
        auto value = dv.long_value();
        if (!value) {
            return std::unexpected(std::move(value.error()));
        }
        if (value.value() != 0 && live.clear(static_cast<size_t>(doc.value()))) {
            num_soft_deletes += 1;
        }
    }

    if (num_soft_deletes == 0) {
        return reader;
    }

    auto num_deleted = reader->num_deleted_docs();
    if (!num_deleted) {
        return std::unexpected(std::move(num_deleted.error()));
    }
    const int32_t num_docs = max_doc.value() - (num_deleted.value() + num_soft_deletes);
    if (num_docs < 0) {
        return err(LuceneError::corrupt_index("more soft deletes than documents in the segment"));
    }

    auto live_docs = std::make_shared<FixedBitSet>(std::move(live));
    return std::shared_ptr<CodecReader>(
            new SoftDeletesFilterCodecReader(std::move(reader), std::move(live_docs), num_docs));
}

Result<std::shared_ptr<DirectoryReader>> wrap_soft_deletes_directory_reader(std::shared_ptr<DirectoryReader> reader,
                                                                           std::string field) {
    if (reader == nullptr) {
        return err(LuceneError::illegal_argument("soft deletes need a directory reader"));
    }
    const SoftDeletesSubReaderWrapper wrapper(std::move(field));
    return wrap_directory_reader(std::move(reader), wrapper);
}

} // namespace lucene
