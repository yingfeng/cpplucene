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

module lucene_core:doc_values_field_updates.impl;

import :doc_values_field_updates;

import :bytes_ref;
import :doc_id_set_iterator;
import :error;
import :index_types;
import :types;

import std;

namespace lucene {

namespace {

/// The index of `doc` in `docs`, which must be sorted.
std::optional<size_t> find_doc(const std::vector<int32_t> &docs, int32_t doc) {
    const auto found = std::lower_bound(docs.begin(), docs.end(), doc);
    if (found == docs.end() || *found != doc) {
        return std::nullopt;
    }
    return static_cast<size_t>(found - docs.begin());
}

/// Sorts `docs` together with `values`/`present`, keeping the last update of
/// each document: a document updated twice in one commit holds its final value.
template <typename T>
void sort_by_doc(std::vector<int32_t> &docs, std::vector<T> &values, std::vector<uint8_t> &present) {
    std::vector<size_t> order(docs.size());
    std::iota(order.begin(), order.end(), size_t{0});
    std::ranges::stable_sort(order, [&docs](size_t left, size_t right) { return docs[left] < docs[right]; });

    std::vector<int32_t> sorted_docs;
    std::vector<T> sorted_values;
    std::vector<uint8_t> sorted_present;
    sorted_docs.reserve(docs.size());
    sorted_values.reserve(values.size());
    sorted_present.reserve(present.size());
    for (size_t position = 0; position < order.size(); ++position) {
        const size_t index = order[position];
        const bool last_of_doc = position + 1 == order.size() || docs[order[position + 1]] != docs[index];
        if (!last_of_doc) {
            continue;
        }
        sorted_docs.push_back(docs[index]);
        sorted_values.push_back(std::move(values[index]));
        sorted_present.push_back(present[index]);
    }
    docs = std::move(sorted_docs);
    values = std::move(sorted_values);
    present = std::move(sorted_present);
}

} // namespace

// ---- NumericDocValuesFieldUpdates ----

namespace {

/// Iterates the documents of a numeric batch that keep a value.
class NumericIterator final : public DocValuesFieldUpdatesIterator {
public:
    explicit NumericIterator(const NumericDocValuesFieldUpdates &updates) : updates_(&updates) {}

    [[nodiscard]] bool next() override {
        if (index_ == npos) {
            index_ = 0;
        } else if (index_ < updates_->docs().size()) {
            ++index_;
        }
        // Skip the documents whose value was removed.
        while (index_ < updates_->docs().size() && updates_->present()[index_] == 0) {
            ++index_;
        }
        return index_ < updates_->docs().size();
    }

    [[nodiscard]] int32_t doc() const override {
        if (index_ >= updates_->docs().size()) {
            return NO_MORE_DOCS;
        }
        return updates_->docs()[index_];
    }

    [[nodiscard]] Result<int64_t> long_value() const override {
        if (index_ >= updates_->values().size()) {
            return err(LuceneError::illegal_state("the iteration is past the end of the updates"));
        }
        return updates_->values()[index_];
    }

    [[nodiscard]] Result<BytesRef> binary_value() const override {
        return err(LuceneError::unsupported_operation(
                "NumericDocValuesFieldUpdatesIterator does not support binary_value"));
    }

private:
    /// `npos` before the first call of `next`.
    static constexpr size_t npos = static_cast<size_t>(-1);

    const NumericDocValuesFieldUpdates *updates_;
    size_t index_{npos};
};

} // namespace

NumericDocValuesFieldUpdates::NumericDocValuesFieldUpdates(std::string field, DocValuesType sub_type)
        : field_(std::move(field)), sub_type_(sub_type) {}

VoidResult NumericDocValuesFieldUpdates::add_numeric(int32_t doc, std::optional<int64_t> value) {
    docs_.push_back(doc);
    values_.push_back(value.value_or(0));
    present_.push_back(value.has_value() ? 1 : 0);
    finished_ = false;
    return {};
}

VoidResult NumericDocValuesFieldUpdates::add_binary(int32_t doc, std::optional<BytesRef> value) {
    static_cast<void>(doc);
    static_cast<void>(value);
    // The reference implementation reports this through its unreachable branch:
    // a numeric batch can only ever hold numeric values.
    return err(LuceneError::unsupported_operation("numericDocValuesFieldUpdates does not support add_binary"));
}

void NumericDocValuesFieldUpdates::finish() {
    sort_by_doc(docs_, values_, present_);
    finished_ = true;
}

int64_t NumericDocValuesFieldUpdates::ram_bytes_used() const {
    return static_cast<int64_t>(sizeof(NumericDocValuesFieldUpdates)) +
           static_cast<int64_t>(docs_.size() * sizeof(int32_t)) +
           static_cast<int64_t>(values_.size() * sizeof(int64_t)) +
           static_cast<int64_t>(present_.size());
}

bool NumericDocValuesFieldUpdates::is_updated(int32_t doc) const {
    return find_doc(docs_, doc).has_value();
}

std::optional<int64_t> NumericDocValuesFieldUpdates::numeric_value(int32_t doc) const {
    const auto index = find_doc(docs_, doc);
    if (!index.has_value() || present_[index.value()] == 0) {
        return std::nullopt;
    }
    return values_[index.value()];
}

std::optional<BytesRef> NumericDocValuesFieldUpdates::binary_value(int32_t doc) const {
    static_cast<void>(doc);
    return std::nullopt;
}

std::unique_ptr<DocValuesFieldUpdatesIterator> NumericDocValuesFieldUpdates::iterator() const {
    if (!finished_) {
        return nullptr;
    }
    return std::unique_ptr<DocValuesFieldUpdatesIterator>(new NumericIterator(*this));
}

// ---- BinaryDocValuesFieldUpdates ----

namespace {

/// Iterates the documents of a binary batch that keep a value.
class BinaryIterator final : public DocValuesFieldUpdatesIterator {
public:
    explicit BinaryIterator(const BinaryDocValuesFieldUpdates &updates) : updates_(&updates) {}

    [[nodiscard]] bool next() override {
        if (index_ == npos) {
            index_ = 0;
        } else if (index_ < updates_->docs().size()) {
            ++index_;
        }
        // Skip the documents whose value was removed.
        while (index_ < updates_->docs().size() && updates_->present()[index_] == 0) {
            ++index_;
        }
        return index_ < updates_->docs().size();
    }

    [[nodiscard]] int32_t doc() const override {
        if (index_ >= updates_->docs().size()) {
            return NO_MORE_DOCS;
        }
        return updates_->docs()[index_];
    }

    [[nodiscard]] Result<int64_t> long_value() const override {
        return err(LuceneError::unsupported_operation(
                "BinaryDocValuesFieldUpdatesIterator does not support long_value"));
    }

    [[nodiscard]] Result<BytesRef> binary_value() const override {
        if (index_ >= updates_->values().size()) {
            return err(LuceneError::illegal_state("the iteration is past the end of the updates"));
        }
        return updates_->values()[index_];
    }

private:
    /// `npos` before the first call of `next`.
    static constexpr size_t npos = static_cast<size_t>(-1);

    const BinaryDocValuesFieldUpdates *updates_;
    size_t index_{npos};
};

} // namespace

BinaryDocValuesFieldUpdates::BinaryDocValuesFieldUpdates(std::string field, DocValuesType sub_type)
        : field_(std::move(field)), sub_type_(sub_type) {}

VoidResult BinaryDocValuesFieldUpdates::add_numeric(int32_t doc, std::optional<int64_t> value) {
    static_cast<void>(doc);
    static_cast<void>(value);
    // See NumericDocValuesFieldUpdates::add_binary.
    return err(LuceneError::unsupported_operation("binaryDocValuesFieldUpdates does not support add_numeric"));
}

VoidResult BinaryDocValuesFieldUpdates::add_binary(int32_t doc, std::optional<BytesRef> value) {
    docs_.push_back(doc);
    values_.push_back(value.value_or(BytesRef{}));
    present_.push_back(value.has_value() ? 1 : 0);
    finished_ = false;
    return {};
}

void BinaryDocValuesFieldUpdates::finish() {
    sort_by_doc(docs_, values_, present_);
    finished_ = true;
}

int64_t BinaryDocValuesFieldUpdates::ram_bytes_used() const {
    int64_t bytes = static_cast<int64_t>(sizeof(BinaryDocValuesFieldUpdates)) +
                    static_cast<int64_t>(docs_.size() * sizeof(int32_t)) + static_cast<int64_t>(present_.size());
    for (size_t i = 0; i < values_.size(); ++i) {
        if (present_[i] != 0) {
            bytes += static_cast<int64_t>(values_[i].length());
        }
    }
    return bytes;
}

bool BinaryDocValuesFieldUpdates::is_updated(int32_t doc) const {
    return find_doc(docs_, doc).has_value();
}

std::optional<int64_t> BinaryDocValuesFieldUpdates::numeric_value(int32_t doc) const {
    static_cast<void>(doc);
    return std::nullopt;
}

std::optional<BytesRef> BinaryDocValuesFieldUpdates::binary_value(int32_t doc) const {
    const auto index = find_doc(docs_, doc);
    if (!index.has_value() || present_[index.value()] == 0) {
        return std::nullopt;
    }
    return values_[index.value()];
}

std::unique_ptr<DocValuesFieldUpdatesIterator> BinaryDocValuesFieldUpdates::iterator() const {
    if (!finished_) {
        return nullptr;
    }
    return std::unique_ptr<DocValuesFieldUpdatesIterator>(new BinaryIterator(*this));
}

} // namespace lucene
