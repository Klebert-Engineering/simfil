// Copyright (c) Navigation Data Standard e.V. - See "LICENSE" file.

#pragma once

#if !defined(SIMFIL_WITH_MODEL_JSON)
#error "Define SIMFIL_WITH_MODEL_JSON"
#endif

#include "model.h"
#include "simfil/error.h"

#include <nlohmann/json_fwd.hpp>
#include <tl/expected.hpp>

namespace simfil::json
{

/**
 * Append a subtree using tagged `_bytes`, `_multimap` and `_bitmask` encodings.
 * Bitmasks require a resolvable named schema; integers must fit int64_t. Field
 * names and string values are interned. The result is not automatically a root,
 * and an error may leave unreachable nodes or dictionary entries in the pool.
 */
auto buildModelNode(const nlohmann::json& input, ModelPool& model) -> tl::expected<ModelNode::Ptr, Error>;

/**
 * Parse JSON, append its root and validate model references. JSON syntax errors
 * throw nlohmann::json exceptions; model conversion/validation errors are returned.
 */
auto parse(std::istream& input, ModelPoolPtr const& model) -> tl::expected<void, Error>;

/** Parse a JSON string and append the resulting root node to `model`. */
auto parse(const std::string& input, ModelPoolPtr const& model) -> tl::expected<void, Error>;

/** Parse a JSON string into a freshly created ModelPool containing one root. */
auto parse(const std::string& input) -> tl::expected<ModelPoolPtr, Error>;

}
