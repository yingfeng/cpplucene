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

module lucene_core:lucene99_codec.impl;

import :lucene99_codec;

import :codec;
import :types;

import std;

namespace lucene {

const Codec &lucene99_codec() {
    // Function-local static: thread safe since C++11 and constructed on first
    // use, which keeps the codec out of static initialisation order problems.
    static const Lucene99Codec codec;
    return codec;
}

Result<const Codec *> codec_for_name(std::string_view name) {
    if (name == Lucene99Codec::CODEC_NAME) {
        return &lucene99_codec();
    }
    return err(LuceneError::illegal_argument("unknown codec: " + std::string(name)));
}

} // namespace lucene
