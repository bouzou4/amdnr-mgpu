// Copyright (c) 2026 the lmxxf_nr plugin authors. MIT licence, see ../../LICENSE.
// Forced include for the proxy's ImGui build: OptiScaler's imconfig.h without FreeType (stb_truetype instead).
#pragma once
#include "imconfig.h"
#undef IMGUI_ENABLE_FREETYPE
