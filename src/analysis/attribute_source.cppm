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

export module lucene_core:attribute_source;

import :types;

import std;

import :attributes;

namespace lucene {

/// Holds the attribute instances shared by every stage of a token stream.
///
/// Port of `core/util/attribute_source.rs`.  Attribute identity is the concrete
/// C++ type, which is the same equivalence the Rust implementation expresses
/// through its attribute enum.
export class AttributeSource {
public:
    AttributeSource() = default;
    AttributeSource(const AttributeSource &other);
    AttributeSource &operator=(const AttributeSource &other);
    AttributeSource(AttributeSource &&) noexcept = default;
    AttributeSource &operator=(AttributeSource &&) noexcept = default;
    ~AttributeSource() = default;

    /// Returns the attribute of type `T`, creating it when missing.
    template <class T>
    T *add_attribute() {
        static_assert(std::is_base_of_v<Attribute, T>, "T must derive from Attribute");
        for (const auto &attribute : attributes_) {
            if (auto *typed = dynamic_cast<T *>(attribute.get()); typed != nullptr) {
                return typed;
            }
        }
        attributes_.push_back(std::make_unique<T>());
        return static_cast<T *>(attributes_.back().get());
    }

    /// Returns the attribute of type `T`, or `nullptr` when absent.
    template <class T>
    [[nodiscard]] T *get_attribute() {
        for (const auto &attribute : attributes_) {
            if (auto *typed = dynamic_cast<T *>(attribute.get()); typed != nullptr) {
                return typed;
            }
        }
        return nullptr;
    }

    /// Const overload of [`get_attribute`].
    template <class T>
    [[nodiscard]] const T *get_attribute() const {
        for (const auto &attribute : attributes_) {
            if (const auto *typed = dynamic_cast<const T *>(attribute.get()); typed != nullptr) {
                return typed;
            }
        }
        return nullptr;
    }

    template <class T>
    [[nodiscard]] bool has_attribute() const {
        return get_attribute<T>() != nullptr;
    }

    /// Adds (and shares) every attribute instance of `other` that is missing
    /// here.  Used when chaining filters onto a tokenizer.
    void add_attributes(const AttributeSource &other);

    /// Resets every attribute to its default value.
    void clear_attributes();

    /// Alias used at the end of a stream (`end_attributes` in rlucene).
    void end_attributes() { clear_attributes(); }

    [[nodiscard]] size_t size() const noexcept { return attributes_.size(); }

    [[nodiscard]] std::string to_string() const;

private:
    std::vector<std::unique_ptr<Attribute>> attributes_{};
};

} // namespace lucene
