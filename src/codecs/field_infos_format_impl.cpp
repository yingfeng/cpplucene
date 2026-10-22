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

module lucene_core:field_infos_format.impl;

import :field_infos_format;

import :directory;
import :error;
import :field_infos;
import :io_context;
import :segment_info;
import :types;

import std;

namespace lucene {

Result<FieldInfos> Lucene94FieldInfosFormatAdapter::read(const Directory &directory, const SegmentInfo &segment_info,
                                                        std::string_view segment_suffix,
                                                        const IOContext &context) const {
    return Lucene94FieldInfosFormat::read(directory, segment_info.name, segment_suffix, segment_info.id, context);
}

VoidResult Lucene94FieldInfosFormatAdapter::write(Directory &directory, const SegmentInfo &segment_info,
                                                  std::string_view segment_suffix, const FieldInfos &infos,
                                                  const IOContext &context) const {
    return Lucene94FieldInfosFormat::write(directory, segment_info.name, segment_suffix, segment_info.id, infos,
                                           context);
}

} // namespace lucene
