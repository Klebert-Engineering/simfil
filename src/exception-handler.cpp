#include "simfil/exception-handler.h"

simfil::ThrowHandler& simfil::ThrowHandler::instance()
{
    static ThrowHandler instance;
    return instance;
}

void simfil::ThrowHandler::set(simfil::CustomThrowHandlerType handler)
{
    customThrowHandler_ = std::move(handler);
}

const simfil::CustomThrowHandlerType& simfil::ThrowHandler::get() const
{
    return customThrowHandler_;
}
