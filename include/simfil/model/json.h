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
 * Build a ModelNode subtree from JSON using simfil's tagged JSON conventions
 * such as `_bytes` and `_multimap`.
 */
auto buildModelNode(const nlohmann::json& input, ModelPool& model) -> tl::expected<ModelNode::Ptr, Error>;

/** Parse JSON from a stream and append the resulting root node to `model`. */
auto parse(std::istream& input, ModelPoolPtr const& model) -> tl::expected<void, Error>;

/** Parse a JSON string and append the resulting root node to `model`. */
auto parse(const std::string& input, ModelPoolPtr const& model) -> tl::expected<void, Error>;

/** Parse a JSON string into a freshly created ModelPool containing one root. */
auto parse(const std::string& input) -> tl::expected<ModelPoolPtr, Error>;

}
