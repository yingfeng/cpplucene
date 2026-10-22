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

module;

#include <gtest/gtest.h>

module lucene_core:ut.competitive_impact_accumulator;

import lucene_core;

using namespace lucene;

TEST(ImpactTest, OrdersByFreqThenDecreasingUnsignedNorm) {
    const Impact low_freq{1, 10};
    const Impact high_freq{2, 1};

    EXPECT_LT(low_freq, high_freq);
    // Same frequency: the larger unsigned norm sorts first.
    EXPECT_LT((Impact{1, 20}), (Impact{1, 10}));
    EXPECT_EQ((to_string(Impact{3, 7})), "{freq=3,norm=7}");
}

TEST(CompetitiveImpactAccumulatorTest, KeepsOnlyCompetitivePairs) {
    CompetitiveImpactAccumulator acc;
    acc.add(1, 10);
    acc.add(2, 10);
    acc.add(3, 10);
    // A lower frequency with the same norm is dropped, so only (3, 10) is left.
    EXPECT_EQ(acc.get_competitive_freq_norm_pairs(), std::vector<Impact>({Impact{3, 10}}));

    acc.add(2, 5);
    // Both survive: (2, 5) is more competitive than (3, 10) thanks to its norm.
    EXPECT_EQ(acc.get_competitive_freq_norm_pairs(), std::vector<Impact>({Impact{2, 5}, Impact{3, 10}}));

    acc.add(1, 5);
    // (1, 5) is dominated by (2, 5) — same norm, lower frequency.
    EXPECT_EQ(acc.get_competitive_freq_norm_pairs(), std::vector<Impact>({Impact{2, 5}, Impact{3, 10}}));
}

TEST(CompetitiveImpactAccumulatorTest, ByteNormsKeepTheLowestNormOfEachFrequency) {
    CompetitiveImpactAccumulator acc;
    acc.add(1, 1);
    acc.add(1, 2);
    acc.add(1, 3);
    // A single frequency is reported at the *lowest* norm that reaches it,
    // because the pairs are emitted in increasing (unsigned) norm order.
    EXPECT_EQ(acc.get_competitive_freq_norm_pairs(), std::vector<Impact>({Impact{1, 1}}));

    // A higher frequency at a higher norm is emitted afterwards.
    acc.add(2, 7);
    EXPECT_EQ(acc.get_competitive_freq_norm_pairs(), std::vector<Impact>({Impact{1, 1}, Impact{2, 7}}));
}

TEST(CompetitiveImpactAccumulatorTest, HandlesWideNormsOutsideByteRange) {
    CompetitiveImpactAccumulator acc;
    acc.add(1, 1000);   // wide norm
    acc.add(2, 2000);   // wide norm, higher freq and higher norm
    acc.add(1, 42);     // byte norm

    const std::vector<Impact> impacts = acc.get_competitive_freq_norm_pairs();
    // (1, 2000) would be dominated by (2, 2000); the byte range entry (1, 42)
    // only remains if a higher norm shows up at a lower frequency.
    EXPECT_EQ(impacts.size(), 2u);
    EXPECT_EQ(impacts[0], (Impact{1, 42}));
    EXPECT_EQ(impacts[1], (Impact{2, 2000}));
    EXPECT_TRUE(acc.assert_consistent());
}

TEST(CompetitiveImpactAccumulatorTest, AddAllMergesAndCopyFromReplaces) {
    CompetitiveImpactAccumulator first;
    first.add(1, 1);
    CompetitiveImpactAccumulator second;
    second.add(3, 5);

    CompetitiveImpactAccumulator merged;
    merged.add_all(first);
    merged.add_all(second);
    EXPECT_EQ(merged.get_competitive_freq_norm_pairs(),
              std::vector<Impact>({Impact{1, 1}, Impact{3, 5}}));

    CompetitiveImpactAccumulator copied;
    copied.add(9, 9);
    copied.copy_from(first);
    EXPECT_EQ(copied.get_competitive_freq_norm_pairs(), std::vector<Impact>({Impact{1, 1}}));

    copied.clear();
    EXPECT_TRUE(copied.get_competitive_freq_norm_pairs().empty());
}

TEST(CompetitiveImpactAccumulatorTest, ToStringMatchesRluceneFormat) {
    CompetitiveImpactAccumulator acc;
    acc.add(1, 2);
    acc.add(3, 4);
    EXPECT_EQ(acc.to_string(), "[{freq=1,norm=2}, {freq=3,norm=4}]");
}
