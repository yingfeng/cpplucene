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

module lucene_core:task_executor.impl;

import :task_executor;

import std;

import :error;
import :types;

namespace lucene {

std::shared_ptr<TaskExecutor> TaskExecutor::direct() {
    return std::shared_ptr<TaskExecutor>(new TaskExecutor(0));
}

Result<std::shared_ptr<TaskExecutor>> TaskExecutor::with_threads(size_t threads) {
    return std::shared_ptr<TaskExecutor>(new TaskExecutor(threads));
}

} // namespace lucene
