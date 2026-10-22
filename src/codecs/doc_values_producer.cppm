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

export module lucene_core:doc_values_producer;

import :types;
import std;

import :binary_doc_values;
import :close;
import :doc_values_skipper;
import :error;
import :field_infos;
import :numeric_doc_values;
import :sorted_doc_values;
import :sorted_numeric_doc_values;
import :sorted_set_doc_values;

namespace lucene {

/// Produces the doc values of a segment.
///
/// Port of the `DocValuesProducer` trait (`core/codecs/doc_values_producer.rs`).
/// Each `get_*` call returns a fresh cursor that is only used by one thread, so
/// the producers themselves may be shared.
///
/// **Deviations** (C++ mechanics only):
///  * rlucene's associated types collapse into `std::unique_ptr<...>` over the
///    format-neutral interfaces;
///  * `get_skipper` returns a null pointer instead of `None`;
///  * the default implementations of the getters report `unsupported_operation`,
///    like rlucene.
export class DocValuesProducer : public CloseableRef {
public:
    ~DocValuesProducer() override = default;

    /// The numeric values of `field`.
    [[nodiscard]] virtual Result<std::unique_ptr<NumericDocValues>> get_numeric(const FieldInfo &field) const {
        static_cast<void>(field);
        return err(LuceneError::unsupported_operation("this producer has no numeric doc values"));
    }

    /// The binary values of `field`.
    [[nodiscard]] virtual Result<std::unique_ptr<BinaryDocValues>> get_binary(const FieldInfo &field) const {
        static_cast<void>(field);
        return err(LuceneError::unsupported_operation("this producer has no binary doc values"));
    }

    /// The sorted values of `field`.
    [[nodiscard]] virtual Result<std::unique_ptr<SortedDocValues>> get_sorted(const FieldInfo &field) const {
        static_cast<void>(field);
        return err(LuceneError::unsupported_operation("this producer has no sorted doc values"));
    }

    /// The sorted numeric values of `field`.
    [[nodiscard]] virtual Result<std::unique_ptr<SortedNumericDocValues>> get_sorted_numeric(
            const FieldInfo &field) const {
        static_cast<void>(field);
        return err(LuceneError::unsupported_operation("this producer has no sorted numeric doc values"));
    }

    /// The sorted set values of `field`.
    [[nodiscard]] virtual Result<std::unique_ptr<SortedSetDocValues>> get_sorted_set(const FieldInfo &field) const {
        static_cast<void>(field);
        return err(LuceneError::unsupported_operation("this producer has no sorted set doc values"));
    }

    /// The skipper of `field`, or a null pointer when the field has no skip index.
    [[nodiscard]] virtual Result<std::unique_ptr<DocValuesSkipper>> get_skipper(const FieldInfo &field) const {
        static_cast<void>(field);
        return std::unique_ptr<DocValuesSkipper>(nullptr);
    }

    /// Validates the checksums of this producer's files.
    [[nodiscard]] virtual VoidResult check_integrity() const = 0;

    /// An instance optimized for merging; `nullptr` means "use this instance".
    [[nodiscard]] virtual Result<std::unique_ptr<DocValuesProducer>> get_merge_instance() const {
        return std::unique_ptr<DocValuesProducer>(nullptr);
    }
};

} // namespace lucene
