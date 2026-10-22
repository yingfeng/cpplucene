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

export module lucene_core:compound_directory;

import :types;
import std;

import :directory;
import :error;

namespace lucene {

/// A read-only [`Directory`] that provides a view over a compound file.
///
/// Port of the `CompoundDirectory` trait (`core/codecs/compound_directory.rs`),
/// minus `CompoundDirectoryEnum`: C++ virtual dispatch makes the enum
/// unnecessary.
export class CompoundDirectory : public Directory {
public:
    /// Checks the consistency of this directory.
    ///
    /// **Note:** this operation may be costly in terms of I/O, for example it
    /// might compute checksum values against large data files.
    [[nodiscard]] virtual VoidResult check_integrity() const = 0;
};

} // namespace lucene
