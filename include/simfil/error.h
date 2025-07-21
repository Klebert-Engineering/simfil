#pragma once

#include <string>

#include "simfil/sourcelocation.h"

namespace simfil
{

struct Token;

struct Error
{
    enum Type {
        ParserError,
        InvalidType,
        InvalidExpression,
        ExpectedEOF,
        NullModel,
        IOError,
    };

    explicit Error(Type type);
    Error(Type type, std::string message);
    Error(Type type, std::string message, SourceLocation location);
    Error(Type type, std::string message, const Token& token);

    Type type;
    SourceLocation location;
    std::string message;
};

}
