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
 * Modified by Cloudius Systems
 *
 * Copyright 2014 Cloudius Systems
 */

#ifndef CQL3_FUNCTIONS_NATIVE_SCALAR_FUNCTION_HH_
#define CQL3_FUNCTIONS_NATIVE_SCALAR_FUNCTION_HH_

#include "native_function.hh"
#include "scalar_function.hh"
#include "core/shared_ptr.hh"

namespace cql3 {
namespace functions {

/**
 * Base class for the <code>ScalarFunction</code> native classes.
 */
class native_scalar_function : public native_function, public scalar_function {
protected:
    native_scalar_function(sstring name, shared_ptr<abstract_type> return_type, std::vector<shared_ptr<abstract_type>> args_type)
            : native_function(std::move(name), std::move(return_type), std::move(args_type)) {
    }

public:
    virtual bool is_aggregate() override {
        return false;
    }
};

template <typename Func, bool Pure>
class native_scalar_function_for : public native_scalar_function {
    Func _func;
public:
    native_scalar_function_for(sstring name,
                               data_type return_type,
                               const std::vector<data_type> arg_types,
                               Func&& func)
            : native_scalar_function(std::move(name), std::move(return_type), std::move(arg_types))
            , _func(std::forward<Func>(func)) {
    }
    virtual bytes execute(int protocol_version, const std::vector<bytes>& parameters) override {
        return _func(protocol_version, parameters);
    }
};

template <bool Pure, typename Func>
shared_ptr<function>
make_native_scalar_function(sstring name,
                            data_type return_type,
                            std::vector<data_type> args_type,
                            Func&& func) {
    return ::make_shared<native_scalar_function_for<Func, Pure>>(std::move(name),
                                                  std::move(return_type),
                                                  std::move(args_type),
                                                  std::forward<Func>(func));
}

}
}

#endif /* CQL3_FUNCTIONS_NATIVE_SCALAR_FUNCTION_HH_ */
