// Copyright (c) Navigation Data Standard e.V. - See "LICENSE" file.

#pragma once

#if !defined(SIMFIL_WITH_MODEL_JSON)
#error "Define SIMFIL_WITH_MODEL_JSON"
#endif

#include "simfil/model.h"

namespace simfil::json
{

void parse(std::istream& input, ModelPtr const& model);
void parse(const std::string& input, ModelPtr const& model);
ModelPtr parse(const std::string& input);

}
