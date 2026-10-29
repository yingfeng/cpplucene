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

module lucene_core:attribute_source.impl;

import :attribute_source;
import :types;
import :debug;
import :attributes;

import std;

namespace lucene {

AttributeSource::AttributeSource(const AttributeSource &other) {
    attributes_.reserve(other.attributes_.size());
    for (const auto &attribute : other.attributes_) {
        attributes_.push_back(attribute->clone());
    }
}

AttributeSource &AttributeSource::operator=(const AttributeSource &other) {
    if (this == &other) {
        return *this;
    }
    attributes_.clear();
    attributes_.reserve(other.attributes_.size());
    for (const auto &attribute : other.attributes_) {
        attributes_.push_back(attribute->clone());
    }
    return *this;
}

void AttributeSource::add_attributes(const AttributeSource &other) {
    for (const auto &attribute : other.attributes_) {
        bool present = false;
        for (const auto &mine : attributes_) {
            if (mine->name() == attribute->name()) {
                present = true;
                break;
            }
        }
        if (!present) {
            attributes_.push_back(attribute->clone());
        }
    }
}

void AttributeSource::clear_attributes() {
    for (auto &attribute : attributes_) {
        attribute->clear();
    }
}

AttributeSource::State AttributeSource::capture_state() const {
    State state;
    state.reserve(attributes_.size());
    for (const auto &attribute : attributes_) {
        state.push_back(attribute->clone());
    }
    return state;
}

void AttributeSource::restore_state(const State &state) {
    const size_t count = std::min(attributes_.size(), state.size());
    for (size_t i = 0; i < count; ++i) {
        attributes_[i]->copy_from(*state[i]);
    }
}

std::string AttributeSource::to_string() const {
    std::string result = "AttributeSource(";
    for (size_t i = 0; i < attributes_.size(); ++i) {
        if (i > 0) {
            result += ",";
        }
        result += attributes_[i]->name();
    }
    result.push_back(')');
    return result;
}

} // namespace lucene
