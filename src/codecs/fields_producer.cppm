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

export module lucene_core:fields_producer;

import :types;
import std;

import :close;
import :error;
import :fields;

namespace lucene {

/// Abstract API that produces terms, doc, freq, prox, offset and payloads
/// postings, on top of the [`Fields`] iteration surface.
///
/// Port of the `FieldsProducer` trait (`core/codecs/fields_producer.rs`).
///
/// **Deviations** (C++ mechanics only):
///  * `get_merge_instance` returns a null pointer instead of `Option<Self>`; the
///    default answers "no merge instance", which is what rlucene's `None` means;
///  * `Fields` already collapses the associated types, so nothing else changes.
export class FieldsProducer : public Fields, public CloseableRef {
public:
    ~FieldsProducer() override = default;

    /// Checks the internal consistency of the underlying index structures.
    ///
    /// This is a costly operation, for tests and `CheckIndex` only.
    [[nodiscard]] virtual VoidResult check_integrity() const = 0;

    /// A merge instance of this producer, or a null pointer when the
    /// implementation has none.
    ///
    /// A merge instance may be faster for merging because it can disable some
    /// optimisations.  The returned instance must be closed independently of
    /// this one.
    [[nodiscard]] virtual Result<std::unique_ptr<FieldsProducer>> get_merge_instance() {
        return std::unique_ptr<FieldsProducer>{};
    }
};

} // namespace lucene
