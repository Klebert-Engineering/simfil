#include "simfil/model/schema-model.h"

#include <deque>
#include <map>
#include <stdexcept>
#include <fmt/format.h>

namespace simfil
{

/** Addresses identify lazy views, not copies of schema definitions or model values. */
class SchemaModel::Impl
{
public:
    enum class View { Descriptor, Fields, Elements, Alternatives, Enum, References,
                      NamedValues, NamedValue, NumericDomain, Ranges, Range };

    /** One traversal position retains ancestry so cycles serialize as explicit references. */
    struct Entry {
        View view = View::Descriptor;
        std::vector<SchemaId> schemas;
        std::optional<std::uint32_t> parent;
        std::optional<bool> required;
        std::string stop;
        std::size_t position = 0;
        std::optional<std::vector<StringId>> keys;
        std::map<std::size_t, std::uint32_t> children;
    };

    /** Reserve one terminal view for allocation-budget exhaustion. */
    Impl(std::shared_ptr<StringPool> strings, Schema::Lookup lookup, std::size_t nodes)
        : strings(std::move(strings)), lookup(std::move(lookup)), maxNodes(nodes)
    {
        if (!this->strings || !this->lookup || nodes < 2 || nodes >= (1u << 24))
            throw std::invalid_argument("SchemaModel requires strings, an owning lookup and 2..2^24-1 nodes");
        Entry terminal;
        terminal.stop = "node-budget";
        entries.push_back(std::move(terminal));
    }

    /** Resolve a single definition; multiple ids denote an implicit field-domain union. */
    auto schema(const Entry& entry) const -> const Schema*
    {
        return entry.schemas.size() == 1 && entry.schemas.front() != NoSchemaId
            ? lookup(entry.schemas.front()) : nullptr;
    }

    /** Stop recursive expansion without disguising the definition as an empty object. */
    auto add(Entry entry) -> std::uint32_t
    {
        if (entries.size() >= maxNodes) {
            exhausted = true;
            return 0;
        }
        if (entry.view == View::Descriptor && entry.schemas.size() == 1 && entry.schemas.front() != NoSchemaId) {
            for (auto parent = entry.parent; parent; parent = entries[*parent].parent) {
                const auto& ancestor = entries[*parent];
                if (ancestor.view == View::Descriptor && ancestor.schemas == entry.schemas) {
                    entry.stop = "cycle";
                    break;
                }
            }
        }
        entries.push_back(std::move(entry));
        return entries.size() - 1;
    }

    /** Enumerate only this descriptor level; children remain unexpanded until accessed. */
    auto keys(std::uint32_t index) -> const std::vector<StringId>&
    {
        auto& entry = entries.at(index);
        if (entry.keys)
            return *entry.keys;
        entry.keys.emplace();
        auto& keys = *entry.keys;
        auto domain = schema(entry);
        if (entry.view == View::NamedValue) {
            keys = {StringPool::SchemaName, StringPool::SchemaValue, StringPool::SchemaRole,
                StringPool::SchemaMeaning, StringPool::SchemaDeprecated, StringPool::SchemaRemoved};
            return keys;
        }
        if (entry.view == View::Range) {
            keys = {StringPool::SchemaMin, StringPool::SchemaMax};
            return keys;
        }
        if (entry.view == View::NumericDomain) {
            auto numeric = domain ? domain->numericDomain() : std::nullopt;
            if (!numeric)
                return keys;
            keys = {StringPool::SchemaEncoding, StringPool::SchemaUnit};
            if (numeric->minimum)
                keys.push_back(StringPool::SchemaMinimum);
            if (numeric->maximum)
                keys.push_back(StringPool::SchemaMaximum);
            keys.insert(keys.end(), {StringPool::SchemaOrdinaryRanges, StringPool::SchemaScale,
                StringPool::SchemaOffset, StringPool::SchemaComplete});
            return keys;
        }
        if (entry.view == View::Fields) {
            if (domain)
                domain->forEachDirectField([&](StringId name, auto) { keys.push_back(name); });
            return keys;
        }
        if (entry.view != View::Descriptor)
            return keys;
        keys.push_back(StringPool::SchemaKind);
        if (entry.schemas.size() > 1 ||
            (entry.schemas.size() == 1 && entry.schemas.front() != NoSchemaId))
            keys.push_back(StringPool::SchemaRef);
        if (!entry.stop.empty()) {
            keys.push_back(StringPool::SchemaTruncated);
            return keys;
        }
        if (domain && !domain->typeName().empty())
            keys.push_back(StringPool::SchemaTypeName);
        if (entry.required)
            keys.push_back(StringPool::SchemaRequired);
        if (domain && domain->nullable())
            keys.push_back(StringPool::SchemaNullable);
        if (entry.schemas.size() > 1 || (domain && domain->composition() != Schema::Composition::None))
            keys.push_back(StringPool::SchemaAlternatives);
        else if (domain) {
            if (domain->hasAffinity(ValueType::Object) && Schema::kindNameId(domain->kind()) != Schema::kindNameId(Schema::Kind::Unknown) && Schema::kindNameId(domain->kind()) != Schema::kindNameId(Schema::Kind::Any)) {
                keys.push_back(StringPool::SchemaFields);
                keys.push_back(StringPool::SchemaOpen);
            }
            if (domain->hasAffinity(ValueType::Array) && Schema::kindNameId(domain->kind()) != Schema::kindNameId(Schema::Kind::Unknown) && Schema::kindNameId(domain->kind()) != Schema::kindNameId(Schema::Kind::Any))
                keys.push_back(StringPool::SchemaElements);
        }
        if (domain && (!domain->enumValues().empty() || !domain->directEnumSymbols().empty()))
            keys.push_back(StringPool::SchemaEnum);
        if (domain) {
            bool named = false;
            domain->forEachNamedValue([&](auto const&) { named = true; });
            if (named)
                keys.push_back(StringPool::SchemaNamedValues);
            if (domain->numericDomain())
                keys.push_back(StringPool::SchemaNumericDomain);
            if (domain->isBitmask())
                keys.push_back(StringPool::SchemaBitWidth);
        }
        return keys;
    }

    /** Array views list possible domains or enum values, not fabricated feature elements. */
    auto schemas(const Entry& entry) const -> std::vector<SchemaId>
    {
        if (entry.view == View::References)
            return entry.schemas;
        auto domain = schema(entry);
        if (entry.view == View::Alternatives) {
            if (entry.schemas.size() > 1)
                return entry.schemas;
            if (domain)
                return {domain->alternatives().begin(), domain->alternatives().end()};
        }
        std::vector<SchemaId> result;
        if (domain && entry.view == View::Elements)
            domain->forEachElementSchema([&](SchemaId id) { result.push_back(id); });
        return result;
    }

    std::shared_ptr<StringPool> strings;
    Schema::Lookup lookup;
    std::size_t maxNodes;
    bool exhausted = false;
    std::deque<Entry> entries;
};

/** The ordinary node protocol always exposes metadata and never feature-domain access. */
class SchemaModel::Node : public ModelNodeBase
{
public:
    /** Bind a lightweight protocol view to one request-local descriptor address. */
    Node(const SchemaModel& owner, const ModelNode& node)
        : ModelNodeBase(node, owner.mpKey_), owner_(owner), index_(node.addr().index()) {}

    /** Describe the metadata container, not the represented feature value. */
    auto type() const -> ValueType override
    {
        auto view = entry().view;
        return view == Impl::View::Descriptor || view == Impl::View::Fields || view == Impl::View::NamedValue ||
            view == Impl::View::NumericDomain || view == Impl::View::Range ? ValueType::Object : ValueType::Array;
    }

    /** Descriptor objects have no feature-domain schema attached. */
    auto schema() const -> SchemaId override { return NoSchemaId; }

    /** Inspect only immediate metadata without expanding child descriptors. */
    auto size() const -> std::uint32_t override
    {
        if (type() == ValueType::Object)
            return impl().keys(index_).size();
        if (entry().view == Impl::View::Enum) {
            auto domain = impl().schema(entry());
            return domain ? domain->directEnumSymbols().size() + domain->enumValues().size() : 0;
        }
        if (entry().view == Impl::View::NamedValues) {
            std::uint32_t count = 0;
            if (auto domain = impl().schema(entry()))
                domain->forEachNamedValue([&](auto const&) { ++count; });
            return count;
        }
        if (entry().view == Impl::View::Ranges) {
            auto domain = impl().schema(entry());
            auto numeric = domain ? domain->numericDomain() : std::nullopt;
            return numeric ? numeric->ordinaryRanges.size() : 0;
        }
        return impl().schemas(entry()).size();
    }

    /** Return object keys in the same order used by indexed access. */
    auto keyAt(int64_t index) const -> StringId override
    {
        if (type() != ValueType::Object || index < 0 || std::size_t(index) >= size())
            return StringPool::Empty;
        return impl().keys(index_)[index];
    }

    /** Resolve one metadata member through its stable cached position. */
    auto get(const StringId& field) const -> Ptr override
    {
        if (type() != ValueType::Object)
            return {};
        const auto& keys = impl().keys(index_);
        auto found = std::ranges::find(keys, field);
        return found == keys.end() ? Ptr{} : at(found - keys.begin());
    }

    /** Materialize only the requested child, retaining scalar payloads on this model. */
    auto at(int64_t position) const -> Ptr override
    {
        if (position < 0 || std::size_t(position) >= size())
            return {};
        auto domain = impl().schema(entry());
        if (entry().view == Impl::View::NamedValue) {
            std::optional<Schema::NamedValue> named;
            std::size_t ordinal = 0;
            domain->forEachNamedValue([&](auto const& value) {
                if (ordinal++ == entry().position)
                    named = value;
            });
            if (!named)
                return {};
            switch (keyAt(position)) {
            case StringPool::SchemaName: return scalar(std::string(named->name));
            case StringPool::SchemaValue:
                return std::visit([&](auto value) { return metadataScalar(value, named->role == Schema::NamedValue::Role::Flag); }, named->value);
            case StringPool::SchemaRole:
                return scalar(std::string(named->role == Schema::NamedValue::Role::Flag ? "flag" :
                    named->role == Schema::NamedValue::Role::Special ? "special" : "constant"));
            case StringPool::SchemaMeaning: {
                switch (named->meaning) {
                case Schema::NamedValue::Meaning::Unknown: return scalar(std::string("unknown"));
                case Schema::NamedValue::Meaning::Variable: return scalar(std::string("variable"));
                case Schema::NamedValue::Meaning::Unbounded: return scalar(std::string("unbounded"));
                case Schema::NamedValue::Meaning::Saturated: return scalar(std::string("saturated"));
                default: return scalar(std::string("ordinary"));
                }
            }
            case StringPool::SchemaDeprecated: return scalar(named->deprecated);
            case StringPool::SchemaRemoved: return scalar(named->removed);
            default: return {};
            }
        }
        if (entry().view == Impl::View::Range) {
            auto numeric = domain->numericDomain();
            if (!numeric || entry().position >= numeric->ordinaryRanges.size())
                return {};
            auto const& range = numeric->ordinaryRanges[entry().position];
            return std::visit([&](auto value) { return metadataScalar(value); },
                keyAt(position) == StringPool::SchemaMin ? range.min : range.max);
        }
        if (entry().view == Impl::View::NumericDomain) {
            auto numeric = domain->numericDomain();
            switch (keyAt(position)) {
            case StringPool::SchemaEncoding: return scalar(std::string(numeric->encoding));
            case StringPool::SchemaUnit: return scalar(std::string(numeric->unit));
            case StringPool::SchemaMinimum: return std::visit([&](auto v) { return metadataScalar(v); }, *numeric->minimum);
            case StringPool::SchemaMaximum: return std::visit([&](auto v) { return metadataScalar(v); }, *numeric->maximum);
            case StringPool::SchemaScale: return scalar(numeric->scale);
            case StringPool::SchemaOffset: return scalar(numeric->offset);
            case StringPool::SchemaComplete: return scalar(numeric->complete);
            default: break;
            }
        }
        if (entry().view == Impl::View::References)
            return scalar(int64_t(entry().schemas[position]));
        if (entry().view == Impl::View::Enum) {
            auto symbols = domain->directEnumSymbols();
            if (std::size_t(position) < symbols.size())
                return scalar(std::string(impl().strings->resolve(symbols[position]).value_or("")));
            return scalar(domain->enumValues()[position - symbols.size()]);
        }
        if (entry().view == Impl::View::Descriptor) {
            switch (keyAt(position)) {
            case StringPool::SchemaKind: {
                auto kind = entry().schemas.size() > 1 ? Schema::Kind::Union
                    : domain ? domain->kind() : Schema::Kind::Unknown;
                auto name = impl().strings->resolve(Schema::kindNameId(kind));
                if (!name)
                    throw std::logic_error("Schema kind name is absent from its string namespace");
                return scalar(std::string(*name));
            }
            case StringPool::SchemaTypeName: return scalar(std::string(domain->typeName()));
            case StringPool::SchemaRequired: return scalar(*entry().required);
            case StringPool::SchemaNullable: return scalar(*domain->nullable());
            case StringPool::SchemaOpen: return scalar(domain->open());
            case StringPool::SchemaBitWidth: return scalar(std::int64_t(domain->bitWidth()));
            case StringPool::SchemaRef:
                if (entry().schemas.size() == 1)
                    return scalar(int64_t(entry().schemas.front()));
                break;
            case StringPool::SchemaTruncated: return scalar(entry().stop);
            default: break;
            }
        }
        if (auto found = entry().children.find(position); found != entry().children.end())
            return owner_.Model::resolve(ModelNodeAddress(FirstColumn, found->second));

        Impl::Entry child;
        child.parent = index_;
        if (entry().view == Impl::View::NamedValues || entry().view == Impl::View::Ranges ||
            entry().view == Impl::View::NumericDomain) {
            child.schemas = entry().schemas;
            child.position = position;
            child.view = entry().view == Impl::View::NamedValues ? Impl::View::NamedValue :
                entry().view == Impl::View::Ranges ? Impl::View::Range : Impl::View::Ranges;
        }
        else if (entry().view == Impl::View::Descriptor) {
            child.schemas = entry().schemas;
            switch (keyAt(position)) {
            case StringPool::SchemaRef:
                child.view = Impl::View::References;
                break;
            case StringPool::SchemaFields: child.view = Impl::View::Fields; break;
            case StringPool::SchemaElements: child.view = Impl::View::Elements; break;
            case StringPool::SchemaAlternatives: child.view = Impl::View::Alternatives; break;
            case StringPool::SchemaEnum: child.view = Impl::View::Enum; break;
            case StringPool::SchemaNamedValues: child.view = Impl::View::NamedValues; break;
            case StringPool::SchemaNumericDomain: child.view = Impl::View::NumericDomain; break;
            default: return {};
            }
        }
        else {
            if (entry().view == Impl::View::Fields) {
                auto field = keyAt(position);
                domain->forEachDirectField([&](StringId name, std::span<const SchemaId> ids) {
                    if (name == field)
                        child.schemas.insert(child.schemas.end(), ids.begin(), ids.end());
                });
                child.required = domain->fieldRequired(field);
                if (child.schemas.empty())
                    child.schemas.push_back(NoSchemaId);
            }
            else
                child.schemas.push_back(impl().schemas(entry())[position]);
        }
        auto address = impl().add(std::move(child));
        entry().children.emplace(position, address);
        return owner_.Model::resolve(ModelNodeAddress(FirstColumn, address));
    }

    /** Visit shallow children in descriptor order and honor consumer short-circuiting. */
    bool iterate(const IterCallback& callback) const override
    {
        for (std::uint32_t i = 0, end = size(); i < end; ++i)
            if (!callback(*at(i)))
                return false;
        return true;
    }

    static constexpr uint8_t FirstColumn = Model::FirstNontrivialColumnId;

private:
    /** Keep large unsigned declarations and all masks exact across JSON/JavaScript boundaries. */
    template<class T>
    auto metadataScalar(T value, bool flag = false) const -> Ptr
    {
        if constexpr (std::is_same_v<T, std::uint64_t> || std::is_same_v<T, std::int64_t>) {
            if (flag)
                return scalar(fmt::format("0x{:016x}", static_cast<std::uint64_t>(value)));
            if constexpr (std::is_same_v<T, std::uint64_t>)
                if (value > std::uint64_t(std::numeric_limits<std::int64_t>::max()))
                    return scalar(std::to_string(value));
            return scalar(static_cast<std::int64_t>(value));
        }
        else if constexpr (std::is_same_v<T, std::string_view>)
            return scalar(std::string(value));
        else
            return scalar(value);
    }
    /** Scalar payloads keep the descriptor owner alive without a temporary ModelPool. */
    auto scalar(ScalarValueType value) const -> Ptr
    {
        return owner_.Model::resolve(ModelNodeAddress(Model::Scalar), std::move(value));
    }
    /** Access the owning request's mutable lazy-view cache. */
    auto impl() const -> Impl& { return *owner_.impl_; }
    /** Locate this view without transferring ownership of its graph metadata. */
    auto entry() const -> Impl::Entry& { return impl().entries.at(index_); }
    const SchemaModel& owner_;
    std::uint32_t index_;
};

SchemaModel::SchemaModel(std::shared_ptr<StringPool> strings, Schema::Lookup lookup,
                         std::size_t maxNodes)
    : impl_(std::make_unique<Impl>(std::move(strings), std::move(lookup), maxNodes)) {}

SchemaModel::~SchemaModel() = default;

auto SchemaModel::root(SchemaId schema) const -> ModelNode::Ptr
{
    Impl::Entry entry;
    entry.schemas.push_back(schema);
    return Model::resolve(ModelNodeAddress(Node::FirstColumn, impl_->add(std::move(entry))));
}

auto SchemaModel::strings() const -> std::shared_ptr<StringPool> { return impl_->strings; }
auto SchemaModel::materializedNodeCount() const -> std::size_t { return impl_->entries.size(); }
auto SchemaModel::exhausted() const -> bool { return impl_->exhausted; }

auto SchemaModel::resolve(const ModelNode& node, const ResolveFn& callback) const -> tl::expected<void, Error>
{
    if (node.addr().column() != Node::FirstColumn)
        return Model::resolve(node, callback);
    if (node.addr().index() >= impl_->entries.size())
        return tl::unexpected<Error>(Error::IndexOutOfRange, "Invalid schema descriptor address");
    callback(Node(*this, node));
    return {};
}

auto SchemaModel::lookupStringId(StringId id) const -> std::optional<std::string_view>
{
    return impl_->strings->resolve(id);
}

}
