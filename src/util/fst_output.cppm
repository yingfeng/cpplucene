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

export module lucene_core:fst_output;

import std;

import :bytes_ref;
import :ints_ref;

namespace lucene {

/// A type-erased output of an [`Arc`](fst.cppm).
///
/// The reference implementation stores a `std::any` here.  A `BytesRef` (or an
/// `IntsRef`) is larger than the small-object buffer of `std::any`, so every copy
/// of an arc output allocated, and a single term lookup copies the outputs of
/// hundreds of arcs while walking the FST — the allocator ended up among the
/// hottest symbols of a lookup.  This container holds the four output types the
/// FST has inline, and answers the same "no output" semantics for a slot that
/// holds nothing (or another type): an empty value of the requested type.
export class FstOutput {
public:
    FstOutput() noexcept = default;

    template <class T, std::enable_if_t<!std::is_same_v<std::decay_t<T>, FstOutput>, int> = 0>
    // NOLINTNEXTLINE(google-explicit-constructor): mirrors `std::any`'s converting constructor.
    FstOutput(T &&value) {
        emplace<std::decay_t<T>>(std::forward<T>(value));
    }

    FstOutput(const FstOutput &other) {
        if (other.table_ != nullptr) {
            other.table_->copy(storage_, other.storage_);
        }
        table_ = other.table_;
    }

    FstOutput(FstOutput &&other) noexcept {
        if (other.table_ != nullptr) {
            other.table_->move(storage_, other.storage_);
        }
        table_ = other.table_;
        other.table_ = nullptr;
    }

    FstOutput &operator=(const FstOutput &other) {
        if (this == &other) {
            return *this;
        }
        reset();
        if (other.table_ != nullptr) {
            other.table_->copy(storage_, other.storage_);
        }
        table_ = other.table_;
        return *this;
    }

    FstOutput &operator=(FstOutput &&other) noexcept {
        if (this == &other) {
            return *this;
        }
        reset();
        if (other.table_ != nullptr) {
            other.table_->move(storage_, other.storage_);
        }
        table_ = other.table_;
        other.table_ = nullptr;
        return *this;
    }

    ~FstOutput() { reset(); }

    [[nodiscard]] bool has_value() const noexcept { return table_ != nullptr; }

    void reset() noexcept {
        if (table_ != nullptr) {
            table_->destroy(storage_);
            table_ = nullptr;
        }
    }

    /// The stored value, or an empty `T` when this slot holds nothing or another type.
    template <class T> [[nodiscard]] const T &as() const {
        const T *value = as_ptr<T>();
        return value != nullptr ? *value : empty_value<T>();
    }

    /// A copy of `as<T>()`, for the call sites that want a value.
    template <class T> [[nodiscard]] T get() const { return as<T>(); }

    template <class T> [[nodiscard]] T *as_ptr() noexcept {
        return table_ == table_for<T>() ? reinterpret_cast<T *>(storage_) : nullptr;
    }

    template <class T> [[nodiscard]] const T *as_ptr() const noexcept {
        return table_ == table_for<T>() ? reinterpret_cast<const T *>(storage_) : nullptr;
    }

private:
    struct VTable {
        void (*copy)(void *dest, const void *source);
        void (*move)(void *dest, void *source);
        void (*destroy)(void *value) noexcept;
    };

    template <class T> static const T &empty_value() {
        static const T empty{};
        return empty;
    }

    template <class T> static const VTable *table_for() noexcept {
        static const VTable table{
            [](void *dest, const void *source) {
                std::construct_at(static_cast<T *>(dest), *static_cast<const T *>(source));
            },
            [](void *dest, void *source) {
                std::construct_at(static_cast<T *>(dest), std::move(*static_cast<T *>(source)));
            },
            [](void *value) noexcept { std::destroy_at(static_cast<T *>(value)); },
        };
        return &table;
    }

    template <class T, class U> void emplace(U &&value) {
        static_assert(sizeof(T) <= sizeof(storage_), "an FST output type must fit the inline buffer");
        static_assert(alignof(T) <= alignof(std::max_align_t));
        reset();
        std::construct_at(reinterpret_cast<T *>(storage_), std::forward<U>(value));
        table_ = table_for<T>();
    }

    /// 64 bytes: the largest output types are `BytesRef` and `IntsRef` (a vector
    /// the size of three words plus two `size_t`s).  Deliberately not
    /// zero-initialised: it is only read through `table_`, which is null until a
    /// value is constructed, and every arc of every seek copies these.
    alignas(std::max_align_t) std::byte storage_[64];
    const VTable *table_{nullptr};
};

} // namespace lucene
