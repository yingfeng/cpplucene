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

export module lucene_core:byte_run_automaton;

import :types;
import std;

import :automaton;
import :byte_runnable;
import :error;
import :run_automaton;

namespace lucene {

/// A [`RunAutomaton`] over bytes.
///
/// Port of `ByteRunAutomaton`
/// (`core/util/automation/byte_run_automaton.rs`).
///
/// A character based (UTF-32) automaton is first converted to its UTF-8
/// equivalent and determinized, so that it can be run one byte at a time.
export class ByteRunAutomaton final : public ByteRunnable {
public:
    /// Converts `a` from UTF-32 to UTF-8 and runs it over bytes.
    [[nodiscard]] static Result<ByteRunAutomaton> create(Automaton a);
    /// Expert use: when `is_binary` is true the automaton already accepts bytes
    /// and is used as is.
    [[nodiscard]] static Result<ByteRunAutomaton> create_with_binary(Automaton a, bool is_binary);

    [[nodiscard]] Result<int32_t> step(int32_t state, int32_t c) override;
    [[nodiscard]] Result<bool> is_accept(int32_t state) const override;
    [[nodiscard]] size_t get_size() const override { return base_.size(); }

    [[nodiscard]] const RunAutomaton &base() const noexcept { return base_; }

private:
    explicit ByteRunAutomaton(RunAutomaton base) : base_(std::move(base)) {}

    RunAutomaton base_;
};

} // namespace lucene
