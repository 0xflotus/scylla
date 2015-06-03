/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Copyright 2015 Cloudius Systems
 *
 * Modified by Cloudius Systems
 */

#pragma once

#include "property_definitions.hh"
#include "core/sstring.hh"

#include <unordered_map>
#include <experimental/optional>

typedef std::unordered_map<sstring, sstring> index_options_map;

namespace cql3 {

namespace statements {

class index_prop_defs : public property_definitions {
public:
    static constexpr auto KW_OPTIONS = "options";

    bool is_custom = false;
    std::experimental::optional<sstring> custom_class;

    void validate();
    index_options_map get_raw_options();
    index_options_map get_options();
};

}
}

