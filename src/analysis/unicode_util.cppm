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

/// Character classification shared by the tokenizers.
///
/// The reference implementation ports Lucene's `CharacterUtils`/`UnicodeProps` generated tables; the
/// full UCD tables are not embedded yet, so these helpers combine exact tables
/// for whitespace with a script-range approximation for letters and digits.
/// Everything the codecs touch (offsets, term bytes) stays exact; only the
/// letter/digit boundaries outside the covered scripts are approximate.
export module lucene_core:unicode_util;

import :types;

import std;

namespace lucene {

/// `Character.isWhitespace`: Unicode `Zs`/`Zl`/`Zp` plus tab, LF, VT, FF, CR,
/// FS, GS, RS, US.  NBSP (U+00A0) is **not** whitespace, matching Java.
[[nodiscard]] bool char_is_whitespace(char32_t codepoint) noexcept;

/// `Character.isLetter` for the scripts covered by the approximation.
[[nodiscard]] bool char_is_letter(char32_t codepoint) noexcept;

/// `Character.isDigit` (decimal digit, category `Nd`).
[[nodiscard]] bool char_is_digit(char32_t codepoint) noexcept;

/// `Character.isLetterOrDigit`.
[[nodiscard]] bool char_is_letter_or_digit(char32_t codepoint) noexcept;

/// `Character.toLowerCase` for the scripts covered by the approximation:
/// ASCII, Latin-1 supplement, Latin Extended-A, Greek and Cyrillic.  Other code
/// points are returned unchanged, which is what the approximation implies.
[[nodiscard]] char32_t char_to_lower(char32_t codepoint) noexcept;

} // namespace lucene
