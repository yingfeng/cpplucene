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

module;

#include <sys/utsname.h>

module clucene:constants.impl;

import :constants;

import :types;
import std;

namespace clucene {

namespace {

struct UtsName {
    utsname info{};
    bool ok{false};

    UtsName() : ok(::uname(&info) == 0) {}
};

const UtsName &uts_name() {
    static const UtsName info;
    return info;
}

std::string capitalize(std::string_view value) {
    if (value.empty()) {
        return std::string(value);
    }
    std::string result(value);
    result[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(result[0])));
    return result;
}

} // namespace

std::string Constants::os_name() { return uts_name().ok ? capitalize(uts_name().info.sysname) : "unknown"; }

std::string Constants::os_arch() { return uts_name().ok ? std::string(uts_name().info.machine) : std::string("unknown"); }

std::string Constants::os_version() { return uts_name().ok ? std::string(uts_name().info.release) : std::string(); }

} // namespace clucene
