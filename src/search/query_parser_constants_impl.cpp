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

module lucene_core:query_parser_constants.impl;

import :query_parser_constants;

import std;

namespace lucene::query_syntax {

const std::string &token_image(int32_t kind) {
    // The list is indexed by token kind, so it carries an entry for the unnamed
    // kind 7 as well.
    static const std::array<std::string, 33> kImages{
            "<EOF>",         "<_NUM_CHAR>",       "<_ESCAPED_CHAR>",  "<_TERM_START_CHAR>", "<_TERM_CHAR>",
            "<_WHITESPACE>", "<_QUOTED_CHAR>",    "<token of kind 7>", "<AND>",              "<OR>",
            "<NOT>",         "\"+\"",             "\"-\"",            "<BAREOPER>",         "\"(\"",
            "\")\"",         "\":\"",            "\"*\"",            "\"^\"",              "<QUOTED>",
            "<TERM>",        "<FUZZY_SLOP>",      "<PREFIXTERM>",     "<WILDTERM>",         "<REGEXPTERM>",
            "\"[\"",         "\"{\"",             "<NUMBER>",         "\"TO\"",             "\"]\"",
            "\"}\"",         "<RANGE_QUOTED>",    "<RANGE_GOOP>",
    };
    static const std::string kUnknown = "<unknown>";
    if (kind < 0 || static_cast<size_t>(kind) >= kImages.size()) {
        return kUnknown;
    }
    return kImages[static_cast<size_t>(kind)];
}

} // namespace lucene::query_syntax
