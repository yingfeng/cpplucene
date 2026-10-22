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

export module lucene_core:lucene99_codec;

import :types;
import :error;

import std;

import :codec;
import :error;
import :field_infos;
import :field_infos_format;
import :lucene90_live_docs_format;
import :live_docs_format;
import :lucene99_segment_info_format;
import :segment_info_format;
import :types;

namespace lucene {

/// The default codec of this port: the Lucene 9.9+ / 10 generation.
///
/// Port of `codecs/lucene99/mod.rs` (`Lucene99Codec`).  It owns the segment
/// metadata format; the field infos format of the same generation is exposed as a
/// name because it is keyed by file, not by the codec's dispatch table.
export class Lucene99Codec : public Codec {
public:
    static constexpr std::string_view CODEC_NAME = "Lucene99";

    [[nodiscard]] std::string_view name() const override { return CODEC_NAME; }

    [[nodiscard]] const SegmentInfoFormat &segment_info_format() const override { return segment_info_format_; }

    [[nodiscard]] const LiveDocsFormat &live_docs_format() const override { return live_docs_format_; }

    [[nodiscard]] const FieldInfosFormat &field_infos_format() const override { return field_infos_format_; }

    /// Codec name in the header of `.fnm` files of this generation.
    [[nodiscard]] static std::string_view field_infos_codec_name() noexcept {
        return Lucene94FieldInfosFormat::CODEC_NAME;
    }

private:
    Lucene99SegmentInfoFormat segment_info_format_;
    Lucene90LiveDocsFormat live_docs_format_;
    Lucene94FieldInfosFormatAdapter field_infos_format_;
};

/// The shared instance, so callers do not have to own a codec object.
[[nodiscard]] const Codec &lucene99_codec();

/// Resolves a codec from the name recorded in `segments_N`.
///
/// **Scope:** the registry holds a single entry, because this port only writes
/// one generation.  The reference implementation additionally resolves every codec it can read.
[[nodiscard]] Result<const Codec *> codec_for_name(std::string_view name);

} // namespace lucene
