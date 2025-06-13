#include "simfil/value.h"

namespace simfil
{

auto TypeFlags::types() const -> std::vector<ValueType>
{
    std::vector<ValueType> result;
    for (uint32_t bit = 0; bit < flags.size(); ++bit)
        if (flags.test(bit))
            result.push_back(static_cast<ValueType>(bit));

    return result;
}

auto TypeFlags::typeNames() const -> std::vector<std::string_view>
{
    auto values = types();

    std::vector<std::string_view> names;
    names.resize(values.size());
    std::transform(values.begin(), values.end(), names.begin(), [](auto value) {
        return valueType2String(value);
    });

    return names;
}

}
