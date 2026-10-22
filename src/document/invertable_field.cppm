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

export module lucene_core:invertable_field;

import :types;
import std;

namespace lucene {

/// How an [`IndexableField`](:indexable_field) must be inverted.
///
/// Port of `InvertableType` (`document/invertable_field.rs`).
export enum class InvertableType : uint8_t {
    /// One term, taken from the field's binary value; the term frequency is one.
    Binary = 0,
    /// The terms come from the field's token stream.
    TokenStream = 1,
};

} // namespace lucene
