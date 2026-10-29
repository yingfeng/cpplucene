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

module lucene_core:spans.impl;

import :spans;

import :doc_id_set_iterator;
import :two_phase_iterator;
import :types;

import std;

namespace lucene {

Result<std::unique_ptr<TwoPhaseIterator>> Spans::as_two_phase_iterator() { return std::unique_ptr<TwoPhaseIterator>{}; }

std::string Spans::to_string() const {
    return std::format("Spans(doc={},start={},end={})", doc_id(), start_position(), end_position());
}

} // namespace lucene
