// Copyright (c) Navigation Data Standard e.V. - See "LICENSE" file.

#pragma once

#include <vector>
#include <string>
#include <string_view>
#include <variant>
#include <cstdint>
#include <tl/expected.hpp>

#include "simfil/error.h"

namespace simfil
{

struct Token
{
    enum Type {
        NIL,           // EOF
        /* Syntax */
        LPAREN,         // (
        RPAREN,         // )
        LBRACK,         // [
        RBRACK,         // ]
        LBRACE,         // {
        RBRACE,         // }
        COMMA,          // ,
        DOT,            // .
        COLON,          // :
        WILDCARD,       // **
        INT,            //
        FLOAT,          //
        STRING,         // [r]"..." or [r]'...'
        REGEXP,         // A string prefixed by re or RE
        WORD,           //
        SELF,           // _
        /* Constants */
        C_NULL,         // null
        C_TRUE,         // true
        C_FALSE,        // false
        /* Arithmetic */
        OP_ADD,         // +
        OP_SUB,         // -
        OP_TIMES,       // *
        OP_DIV,         // /
        OP_MOD,         // %
        /* Bitwise */
        OP_LSHIFT,      // <<
        OP_RSHIFT,      // >>
        OP_BITAND,      // &
        OP_BITOR,       // |
        OP_BITXOR,      // ^
        OP_BITINV,      // ~
        /* Boolean */
        OP_NOT,         // not
        OP_AND,         // and
        OP_OR,          // or
        OP_EQ,          // ==
        OP_NOT_EQ,      // !=
        OP_LT, OP_LTEQ, // < <=
        OP_GT, OP_GTEQ, // > >=
        OP_BOOL,        // ?
        /* Special */
        OP_LEN,         // #
        /* Path */
        OP_TYPEOF,      // typeof
        /* Cast/Type */
        OP_CAST,        // as
        /* Lists */
        OP_UNPACK,      // ...
    } type;
    std::variant<
        std::monostate,
        std::string,
        int64_t,
        double
    > value;
    size_t begin = 0;
    size_t end = 0;

    Token(Type t, size_t begin, size_t end)
        : type(t), begin(begin), end(end)
    {}

    template <class Value>
    Token(Type t, Value&& v, size_t begin, size_t end)
        : type(t), value(std::forward<Value>(v)), begin(begin), end(end)
    {}

    Token(const Token& o) = default;
    Token(Token&&) = default;

    static auto toString(Type) -> std::string;
    auto toString() const -> std::string;

    auto containsPoint(size_t point) const -> bool;
};

std::ostream& operator<<(std::ostream&, const Token&);

/**
 * Split a SIMFIL expression `expr` into parser tokens
 */
auto tokenize(std::string_view expr) -> tl::expected<std::vector<Token>, Error>;

}
