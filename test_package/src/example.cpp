#include "simfil/value.h"

#ifndef SIMFIL_WITH_MODEL_JSON
#  error "Definition SIMFIL_WITH_MODEL_JSON is not visible to the consumer!"
#endif

int main() {
    auto value = simfil::Value::make(static_cast<int64_t>(123));
    (void)value;

    return 0;
}
