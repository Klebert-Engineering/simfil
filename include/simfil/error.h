#pragma once

#include <string>

#include "simfil/sourcelocation.h"

namespace simfil
{

struct Token;

struct Error
{
    enum Type {
        // Parser Errors
        ParserError,
        InvalidType,
        InvalidExpression,
        ExpectedEOF,
        NullModel,
        IOError,

        // Evaluation errors
        DivisionByZero,
        UnknownFunction,
        RuntimeError,
        InternalError,
        InvalidOperator,
        InvalidOperands,
        InvalidArguments,
        ExpectedSingleValue,
        TypeMissmatch,

        // Model related runtime errors
        StringPoolOverflow,
        EncodeDecodeError,
        FieldNotFound,
        IndexOutOfRange,

        Unimplemented,
    };

    explicit Error(Type type);
    Error(Type type, std::string message);
    Error(Type type, std::string message, SourceLocation location);
    Error(Type type, std::string message, const Token& token);

    auto operator==(const Error& o) const -> bool = default;

    Type type;
    SourceLocation location;
    std::string message;
};

}
