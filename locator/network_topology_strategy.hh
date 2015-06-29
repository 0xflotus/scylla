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
 *
 * Modified by Cloudius Systems.
 * Copyright 2015 Cloudius Systems.
 */

#pragma once

#include "locator/abstract_replication_strategy.hh"
#include "exceptions/exceptions.hh"

namespace locator {
class network_topology_strategy : public abstract_replication_strategy {
public:
    network_topology_strategy(
        const sstring& keyspace_name,
        token_metadata& token_metadata,
        snitch_ptr& snitch,
        const std::map<sstring,sstring>& config_options);

    virtual size_t get_replication_factor() const override {
        return _rep_factor;
    }

    size_t get_replication_factor(const sstring& dc) const {
        auto dc_factor = _dc_rep_factor.find(dc);
        return (dc_factor == _dc_rep_factor.end()) ? 0 : dc_factor->second;
    }

protected:
    /**
     * calculate endpoints in one pass through the tokens by tracking our
     * progress in each DC, rack etc.
     */
    virtual std::vector<inet_address> calculate_natural_endpoints(
        const token& search_token) override;

private:
    bool has_sufficient_replicas(
        const sstring& dc,
        std::unordered_map<sstring,
                           std::unordered_set<inet_address>>& dc_replicas,
        std::unordered_map<sstring,
                           std::unordered_set<inet_address>>& all_endpoints);

    bool has_sufficient_replicas(
        std::unordered_map<sstring,
                           std::unordered_set<inet_address>>& dc_replicas,
        std::unordered_map<sstring,
                           std::unordered_set<inet_address>>& all_endpoints);

    const std::vector<sstring>& get_datacenters() const {
        return _datacenteres;
    }

    void validate_options() {
        for (auto& c : _config_options)
        {
            if (c.first == sstring("replication_factor"))
                throw exceptions::configuration_exception(
                    "replication_factor is an option for simple_strategy, not "
                    "network_topology_strategy");

            validate_replication_factor(c.second);
        }
    }

    // ????
    #if 0
    public Collection<String> recognized_options()
    {
        // We explicitely allow all options
        return null;
    }
    #endif

private:
    // map: data centers -> replication factor
    std::unordered_map<sstring, size_t> _dc_rep_factor;

    std::vector<sstring> _datacenteres;
    size_t _rep_factor;
};
} // namespace locator
