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

export module lucene_core:codec_formats;

import :types;
import std;

import :codec;
import :error;

namespace lucene {

/// Registry of the codecs this port knows by name.
///
/// Port of `CodecFormats` (`core/codecs/codec_formats.rs`).
///
/// **Deviations**:
///  * rlucene's file also declares every historical codec (Lucene90, Lucene94,
///    Lucene95, Lucene99 and their format parameter classes).  By the project's
///    "current format only" rule this port keeps two entries: the codec it writes
///    ([`Lucene101Codec`]) and the metadata-only [`Lucene99Codec`] that the
///    existing `segments_N` tests use;
///  * the registry is a function-local static map behind a mutex instead of a
///    `OnceLock<HashMap<..>>`, so that [`register_codec`] can add entries at
///    runtime (tests use it to check the lookup failure paths);
///  * `for_name` returns a borrowed pointer (`nullptr` when unknown) rather than
///    `Result<Arc<dyn Codec>>`.
export class CodecFormats {
public:
    /// The codec written by this port (`Lucene101`).
    [[nodiscard]] static const Codec &default_codec();

    /// The codec named `name`, or null when the name is unknown.
    [[nodiscard]] static const Codec *for_name(std::string_view name);

    /// Names of all registered codecs, sorted.
    [[nodiscard]] static std::vector<std::string_view> available_codecs();

    /// Registers `codec`, replacing an entry with the same name.
    ///
    /// The first call installs the built-in codecs; registering before that is
    /// fine and the built-ins are added afterwards unless the name clashes.
    static void register_codec(std::shared_ptr<const Codec> codec);
};

} // namespace lucene
