// Copyright (c) Navigation Data Standard e.V. - See "LICENSE" file.

#pragma once

#if !defined(SIMFIL_WITH_MODEL_JSON)
#error "Define SIMFIL_WITH_MODEL_JSON"
#endif

#include "model.h"
#include "simfil/error.h"

#include <tl/expected.hpp>

namespace simfil::json
{

auto parse(std::istream& input, ModelPoolPtr const& model) -> tl::expected<void, Error>;
auto parse(const std::string& input, ModelPoolPtr const& model) -> tl::expected<void, Error>;
auto parse(const std::string& input) -> tl::expected<ModelPoolPtr, Error>;

}
