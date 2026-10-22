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

export module lucene_core:terms_hash;

import :types;
import std;

namespace lucene {

/// Common state shared by the terms-hash chain of one indexing buffer.
///
/// Port of `terms_hash.rs`.  The reference implementation's struct only holds the shared
/// `bytes_used` counter; the port has no `Accountable`/`Counter`
/// infrastructure, so the class is an empty base that exists to keep the
/// The reference implementation type shape (and to give the per-field classes a common name).
export class TermsHash {
public:
    TermsHash() = default;
    virtual ~TermsHash() = default;
};

} // namespace lucene
