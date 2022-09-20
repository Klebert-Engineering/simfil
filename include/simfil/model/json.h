// Copyright (c) Navigation Data Standard e.V. - See "LICENSE" file.

#pragma once

#if !defined(SIMFIL_WITH_MODEL_JSON)
#error "Define SIMFIL_WITH_MODEL_JSON"
#endif

#include "simfil/model.h"

namespace simfil::json
{

std::unique_ptr<Model> parse(std::istream& input);
std::unique_ptr<Model> parse(const std::string& input);

}
