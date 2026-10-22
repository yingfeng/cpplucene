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

export module lucene_core:bytes_ref_iterator;

import :types;
import std;

import :bytes_ref;
import :error;

namespace lucene {

/// Iterates over a sorted sequence of [`BytesRef`]s.
///
/// Port of the `BytesRefIterator` trait (`core/util/bytes_ref_iterator.rs`).
///
/// **Deviation:** the generic associated type `Value<'a>: BytesRefValue<'a>`
/// (owned, borrowed or `Cow`) is spelled as a borrowed `const BytesRef *`,
/// which is the `Cow::Borrowed` case the reference implementation actually uses for term
/// dictionaries.  A null pointer means "end of iteration"; the term stays valid
/// until the iterator is advanced or destroyed.
export class BytesRefIterator {
public:
    virtual ~BytesRefIterator() = default;

    /// The next term, or a null pointer once the iterator is exhausted.
    ///
    /// The pointer may be reused across calls.  Calling it again after it
    /// returned a null pointer is undefined.
    [[nodiscard]] virtual Result<const BytesRef *> next() {
        return err(LuceneError::need_implemented("BytesRefIterator::next"));
    }

    /// The `set_next` of the reference implementation: positions on the next element and reports
    /// whether there is one.
    [[nodiscard]] virtual Result<bool> set_next() {
        return err(LuceneError::need_implemented("BytesRefIterator::set_next"));
    }
};

/// An iterator over nothing (`EmptyBytesRefIterator::EMPTY`).
export class EmptyBytesRefIterator : public BytesRefIterator {
public:
    [[nodiscard]] Result<const BytesRef *> next() override { return static_cast<const BytesRef *>(nullptr); }
};

} // namespace lucene
