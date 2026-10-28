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

module lucene_core:double_values_source.impl;

import :double_values_source;
import :types;
import std;

import :double_values;
import :error;
import :explanation;
import :leaf_reader_context;
import :sort_field;

namespace lucene {

Result<std::unique_ptr<DoubleValuesSource>> DoubleValuesSource::rewrite(const IndexSearcher & /*searcher*/) const {
    return err(LuceneError::need_implemented("a double values source must implement rewrite()"));
}

Result<Explanation> DoubleValuesSource::explain(const LeafReaderContext & /*ctx*/, int32_t /*doc_id*/,
                                                const Explanation & /*score_explanation*/) const {
    return err(LuceneError::unsupported_operation("explain() is not implemented for this double values source"));
}

Result<std::unique_ptr<SortField>> DoubleValuesSource::get_sort_field(bool /*reverse*/) const {
    return std::unique_ptr<SortField>{};
}

} // namespace lucene
