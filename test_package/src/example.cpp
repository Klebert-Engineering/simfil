#include "simfil/value.h"

int main() {
    auto value = simfil::Value::make(static_cast<int64_t>(123));
    (void)value;

    return 0;
}
