#include "completion.h"

#include "expressions.h"
#include "simfil/model/schema.h"
#include "simfil/model/string-pool.h"
#include "simfil/result.h"
#include "simfil/simfil.h"
#include "simfil/function.h"

#include <cctype>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace
{

/// Returns true if the given string contains at least one
/// uppercase character.
auto containsUppercaseCharacter(std::string_view str)
{
    static const auto loc = std::locale();

    return std::ranges::any_of(str, [](auto c) {
        return std::isupper(c, loc);
    });
}

/// Returns if `str` starts with `prefix`.
auto startsWith(std::string_view str, std::string_view prefix, bool caseSensitive)
{
    static const auto loc = std::locale();

    if (prefix.size() > str.size())
        return false;

    for (auto i = 0; i < std::min<std::string_view::size_type>(str.size(), prefix.size()); ++i) {
        if (caseSensitive && str[i] != prefix[i])
            return false;
        if (!caseSensitive && std::tolower(str[i], loc) != std::tolower(prefix[i], loc))
            return false;
    }

    return true;
}

/// Returns true, if the given field name needs
/// escaping by using the index operator: `["<field-name>"]`.
auto needsEscaping(std::string_view str)
{
    if (!str.empty() && isdigit(str[0]))
        return true;

    auto i = 0;
    return std::any_of(str.begin(), str.end(), [&i](const auto chr) {
        if (!((chr >= 'a' && chr <= 'z') ||
              (chr >= 'A' && chr <= 'Z') ||
              (chr == '_') ||
              (i > 0 && chr >= '0' && chr <= '9')))
            return true;

        ++i;
        return false;
    });
}

auto escapeKey(std::string_view str)
{
    std::string escaped = "[\"";
    escaped.reserve(str.size() + 4);

    for (auto i = 0; i < str.size(); ++i) {
        if (str[i] == '"' || str[i] == '\\')
            escaped.push_back('\\');
        escaped.push_back(str[i]);
    }

    escaped += "\"]";
    return escaped;
}

/// Escape a string value as a SIMFIL string literal completion.
auto escapeStringLiteral(std::string_view str)
{
    std::string escaped = "\"";
    escaped.reserve(str.size() + 2);

    for (auto c : str) {
        if (c == '"' || c == '\\')
            escaped.push_back('\\');
        escaped.push_back(c);
    }

    escaped.push_back('"');
    return escaped;
}

/// Return the text that should be inserted for an enum-like string symbol.
auto enumSymbolCompletionText(std::string_view str)
{
    return escapeStringLiteral(str);
}

/// Add one field completion candidate if it matches the current prefix.
auto completeFieldName(
    std::string_view key,
    std::string_view prefix,
    bool caseSensitive,
    simfil::Completion& comp,
    simfil::SourceLocation loc) -> simfil::Result
{
    if (comp.size() >= comp.limit)
        return simfil::Result::Stop;

    if (!startsWith(key, prefix, caseSensitive))
        return simfil::Result::Continue;

    if (needsEscaping(key))
        comp.add(escapeKey(key), loc, simfil::CompletionCandidate::Type::FIELD);
    else
        comp.add(std::string{key}, loc, simfil::CompletionCandidate::Type::FIELD);

    return simfil::Result::Continue;
}

/// Complete fields listed by the node schema but not necessarily present in the model.
auto completeSchemaFields(
    const simfil::Context& ctx,
    const simfil::ModelNode& node,
    std::string_view prefix,
    simfil::Completion& comp,
    simfil::SourceLocation loc) -> simfil::Result
{
    const auto* schema = ctx.env->querySchema(node.schema());
    if (!schema)
        return simfil::Result::Continue;

    const auto caseSensitive = comp.options.smartCase && containsUppercaseCharacter(prefix);
    for (auto fieldId : schema->directFields()) {
        auto fieldName = ctx.env->strings()->resolve(fieldId);
        if (!fieldName || fieldName->empty())
            continue;

        if (auto r = completeFieldName(*fieldName, prefix, caseSensitive, comp, loc); r != simfil::Result::Continue)
            return r;
    }

    return simfil::Result::Continue;
}

/// Complete schema fields reachable below the current node as shorthand root tokens.
auto completeSchemaShorthandFields(
    const simfil::Context& ctx,
    const simfil::ModelNode& node,
    std::string_view prefix,
    simfil::Completion& comp,
    simfil::SourceLocation loc) -> simfil::Result
{
    const auto* schema = ctx.env->querySchema(node.schema());
    if (!schema)
        return simfil::Result::Continue;

    const auto directFields = schema->directFields();
    const auto isDirectField = [&](simfil::StringId fieldId) {
        return std::ranges::find(directFields, fieldId) != directFields.end();
    };

    const auto caseSensitive = comp.options.smartCase && containsUppercaseCharacter(prefix);
    for (auto fieldId : schema->nestedFields()) {
        if (comp.size() >= comp.limit)
            return simfil::Result::Stop;
        if (isDirectField(fieldId)) {
            continue;
        }

        auto fieldName = ctx.env->strings()->resolve(fieldId);
        if (!fieldName || fieldName->empty())
            continue;

        if (auto r = completeFieldName(*fieldName, prefix, caseSensitive, comp, loc); r != simfil::Result::Continue)
            return r;
    }

    return simfil::Result::Continue;
}

/// Complete enum-like string symbols reachable from the node schema.
auto completeSchemaEnumSymbols(
    const simfil::Context& ctx,
    const simfil::ModelNode& node,
    std::string_view prefix,
    simfil::Completion& comp,
    simfil::SourceLocation loc) -> simfil::Result
{
    const auto* schema = ctx.env->querySchema(node.schema());
    if (!schema)
        return simfil::Result::Continue;

    const auto caseSensitive = comp.options.smartCase && containsUppercaseCharacter(prefix);
    for (auto symbolId : schema->nestedEnumSymbols()) {
        if (comp.size() >= comp.limit)
            return simfil::Result::Stop;

        auto symbol = ctx.env->strings()->resolve(symbolId);
        if (!symbol || symbol->empty() || !startsWith(*symbol, prefix, caseSensitive))
            continue;
        // Unknown descendants are not evidence of an actual field-name collision.
        if (std::ranges::find(schema->nestedFields(), symbolId) != schema->nestedFields().end()) {
            continue;
        }

        comp.add(enumSymbolCompletionText(*symbol), loc, simfil::CompletionCandidate::Type::CONSTANT);
    }

    return simfil::Result::Continue;
}

/// Complete a function name staritng with `prefix` at `loc`.
auto completeFunctions(const simfil::Context& ctx, std::string_view prefix, simfil::Completion& comp, simfil::SourceLocation loc) -> simfil::Result
{
    using simfil::Result;

    const auto caseSensitive = comp.options.smartCase && containsUppercaseCharacter(prefix);
    for (const auto& [ident, fn] : ctx.env->functions) {
        if (comp.size() >= comp.limit)
            return Result::Stop;
        if (startsWith(ident, prefix, caseSensitive)) {
            comp.add(ident, loc, simfil::CompletionCandidate::Type::FUNCTION, fn->ident().signature);
        }
    }

    return Result::Continue;
}

/// Complete a single WORD starting with `prefix` at `loc`.
auto completeWords(
    const simfil::Context& ctx,
    std::string_view prefix,
    simfil::Completion& comp,
    simfil::SourceLocation loc,
    const simfil::ModelNode* node = nullptr) -> simfil::Result
{
    using simfil::Result;

    // String values from the model are string literals in SIMFIL. They are
    // suggested quoted because bare words are fields unless schema compilation
    // later proves that a token is an enum-like operand.
    auto stringPool = ctx.env->strings();
    const auto& strings = stringPool->strings();
    const auto* schema = node ? ctx.env->querySchema(node->schema()) : nullptr;

    const auto caseSensitive = comp.options.smartCase && containsUppercaseCharacter(prefix);
    for (const auto& str : strings) {
        if (comp.size() >= comp.limit)
            return Result::Stop;

        // Check if string is all uppercase + underscores + digits.
        const auto isWORD = !str.empty() && std::ranges::all_of(str, [](char c) {
            return std::isupper(c) || c == '_' || std::isdigit(c);
        });

        if (isWORD && str.size() >= prefix.size() && startsWith(str, prefix, caseSensitive)) {
            auto const stringId = stringPool->get(str);
            if (schema && stringId != simfil::StringPool::Empty && schema->canHaveEnumSymbol(stringId)) {
                // Schema enum values are completed below from schema metadata,
                // which also prevents datasource string-pool duplicates.
                continue;
            }
            comp.add(escapeStringLiteral(str), loc, simfil::CompletionCandidate::Type::CONSTANT);
        }
    }

    if (node) {
        if (auto r = completeSchemaShorthandFields(ctx, *node, prefix, comp, loc); r != Result::Continue)
            return r;

        if (auto r = completeSchemaEnumSymbols(ctx, *node, prefix, comp, loc); r != Result::Continue)
            return r;
    }

    return Result::Continue;
}

}

namespace simfil
{

auto Completion::visitDomain(Context& ctx) -> bool
{
    if (ctx.canceled() || domainVisits_ >= options.maxSchemaVisits)
        return false;
    ++domainVisits_;
    return true;
}

auto Completion::expandDomains(Context& ctx, const std::vector<SchemaId>& input, bool recursive)
    -> std::vector<SchemaId>
{
    std::vector<SchemaId> result, pending(input.rbegin(), input.rend()), visited;
    if (pending.size() > options.maxSchemaVisits)
        pending.resize(options.maxSchemaVisits);
    auto append = [&](SchemaId child) {
        // Fan-out must not allocate a whole registry before the next budget check.
        if (!ctx.canceled() && pending.size() < options.maxSchemaVisits - domainVisits_)
            pending.push_back(child);
    };
    while (!pending.empty() && visitDomain(ctx)) {
        auto id = pending.back();
        pending.pop_back();
        if (std::ranges::find(visited, id) != visited.end())
            continue;
        visited.push_back(id);
        auto schema = ctx.env->querySchema(id);
        if (!schema) {
            result.push_back(NoSchemaId);
            continue;
        }
        if (!Schema::affinities(schema->kind()))
            continue;
        if (schema->composition() == Schema::Composition::None)
            result.push_back(id);
        auto alternatives = schema->alternatives();
        for (auto child = alternatives.rbegin(); child != alternatives.rend(); ++child)
            append(*child);
        if (recursive) {
            schema->forEachDirectField([&](StringId, std::span<const SchemaId> children) {
                for (auto child = children.rbegin(); child != children.rend(); ++child)
                    append(*child);
            });
            schema->forEachElementSchema(append);
        }
    }
    return result;
}

void Completion::suggestDomain(Context& ctx, const std::vector<SchemaId>& input,
                               const std::vector<SchemaId>& expected, std::string_view prefix,
                               SourceLocation location, bool inPath, bool literalsOnly)
{
    const bool caseSensitive = options.smartCase && containsUppercaseCharacter(prefix);
    auto domains = expandDomains(ctx, input, false);
    const bool shorthand = !inPath && expected.empty();
    if (shorthand && std::ranges::any_of(domains, [&](SchemaId id) {
            auto schema = ctx.env->querySchema(id);
            return schema && !schema->reachabilityComplete();
        }))
        domains = expandDomains(ctx, input, true);
    auto isKnownField = [&](std::string_view text) {
        if (!expected.empty() || literalsOnly)
            return false;
        auto replacement = needsEscaping(text) ? escapeKey(text) : std::string(text);
        return candidates.contains(CompletionCandidate(std::move(replacement), location,
                                                       CompletionCandidate::Type::FIELD, {}));
    };
    if (!literalsOnly) {
        for (auto id : domains) {
            auto schema = ctx.env->querySchema(id);
            if (!schema)
                continue;
            auto fields = shorthand ? schema->nestedFields() : schema->directFields();
            // Dirty or custom schemas may only provide direct field enumeration.
            for (auto field : fields) {
                if (!visitDomain(ctx) || size() >= limit)
                    return;
                if (auto name = ctx.env->strings()->resolve(field))
                    completeFieldName(*name, prefix, caseSensitive, *this, location);
            }
            if (fields.empty())
                schema->forEachDirectField([&](StringId field, auto) {
                    if (visitDomain(ctx) && size() < limit)
                        if (auto name = ctx.env->strings()->resolve(field))
                            completeFieldName(*name, prefix, caseSensitive, *this, location);
                });
        }
    }
    if (inPath)
        return;

    auto enumDomains = expandDomains(ctx, expected.empty() ? input : expected, expected.empty());
    for (auto id : enumDomains) {
        auto schema = ctx.env->querySchema(id);
        if (!schema)
            continue;
        for (auto symbol : schema->directEnumSymbols()) {
            if (!visitDomain(ctx) || size() >= limit)
                return;
            auto text = ctx.env->strings()->resolve(symbol);
            // Reuse field candidates for precedence instead of building a second
            // complete field-name set on every keystroke.
            if (text && startsWith(*text, prefix, caseSensitive) && !isKnownField(*text))
                add(escapeStringLiteral(*text), location, CompletionCandidate::Type::CONSTANT);
        }
        for (const auto& literal : schema->enumValues()) {
            if (!visitDomain(ctx) || size() >= limit)
                return;
            auto value = Value(ScalarValueType(literal));
            auto text = value.isa(ValueType::String) ? std::string(value.as<ValueType::String>()) : value.toString();
            if (isKnownField(text))
                continue;
            if (startsWith(text, prefix, caseSensitive))
                add(value.isa(ValueType::String) ? escapeStringLiteral(text) : text,
                    location, CompletionCandidate::Type::CONSTANT);
        }
    }
    if (!literalsOnly && !ctx.canceled() && domainVisits_ < options.maxSchemaVisits)
        completeFunctions(ctx, prefix, *this, location);
}

auto Completion::walkDomain(Context& ctx, const Expr& expression, const std::vector<SchemaId>& input,
                            const std::vector<SchemaId>& expected, std::size_t depth, bool insidePath) -> std::vector<SchemaId>
{
    if (depth >= options.maxSchemaDepth || !visitDomain(ctx))
        return {NoSchemaId};
    auto lookup = [&](SchemaId id) { return ctx.env->querySchema(id); };
    auto walk = [&](const Expr& child, const std::vector<SchemaId>& domains,
                    const std::vector<SchemaId>& expectedDomains = std::vector<SchemaId>{},
                    std::optional<bool> childInsidePath = {}) {
        return walkDomain(ctx, child, domains, expectedDomains, depth + 1, childInsidePath.value_or(insidePath));
    };
    if (auto completion = dynamic_cast<const CompletionFieldOrWordExpr*>(&expression)) {
        suggestDomain(ctx, input, expected, completion->prefix_, completion->sourceLocation(), completion->inPath_, false);
        return {NoSchemaId};
    }
    if (auto completion = dynamic_cast<const CompletionWordExpr*>(&expression)) {
        suggestDomain(ctx, input, expected, completion->prefix_, completion->sourceLocation(), false, true);
        return {NoSchemaId};
    }
    if (auto field = dynamic_cast<const FieldExpr*>(&expression)) {
        if (field->isCurrent())
            return input;
        auto domains = expandDomains(ctx, input, false);
        auto name = ctx.env->strings()->get(field->name_);
        std::vector<SchemaId> result;
        std::vector<SchemaPath> aliases;
        // Match compile-time operand rewriting: root-owned hooks win over direct
        // fields, but neither whole tokens nor explicit paths reinterpret members.
        if (depth > 0 && !insidePath)
            if (auto root = lookup(rootSchema_))
                aliases = root->scalarFieldPathsForSymbol(name, lookup);
        for (auto id : domains) {
            if (aliases.empty()) {
                auto children = Schema::fieldSchemas(id, lookup, name);
                result.insert(result.end(), children.begin(), children.end());
            }
            else {
                // Mapget-owned scalar aliases remain schema hooks, not hardcoded paths.
                for (const auto& path : aliases) {
                    if (!visitDomain(ctx))
                        return result;
                    std::vector<SchemaId> selected{id};
                    for (const auto& segment : path) {
                        std::vector<SchemaId> next;
                        for (auto current : selected) {
                            if (!visitDomain(ctx))
                                return result;
                            auto children = segment.kind == SchemaPathSegment::Kind::Field
                                ? Schema::fieldSchemas(current, lookup, segment.field)
                                : Schema::itemSchemas(current, lookup);
                            next.insert(next.end(), children.begin(), children.end());
                        }
                        selected = std::move(next);
                    }
                    result.insert(result.end(), selected.begin(), selected.end());
                }
            }
        }
        return result;
    }
    if (auto path = dynamic_cast<const PathExpr*>(&expression))
        return walk(*path->right_, walk(*path->left_, input, {}, true), expected, true);
    if (auto sub = dynamic_cast<const SubExpr*>(&expression)) {
        auto domains = walk(*sub->left_, input);
        walk(*sub->sub_, domains, expected);
        // Filtering changes presence, not the domain of the surviving value.
        return domains;
    }
    if (auto subscript = dynamic_cast<const SubscriptExpr*>(&expression)) {
        auto domains = walk(*subscript->left_, input);
        if (auto key = dynamic_cast<const CompletionWordExpr*>(subscript->index_.get())) {
            // A quoted subscript completes member names, not enum values.
            const bool smartCase = options.smartCase && containsUppercaseCharacter(key->prefix_);
            for (auto id : expandDomains(ctx, domains, false))
                if (auto schema = lookup(id))
                    schema->forEachDirectField([&](StringId field, auto) {
                        if (!visitDomain(ctx) || size() >= limit)
                            return;
                        auto name = ctx.env->strings()->resolve(field);
                        if (name && startsWith(*name, key->prefix_, smartCase))
                            add(escapeStringLiteral(*name), key->sourceLocation(), CompletionCandidate::Type::FIELD);
                    });
            return {NoSchemaId};
        }
        walk(*subscript->index_, input);
        auto index = dynamic_cast<const ConstExpr*>(subscript->index_.get());
        // Inspect a negative literal syntactically; completion must not evaluate it.
        if (auto negative = dynamic_cast<const UnaryExpr<OperatorNegate>*>(subscript->index_.get()))
            index = dynamic_cast<const ConstExpr*>(negative->childAt(0).get());
        if (!index)
            return {NoSchemaId};
        std::vector<SchemaId> result;
        for (auto id : domains) {
            std::vector<SchemaId> children;
            if (index->value().isa(ValueType::Int))
                children = Schema::itemSchemas(id, lookup);
            else if (index->value().isa(ValueType::String))
                children = Schema::fieldSchemas(id, lookup, ctx.env->strings()->get(index->value().as<ValueType::String>()));
            else
                return {NoSchemaId};
            result.insert(result.end(), children.begin(), children.end());
        }
        return result;
    }
    if (dynamic_cast<const WildcardExpr*>(&expression))
        return expandDomains(ctx, input, true);
    if (dynamic_cast<const AnyChildExpr*>(&expression)) {
        std::vector<SchemaId> result;
        for (auto id : expandDomains(ctx, input, false)) {
            if (auto schema = lookup(id)) {
                schema->forEachDirectField([&](StringId, std::span<const SchemaId> children) {
                    for (auto child : children)
                        if (visitDomain(ctx))
                            result.push_back(child);
                });
                schema->forEachElementSchema([&](SchemaId child) {
                    if (visitDomain(ctx))
                        result.push_back(child);
                });
            }
            else
                result.push_back(NoSchemaId);
        }
        return result;
    }
    if (auto conjunction = dynamic_cast<const CompletionAndExpr*>(&expression)) {
        if (conjunction->left_) walk(*conjunction->left_, input);
        if (conjunction->right_) walk(*conjunction->right_, input);
        return {NoSchemaId};
    }
    if (auto disjunction = dynamic_cast<const CompletionOrExpr*>(&expression)) {
        if (disjunction->left_) walk(*disjunction->left_, input);
        if (disjunction->right_) walk(*disjunction->right_, input);
        return {NoSchemaId};
    }
    const bool comparison = dynamic_cast<const ComparisonExprBase*>(&expression)
        || dynamic_cast<const BinaryExpr<OperatorEq>*>(&expression)
        || dynamic_cast<const BinaryExpr<OperatorNeq>*>(&expression)
        || dynamic_cast<const BinaryExpr<OperatorLt>*>(&expression)
        || dynamic_cast<const BinaryExpr<OperatorLtEq>*>(&expression)
        || dynamic_cast<const BinaryExpr<OperatorGt>*>(&expression)
        || dynamic_cast<const BinaryExpr<OperatorGtEq>*>(&expression);
    if (comparison) {
        auto left = walk(*expression.childAt(0), input);
        if (left.empty())
            left.push_back(NoSchemaId); // Unknown/absent operands do not license unrelated enum suggestions.
        walk(*expression.childAt(1), input, left);
        return {};
    }
    // Calls and dynamic expressions have unknown results. Their arguments still
    // carry useful cursor-local contexts, but no registered function is executed.
    for (std::size_t i = 0; i < expression.numChildren(); ++i)
        walk(*expression.childAt(i), input);
    return dynamic_cast<const ConstExpr*>(&expression) ? std::vector<SchemaId>{} : std::vector<SchemaId>{NoSchemaId};
}

void Completion::completeDomain(Context ctx, const Expr& expression, SchemaId root)
{
    domainVisits_ = 0;
    rootSchema_ = root;
    walkDomain(ctx, expression, {root}, {}, 0);
}

CompletionFieldOrWordExpr::CompletionFieldOrWordExpr(std::string prefix, Completion* comp, const Token& token, bool inPath)
    : Expr(token)
    , prefix_(std::move(prefix))
    , comp_(comp)
    , inPath_(inPath)
{}

auto CompletionFieldOrWordExpr::type() const -> Type
{
    return Type::FIELD;
}

auto CompletionFieldOrWordExpr::ieval(Context ctx, const Value& val, const ResultFn& res) const -> tl::expected<Result, Error>
{
    if (ctx.phase == Context::Phase::Compilation)
        return res(ctx, Value::undef());

    if (val.isa(ValueType::Undef))
        return res(ctx, val);

    const auto node = val.node();
    if (!node)
        return res(ctx, val);

    const auto caseSensitive = comp_->options.smartCase && containsUppercaseCharacter(prefix_);

    // First we try to complete fields already present in the model.
    for (StringId id : node->fieldNames()) {
        if (id == StringPool::Empty)
            continue;

        auto keyPtr = ctx.env->strings()->resolve(id);
        if (!keyPtr || keyPtr->empty())
            continue;
        const auto& key = *keyPtr;

        if (auto r = completeFieldName(key, prefix_, caseSensitive, *comp_, sourceLocation()); r != Result::Continue)
            return r;
    }

    if (auto r = completeSchemaFields(ctx, *node, prefix_, *comp_, sourceLocation()); r != Result::Continue)
        return r;

    // If not in a path, we try to complete words and functions
    if (!inPath_) {
        if (auto r = completeWords(ctx, prefix_, *comp_, sourceLocation(), node); r != Result::Continue)
            return r;

        if (auto r = completeFunctions(ctx, prefix_, *comp_, sourceLocation()); r != Result::Continue)
            return r;
    }

    return res(ctx, Value::null());
}

auto CompletionFieldOrWordExpr::toString() const -> std::string
{
    return prefix_;
}

auto CompletionFieldOrWordExpr::accept(ExprVisitor& v) const -> void
{
    v.visit(*this);
}

namespace
{

struct FindExpressionRange : ExprVisitor
{
    size_t min = std::numeric_limits<size_t>::max();
    size_t max = std::numeric_limits<size_t>::min();

    auto contains(size_t point) const
    {
        return min <= point && point <= max;
    }

    using ExprVisitor::visit;

    void visit(const Expr& expr) override
    {
        ExprVisitor::visit(expr);

        auto loc = expr.sourceLocation();
        if (loc.size > 0) {
            min = std::min<size_t>(min, loc.offset);
            max = std::max<size_t>(max, loc.offset + loc.size);
        }
    }
};

}

CompletionAndExpr::CompletionAndExpr(ExprPtr left, ExprPtr right, const Completion* comp)
    : left_(std::move(left))
    , right_(std::move(right))
{
    FindExpressionRange leftRange;
    if (left_)
        left_->accept(leftRange);

    if (!leftRange.contains(comp->point))
        left_ = nullptr;

    FindExpressionRange rightRange;
    if (right_)
        right_->accept(rightRange);

    if (!rightRange.contains(comp->point))
        right_ = nullptr;
}

auto CompletionAndExpr::type() const -> Type
{
    return Type::VALUE;
}

auto CompletionAndExpr::ieval(Context ctx, const Value& val, const ResultFn& res) const -> tl::expected<Result, Error>
{
    if (left_)
        (void)left_->eval(ctx, val, LambdaResultFn([](const Context&, const Value&) {
            return Result::Continue;
        }));

    if (right_)
        (void)right_->eval(ctx, val, LambdaResultFn([](const Context&, const Value&) {
            return Result::Continue;
        }));

    return Result::Continue;
}

void CompletionAndExpr::accept(ExprVisitor& v) const
{
    v.visit(*this);
}

auto CompletionAndExpr::toString() const -> std::string
{
    if (left_ && right_)
        return fmt::format("(and {} {})", left_->toString(), right_->toString());
    else if (left_)
        return fmt::format("(and {} ?)", left_->toString());
    else if (right_)
        return fmt::format("(and ? {})", right_->toString());
    return "(and ? ?)";
}

CompletionOrExpr::CompletionOrExpr(ExprPtr left, ExprPtr right, const Completion* comp)
    : left_(std::move(left))
    , right_(std::move(right))
{
    FindExpressionRange leftRange;
    if (left_)
        left_->accept(leftRange);

    if (!leftRange.contains(comp->point))
        left_ = nullptr;

    FindExpressionRange rightRange;
    if (right_)
        right_->accept(rightRange);

    if (!rightRange.contains(comp->point))
        right_ = nullptr;
}

auto CompletionOrExpr::type() const -> Type
{
    return Type::VALUE;
}

auto CompletionOrExpr::ieval(Context ctx, const Value& val, const ResultFn& res) const -> tl::expected<Result, Error>
{
    if (left_)
        (void)left_->eval(ctx, val, LambdaResultFn([](const Context&, const Value&) {
            return Result::Continue;
        }));

    if (right_)
        (void)right_->eval(ctx, val, LambdaResultFn([](const Context&, const Value&) {
            return Result::Continue;
        }));

    return Result::Continue;
}

void CompletionOrExpr::accept(ExprVisitor& v) const
{
    v.visit(*this);
}

auto CompletionOrExpr::toString() const -> std::string
{
    if (left_ && right_)
        return fmt::format("(or {} {})", left_->toString(), right_->toString());
    else if (left_)
        return fmt::format("(or {} ?)", left_->toString());
    else if (right_)
        return fmt::format("(or ? {})", right_->toString());
    return "(or ? ?)";
}

CompletionWordExpr::CompletionWordExpr(std::string prefix, Completion* comp, const Token& token)
    : Expr(token)
    , prefix_(std::move(prefix))
    , comp_(comp)
{}

auto CompletionWordExpr::type() const -> Type
{
    return Type::VALUE;
}

auto CompletionWordExpr::constant() const -> bool
{
    return true;
}

auto CompletionWordExpr::ieval(Context ctx, const Value& val, const ResultFn& res) const -> tl::expected<Result, Error>
{
    if (ctx.phase == Context::Phase::Compilation)
        return res(ctx, Value::undef());

    auto node = val.node();
    if (auto r = completeWords(ctx, prefix_, *comp_, sourceLocation(), node); r != Result::Continue)
        return r;

    return res(ctx, Value::undef());
}

auto CompletionWordExpr::toString() const -> std::string
{
    return prefix_;
}

auto CompletionWordExpr::accept(ExprVisitor& v) const -> void
{
    v.visit(*this);
}

auto CompletionConstExpr::constant() const -> bool
{
    return false;
}

auto CompletionConstExpr::ieval(Context ctx, const Value&, const ResultFn& res) const -> tl::expected<Result, Error>
{
    if (ctx.phase == Context::Compilation)
        return res(ctx, Value::undef());
    return res(ctx, value_);
}

}
