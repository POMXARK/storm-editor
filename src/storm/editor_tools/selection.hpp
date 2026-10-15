#pragma once

#include "entity.h"

namespace storm::editor_tools
{

// The entity picked in the window Entities; the other tools (Gizmo) work on it
entid_t &SelectedEntity();

} // namespace storm::editor_tools
