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

export module lucene_core:synonym_filter;

import :synonym_map;
import :token_stream;
import :types;

import std;

namespace lucene {

/// Replaces the words of a [`SynonymMap`] with their alternatives.
///
/// Port of `SynonymGraphFilter` (`analysis/common/synonym/synonym_graph_filter.rs`),
/// with the matching done against the map's word sequences instead of an FST.
/// The **longest** rule wins; the alternatives it produces stay at the position
/// of the first matched word, so `fast` mapped to `quick` and `speedy` indexes
/// as two alternatives of one position.
///
/// **Deviations** from the reference implementation:
///
/// 1. No graph positioning: a rule whose input is longer than its output (or the
///    other way round) advances the position by one instead of adjusting
///    `PositionLengthAttribute`, so a following phrase sees the words at
///    consecutive positions.
/// 2. `ignore_case` lower cases ASCII only, and the map's keys must already be
///    in the case the filter will compare against.
/// 3. There is no `keepOrig` per rule beyond what
///    [`SynonymMap::Builder::add`] recorded, and no `preservePositionIncrements`
///    tuning.
export class SynonymFilter : public TokenFilter {
public:
    SynonymFilter(std::unique_ptr<TokenStream> input, const SynonymMap &map, bool ignore_case = false);

    [[nodiscard]] VoidResult reset() override;
    [[nodiscard]] Result<bool> increment_token() override;

private:
    struct BufferedToken {
        std::string term;
        uint32_t position_increment{1};
    };

    struct OutputToken {
        std::string term;
        uint32_t position_increment{0};
    };

    [[nodiscard]] std::string normalize(std::string_view term) const;

    const SynonymMap *map_;
    bool ignore_case_;
    std::deque<BufferedToken> buffer_;
    std::deque<OutputToken> output_;
    bool exhausted_{false};
};

} // namespace lucene
