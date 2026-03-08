//
// Created by akos on 6. 3. 2026.
//

#ifndef CPGDB_DATAMODELJSONSERIALIZER_HPP
#define CPGDB_DATAMODELJSONSERIALIZER_HPP

#include <data_model/Value.hpp>
#include <utilities/json.hpp>

class DataModelJsonSerializer {
public:
    value_t json_primitive_to_value(const json& j) {
        if (j.is_number_integer()) return _prim_impl<int>(j);
        if (j.is_number_unsigned()) return _prim_impl<unsigned>(j);
        if (j.is_boolean()) return _prim_impl<bool>(j);
        if (j.is_number_float()) return _prim_impl<float>(j);
        if (j.is_string()) return _prim_impl<std::string>(j);

        return nullptr;
    }
private:

    template<typename T>
    value_t _prim_impl(const json& j) {
        return std::make_unique<Value>(j.get<T>());
    }
};

#endif //CPGDB_DATAMODELJSONSERIALIZER_HPP