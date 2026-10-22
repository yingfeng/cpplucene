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

export module lucene_core:bits;

import :types;
import :error;

import std;

namespace lucene {

/// Random access to a fixed-length bit set.  Port of the `Bits` trait
/// (`util/bits.rs`).
///
/// **Deviation:** rlucene's `copy_of()` returns a `FixedBitSet`; here the
/// materialising type lives in `:fixed_bit_set` and implements `Bits`, so a
/// `copy_of()` helper would only add a dependency cycle.
export class Bits {
public:
    virtual ~Bits() = default;

    /// Value of the bit at `index`, which must be below [`length`].
    [[nodiscard]] virtual Result<bool> get(size_t index) const = 0;

    /// Number of bits in this set.
    [[nodiscard]] virtual size_t length() const = 0;

    [[nodiscard]] virtual std::string to_string() const { return "Bits"; }
};

/// All bits set.  Lucene's `Bits.MatchAllBits`.
export class MatchAllBits final : public Bits {
public:
    explicit MatchAllBits(size_t len) : len_(len) {}

    [[nodiscard]] Result<bool> get(size_t index) const override {
        if (index >= len_) {
            return err(LuceneError::array_index_out_of_bounds("index " + std::to_string(index) + " >= " +
                                                             std::to_string(len_)));
        }
        return true;
    }

    [[nodiscard]] size_t length() const override { return len_; }
    [[nodiscard]] std::string to_string() const override { return "MatchAllBits(" + std::to_string(len_) + ")"; }

private:
    size_t len_;
};

/// No bit set.  Lucene's `Bits.MatchNoBits`.
export class MatchNoBits final : public Bits {
public:
    explicit MatchNoBits(size_t len) : len_(len) {}

    [[nodiscard]] Result<bool> get(size_t index) const override {
        if (index >= len_) {
            return err(LuceneError::array_index_out_of_bounds("index " + std::to_string(index) + " >= " +
                                                             std::to_string(len_)));
        }
        return false;
    }

    [[nodiscard]] size_t length() const override { return len_; }
    [[nodiscard]] std::string to_string() const override { return "MatchNoBits(" + std::to_string(len_) + ")"; }

private:
    size_t len_;
};

} // namespace lucene
