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

module lucene_core:intro_selector.impl;

import :intro_selector;
import :types;
import :error;
import :intro_sorter;
import :selector;

import std;

namespace lucene {

namespace {

/// Thread-local generator for [`IntroSelector::shuffle`]; a select never needs
/// reproducible randomness, only a distribution that defeats adversarial input.
[[nodiscard]] std::mt19937 &shuffle_engine() {
    thread_local std::mt19937 engine{std::random_device{}()};
    return engine;
}

} // namespace

Result<int32_t> IntroSelectorHooks::compare(size_t i, size_t j) {
    if (auto r = set_pivot(i); !r) {
        return std::unexpected(std::move(r.error()));
    }
    return compare_pivot(j);
}

VoidResult IntroSelector::select(size_t from, size_t to, size_t k) {
    if (auto checked = check_args(from, to, k); !checked) {
        return checked;
    }
    const size_t size = to - from;
    // 2 * floor(log2(size)), the same budget `IntroSorter` uses.
    const int32_t max_depth = size <= 1 ? 0 : static_cast<int32_t>(2 * (std::bit_width(size) - 1));
    return select_in_intro(from, to, k, max_depth);
}

VoidResult IntroSelector::select_in_intro(size_t from, size_t to, size_t k, int32_t max_depth) {
    size_t size = 0;
    while (true) {
        size = to - from;
        if (size <= 3) {
            break;
        }
        max_depth -= 1;
        if (max_depth == -1) {
            // The budget is exhausted: shuffle once, then keep going.
            if (auto r = shuffle(from, to); !r) {
                return r;
            }
        }

        const size_t last = to - 1;
        const size_t mid = (from + last) >> 1;
        size_t pivot = 0;
        if (size <= SINGLE_MEDIAN_THRESHOLD) {
            const size_t range = size >> 2;
            auto chosen = median(mid - range, mid, mid + range);
            if (!chosen) {
                return std::unexpected(std::move(chosen.error()));
            }
            pivot = chosen.value();
        } else {
            const size_t range = size >> 3;
            const size_t double_range = range << 1;
            auto first = median(from, from + range, from + double_range);
            if (!first) {
                return std::unexpected(std::move(first.error()));
            }
            auto middle = median(mid - range, mid, mid + range);
            if (!middle) {
                return std::unexpected(std::move(middle.error()));
            }
            auto last_median = median(last - double_range, last - range, last);
            if (!last_median) {
                return std::unexpected(std::move(last_median.error()));
            }
            if (k - from < range) {
                auto chosen = min_of_three(first.value(), middle.value(), last_median.value());
                if (!chosen) {
                    return std::unexpected(std::move(chosen.error()));
                }
                pivot = chosen.value();
            } else if (to - k <= range) {
                auto chosen = max_of_three(first.value(), middle.value(), last_median.value());
                if (!chosen) {
                    return std::unexpected(std::move(chosen.error()));
                }
                pivot = chosen.value();
            } else {
                auto chosen = median(first.value(), middle.value(), last_median.value());
                if (!chosen) {
                    return std::unexpected(std::move(chosen.error()));
                }
                pivot = chosen.value();
            }
        }

        // Bentley-McIlroy 3-way partitioning.
        if (auto r = hooks_.set_pivot(pivot); !r) {
            return r;
        }
        if (auto r = hooks_.swap(from, pivot); !r) {
            return r;
        }

        int64_t i = static_cast<int64_t>(from);
        int64_t j = static_cast<int64_t>(to);
        int64_t p = static_cast<int64_t>(from) + 1;
        int64_t q = static_cast<int64_t>(last);
        int32_t left_cmp = 0;
        int32_t right_cmp = 0;

        while (true) {
            do {
                auto compared = hooks_.compare_pivot(static_cast<size_t>(i + 1));
                if (!compared) {
                    return std::unexpected(std::move(compared.error()));
                }
                left_cmp = compared.value();
                i += 1;
            } while (left_cmp > 0);

            do {
                auto compared = hooks_.compare_pivot(static_cast<size_t>(j - 1));
                if (!compared) {
                    return std::unexpected(std::move(compared.error()));
                }
                right_cmp = compared.value();
                j -= 1;
            } while (right_cmp < 0);

            if (i >= j) {
                if (i == j && right_cmp == 0) {
                    if (auto r = hooks_.swap(static_cast<size_t>(i), static_cast<size_t>(p)); !r) {
                        return r;
                    }
                }
                break;
            }

            if (auto r = hooks_.swap(static_cast<size_t>(i), static_cast<size_t>(j)); !r) {
                return r;
            }
            if (right_cmp == 0) {
                if (auto r = hooks_.swap(static_cast<size_t>(i), static_cast<size_t>(p)); !r) {
                    return r;
                }
                p += 1;
            }
            if (left_cmp == 0) {
                if (auto r = hooks_.swap(static_cast<size_t>(j), static_cast<size_t>(q)); !r) {
                    return r;
                }
                q -= 1;
            }
        }

        i = j + 1;
        for (int64_t l = static_cast<int64_t>(from); l < p; ++l) {
            if (auto r = hooks_.swap(static_cast<size_t>(l), static_cast<size_t>(j)); !r) {
                return r;
            }
            j -= 1;
        }
        for (int64_t l = static_cast<int64_t>(last); l > q; --l) {
            if (auto r = hooks_.swap(static_cast<size_t>(l), static_cast<size_t>(i)); !r) {
                return r;
            }
            i += 1;
        }

        if (static_cast<int64_t>(k) <= j) {
            to = static_cast<size_t>(j + 1);
        } else if (static_cast<int64_t>(k) >= i) {
            from = static_cast<size_t>(i);
        } else {
            return {};
        }
    }

    // Sort the final tiny range with a specialised sort.
    if (size == 2) {
        auto compared = hooks_.compare(from, from + 1);
        if (!compared) {
            return std::unexpected(std::move(compared.error()));
        }
        if (compared.value() > 0) {
            return hooks_.swap(from, from + 1);
        }
    } else if (size == 3) {
        return sort3(from);
    }
    return {};
}

Result<size_t> IntroSelector::min_of_three(size_t i, size_t j, size_t k) {
    auto left = hooks_.compare(i, j);
    if (!left) {
        return std::unexpected(std::move(left.error()));
    }
    if (left.value() <= 0) {
        auto right = hooks_.compare(i, k);
        if (!right) {
            return std::unexpected(std::move(right.error()));
        }
        return right.value() <= 0 ? i : k;
    }
    auto right = hooks_.compare(j, k);
    if (!right) {
        return std::unexpected(std::move(right.error()));
    }
    return right.value() <= 0 ? j : k;
}

Result<size_t> IntroSelector::max_of_three(size_t i, size_t j, size_t k) {
    auto left = hooks_.compare(i, j);
    if (!left) {
        return std::unexpected(std::move(left.error()));
    }
    if (left.value() <= 0) {
        auto right = hooks_.compare(j, k);
        if (!right) {
            return std::unexpected(std::move(right.error()));
        }
        return right.value() < 0 ? k : j;
    }
    auto right = hooks_.compare(i, k);
    if (!right) {
        return std::unexpected(std::move(right.error()));
    }
    return right.value() < 0 ? k : i;
}

Result<size_t> IntroSelector::median(size_t i, size_t j, size_t k) {
    auto first = hooks_.compare(i, j);
    if (!first) {
        return std::unexpected(std::move(first.error()));
    }
    if (first.value() < 0) {
        auto second = hooks_.compare(j, k);
        if (!second) {
            return std::unexpected(std::move(second.error()));
        }
        if (second.value() <= 0) {
            return j;
        }
        auto third = hooks_.compare(i, k);
        if (!third) {
            return std::unexpected(std::move(third.error()));
        }
        return third.value() < 0 ? k : i;
    }
    auto second = hooks_.compare(j, k);
    if (!second) {
        return std::unexpected(std::move(second.error()));
    }
    if (second.value() >= 0) {
        return j;
    }
    auto third = hooks_.compare(i, k);
    if (!third) {
        return std::unexpected(std::move(third.error()));
    }
    return third.value() < 0 ? i : k;
}

VoidResult IntroSelector::sort3(size_t from) {
    const size_t mid = from + 1;
    const size_t last = from + 2;

    auto first = hooks_.compare(from, mid);
    if (!first) {
        return std::unexpected(std::move(first.error()));
    }
    if (first.value() <= 0) {
        auto second = hooks_.compare(mid, last);
        if (!second) {
            return std::unexpected(std::move(second.error()));
        }
        if (second.value() > 0) {
            if (auto r = hooks_.swap(mid, last); !r) {
                return r;
            }
            auto third = hooks_.compare(from, mid);
            if (!third) {
                return std::unexpected(std::move(third.error()));
            }
            if (third.value() > 0) {
                return hooks_.swap(from, mid);
            }
        }
        return {};
    }
    auto second = hooks_.compare(mid, last);
    if (!second) {
        return std::unexpected(std::move(second.error()));
    }
    if (second.value() >= 0) {
        return hooks_.swap(from, last);
    }
    if (auto r = hooks_.swap(from, mid); !r) {
        return r;
    }
    auto third = hooks_.compare(mid, last);
    if (!third) {
        return std::unexpected(std::move(third.error()));
    }
    if (third.value() > 0) {
        return hooks_.swap(mid, last);
    }
    return {};
}

VoidResult IntroSelector::shuffle(size_t from, size_t to) {
    if (to <= from + 1) {
        return {};
    }
    auto &engine = shuffle_engine();
    for (int64_t i = static_cast<int64_t>(to) - 1; i >= static_cast<int64_t>(from); --i) {
        std::uniform_int_distribution<int64_t> distribution(static_cast<int64_t>(from), i);
        const int64_t j = distribution(engine);
        if (auto r = hooks_.swap(static_cast<size_t>(i), static_cast<size_t>(j)); !r) {
            return r;
        }
    }
    return {};
}

} // namespace lucene
