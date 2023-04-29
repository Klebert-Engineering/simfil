// Copyright (c) Navigation Data Standard e.V. - See "LICENSE" file.

#pragma once

#if !defined(SIMFIL_WITH_MODEL_JSON)
#error "Define SIMFIL_WITH_MODEL_JSON"
#endif

#include "model.h"

namespace simfil::json
{

void parse(std::istream& input, ModelPoolPtr const& model);
void parse(const std::string& input, ModelPoolPtr const& model);
ModelPoolPtr parse(const std::string& input);

}
