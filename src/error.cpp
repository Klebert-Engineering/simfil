#include "simfil/error.h"

#include "simfil/token.h"

namespace simfil
{

Error::Error(Type type)
    : type(type)
{}

Error::Error(Type type, std::string message)
    : type(type), message(std::move(message))
{}

Error::Error(Type type, std::string message, SourceLocation location)
    : type(type), message(std::move(message)), location(location)
{}

Error::Error(Type type, std::string message, const Token& token)
    : type(type), message(std::move(message)), location(token.begin, token.begin - token.end)
{}

}
