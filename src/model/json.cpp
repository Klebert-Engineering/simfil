// Copyright (c) Navigation Data Standard e.V. - See "LICENSE" file.

#include "simfil/model/json.h"
#include "simfil/model/model.h"

#include <cstdint>
#include <limits>
#include <string>

#include <nlohmann/json.hpp>

#include "../expected.h"

namespace simfil::json
{
using json = nlohmann::json;

namespace
{
/** Resolve the canonical simfil null node instance. */
auto nullNode(ModelPool& model) -> ModelNode::Ptr
{
    return model.resolve<ModelNode>(
        ModelNodeAddress{Model::Null, 1},
        ScalarValueType{});
}

/** Decode one object field through the shared recursive JSON builder. */
auto buildObjectField(
    model_ptr<Object>& object,
    std::string const& key,
    const json& value,
    ModelPool& model) -> tl::expected<void, Error>
{
    auto child = buildModelNode(value, model);
    TRY_EXPECTED(child);
    auto result = object->addField(key, *child);
    TRY_EXPECTED(result);
    return {};
}

/** Expand `_multimap`-tagged objects into repeated simfil object fields. */
auto buildMultimapObject(
    const json& input,
    model_ptr<Object>& object,
    ModelPool& model) -> tl::expected<void, Error>
{
    for (auto&& [key, value] : input.items()) {
        if (key == "_multimap") {
            continue;
        }

        if (!value.is_array()) {
            return tl::unexpected<Error>(
                Error::ParserError,
                "Invalid multimap object: expected array values for every field");
        }

        for (const auto& item : value) {
            auto result = buildObjectField(object, key, item, model);
            TRY_EXPECTED(result);
        }
    }

    return {};
}
}

/** Build a simfil node tree from JSON while honoring simfil's tagged encodings. */
auto buildModelNode(const json& input, ModelPool& model) -> tl::expected<ModelNode::Ptr, Error>
{
    switch (input.type()) {
    case json::value_t::null:
        return nullNode(model);
    case json::value_t::boolean:
        return model.newSmallValue(input.get<bool>());
    case json::value_t::number_float:
        return model.tryNewValue(input.get<double>());
    case json::value_t::number_unsigned:
        if (input.get<uint64_t>() > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
            // simfil stores JSON integers as signed int64_t, so reject lossy unsigned values.
            return tl::unexpected<Error>(
                Error::ParserError,
                "Unsigned integer does not fit into simfil's signed JSON number storage");
        }
        return model.tryNewValue(static_cast<int64_t>(input.get<uint64_t>()));
    case json::value_t::number_integer:
        return model.tryNewValue(input.get<int64_t>());
    case json::value_t::string:
        {
            // JSON strings participate in pooled-string facilities such as
            // string-literal completion.
            auto stringId = model.strings()->emplace(input.get<std::string>());
            if (!stringId) {
                return tl::unexpected<Error>(stringId.error());
            }
            return model.tryNewValue(static_cast<StringId>(*stringId));
        }
    default:
        break;
    }

    if (input.is_object()) {
        if (input.size() == 4 && input.contains("_bitmask") && input["_bitmask"] == true &&
            input.contains("bits") && input.contains("schema") && input.contains("domain")) {
            // Only the complete reserved shape is a tag; application objects with extra keys remain objects.
            if (!input["bits"].is_string() || !input["schema"].is_string() || !input["domain"].is_number_integer() ||
                input["domain"] < 1 || input["domain"] > std::numeric_limits<SchemaId>::max())
                return tl::unexpected(Error{Error::ParserError, "Invalid tagged bitmask fields"});
            auto bits = Bitmask::parseBits(input["bits"].get_ref<const std::string&>());
            TRY_EXPECTED(bits);
            auto binding = model.schemaBinding(input["schema"].get_ref<const std::string&>());
            if (!binding)
                return tl::unexpected(Error{Error::ParserError, "Tagged bitmask requires its exact schema graph"});
            try {
                return model.newBitmask(*bits, {std::move(binding), input["domain"].get<SchemaId>()});
            }
            catch (std::exception const& error) {
                return tl::unexpected(Error{Error::ParserError, error.what()});
            }
        }
        if (auto it = input.find("_bytes"); it != input.end() && it->is_boolean() && it->get<bool>()) {
            // `_bytes` is the tagged JSON form emitted by ModelNode::toJson() for byte arrays.
            auto hex = input.find("hex");
            if (hex == input.end() || !hex->is_string()) {
                return tl::unexpected<Error>(
                    Error::ParserError,
                    "Invalid tagged bytes object: expected string field 'hex'");
            }

            auto decoded = ByteArray::fromHex(hex->get<std::string>());
            if (!decoded) {
                return tl::unexpected<Error>(
                    Error::ParserError,
                    "Invalid tagged bytes object: hex decode failed");
            }

            return model.tryNewValue(std::move(*decoded));
        }

        auto multimap = input.find("_multimap");
        bool const isMultimap = multimap != input.end() && multimap->is_boolean() && multimap->get<bool>();
        size_t capacity = isMultimap ? 0 : input.size();
        if (isMultimap) {
            for (auto const& [key, values] : input.items()) {
                if (key == "_multimap") continue;
                if (!values.is_array())
                    return tl::unexpected<Error>(Error::ParserError,
                        "Invalid multimap object: expected array values for every field");
                if (values.size() > std::numeric_limits<size_t>::max() - capacity)
                    return tl::unexpected<Error>(Error::IndexOutOfRange,
                        "Multimap capacity exceeds model storage representation");
                capacity += values.size();
            }
        }
        auto created = model.tryNewObject(capacity, true);
        TRY_EXPECTED(created);
        auto object = *created;
        if (isMultimap) {
            // Repeated object keys are reconstructed only for explicitly tagged multimaps.
            auto result = buildMultimapObject(input, object, model);
            TRY_EXPECTED(result);
            return object;
        }

        for (auto&& [key, value] : input.items()) {
            auto result = buildObjectField(object, key, value, model);
            TRY_EXPECTED(result);
        }
        return object;
    }

    if (input.is_array()) {
        auto created = model.tryNewArray(input.size(), true);
        TRY_EXPECTED(created);
        auto array = *created;
        for (const auto& value : input) {
            auto child = buildModelNode(value, model);
            TRY_EXPECTED(child);
            array->append(*child);
        }
        return array;
    }

    return tl::unexpected<Error>(
        Error::ParserError,
        "Unsupported JSON value type");
}

/** Parse JSON from a stream into an existing model pool. */
auto parse(std::istream& input, ModelPoolPtr const& model) -> tl::expected<void, Error>
{
    auto root = buildModelNode(json::parse(input), *model);
    if (!root)
        return tl::unexpected<Error>(root.error());
    model->addRoot(*root);
    return model->validate();
}

/** Parse a JSON string into an existing model pool. */
auto parse(const std::string& input, ModelPoolPtr const& model) -> tl::expected<void, Error>
{
    auto root = buildModelNode(json::parse(input), *model);
    if (!root)
        return tl::unexpected<Error>(root.error());
    model->addRoot(*root);
    return model->validate();
}

/** Parse a JSON string into a new model pool with one validated root. */
auto parse(const std::string& input) -> tl::expected<ModelPoolPtr, Error>
{
    auto model = std::make_shared<simfil::ModelPool>();
    auto root = buildModelNode(json::parse(input), *model);
    if (!root)
        return tl::unexpected<Error>(root.error());
    model->addRoot(*root);
    if (auto res = model->validate(); !res)
        return tl::unexpected<Error>(res.error());
    return model;
}

}
