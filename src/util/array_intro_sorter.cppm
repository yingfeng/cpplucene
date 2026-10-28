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

export module lucene_core:array_intro_sorter;

import :types;
import std;

import :error;
import :intro_sorter;
import :sorter;

namespace lucene {

/// An [`IntroSorter`] over a slice of copyable values.
///
/// Port of `util/array_intro_sorter.rs`.  `Compare` is any callable taking two
/// `const T &` and returning `Result<int32_t>` (negative when the first sorts
/// first).
///
/// Not stable.
export template <typename T, typename Compare>
class ArrayIntroSorter final : public IntroSorter {
public:
    ArrayIntroSorter(std::span<T> array, Compare compare)
        : array_(array), compare_(std::move(compare)) {}

    [[nodiscard]] Result<int32_t> compare(size_t i, size_t j) override { return compare_(array_[i], array_[j]); }

    [[nodiscard]] VoidResult swap(size_t i, size_t j) override {
        std::swap(array_[i], array_[j]);
        return {};
    }

    [[nodiscard]] VoidResult set_pivot(size_t i) override {
        pivot_ = array_[i];
        return {};
    }

    [[nodiscard]] Result<int32_t> compare_pivot(size_t j) override { return compare_(pivot_, array_[j]); }

private:
    std::span<T> array_;
    Compare compare_;
    T pivot_{};
};

} // namespace lucene
