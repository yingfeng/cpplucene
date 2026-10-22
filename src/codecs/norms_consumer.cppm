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

export module lucene_core:norms_consumer;

import :types;
import std;

import :close;
import :error;
import :field_infos;
import :norms_producer;

namespace lucene {

/// Consumes normalization values.
///
/// Concrete implementations actually do *something* with the norms, such as
/// writing them into the index in a specific format.
///
/// Lifecycle:
///  1. the consumer is created by `NormsFormat::norms_consumer`;
///  2. [`add_norms_field`] is called for each field with normalization values:
///     the API is *pull* based, so the implementation may iterate over the
///     values multiple times;
///  3. after all fields are added the consumer is closed.
///
/// Port of the `NormsConsumer` trait (`core/codecs/norms_consumer.rs`).
///
/// **Deviation:** `merge` / `merge_norms_field` are not ported yet: they need
/// the merge subsystem (`MergeState`, `CodecReader`, `DocIDMerger`), which does
/// not exist in this port.  The push API above is all the writers need.
export class NormsConsumer : public Closeable {
public:
    ~NormsConsumer() override = default;

    /// Writes normalization values for `field`.
    [[nodiscard]] virtual VoidResult add_norms_field(const FieldInfo &field,
                                                     NormsProducer &norms_producer) = 0;
};

/// The `NormsConsumerEnum2` variants of rlucene, expressed as one variadic type.
export template <class... Ts>
class NormsConsumerEnum : public NormsConsumer {
public:
    explicit NormsConsumerEnum(std::variant<Ts...> inner) : inner_(std::move(inner)) {}

    [[nodiscard]] VoidResult add_norms_field(const FieldInfo &field,
                                             NormsProducer &norms_producer) override {
        return std::visit(
                [&field, &norms_producer](auto &consumer) {
                    return consumer.add_norms_field(field, norms_producer);
                },
                inner_);
    }

    [[nodiscard]] VoidResult close() override {
        return std::visit([](auto &consumer) { return consumer.close(); }, inner_);
    }

    [[nodiscard]] const std::variant<Ts...> &value() const noexcept { return inner_; }
    [[nodiscard]] std::variant<Ts...> &value_mut() noexcept { return inner_; }

private:
    std::variant<Ts...> inner_;
};

} // namespace lucene
