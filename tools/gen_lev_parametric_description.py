#!/usr/bin/env python3
"""Generates the Levenshtein parametric description tables of clucene from the
auto-generated rlucene sources (they were produced by moman/finenight)."""

import re
import sys

RLUCENE = "/home/infominer/codebase/workspace/rlucene/src/core/util/automation"
OUT = "/home/infominer/codebase/workspace/clucene/src/util/automaton/lev_parametric_description_impl.cpp"

# (file, factory name, degree, cases) where a case is
# (max_state, multiplier, offset_bits, to_bits)
FILES = [
    ("lev1_parametric_description.rs", "Lev1", 1,
     [(2, 2, 1, 2), (3, 3, 1, 2), (5, 5, 2, 3), (5, 5, 2, 3)]),
    ("lev1t_parametric_description.rs", "Lev1T", 1,
     [(2, 2, 1, 2), (3, 3, 1, 2), (6, 6, 2, 3), (6, 6, 2, 3)]),
    ("lev2_parametric_description.rs", "Lev2", 2,
     [(3, 3, 1, 2), (5, 5, 1, 3), (11, 11, 2, 4), (21, 21, 2, 5), (30, 30, 3, 5), (30, 30, 3, 5)]),
    ("lev2t_parametric_description.rs", "Lev2T", 2,
     [(3, 3, 1, 2), (5, 5, 1, 3), (13, 13, 2, 4), (28, 28, 2, 5), (45, 45, 3, 6), (45, 45, 3, 6)]),
]

HEADER = '''/*
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

// The packed tables below were generated with the moman/finenight package
// (MIT licensed, see NOTICE.txt) and are copied over from the auto-generated
// rlucene sources; do not edit them by hand.

module lucene_core:lev_parametric_description.impl;

import :lev_parametric_description;

import :debug;
import :parametric_description;
import :types;

import std;

namespace lucene {

namespace {

/// One case of a generated description: how the states of a position behave.
struct ParametricCase {
    /// States below this one take part in the transition.
    int32_t max_state{0};
    /// `loc = vector * multiplier + state`, the index into the packed tables.
    int32_t multiplier{1};
    /// Width of one offset increment.
    size_t offset_bits{1};
    /// Width of one target state.
    size_t to_bits{2};
    /// Packed target states, biased by one.
    std::span<const int64_t> to_states{};
    /// Packed offset increments.
    std::span<const int64_t> offset_incrs{};
};

/// A parametric description backed by the packed tables of one degree.
///
/// rlucene has one generated type per degree; the C++ port keeps the tables and
/// describes them with [`ParametricCase`], so the transition algorithm lives in
/// one place instead of four.
class LevTableDescription final : public ParametricDescriptionBase {
public:
    explicit LevTableDescription(std::vector<ParametricCase> cases) : cases_(std::move(cases)) {}

    [[nodiscard]] int32_t transition(int32_t abs_state, int32_t position, int32_t vector,
                                     const ParametricDescription &base) const override {
        // The empty state is never passed in.
        debug_check(abs_state != -1, "the absolute state is not the empty state");

        // Decode abs_state into state and offset.
        int32_t state = abs_state / (base.w() + 1);
        int32_t offset = abs_state % (base.w() + 1);
        debug_check(offset >= 0, "the offset is not negative");

        const size_t exact_cases = cases_.size() - 1;
        const ParametricCase *chosen = &cases_.back();
        const int32_t delta = base.w() - position;
        if (delta >= 0 && static_cast<size_t>(delta) < exact_cases) {
            chosen = &cases_[static_cast<size_t>(delta)];
        }

        if (state < chosen->max_state) {
            const int32_t loc = vector * chosen->multiplier + state;
            offset += unpack(chosen->offset_incrs, loc, chosen->offset_bits);
            state = unpack(chosen->to_states, loc, chosen->to_bits) - 1;
        }

        if (state == -1) {
            // The empty state.
            return -1;
        }
        // Translate back to an absolute state.
        return state * (base.w() + 1) + offset;
    }

private:
    std::vector<ParametricCase> cases_;
};

'''

FOOTER = '''
} // namespace lucene
'''


def parse_arrays(text):
    arrays = {}
    pattern = re.compile(r"const (TO_STATES|OFFSET_INCRS)_(\d+):\s*\[i64;\s*(\d+)\]\s*=\s*\[(.*?)\];", re.S)
    for match in pattern.finditer(text):
        name = f"{match.group(1)}_{match.group(2)}"
        declared = int(match.group(3))
        values = []
        for literal in re.finditer(r"0x[0-9a-fA-F]+|\b\d+\b", match.group(4)):
            token = literal.group(0)
            if token.startswith("0x"):
                values.append(int(token, 16))
            else:
                # Skip the index of `long(...)` wrappers, which are hex only.
                values.append(int(token, 10))
        if len(values) != declared:
            sys.exit(f"{name}: declared {declared} values, parsed {len(values)}")
        arrays[name] = values
    return arrays


def parse_min_errors(text):
    match = re.search(r"ParametricDescription::new\(\s*w,\s*(\d+),\s*&\[(.*?)\]", text, re.S)
    if not match:
        sys.exit("min_errors not found")
    degree = int(match.group(1))
    values = [int(v) for v in re.findall(r"-?\d+", match.group(2))]
    return degree, values


def emit_array(name, values):
    body = ", ".join(f"static_cast<int64_t>(0x{v:x}ULL)" for v in values)
    return f"constexpr std::array<int64_t, {len(values)}> {name}{{{body}}};\n"


def main():
    parts = [HEADER]
    factories = []

    for filename, name, expected_degree, cases in FILES:
        with open(f"{RLUCENE}/{filename}", encoding="utf-8") as handle:
            text = handle.read()

        degree, min_errors = parse_min_errors(text)
        if degree != expected_degree:
            sys.exit(f"{filename}: unexpected degree {degree}")

        arrays = parse_arrays(text)
        needed = [f"TO_STATES_{i}" for i in range(len(cases))] + [f"OFFSET_INCRS_{i}" for i in range(len(cases))]
        for key in needed:
            if key not in arrays:
                sys.exit(f"{filename}: missing array {key}")

        label = filename.replace("_parametric_description.rs", "").upper()
        parts.append(f"// ---- {label} ----\n")
        for key in needed:
            parts.append(emit_array(f"{label}_{key}", arrays[key]))
        parts.append("\n")

        case_lines = []
        for index, (max_state, multiplier, offset_bits, to_bits) in enumerate(cases):
            case_lines.append(
                f"    ParametricCase{{{max_state}, {multiplier}, {offset_bits}, {to_bits}, "
                f"{label}_TO_STATES_{index}, {label}_OFFSET_INCRS_{index}}},\n")
        parts.append(f"std::vector<ParametricCase> {name.lower()}_cases() {{\n    return {{\n")
        parts.extend(case_lines)
        parts.append("    };\n}\n\n")

        errors = ", ".join(str(v) for v in min_errors)
        factories.append(
            f"ParametricDescription create_{name.lower()}_description(int32_t w) {{\n"
            f"    return ParametricDescription(w, {degree}, std::vector<int32_t>{{{errors}}},\n"
            f"                                 std::make_shared<LevTableDescription>({name.lower()}_cases()));\n"
            f"}}\n\n")

    parts.append("} // namespace\n\n")
    parts.extend(factories)
    parts.append(FOOTER)

    with open(OUT, "w", encoding="utf-8") as handle:
        handle.write("".join(parts))
    print(f"wrote {OUT}")


if __name__ == "__main__":
    main()
