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

module lucene_core:segment_info.impl;

import :segment_info;

import :error;
import :types;
import :version;

import std;

namespace lucene {

Result<std::string> parse_segment_name(std::string_view file_name) {
    if (file_name.empty()) {
        return err(LuceneError::illegal_argument("a file name must not be empty"));
    }
    const size_t dot = file_name.find('.');
    const std::string_view segment = dot == std::string_view::npos ? file_name : file_name.substr(0, dot);
    if (segment.empty()) {
        // ".fnm" has no segment part, which is what Lucene reports too.
        return err(LuceneError::illegal_argument("cannot parse segment index from file name " +
                                                 std::string(file_name)));
    }
    return std::string(segment);
}

std::string segment_file_name(std::string_view segment, std::string_view suffix, std::string_view extension) {
    std::string name(segment);
    if (!suffix.empty()) {
        name += '_';
        name += suffix;
    }
    if (!extension.empty()) {
        name += '.';
        name += extension;
    }
    return name;
}

VoidResult check_file_name(std::string_view file_name, std::string_view segment) {
    auto parsed = parse_segment_name(file_name);
    if (!parsed.has_value()) {
        return err(parsed.error());
    }
    if (parsed.value() != segment) {
        return err(LuceneError::illegal_argument("invalid file name for segment " + std::string(segment) + ": " +
                                                 std::string(file_name)));
    }
    return {};
}

VoidResult SegmentInfo::validate() const {
    if (name.empty()) {
        return err(LuceneError::illegal_argument("the segment name must not be empty"));
    }
    if (name.size() > MAX_SEGMENT_NAME_LENGTH) {
        return err(LuceneError::illegal_argument("the segment name must not be longer than " +
                                                 std::to_string(MAX_SEGMENT_NAME_LENGTH) + " characters"));
    }
    if (name.find('.') != std::string::npos) {
        return err(LuceneError::illegal_argument("the segment name must not contain a dot: " + name));
    }
    if (id.size() != ID_LENGTH) {
        return err(LuceneError::illegal_argument("the segment id must be " + std::to_string(ID_LENGTH) +
                                                 " bytes long, got " + std::to_string(id.size())));
    }
    if (max_doc < 0) {
        return err(LuceneError::illegal_argument("max_doc must not be negative"));
    }
    if (sort_field_count != 0) {
        return err(LuceneError::illegal_state("index sorting is not ported yet"));
    }
    return {};
}

VoidResult SegmentInfo::add_file(std::string_view file) {
    if (auto checked = check_file_name(file, name); !checked.has_value()) {
        return checked;
    }
    files.emplace(file);
    return {};
}

VoidResult SegmentInfo::set_files(std::set<std::string> new_files) {
    for (const auto &file : new_files) {
        if (auto checked = check_file_name(file, name); !checked.has_value()) {
            return checked;
        }
    }
    files = std::move(new_files);
    return {};
}

} // namespace lucene
