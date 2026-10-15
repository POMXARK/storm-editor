#pragma once

#include "attributes.h"
#include "entity.h"

#include <imgui.h>

namespace storm::editor_tools
{

// The attributes scripts gave an entity, as a tree to edit live: a number drags, a colour (argb in an
// int, "color" in the name) has a picker, text is set on Enter. An edit reaches the entity the way a
// script assignment does, through AttributeChanged. The filter keeps the leaves with a matching
// name and opens their branches.
void ShowAttributes(Entity &entity, ATTRIBUTES &attributes, const ImGuiTextFilter &filter);

} // namespace storm::editor_tools
