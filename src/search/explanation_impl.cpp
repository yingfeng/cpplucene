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

module lucene_core:explanation.impl;

import :explanation;

import :number;
import :types;

import std;

namespace lucene {

Explanation::Explanation(bool matched, Number value, std::string description, std::vector<Explanation> details)
    : matched_(matched), value_(value), description_(std::move(description)), details_(std::move(details)) {}

Explanation Explanation::match_no_details(Number value, std::string description) {
    return Explanation(true, value, std::move(description), {});
}

Explanation Explanation::match_(Number value, std::string description, std::vector<Explanation> details) {
    return Explanation(true, value, std::move(description), std::move(details));
}

Explanation Explanation::no_match_no_details(std::string description) {
    return Explanation(false, Number(0.0F), std::move(description), {});
}

Explanation Explanation::no_match(std::string description, std::vector<Explanation> details) {
    return Explanation(false, Number(0.0F), std::move(description), std::move(details));
}

Explanation Explanation::error_explanation(std::string description) {
    return Explanation(true, Number(int32_t{0}), std::move(description), {});
}

void Explanation::append_to(std::string &buffer, size_t depth) const {
    for (size_t i = 0; i < depth; ++i) {
        buffer += "  ";
    }
    buffer += number_to_string(value_);
    buffer += " = ";
    buffer += description_;
    buffer += '\n';
    for (const Explanation &detail : details_) {
        detail.append_to(buffer, depth + 1);
    }
}

std::string Explanation::to_string() const {
    std::string buffer;
    append_to(buffer, 0);
    return buffer;
}

} // namespace lucene
