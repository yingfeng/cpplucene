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

module lucene_core:term_vectors_writer.impl;

import :term_vectors_writer;

import :data_input;
import :error;

import std;

namespace lucene {

VoidResult TermVectorsWriter::add_prox(size_t /*num_prox*/, DataInput * /*positions*/, DataInput * /*offsets*/) {
    return err(LuceneError::need_implemented("TermVectorsWriter::add_prox"));
}

} // namespace lucene
