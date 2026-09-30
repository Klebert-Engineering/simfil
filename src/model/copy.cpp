#include "simfil/model/model.h"
#include "simfil/value.h"

#include <algorithm>

namespace simfil
{

/** Owns one bounded copy traversal; only the active path participates in cycle detection. */
class ModelPool::NodeCopy
{
public:
    /** Bind one destination and reset all per-operation materialization limits. */
    NodeCopy(ModelPool& target, CopyOptions const& options) : target_(target), options_(options) {}

    /** Retain each source owner and surface resolution failures before accessing its protocol. */
    auto copy(ModelNode const& source, size_t depth = 0) -> tl::expected<ModelNode::Ptr, Error>
    {
        if (depth > options_.maxDepth || nodes_ >= options_.maxNodes)
            return failure("Node copy depth or node budget exceeded");
        ++nodes_;
        auto owner = source.owningModel();
        if (!owner)
            return copyResolved(source, depth);

        tl::expected<ModelNode::Ptr, Error> result = failure("Model did not resolve node for copying");
        auto resolved = owner->resolve(source, Model::Lambda([&](ModelNode const& node) {
            result = copyResolved(node, depth);
        }));
        if (!resolved)
            return tl::unexpected(resolved.error());
        return result;
    }

    /** Share limits across the outer sequence and every scalar/compound result. */
    auto sequence(std::span<Value const> values) -> tl::expected<model_ptr<Array>, Error>
    {
        if (options_.maxNodes == 0 || values.size() > options_.maxNodes - 1 ||
            !charge(sizeof(ModelNodeAddress) + values.size() * sizeof(ModelNodeAddress)))
            return failure("Result sequence copy budget exceeded");
        ++nodes_;
        // The exact count is known: singleton slots use compact arena storage.
        auto result = target_.newArray(std::max(size_t{1}, values.size()), true);
        for (auto const& value : values) {
            tl::expected<ModelNode::Ptr, Error> copied;
            if (auto node = value.node()) {
                copied = copy(*node, 1);
            }
            else if (value.type == ValueType::Undef) {
                auto undefined = target_.newUndefined();
                copied = copy(*undefined, 1);
            }
            else if (value.type == ValueType::Object || value.type == ValueType::Array ||
                value.type == ValueType::TransientObject || value.type == ValueType::LAST_) {
                return failure("Unsupported result value in native copy");
            }
            else {
                auto scalar = model_ptr<ValueNode>::make(value.getScalar(), target_.shared_from_this());
                copied = copy(*scalar, 1);
            }
            if (!copied)
                return tl::unexpected(copied.error());
            result->append(*copied);
        }
        return result;
    }

private:
    /** Copy scalars directly; collections use protocol order, not a JSON serialization override. */
    auto copyResolved(ModelNode const& source, size_t depth) -> tl::expected<ModelNode::Ptr, Error>
    {
        if (!charge(sizeof(ModelNodeAddress)))
            return failure("Node copy byte budget exceeded");
        switch (source.type()) {
        case ValueType::Undef: return target_.newUndefined();
        case ValueType::Null: return target_.resolve(ModelNodeAddress{Model::Null, 1});
        case ValueType::Bool: return target_.newSmallValue(std::get<bool>(source.value()));
        case ValueType::Int:
            if (!charge(sizeof(int64_t)))
                return failure("Node copy byte budget exceeded");
            return target_.newValue(std::get<int64_t>(source.value()));
        case ValueType::Float:
            if (!charge(sizeof(double)))
                return failure("Node copy byte budget exceeded");
            return target_.newValue(std::get<double>(source.value()));
        case ValueType::String: {
            auto value = source.value();
            auto view = std::holds_alternative<std::string>(value)
                ? std::string_view(std::get<std::string>(value)) : std::get<std::string_view>(value);
            if (!charge(view.size()) || !charge(2 * sizeof(uint32_t)))
                return failure("Node copy byte budget exceeded");
            // newValue owns bytes, even when the source uses a pooled or borrowed string.
            return target_.newValue(view);
        }
        case ValueType::Bytes: {
            auto value = std::get<simfil::ByteArray>(source.value());
            if (!charge(value.bytes.size()) || !charge(2 * sizeof(uint32_t)))
                return failure("Node copy byte budget exceeded");
            return target_.newValue(value);
        }
        case ValueType::Array:
        case ValueType::Object:
            return copyCollection(source, depth);
        case ValueType::TransientObject:
        case ValueType::LAST_:
            return failure("Unsupported node type in native copy");
        }
        return failure("Invalid node type in native copy");
    }

    /** Copy repeated keys and shared children without conflating shared subtrees with cycles. */
    auto copyCollection(ModelNode const& source, size_t depth) -> tl::expected<ModelNode::Ptr, Error>
    {
        auto identity = std::pair{source.owningModel().get(), source.addr().value_};
        if (std::ranges::find(active_, identity) != active_.end())
            return failure("Cyclic node cannot be materialized as a tree");
        auto count = source.size();
        if (count > options_.maxNodes - nodes_)
            return failure("Node copy node budget exceeded");
        if (!charge(size_t(count) * (sizeof(ModelNodeAddress) + sizeof(StringId))))
            return failure("Node copy byte budget exceeded");

        active_.push_back(identity);
        auto capacity = std::max(uint32_t{1}, count);
        auto array = source.type() == ValueType::Array ? target_.newArray(capacity, true) : model_ptr<Array>{};
        auto object = source.type() == ValueType::Object ? target_.newObject(capacity, true) : model_ptr<Object>{};
        for (uint32_t i = 0; i < count; ++i) {
            std::optional<std::string_view> name;
            if (object) {
                auto owner = source.owningModel();
                name = owner ? owner->lookupStringId(source.keyAt(i)) : std::nullopt;
                // The empty name is a known static key, despite sharing the missing-id sentinel.
                if (!name || (!name->empty() && !target_.strings()->get(*name)))
                    return failure("Projected field name is absent from the destination dictionary");
            }
            auto child = source.at(i);
            auto value = copy(*child, depth + 1);
            if (!value)
                return value;
            if (array)
                array->append(*value);
            else {
                auto added = object->addField(*name, *value);
                if (!added)
                    return tl::unexpected(added.error());
            }
        }
        active_.pop_back();
        return array ? ModelNode::Ptr(array) : ModelNode::Ptr(object);
    }

    /** Check subtraction first so adversarial sizes cannot overflow the remaining budget. */
    bool charge(size_t bytes)
    {
        if (bytes > options_.maxBytes - bytes_)
            return false;
        bytes_ += bytes;
        return true;
    }

    /** Uniform materialization diagnostics remain independent of HTTP or filter policy. */
    static auto failure(std::string message) -> tl::unexpected<Error>
    {
        return tl::unexpected(Error{Error::RuntimeError, std::move(message)});
    }

    ModelPool& target_;
    CopyOptions const& options_;
    size_t nodes_ = 0;
    size_t bytes_ = 0;
    std::vector<std::pair<Model const*, uint32_t>> active_;
};

auto ModelPool::copyNode(ModelNode const& source, CopyOptions const& options)
    -> tl::expected<ModelNode::Ptr, Error>
{
    return NodeCopy(*this, options).copy(source);
}

auto ModelPool::copyNode(ModelNode const& source) -> tl::expected<ModelNode::Ptr, Error>
{
    return copyNode(source, CopyOptions{});
}

auto ModelPool::copySequence(std::span<Value const> values, CopyOptions const& options)
    -> tl::expected<model_ptr<Array>, Error>
{
    return NodeCopy(*this, options).sequence(values);
}

auto ModelPool::copySequence(std::span<Value const> values) -> tl::expected<model_ptr<Array>, Error>
{
    return copySequence(values, CopyOptions{});
}

}
