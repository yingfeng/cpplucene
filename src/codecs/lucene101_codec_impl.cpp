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

module lucene_core:lucene101_codec.impl;

import :lucene101_codec;

import :error;
import :lucene90_stored_fields_format;
import :lucene90_term_vectors_format;
import :types;

import std;

namespace lucene {

Result<std::unique_ptr<Lucene101Codec>> Lucene101Codec::create(Mode mode) {
    // The term vectors format is a fixed parameter set; building it here fails
    // loudly if the defaults ever become invalid instead of silently degrading.
    auto term_vectors = Lucene90TermVectorsFormat::create();
    if (!term_vectors.has_value()) {
        return std::unexpected(std::move(term_vectors.error()));
    }

    Lucene90StoredFieldsFormat::Mode stored_mode = Lucene90StoredFieldsFormat::Mode::BestSpeed;
    switch (mode) {
        case Mode::BestSpeed:
            stored_mode = Lucene90StoredFieldsFormat::Mode::BestSpeed;
            break;
        case Mode::BestCompression:
            stored_mode = Lucene90StoredFieldsFormat::Mode::BestCompression;
            break;
    }

    // `BestCompression` is refused when the stored fields format is actually
    // used, so that `create` stays cheap; a codec that cannot write is worse
    // than a codec that fails on the first write.
    auto codec = std::unique_ptr<Lucene101Codec>(
            new Lucene101Codec(Lucene90StoredFieldsFormat(stored_mode), std::move(term_vectors.value())));
    codec->mode_ = mode;
    return codec;
}

} // namespace lucene
