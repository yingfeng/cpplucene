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

export module lucene_core:doc_values;

import :types;
import std;

import :binary_doc_values;
import :bytes_ref;
import :doc_values_iterator;
import :error;
import :numeric_doc_values;
import :sorted_doc_values;
import :sorted_numeric_doc_values;
import :sorted_set_doc_values;
import :terms_enum;

namespace lucene {

/// The doc values of a field that holds no value at all.
///
/// The empty iterators report `NO_MORE_DOCS` from the first call and return a
/// neutral value (`0`, an empty `BytesRef`, ordinal `-1`) instead of failing, which
/// is what the reference implementation's `EmptyNumeric`/`EmptyBinary`/`EmptySorted`/`EmptySortedSet`
/// do; callers are expected to notice that nothing is positioned.
export class EmptyNumericDocValues final : public NumericDocValues {
public:
    [[nodiscard]] int32_t doc_id() const override { return doc_; }
    [[nodiscard]] Result<int32_t> next_doc() override { return done(); }
    [[nodiscard]] Result<int32_t> advance(int32_t /*target*/) override { return done(); }
    [[nodiscard]] Result<int32_t> slow_advance(int32_t target) override { return advance(target); }
    [[nodiscard]] Result<int64_t> cost() const override { return int64_t{0}; }
    [[nodiscard]] Result<bool> advance_exact(int32_t target) override {
        doc_ = target;
        return false;
    }
    [[nodiscard]] Result<int64_t> long_value() override { return int64_t{0}; }

private:
    [[nodiscard]] Result<int32_t> done() {
        doc_ = NO_MORE_DOCS;
        return doc_;
    }

    int32_t doc_{-1};
};

/// See [`EmptyNumericDocValues`].
export class EmptyBinaryDocValues final : public BinaryDocValues {
public:
    [[nodiscard]] int32_t doc_id() const override { return doc_; }
    [[nodiscard]] Result<int32_t> next_doc() override { return done(); }
    [[nodiscard]] Result<int32_t> advance(int32_t /*target*/) override { return done(); }
    [[nodiscard]] Result<int32_t> slow_advance(int32_t target) override { return advance(target); }
    [[nodiscard]] Result<int64_t> cost() const override { return int64_t{0}; }
    [[nodiscard]] Result<bool> advance_exact(int32_t target) override {
        doc_ = target;
        return false;
    }
    [[nodiscard]] Result<BytesRef> binary_value() override { return BytesRef(); }

private:
    [[nodiscard]] Result<int32_t> done() {
        doc_ = NO_MORE_DOCS;
        return doc_;
    }

    int32_t doc_{-1};
};

/// See [`EmptyNumericDocValues`].
export class EmptySortedDocValues final : public SortedDocValues {
public:
    [[nodiscard]] int32_t doc_id() const override { return doc_; }
    [[nodiscard]] Result<int32_t> next_doc() override { return done(); }
    [[nodiscard]] Result<int32_t> advance(int32_t /*target*/) override { return done(); }
    [[nodiscard]] Result<int32_t> slow_advance(int32_t target) override { return advance(target); }
    [[nodiscard]] Result<int64_t> cost() const override { return int64_t{0}; }
    [[nodiscard]] Result<bool> advance_exact(int32_t target) override {
        doc_ = target;
        return false;
    }
    [[nodiscard]] Result<int32_t> ord_value() override { return -1; }
    [[nodiscard]] Result<BytesRef> lookup_ord(int32_t /*ord*/) override { return BytesRef(); }
    [[nodiscard]] Result<int32_t> get_value_count() const override { return 0; }

private:
    [[nodiscard]] Result<int32_t> done() {
        doc_ = NO_MORE_DOCS;
        return doc_;
    }

    int32_t doc_{-1};
};

/// See [`EmptyNumericDocValues`].
export class EmptySortedNumericDocValues final : public SortedNumericDocValues {
public:
    [[nodiscard]] int32_t doc_id() const override { return doc_; }
    [[nodiscard]] Result<int32_t> next_doc() override { return done(); }
    [[nodiscard]] Result<int32_t> advance(int32_t /*target*/) override { return done(); }
    [[nodiscard]] Result<int32_t> slow_advance(int32_t target) override { return advance(target); }
    [[nodiscard]] Result<int64_t> cost() const override { return int64_t{0}; }
    [[nodiscard]] Result<bool> advance_exact(int32_t target) override {
        doc_ = target;
        return false;
    }
    [[nodiscard]] Result<int64_t> next_value() override { return int64_t{0}; }
    [[nodiscard]] Result<int32_t> doc_value_count() override { return 0; }

private:
    [[nodiscard]] Result<int32_t> done() {
        doc_ = NO_MORE_DOCS;
        return doc_;
    }

    int32_t doc_{-1};
};

/// See [`EmptyNumericDocValues`].
export class EmptySortedSetDocValues final : public SortedSetDocValues {
public:
    [[nodiscard]] int32_t doc_id() const override { return doc_; }
    [[nodiscard]] Result<int32_t> next_doc() override { return done(); }
    [[nodiscard]] Result<int32_t> advance(int32_t /*target*/) override { return done(); }
    [[nodiscard]] Result<int32_t> slow_advance(int32_t target) override { return advance(target); }
    [[nodiscard]] Result<int64_t> cost() const override { return int64_t{0}; }
    [[nodiscard]] Result<bool> advance_exact(int32_t target) override {
        doc_ = target;
        return false;
    }
    [[nodiscard]] Result<int64_t> next_ord() override { return NO_MORE_ORDS; }
    [[nodiscard]] Result<int32_t> doc_value_count() override { return 0; }
    [[nodiscard]] Result<BytesRef> lookup_ord(int64_t /*ord*/) override { return BytesRef(); }
    [[nodiscard]] Result<int64_t> get_value_count() const override { return int64_t{0}; }

private:
    [[nodiscard]] Result<int32_t> done() {
        doc_ = NO_MORE_DOCS;
        return doc_;
    }

    int32_t doc_{-1};
};

} // namespace lucene
