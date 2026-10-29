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

export module lucene_core:html_strip_char_filter;

import :char_filter;
import :types;

import std;

namespace lucene {

/// Removes HTML markup and decodes character entities, so that a web page can be
/// indexed as text.
///
/// Port of `HTMLStripCharFilter`
/// (`analysis/common/charfilter/html_strip_char_filter.rs`).  Tags are dropped,
/// the content of `script` and `style` elements is dropped with them, and the
/// entities of the text (`&amp;`, `&#233;`, `&#xE9;`) are decoded.
///
/// **Deviation:** the reference implementation drives a full HTML state machine
/// and knows every named entity of HTML 4; this port scans for `<` and `&`
/// (which covers well-formed documents) and decodes numeric entities plus a table
/// of the common named ones.  Unknown named entities are kept verbatim.
export class HTMLStripCharFilter final : public CharFilter {
public:
    HTMLStripCharFilter() = default;

    [[nodiscard]] Result<std::string> filter(std::string_view input) const override;

    [[nodiscard]] std::string to_string() const override { return "HTMLStripCharFilter"; }
};

} // namespace lucene
