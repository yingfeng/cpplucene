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

export module lucene_core:indexing_context;

import :types;
import std;

import :byte_block_pool;
import :int_block_pool;

namespace lucene {

/// The pools the per-field writers of one indexing buffer share.
///
/// Port of `IndexContext` (`indexing_chain.rs`).  The reference implementation declares it next to
/// `IndexingChain` and the per-field writers import that module; C++ module
/// partitions cannot form that cycle (`indexing_chain` imports the writers), so
/// the struct lives in its own partition and both sides import it.
export struct IndexContext {
    /// Streams of the term vectors of the current field.
    IntBlockPool term_vectors_int_pool;
    /// Streams of the postings of the current field.
    IntBlockPool freq_prox_term_int_pool;
    /// Term text bytes of the term vectors.
    ByteBlockPool term_vectors_byte_pool;
    /// Term text bytes of the postings.
    ByteBlockPool byte_pool;
};

} // namespace lucene
