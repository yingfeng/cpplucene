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

// The packed tables below were generated with the moman/finenight package
// (MIT licensed, see NOTICE.txt) and are copied over from the auto-generated
// upstream sources; do not edit them by hand.

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
/// The upstream implementation has one generated type per degree; the C++ port
/// keeps the tables and describes them with [`ParametricCase`], so the
/// transition algorithm lives in one place instead of four.
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

// ---- LEV1 ----
constexpr std::array<int64_t, 1> LEV1_TO_STATES_0{static_cast<int64_t>(0x2ULL)};
constexpr std::array<int64_t, 1> LEV1_TO_STATES_1{static_cast<int64_t>(0xa43ULL)};
constexpr std::array<int64_t, 1> LEV1_TO_STATES_2{static_cast<int64_t>(0x4da292442420003ULL)};
constexpr std::array<int64_t, 2> LEV1_TO_STATES_3{static_cast<int64_t>(0x14d0812112018003ULL), static_cast<int64_t>(0xb1a29b46d48a49ULL)};
constexpr std::array<int64_t, 1> LEV1_OFFSET_INCRS_0{static_cast<int64_t>(0x0ULL)};
constexpr std::array<int64_t, 1> LEV1_OFFSET_INCRS_1{static_cast<int64_t>(0x38ULL)};
constexpr std::array<int64_t, 1> LEV1_OFFSET_INCRS_2{static_cast<int64_t>(0x5555528000ULL)};
constexpr std::array<int64_t, 2> LEV1_OFFSET_INCRS_3{static_cast<int64_t>(0x555555e80a0f0000ULL), static_cast<int64_t>(0x5555ULL)};

std::vector<ParametricCase> lev1_cases() {
    return {
    ParametricCase{2, 2, 1, 2, LEV1_TO_STATES_0, LEV1_OFFSET_INCRS_0},
    ParametricCase{3, 3, 1, 2, LEV1_TO_STATES_1, LEV1_OFFSET_INCRS_1},
    ParametricCase{5, 5, 2, 3, LEV1_TO_STATES_2, LEV1_OFFSET_INCRS_2},
    ParametricCase{5, 5, 2, 3, LEV1_TO_STATES_3, LEV1_OFFSET_INCRS_3},
    };
}

// ---- LEV1T ----
constexpr std::array<int64_t, 1> LEV1T_TO_STATES_0{static_cast<int64_t>(0x2ULL)};
constexpr std::array<int64_t, 1> LEV1T_TO_STATES_1{static_cast<int64_t>(0xa43ULL)};
constexpr std::array<int64_t, 2> LEV1T_TO_STATES_2{static_cast<int64_t>(0xb45a491412180003ULL), static_cast<int64_t>(0x69ULL)};
constexpr std::array<int64_t, 3> LEV1T_TO_STATES_3{static_cast<int64_t>(0xa1904864900c0003ULL), static_cast<int64_t>(0x5a6d196a45a49169ULL), static_cast<int64_t>(0x9634ULL)};
constexpr std::array<int64_t, 1> LEV1T_OFFSET_INCRS_0{static_cast<int64_t>(0x0ULL)};
constexpr std::array<int64_t, 1> LEV1T_OFFSET_INCRS_1{static_cast<int64_t>(0x38ULL)};
constexpr std::array<int64_t, 1> LEV1T_OFFSET_INCRS_2{static_cast<int64_t>(0x5555558a0000ULL)};
constexpr std::array<int64_t, 2> LEV1T_OFFSET_INCRS_3{static_cast<int64_t>(0x5555ba08a0fc0000ULL), static_cast<int64_t>(0x55555555ULL)};

std::vector<ParametricCase> lev1t_cases() {
    return {
    ParametricCase{2, 2, 1, 2, LEV1T_TO_STATES_0, LEV1T_OFFSET_INCRS_0},
    ParametricCase{3, 3, 1, 2, LEV1T_TO_STATES_1, LEV1T_OFFSET_INCRS_1},
    ParametricCase{6, 6, 2, 3, LEV1T_TO_STATES_2, LEV1T_OFFSET_INCRS_2},
    ParametricCase{6, 6, 2, 3, LEV1T_TO_STATES_3, LEV1T_OFFSET_INCRS_3},
    };
}

// ---- LEV2 ----
constexpr std::array<int64_t, 1> LEV2_TO_STATES_0{static_cast<int64_t>(0xeULL)};
constexpr std::array<int64_t, 1> LEV2_TO_STATES_1{static_cast<int64_t>(0x1a688a2cULL)};
constexpr std::array<int64_t, 3> LEV2_TO_STATES_2{static_cast<int64_t>(0x3a07603570707054ULL), static_cast<int64_t>(0x522323232103773aULL), static_cast<int64_t>(0x352254543213ULL)};
constexpr std::array<int64_t, 14> LEV2_TO_STATES_3{static_cast<int64_t>(0x7000a560180380a4ULL), static_cast<int64_t>(0xc015a0180a0194aULL), static_cast<int64_t>(0x8032c58318a301c0ULL), static_cast<int64_t>(0x9d8350d403980318ULL), static_cast<int64_t>(0x3006028ca73a8602ULL), static_cast<int64_t>(0xc51462640b21a807ULL), static_cast<int64_t>(0x2310c4100c62194eULL), static_cast<int64_t>(0xce35884218ce248dULL), static_cast<int64_t>(0xa9285a0691882358ULL), static_cast<int64_t>(0x1046b5a86b1252b5ULL), static_cast<int64_t>(0x2110a33892521483ULL), static_cast<int64_t>(0xe62906208d63394eULL), static_cast<int64_t>(0xd6a29c4921d6a4a0ULL), static_cast<int64_t>(0x1aULL)};
constexpr std::array<int64_t, 38> LEV2_TO_STATES_4{static_cast<int64_t>(0x7000a560180380a4ULL), static_cast<int64_t>(0xa000000280e0294aULL), static_cast<int64_t>(0x6c0b00e029000000ULL), static_cast<int64_t>(0x8c4350c59cdc6039ULL), static_cast<int64_t>(0x600ad00c03380601ULL), static_cast<int64_t>(0x2962c18c5180e00ULL), static_cast<int64_t>(0x18c4000c6028c4ULL), static_cast<int64_t>(0x8a314603801802b4ULL), static_cast<int64_t>(0x6328c4520c59c5ULL), static_cast<int64_t>(0x60d43500e600c651ULL), static_cast<int64_t>(0x280e339cea180a7ULL), static_cast<int64_t>(0x4039800000a318c6ULL), static_cast<int64_t>(0xd57be96039ec3d0dULL), static_cast<int64_t>(0xc0338d6358c4352ULL), static_cast<int64_t>(0x28c4c81643500e60ULL), static_cast<int64_t>(0x3194a028c4339d8aULL), static_cast<int64_t>(0x590d403980018c4ULL), static_cast<int64_t>(0xc4522d57b68e3132ULL), static_cast<int64_t>(0xc4100c6510d6538ULL), static_cast<int64_t>(0x9884218ce248d231ULL), static_cast<int64_t>(0x318ce318c6398d83ULL), static_cast<int64_t>(0xa3609c370c431046ULL), static_cast<int64_t>(0xea3ad6958568f7beULL), static_cast<int64_t>(0x2d0348c411d47560ULL), static_cast<int64_t>(0x9ad43989295ad494ULL), static_cast<int64_t>(0x3104635ad431ad63ULL), static_cast<int64_t>(0x8f73a6b5250b40d2ULL), static_cast<int64_t>(0x57350eab9d693956ULL), static_cast<int64_t>(0x8ce24948520c411dULL), static_cast<int64_t>(0x294a398d85608442ULL), static_cast<int64_t>(0x5694831046318ce5ULL), static_cast<int64_t>(0x958460f7b623609cULL), static_cast<int64_t>(0xc411d475616258d6ULL), static_cast<int64_t>(0x9243ad4941cc520ULL), static_cast<int64_t>(0x5ad4529ce39ad456ULL), static_cast<int64_t>(0xb525073148310463ULL), static_cast<int64_t>(0x27656939460f7358ULL), static_cast<int64_t>(0x1d573516ULL)};
constexpr std::array<int64_t, 75> LEV2_TO_STATES_5{static_cast<int64_t>(0x7000a560180380a4ULL), static_cast<int64_t>(0xa000000280e0294aULL), static_cast<int64_t>(0x580600e029000000ULL), static_cast<int64_t>(0x80e0600e529c0029ULL), static_cast<int64_t>(0x380a418c6388c631ULL), static_cast<int64_t>(0x316737180e5b02c0ULL), static_cast<int64_t>(0x300ce01806310d4ULL), static_cast<int64_t>(0xc60396c0b00e0290ULL), static_cast<int64_t>(0xca328c4350c59cdULL), static_cast<int64_t>(0x80e00600ad194656ULL), static_cast<int64_t>(0x28c402962c18c51ULL), static_cast<int64_t>(0x802b40018c4000c6ULL), static_cast<int64_t>(0xe58b06314603801ULL), static_cast<int64_t>(0x8d6b48c6b580e348ULL), static_cast<int64_t>(0x28c5180e00600ad1ULL), static_cast<int64_t>(0x18ca31148316716ULL), static_cast<int64_t>(0x3801802b4031944ULL), static_cast<int64_t>(0xc4520c59c58a3146ULL), static_cast<int64_t>(0xe61956748cab38ULL), static_cast<int64_t>(0x39cea180a760d435ULL), static_cast<int64_t>(0xa318c60280e3ULL), static_cast<int64_t>(0x6029d8350d403980ULL), static_cast<int64_t>(0x6b5a80e060d873a8ULL), static_cast<int64_t>(0xf43500e618c638dULL), static_cast<int64_t>(0x10d4b55efa580e7bULL), static_cast<int64_t>(0x3980300ce358d63ULL), static_cast<int64_t>(0x57be96039ec3d0d4ULL), static_cast<int64_t>(0x4656567598c4352dULL), static_cast<int64_t>(0x8c4c81643500e619ULL), static_cast<int64_t>(0x194a028c4339d8a2ULL), static_cast<int64_t>(0x590d403980018c43ULL), static_cast<int64_t>(0xe348d87628a31320ULL), static_cast<int64_t>(0xe618d6b4d6b1880ULL), static_cast<int64_t>(0x5eda38c4c8164350ULL), static_cast<int64_t>(0x19443594e31148b5ULL), static_cast<int64_t>(0x31320590d4039803ULL), static_cast<int64_t>(0x7160c4522d57b68eULL), static_cast<int64_t>(0xd2310c41195674d6ULL), static_cast<int64_t>(0x8d839884218ce248ULL), static_cast<int64_t>(0x1046318ce318c639ULL), static_cast<int64_t>(0x2108633892348c43ULL), static_cast<int64_t>(0xdebfbdef0f63b0f6ULL), static_cast<int64_t>(0xd8270dc310c41f7bULL), static_cast<int64_t>(0x8eb5a5615a3defa8ULL), static_cast<int64_t>(0x70c43104751d583aULL), static_cast<int64_t>(0x58568f7bea3609c3ULL), static_cast<int64_t>(0x41f77ddb7bbeed69ULL), static_cast<int64_t>(0x9295ad4942d0348cULL), static_cast<int64_t>(0xad431ad639ad4398ULL), static_cast<int64_t>(0x5250b40d23104635ULL), static_cast<int64_t>(0xce0f6bd0f624a56bULL), static_cast<int64_t>(0x348c41f7b9cd7bdULL), static_cast<int64_t>(0xe55a3dce9ad4942dULL), static_cast<int64_t>(0x4755cd43aae75a4ULL), static_cast<int64_t>(0x73a6b5250b40d231ULL), static_cast<int64_t>(0xbd7bbcdd6939568fULL), static_cast<int64_t>(0xe24948520c41f779ULL), static_cast<int64_t>(0x4a398d856084428cULL), static_cast<int64_t>(0x14831046318ce529ULL), static_cast<int64_t>(0xb16c2110a3389252ULL), static_cast<int64_t>(0x1f7bdebe739c8f63ULL), static_cast<int64_t>(0xed88d82715a520c4ULL), static_cast<int64_t>(0x58589635a561183dULL), static_cast<int64_t>(0x9c569483104751dULL), static_cast<int64_t>(0xc56958460f7b6236ULL), static_cast<int64_t>(0x520c41f77ddb6719ULL), static_cast<int64_t>(0x45609243ad4941ccULL), static_cast<int64_t>(0x4635ad4529ce39adULL), static_cast<int64_t>(0x90eb525073148310ULL), static_cast<int64_t>(0xd6737b8f6bd16c24ULL), static_cast<int64_t>(0x941cc520c41f7b9cULL), static_cast<int64_t>(0x95a4e5183dcd62d4ULL), static_cast<int64_t>(0x483104755cd4589dULL), static_cast<int64_t>(0x460f7358b5250731ULL), static_cast<int64_t>(0xf779bd6717b56939ULL)};
constexpr std::array<int64_t, 1> LEV2_OFFSET_INCRS_0{static_cast<int64_t>(0x0ULL)};
constexpr std::array<int64_t, 1> LEV2_OFFSET_INCRS_1{static_cast<int64_t>(0x3e0ULL)};
constexpr std::array<int64_t, 2> LEV2_OFFSET_INCRS_2{static_cast<int64_t>(0x5555520880080000ULL), static_cast<int64_t>(0x555555ULL)};
constexpr std::array<int64_t, 6> LEV2_OFFSET_INCRS_3{static_cast<int64_t>(0xf0c000c8c0080000ULL), static_cast<int64_t>(0xca808822003f303ULL), static_cast<int64_t>(0x5555553fa02f0880ULL), static_cast<int64_t>(0x5555555555555555ULL), static_cast<int64_t>(0x5555555555555555ULL), static_cast<int64_t>(0x5555ULL)};
constexpr std::array<int64_t, 23> LEV2_OFFSET_INCRS_4{static_cast<int64_t>(0x610600010000000ULL), static_cast<int64_t>(0x2040000000001000ULL), static_cast<int64_t>(0x1044209245200ULL), static_cast<int64_t>(0x80d86d86006d80c0ULL), static_cast<int64_t>(0x2001b6030000006dULL), static_cast<int64_t>(0x8200011b6237237ULL), static_cast<int64_t>(0x12490612400410ULL), static_cast<int64_t>(0x2449001040208000ULL), static_cast<int64_t>(0x4d80820001044925ULL), static_cast<int64_t>(0x6da4906da400ULL), static_cast<int64_t>(0x9252369001360208ULL), static_cast<int64_t>(0x24924924924911b6ULL), static_cast<int64_t>(0x9249249249249249ULL), static_cast<int64_t>(0x4924924924924924ULL), static_cast<int64_t>(0x2492492492492492ULL), static_cast<int64_t>(0x9249249249249249ULL), static_cast<int64_t>(0x4924924924924924ULL), static_cast<int64_t>(0x2492492492492492ULL), static_cast<int64_t>(0x9249249249249249ULL), static_cast<int64_t>(0x4924924924924924ULL), static_cast<int64_t>(0x2492492492492492ULL), static_cast<int64_t>(0x9249249249249249ULL), static_cast<int64_t>(0x24924924ULL)};
constexpr std::array<int64_t, 45> LEV2_OFFSET_INCRS_5{static_cast<int64_t>(0x610600010000000ULL), static_cast<int64_t>(0x40000000001000ULL), static_cast<int64_t>(0xb6d56da184180ULL), static_cast<int64_t>(0x824914800810000ULL), static_cast<int64_t>(0x2002040000000411ULL), static_cast<int64_t>(0xc0000b2c5659245ULL), static_cast<int64_t>(0x6d80d86d86006d8ULL), static_cast<int64_t>(0x1b61801b60300000ULL), static_cast<int64_t>(0x6d80c0000b5b76b6ULL), static_cast<int64_t>(0x46d88dc8dc800ULL), static_cast<int64_t>(0x6372372001b60300ULL), static_cast<int64_t>(0x400410082000b1b7ULL), static_cast<int64_t>(0x2080000012490612ULL), static_cast<int64_t>(0x6d49241849001040ULL), static_cast<int64_t>(0x912400410082000bULL), static_cast<int64_t>(0x402080004112494ULL), static_cast<int64_t>(0xb2c49252449001ULL), static_cast<int64_t>(0x4906da4004d80820ULL), static_cast<int64_t>(0x136020800006daULL), static_cast<int64_t>(0x82000b5b69241b69ULL), static_cast<int64_t>(0x6da4948da4004d80ULL), static_cast<int64_t>(0x3690013602080004ULL), static_cast<int64_t>(0x49249249b1b69252ULL), static_cast<int64_t>(0x2492492492492492ULL), static_cast<int64_t>(0x9249249249249249ULL), static_cast<int64_t>(0x4924924924924924ULL), static_cast<int64_t>(0x2492492492492492ULL), static_cast<int64_t>(0x9249249249249249ULL), static_cast<int64_t>(0x4924924924924924ULL), static_cast<int64_t>(0x2492492492492492ULL), static_cast<int64_t>(0x9249249249249249ULL), static_cast<int64_t>(0x4924924924924924ULL), static_cast<int64_t>(0x2492492492492492ULL), static_cast<int64_t>(0x9249249249249249ULL), static_cast<int64_t>(0x4924924924924924ULL), static_cast<int64_t>(0x2492492492492492ULL), static_cast<int64_t>(0x9249249249249249ULL), static_cast<int64_t>(0x4924924924924924ULL), static_cast<int64_t>(0x2492492492492492ULL), static_cast<int64_t>(0x9249249249249249ULL), static_cast<int64_t>(0x4924924924924924ULL), static_cast<int64_t>(0x2492492492492492ULL), static_cast<int64_t>(0x9249249249249249ULL), static_cast<int64_t>(0x4924924924924924ULL), static_cast<int64_t>(0x2492492492492492ULL)};

std::vector<ParametricCase> lev2_cases() {
    return {
    ParametricCase{3, 3, 1, 2, LEV2_TO_STATES_0, LEV2_OFFSET_INCRS_0},
    ParametricCase{5, 5, 1, 3, LEV2_TO_STATES_1, LEV2_OFFSET_INCRS_1},
    ParametricCase{11, 11, 2, 4, LEV2_TO_STATES_2, LEV2_OFFSET_INCRS_2},
    ParametricCase{21, 21, 2, 5, LEV2_TO_STATES_3, LEV2_OFFSET_INCRS_3},
    ParametricCase{30, 30, 3, 5, LEV2_TO_STATES_4, LEV2_OFFSET_INCRS_4},
    ParametricCase{30, 30, 3, 5, LEV2_TO_STATES_5, LEV2_OFFSET_INCRS_5},
    };
}

// ---- LEV2T ----
constexpr std::array<int64_t, 1> LEV2T_TO_STATES_0{static_cast<int64_t>(0xeULL)};
constexpr std::array<int64_t, 1> LEV2T_TO_STATES_1{static_cast<int64_t>(0x1a688a2cULL)};
constexpr std::array<int64_t, 4> LEV2T_TO_STATES_2{static_cast<int64_t>(0xdc0703570707054ULL), static_cast<int64_t>(0x2323213a03dd3a3aULL), static_cast<int64_t>(0x2254543215435223ULL), static_cast<int64_t>(0x5435ULL)};
constexpr std::array<int64_t, 18> LEV2T_TO_STATES_3{static_cast<int64_t>(0x700a5701c0380a4ULL), static_cast<int64_t>(0x180a000ca529c0ULL), static_cast<int64_t>(0xc5498e60a80af180ULL), static_cast<int64_t>(0x8c4300e85a546398ULL), static_cast<int64_t>(0xd8d43501ac18c601ULL), static_cast<int64_t>(0x51976d6a863500adULL), static_cast<int64_t>(0xc3501ac28ca0180aULL), static_cast<int64_t>(0x76dda8a5b0c5be16ULL), static_cast<int64_t>(0xc41294a018c4519ULL), static_cast<int64_t>(0x1086520ce248d231ULL), static_cast<int64_t>(0x13946358ce31ac42ULL), static_cast<int64_t>(0x6732d4942d0348c4ULL), static_cast<int64_t>(0xd635ad4b1ad224a5ULL), static_cast<int64_t>(0xce24948520c4139ULL), static_cast<int64_t>(0x58ce729d22110a52ULL), static_cast<int64_t>(0x941cc520c41394e3ULL), static_cast<int64_t>(0x4729d22490e732d4ULL), static_cast<int64_t>(0x39ce35adULL)};
constexpr std::array<int64_t, 68> LEV2T_TO_STATES_4{static_cast<int64_t>(0x1453803801c0144ULL), static_cast<int64_t>(0xc000514514700038ULL), static_cast<int64_t>(0x1400001401ULL), static_cast<int64_t>(0x140000ULL), static_cast<int64_t>(0x6301f00700510000ULL), static_cast<int64_t>(0xa186178301f00d1ULL), static_cast<int64_t>(0xc20c30c20ca0c3ULL), static_cast<int64_t>(0xc00c00cd0c30030cULL), static_cast<int64_t>(0x4c054014f0c00c30ULL), static_cast<int64_t>(0x55150c34c30944c3ULL), static_cast<int64_t>(0x430c014308300550ULL), static_cast<int64_t>(0xc30850c00050c31ULL), static_cast<int64_t>(0x50053c50c3143000ULL), static_cast<int64_t>(0x850d30c25130d301ULL), static_cast<int64_t>(0xc21441430a08608ULL), static_cast<int64_t>(0x2145003143142145ULL), static_cast<int64_t>(0x4c1431451400c314ULL), static_cast<int64_t>(0x28014d6c32832803ULL), static_cast<int64_t>(0x1c50c76cd34a0c3ULL), static_cast<int64_t>(0x430c30c31c314014ULL), static_cast<int64_t>(0xc30050000001431ULL), static_cast<int64_t>(0xd36d0e40ca00d303ULL), static_cast<int64_t>(0xcb2abb2c90b0e400ULL), static_cast<int64_t>(0x2c32ca2c70c20ca1ULL), static_cast<int64_t>(0x31c00c00cd2c70cbULL), static_cast<int64_t>(0x558328034c2c32cULL), static_cast<int64_t>(0x6cd6ca14558309b7ULL), static_cast<int64_t>(0x51c51401430850c7ULL), static_cast<int64_t>(0xc30871430c714ULL), static_cast<int64_t>(0xca00d3071451450ULL), static_cast<int64_t>(0xb9071560c26dc156ULL), static_cast<int64_t>(0xc70c21441cb2abb2ULL), static_cast<int64_t>(0x1421c70cb1c51ca1ULL), static_cast<int64_t>(0x30811c51c51c00c3ULL), static_cast<int64_t>(0xc51031c224324308ULL), static_cast<int64_t>(0x5c33830d70820820ULL), static_cast<int64_t>(0x30c30c30c33850c3ULL), static_cast<int64_t>(0x451450c30c30c31cULL), static_cast<int64_t>(0xda0920d20c20c20ULL), static_cast<int64_t>(0x365961145145914fULL), static_cast<int64_t>(0xd964365351965865ULL), static_cast<int64_t>(0x51964364365a6590ULL), static_cast<int64_t>(0x920b203243081505ULL), static_cast<int64_t>(0xd72422492c718b28ULL), static_cast<int64_t>(0x2cb3872c35cb28b0ULL), static_cast<int64_t>(0xb0c32cb2972c30d7ULL), static_cast<int64_t>(0xc80c90c204e1c75cULL), static_cast<int64_t>(0x4504171c62ca2482ULL), static_cast<int64_t>(0x33976585d65d9610ULL), static_cast<int64_t>(0x4b5ca5d70d95cb5dULL), static_cast<int64_t>(0x1030813873975c36ULL), static_cast<int64_t>(0x41451031c2245105ULL), static_cast<int64_t>(0xc35c338714e24208ULL), static_cast<int64_t>(0x1c51c51451453851ULL), static_cast<int64_t>(0x20451450c70c30c3ULL), static_cast<int64_t>(0x4f0da09214f1440cULL), static_cast<int64_t>(0x6533944d04513d41ULL), static_cast<int64_t>(0xe15450551350e658ULL), static_cast<int64_t>(0x551938364365a50ULL), static_cast<int64_t>(0x2892071851030815ULL), static_cast<int64_t>(0x714e2422441c718bULL), static_cast<int64_t>(0x4e1c73871c35cb28ULL), static_cast<int64_t>(0x5c70c32cb28e1c51ULL), static_cast<int64_t>(0x81c61440c204e1c7ULL), static_cast<int64_t>(0xd04503ce1c62ca24ULL), static_cast<int64_t>(0x39338e6585d63944ULL), static_cast<int64_t>(0x364b5ca38e154387ULL), static_cast<int64_t>(0x38739738ULL)};
constexpr std::array<int64_t, 135> LEV2T_TO_STATES_5{static_cast<int64_t>(0x1453803801c0144ULL), static_cast<int64_t>(0xc000514514700038ULL), static_cast<int64_t>(0x1400001401ULL), static_cast<int64_t>(0x140000ULL), static_cast<int64_t>(0x4e00e00700510000ULL), static_cast<int64_t>(0x3451451c000e0051ULL), static_cast<int64_t>(0x30cd00000d015000ULL), static_cast<int64_t>(0xc30c30d40c30c30cULL), static_cast<int64_t>(0x7c01c01440c30c30ULL), static_cast<int64_t>(0x185e0c07c03458c0ULL), static_cast<int64_t>(0x830c30832830c286ULL), static_cast<int64_t>(0x33430c00c30030ULL), static_cast<int64_t>(0x70051030030c3003ULL), static_cast<int64_t>(0x8301f00d16301f00ULL), static_cast<int64_t>(0xc20ca0c30a18617ULL), static_cast<int64_t>(0xb1450c51431420c3ULL), static_cast<int64_t>(0x4f14314514314315ULL), static_cast<int64_t>(0x4c30944c34c05401ULL), static_cast<int64_t>(0x30830055055150c3ULL), static_cast<int64_t>(0xc00050c31430c014ULL), static_cast<int64_t>(0xc31430000c30850ULL), static_cast<int64_t>(0x25130d30150053c5ULL), static_cast<int64_t>(0xc03541545430d30cULL), static_cast<int64_t>(0x1cb2cd0c300d0c90ULL), static_cast<int64_t>(0x72c30cb2c91cb0c3ULL), static_cast<int64_t>(0xc34c054014f1cb2cULL), static_cast<int64_t>(0x8218221434c30944ULL), static_cast<int64_t>(0x50851430851050c2ULL), static_cast<int64_t>(0x30c50851400c50cULL), static_cast<int64_t>(0x150053c50c51450ULL), static_cast<int64_t>(0x8850d30c25130d3ULL), static_cast<int64_t>(0x450c21441430a086ULL), static_cast<int64_t>(0x1c91c70c51cb1c21ULL), static_cast<int64_t>(0x34c1cb1c71c314bULL), static_cast<int64_t>(0xc328014d6c328328ULL), static_cast<int64_t>(0x1401c50c76cd34a0ULL), static_cast<int64_t>(0x31430c30c31c3140ULL), static_cast<int64_t>(0x30c300500000014ULL), static_cast<int64_t>(0x535b0ca0ca00d3ULL), static_cast<int64_t>(0x514369b34d2830caULL), static_cast<int64_t>(0x5965965a0c500d01ULL), static_cast<int64_t>(0x6435030c30d46546ULL), static_cast<int64_t>(0xdb4390328034c659ULL), static_cast<int64_t>(0xcaaecb242c390034ULL), static_cast<int64_t>(0xcb28b1c30832872ULL), static_cast<int64_t>(0x700300334b1c32cbULL), static_cast<int64_t>(0xe40ca00d30b0cb0cULL), static_cast<int64_t>(0xb2c90b0e400d36d0ULL), static_cast<int64_t>(0xa2c70c20ca1cb2abULL), static_cast<int64_t>(0x4315b5ce6575d95cULL), static_cast<int64_t>(0x28034c5d95c53831ULL), static_cast<int64_t>(0xa14558309b705583ULL), static_cast<int64_t>(0x401430850c76cd6cULL), static_cast<int64_t>(0x871430c71451c51ULL), static_cast<int64_t>(0xd3071451450000c3ULL), static_cast<int64_t>(0x560c26dc1560ca00ULL), static_cast<int64_t>(0xc914369b35b2851ULL), static_cast<int64_t>(0x465939451a14500dULL), static_cast<int64_t>(0x945075030cb2c939ULL), static_cast<int64_t>(0x9b70558328034c3ULL), static_cast<int64_t>(0x72caaecae41c5583ULL), static_cast<int64_t>(0xc71472871c308510ULL), static_cast<int64_t>(0x1470030c50871c32ULL), static_cast<int64_t>(0xc1560ca00d307147ULL), static_cast<int64_t>(0xabb2b9071560c26dULL), static_cast<int64_t>(0x38a1c70c21441cb2ULL), static_cast<int64_t>(0x314b1c938e657394ULL), static_cast<int64_t>(0x4308308139438738ULL), static_cast<int64_t>(0x820c51031c22432ULL), static_cast<int64_t>(0x50c35c33830d7082ULL), static_cast<int64_t>(0xc31c30c30c30c338ULL), static_cast<int64_t>(0xc20451450c30c30ULL), static_cast<int64_t>(0x31440c70890c90c2ULL), static_cast<int64_t>(0xea0df0c3a8208208ULL), static_cast<int64_t>(0xa28a28a28a231430ULL), static_cast<int64_t>(0x1861868a28a28a1eULL), static_cast<int64_t>(0xc368248348308308ULL), static_cast<int64_t>(0x4d96584514516453ULL), static_cast<int64_t>(0x36590d94d4659619ULL), static_cast<int64_t>(0x546590d90d969964ULL), static_cast<int64_t>(0x920d20c20c20541ULL), static_cast<int64_t>(0x961145145914f0daULL), static_cast<int64_t>(0xe89d351965865365ULL), static_cast<int64_t>(0x9e89e89e99e7a279ULL), static_cast<int64_t>(0xb203243081821827ULL), static_cast<int64_t>(0x422492c718b28920ULL), static_cast<int64_t>(0x3872c35cb28b0d72ULL), static_cast<int64_t>(0x32cb2972c30d72cbULL), static_cast<int64_t>(0xc90c204e1c75cb0cULL), static_cast<int64_t>(0x24b1c62ca2482c80ULL), static_cast<int64_t>(0xb0ea2e42c3a89089ULL), static_cast<int64_t>(0xa4966a289669a31cULL), static_cast<int64_t>(0x8175e7a59a8a269ULL), static_cast<int64_t>(0x718b28920b203243ULL), static_cast<int64_t>(0x175976584114105cULL), static_cast<int64_t>(0x5c36572d74ce5d96ULL), static_cast<int64_t>(0xe1ce5d70d92d7297ULL), static_cast<int64_t>(0xca2482c80c90c204ULL), static_cast<int64_t>(0x5d96104504171c62ULL), static_cast<int64_t>(0x79669533976585d6ULL), static_cast<int64_t>(0x659689e6964965a2ULL), static_cast<int64_t>(0x24510510308175e7ULL), static_cast<int64_t>(0xe2420841451031c2ULL), static_cast<int64_t>(0x453851c35c338714ULL), static_cast<int64_t>(0xc30c31c51c51451ULL), static_cast<int64_t>(0x41440c20451450c7ULL), static_cast<int64_t>(0x821051440c708914ULL), static_cast<int64_t>(0x1470ea0df1c58c90ULL), static_cast<int64_t>(0x8a1e85e861861863ULL), static_cast<int64_t>(0x30818618687a8a2ULL), static_cast<int64_t>(0x5053c36824853c51ULL), static_cast<int64_t>(0x96194ce51341144fULL), static_cast<int64_t>(0x943855141544d439ULL), static_cast<int64_t>(0x5415464e0d90d96ULL), static_cast<int64_t>(0xf0da09214f1440c2ULL), static_cast<int64_t>(0x533944d04513d414ULL), static_cast<int64_t>(0x86082181350e6586ULL), static_cast<int64_t>(0x18277689e89e981dULL), static_cast<int64_t>(0x8920718510308182ULL), static_cast<int64_t>(0x14e2422441c718b2ULL), static_cast<int64_t>(0xe1c73871c35cb287ULL), static_cast<int64_t>(0xc70c32cb28e1c514ULL), static_cast<int64_t>(0x1c61440c204e1c75ULL), static_cast<int64_t>(0x90891071c62ca248ULL), static_cast<int64_t>(0xa31c70ea2e41c58cULL), static_cast<int64_t>(0xa269a475e86175e7ULL), static_cast<int64_t>(0x510308175e7a57a8ULL), static_cast<int64_t>(0xf38718b28920718ULL), static_cast<int64_t>(0x39961758e5134114ULL), static_cast<int64_t>(0x728e38550e1ce4ceULL), static_cast<int64_t>(0xc204e1ce5ce0d92dULL), static_cast<int64_t>(0x1c62ca2481c61440ULL), static_cast<int64_t>(0x85d63944d04503ceULL), static_cast<int64_t>(0x5d86075e75338e65ULL), static_cast<int64_t>(0x75e7657689e69647ULL)};
constexpr std::array<int64_t, 1> LEV2T_OFFSET_INCRS_0{static_cast<int64_t>(0x0ULL)};
constexpr std::array<int64_t, 1> LEV2T_OFFSET_INCRS_1{static_cast<int64_t>(0x3e0ULL)};
constexpr std::array<int64_t, 2> LEV2T_OFFSET_INCRS_2{static_cast<int64_t>(0x5558208800080000ULL), static_cast<int64_t>(0x5555555555ULL)};
constexpr std::array<int64_t, 7> LEV2T_OFFSET_INCRS_3{static_cast<int64_t>(0xc0c83000080000ULL), static_cast<int64_t>(0x2200fcff300f3c30ULL), static_cast<int64_t>(0x3c2200a8caa00a08ULL), static_cast<int64_t>(0x55555555a8fea00aULL), static_cast<int64_t>(0x5555555555555555ULL), static_cast<int64_t>(0x5555555555555555ULL), static_cast<int64_t>(0x5555555555555555ULL)};
constexpr std::array<int64_t, 34> LEV2T_OFFSET_INCRS_4{static_cast<int64_t>(0xc0000010000000ULL), static_cast<int64_t>(0x40000060061ULL), static_cast<int64_t>(0x8001000800000000ULL), static_cast<int64_t>(0x8229048249248a4ULL), static_cast<int64_t>(0x6c360300002092ULL), static_cast<int64_t>(0x6db6036db61b6c30ULL), static_cast<int64_t>(0x361b0180000db6c0ULL), static_cast<int64_t>(0xdb11b71b91b72000ULL), static_cast<int64_t>(0x100820006db6236ULL), static_cast<int64_t>(0x2492490612480012ULL), static_cast<int64_t>(0x8041000248200049ULL), static_cast<int64_t>(0x4924a48924000900ULL), static_cast<int64_t>(0x2080012510822492ULL), static_cast<int64_t>(0x9241b69200048360ULL), static_cast<int64_t>(0x4000926806da4924ULL), static_cast<int64_t>(0x291b49000241b010ULL), static_cast<int64_t>(0x494934236d249249ULL), static_cast<int64_t>(0x2492492492492492ULL), static_cast<int64_t>(0x9249249249249249ULL), static_cast<int64_t>(0x4924924924924924ULL), static_cast<int64_t>(0x2492492492492492ULL), static_cast<int64_t>(0x9249249249249249ULL), static_cast<int64_t>(0x4924924924924924ULL), static_cast<int64_t>(0x2492492492492492ULL), static_cast<int64_t>(0x9249249249249249ULL), static_cast<int64_t>(0x4924924924924924ULL), static_cast<int64_t>(0x2492492492492492ULL), static_cast<int64_t>(0x9249249249249249ULL), static_cast<int64_t>(0x4924924924924924ULL), static_cast<int64_t>(0x2492492492492492ULL), static_cast<int64_t>(0x9249249249249249ULL), static_cast<int64_t>(0x4924924924924924ULL), static_cast<int64_t>(0x2492492492492492ULL), static_cast<int64_t>(0x249249249249ULL)};
constexpr std::array<int64_t, 68> LEV2T_OFFSET_INCRS_5{static_cast<int64_t>(0xc0000010000000ULL), static_cast<int64_t>(0x40000060061ULL), static_cast<int64_t>(0x6000000800000000ULL), static_cast<int64_t>(0xdb6ab6db6b003080ULL), static_cast<int64_t>(0x80040000002db6ULL), static_cast<int64_t>(0x1148241249245240ULL), static_cast<int64_t>(0x4002000000104904ULL), static_cast<int64_t>(0xa4b2592492292000ULL), static_cast<int64_t>(0xd80c00009649658ULL), static_cast<int64_t>(0x80db6d86db0c001bULL), static_cast<int64_t>(0xc06000036db01b6dULL), static_cast<int64_t>(0x6db6c36d86000d86ULL), static_cast<int64_t>(0x300001b6ddadb6edULL), static_cast<int64_t>(0xe37236e40006c360ULL), static_cast<int64_t>(0xdb6c46db6236ULL), static_cast<int64_t>(0xb91b72000361b018ULL), static_cast<int64_t>(0x6db7636dbb1b71ULL), static_cast<int64_t>(0x6124800120100820ULL), static_cast<int64_t>(0x2482000492492490ULL), static_cast<int64_t>(0x9240009008041000ULL), static_cast<int64_t>(0x555b6a4924924830ULL), static_cast<int64_t>(0x2000480402080012ULL), static_cast<int64_t>(0x8411249249252449ULL), static_cast<int64_t>(0x24020104000928ULL), static_cast<int64_t>(0x5892492492922490ULL), static_cast<int64_t>(0x120d808200049456ULL), static_cast<int64_t>(0x6924924906da4800ULL), static_cast<int64_t>(0x6c041000249a01bULL), static_cast<int64_t>(0x924924836d240009ULL), static_cast<int64_t>(0x6020800124d5adb4ULL), static_cast<int64_t>(0x2492523692000483ULL), static_cast<int64_t>(0x104000926846da49ULL), static_cast<int64_t>(0x49291b49000241b0ULL), static_cast<int64_t>(0x92494935636d2492ULL), static_cast<int64_t>(0x4924924924924924ULL), static_cast<int64_t>(0x2492492492492492ULL), static_cast<int64_t>(0x9249249249249249ULL), static_cast<int64_t>(0x4924924924924924ULL), static_cast<int64_t>(0x2492492492492492ULL), static_cast<int64_t>(0x9249249249249249ULL), static_cast<int64_t>(0x4924924924924924ULL), static_cast<int64_t>(0x2492492492492492ULL), static_cast<int64_t>(0x9249249249249249ULL), static_cast<int64_t>(0x4924924924924924ULL), static_cast<int64_t>(0x2492492492492492ULL), static_cast<int64_t>(0x9249249249249249ULL), static_cast<int64_t>(0x4924924924924924ULL), static_cast<int64_t>(0x2492492492492492ULL), static_cast<int64_t>(0x9249249249249249ULL), static_cast<int64_t>(0x4924924924924924ULL), static_cast<int64_t>(0x2492492492492492ULL), static_cast<int64_t>(0x9249249249249249ULL), static_cast<int64_t>(0x4924924924924924ULL), static_cast<int64_t>(0x2492492492492492ULL), static_cast<int64_t>(0x9249249249249249ULL), static_cast<int64_t>(0x4924924924924924ULL), static_cast<int64_t>(0x2492492492492492ULL), static_cast<int64_t>(0x9249249249249249ULL), static_cast<int64_t>(0x4924924924924924ULL), static_cast<int64_t>(0x2492492492492492ULL), static_cast<int64_t>(0x9249249249249249ULL), static_cast<int64_t>(0x4924924924924924ULL), static_cast<int64_t>(0x2492492492492492ULL), static_cast<int64_t>(0x9249249249249249ULL), static_cast<int64_t>(0x4924924924924924ULL), static_cast<int64_t>(0x2492492492492492ULL), static_cast<int64_t>(0x9249249249249249ULL), static_cast<int64_t>(0x24924924ULL)};

std::vector<ParametricCase> lev2t_cases() {
    return {
    ParametricCase{3, 3, 1, 2, LEV2T_TO_STATES_0, LEV2T_OFFSET_INCRS_0},
    ParametricCase{5, 5, 1, 3, LEV2T_TO_STATES_1, LEV2T_OFFSET_INCRS_1},
    ParametricCase{13, 13, 2, 4, LEV2T_TO_STATES_2, LEV2T_OFFSET_INCRS_2},
    ParametricCase{28, 28, 2, 5, LEV2T_TO_STATES_3, LEV2T_OFFSET_INCRS_3},
    ParametricCase{45, 45, 3, 6, LEV2T_TO_STATES_4, LEV2T_OFFSET_INCRS_4},
    ParametricCase{45, 45, 3, 6, LEV2T_TO_STATES_5, LEV2T_OFFSET_INCRS_5},
    };
}

} // namespace

ParametricDescription create_lev1_description(int32_t w) {
    return ParametricDescription(w, 1, std::vector<int32_t>{0, 1, 0, -1, -1},
                                 std::make_shared<LevTableDescription>(lev1_cases()));
}

ParametricDescription create_lev1t_description(int32_t w) {
    return ParametricDescription(w, 1, std::vector<int32_t>{0, 1, 0, -1, -1, -1},
                                 std::make_shared<LevTableDescription>(lev1t_cases()));
}

ParametricDescription create_lev2_description(int32_t w) {
    return ParametricDescription(w, 2, std::vector<int32_t>{0, 1, 2, 0, 1, -1, 0, -1, 0, -1, 0, -1, -1, -1, -1, -2, -1, -2, -1, -2, -1, -2, -2, -2, -2, -2, -2, -2, -2, -2},
                                 std::make_shared<LevTableDescription>(lev2_cases()));
}

ParametricDescription create_lev2t_description(int32_t w) {
    return ParametricDescription(w, 2, std::vector<int32_t>{0, 1, 2, 0, 1, -1, 0, -1, 0, -1, 0, -1, 0, -1, -1, -1, -1, -1, -2, -1, -1, -2, -1, -2, -1, -1, -1, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2, -2},
                                 std::make_shared<LevTableDescription>(lev2t_cases()));
}


} // namespace lucene
