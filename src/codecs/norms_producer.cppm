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

export module lucene_core:norms_producer;

import :types;
import std;

import :close;
import :error;
import :field_infos;
import :numeric_doc_values;

namespace lucene {

/// Produces field normalization values.
///
/// Port of the `NormsProducer` trait (`core/codecs/norms_producer.rs`).
///
/// **Deviations** (C++ mechanics only):
///  * The `NumericDocValues` / `MergeInstance` associated types become
///    `std::unique_ptr<NumericDocValues>` / `std::unique_ptr<NormsProducer>`.
///  * `get_merge_instance() -> Option<MergeInstance>` keeps the reference implementation's "no new
///    instance required" meaning: a null pointer says the current instance
///    should be used for merging.
export class NormsProducer : public CloseableRef {
public:
    ~NormsProducer() override = default;

    /// Norms for `field`.  The returned instance is not required to be
    /// thread-safe: it is only used by a single thread.
    ///
    /// Behaviour is undefined when the field does not have norms enabled.
    [[nodiscard]] virtual Result<std::unique_ptr<NumericDocValues>> get_norms(const FieldInfo &field) const = 0;

    /// Checks consistency of this producer.
    ///
    /// NOTE: this may be expensive in terms of I/O, for example it may compute a
    /// checksum over large data files.
    [[nodiscard]] virtual VoidResult check_integrity() const = 0;

    /// An instance optimized for merging; it may only be used from the thread
    /// that acquires it.
    [[nodiscard]] virtual Result<std::unique_ptr<NormsProducer>> get_merge_instance() const {
        return std::unique_ptr<NormsProducer>(nullptr);
    }
};

/// The `either_normsproducer!` variants of the reference implementation (`NormsProducerEnum2`, ...),
/// expressed as one variadic type.
export template <class... Ts>
class NormsProducerEnum : public NormsProducer {
public:
    explicit NormsProducerEnum(std::variant<Ts...> inner) : inner_(std::move(inner)) {}

    [[nodiscard]] Result<std::unique_ptr<NumericDocValues>> get_norms(const FieldInfo &field) const override {
        return std::visit([&field](const auto &producer) { return producer.get_norms(field); }, inner_);
    }

    [[nodiscard]] VoidResult check_integrity() const override {
        return std::visit([](const auto &producer) { return producer.check_integrity(); }, inner_);
    }

    /// **Deviation:** the reference implementation re-wraps the inner merge instance into
    /// `Enum<A::MergeInstance, B::MergeInstance>`.  A type-erased
    /// `std::unique_ptr<NormsProducer>` cannot be put back into a concrete
    /// variant alternative, so this reports "use the current instance", which
    /// is the reference implementation's own default.
    [[nodiscard]] Result<std::unique_ptr<NormsProducer>> get_merge_instance() const override {
        return std::unique_ptr<NormsProducer>(nullptr);
    }

private:
    std::variant<Ts...> inner_;
};

} // namespace lucene
