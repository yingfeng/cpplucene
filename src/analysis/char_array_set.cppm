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

export module lucene_core:char_array_set;

import :types;
import std;

import :char_array_map;

namespace lucene {

/// A set of character arrays, built on [`CharArrayMap`].
///
/// Port of `analysis/char_array_set.rs`.  The stop word lists of the analyzers
/// are the main user; like the reference, adding a key that is already present
/// is not an error.
export class CharArraySet {
public:
    /// `ignore_case` requests case-insensitive keys.
    explicit CharArraySet(bool ignore_case) : map_(ignore_case) {}
    explicit CharArraySet(CharArrayMap<bool> map) : map_(std::move(map)) {}

    /// An empty, case-sensitive set.
    [[nodiscard]] static CharArraySet empty_set() { return CharArraySet(false); }

    // ---- insert ----

    /// Adds `key`; returns `true` when it was already present.
    bool add(const std::u32string &key) { return map_.put(key, true).has_value(); }
    /// Adds the UTF-8 `key`; returns `true` when it was already present.
    bool add_str(std::string_view key) { return map_.put_str(key, true).has_value(); }
    /// Adds every UTF-8 key of `keys`.
    void add_all(std::span<const std::string> keys) {
        for (const std::string &key : keys) {
            (void)add_str(key);
        }
    }

    void clear() noexcept { map_.clear(); }

    // ---- lookup ----

    [[nodiscard]] bool contains_key(const std::u32string &key) const { return map_.contains_key(key); }
    [[nodiscard]] bool contains_key(const std::u32string &key, size_t offset, size_t length) const {
        return map_.contains_key(key, offset, length);
    }
    [[nodiscard]] bool contains_key_str(std::string_view key) const { return map_.contains_key_str(key); }
    [[nodiscard]] bool contains_key_str(std::string_view key, size_t offset, size_t length) const {
        return map_.contains_key_str(key, offset, length);
    }

    [[nodiscard]] size_t size() const noexcept { return map_.size(); }
    [[nodiscard]] bool is_empty() const noexcept { return map_.is_empty(); }
    [[nodiscard]] bool ignore_case() const noexcept { return map_.ignore_case(); }
    [[nodiscard]] const CharArrayMap<bool> &map() const noexcept { return map_; }

    /// `[key, key, ...]`, in unspecified order.
    [[nodiscard]] std::string to_string() const {
        std::string text = "[";
        bool first = true;
        for (const std::u32string &key : map_.keys()) {
            if (!first) {
                text += ", ";
            }
            first = false;
            text += CharArrayMap<bool>::utf8(key);
        }
        text += ']';
        return text;
    }

private:
    CharArrayMap<bool> map_;
};

/// The stop words of [`StopFilter`], as a [`CharArraySet`].
///
/// **Deviation:** [`StopFilter`] still stores a `std::set<std::string>` (its
/// public constructor takes one), so this factory bridges the two until the
/// filter is migrated to `CharArraySet`.
[[nodiscard]] inline CharArraySet to_char_array_set(const std::set<std::string, std::less<>> &words,
                                                    bool ignore_case = true) {
    CharArraySet set(ignore_case);
    for (const std::string &word : words) {
        (void)set.add_str(word);
    }
    return set;
}

} // namespace lucene
