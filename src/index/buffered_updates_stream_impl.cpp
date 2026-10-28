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

module lucene_core:buffered_updates_stream.impl;

import :buffered_updates_stream;

import std;

import :error;
import :frozen_buffered_updates;
import :types;

namespace lucene {

Result<int64_t> BufferedUpdatesStream::push(const std::shared_ptr<FrozenBufferedUpdates> &packet) {
    if (packet == nullptr) {
        return err(LuceneError::illegal_argument("cannot push a null packet"));
    }
    if (packet->del_gen() != -1) {
        return err(LuceneError::illegal_argument(
                std::format("the packet already has the generation {}", packet->del_gen())));
    }
    const int64_t gen = next_del_gen_;
    ++next_del_gen_;
    packet->set_del_gen(gen);
    packets_.push_back(packet);
    return gen;
}

size_t BufferedUpdatesStream::num_unapplied_packets() const noexcept {
    size_t count = 0;
    for (const auto &packet : packets_) {
        if (!packet->is_applied()) {
            ++count;
        }
    }
    return count;
}

int64_t BufferedUpdatesStream::total_del_count() const noexcept {
    int64_t total = 0;
    for (const auto &packet : packets_) {
        total += packet->total_del_count();
    }
    return total;
}

std::vector<std::string> BufferedUpdatesStream::packets_to_string() const {
    std::vector<std::string> out;
    out.reserve(packets_.size());
    for (const auto &packet : packets_) {
        out.push_back(packet->to_string());
    }
    return out;
}

void BufferedUpdatesStream::prune_applied() {
    // Keep a packet the generation counter still points at: `applied_gen` has to
    // stay meaningful after its packets are gone.
    packets_.erase(std::remove_if(packets_.begin(), packets_.end(),
                                  [](const std::shared_ptr<FrozenBufferedUpdates> &packet) {
                                      return packet->is_applied();
                                  }),
                   packets_.end());
}

void BufferedUpdatesStream::clear() {
    packets_.clear();
    next_del_gen_ = 1;
    applied_gen_ = 0;
}

std::string BufferedUpdatesStream::to_string() const {
    return std::format("BufferedUpdatesStream(nextDelGen={} appliedGen={} packets={} unapplied={})", next_del_gen_,
                       applied_gen_, packets_.size(), num_unapplied_packets());
}

} // namespace lucene
