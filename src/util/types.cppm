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

/// Fixed-width type aliases for the whole port.
///
/// `import std;` only exports the `std::` names, so the global C typedefs
/// (`::int32_t`, `::uint8_t`, ...) are not available inside module purview.
/// Aliasing them into `clucene` keeps the ported code reading like the Rust
/// original (`i32`, `u8`, ...) while still having the C names at hand.
export module clucene:types;

import std;

namespace clucene {

export using i8 = std::int8_t;
export using i16 = std::int16_t;
export using i32 = std::int32_t;
export using i64 = std::int64_t;
export using u8 = std::uint8_t;
export using u16 = std::uint16_t;
export using u32 = std::uint32_t;
export using u64 = std::uint64_t;

export using f32 = float;
export using f64 = double;

export using usize = std::size_t;
export using isize = std::ptrdiff_t;

/// C-compatible spellings, so ported declarations can use `int32_t` etc.
export using std::int8_t;
export using std::int16_t;
export using std::int32_t;
export using std::int64_t;
export using std::uint8_t;
export using std::uint16_t;
export using std::uint32_t;
export using std::uint64_t;
export using std::size_t;

} // namespace clucene
