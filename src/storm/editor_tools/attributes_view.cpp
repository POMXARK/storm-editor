#include "attributes_view.hpp"

#include <fmt/format.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>

namespace storm::editor_tools
{
namespace
{

void ShowValue(Entity &entity, ATTRIBUTES &attr, const char *label)
{
    const std::string &value = attr.GetValue();
    char *end = nullptr;
    float number = std::strtof(value.c_str(), &end);
    const bool numeric = !value.empty() && end != nullptr && *end == '\0';
    const bool integral = numeric && value.find_first_of(".eE") == std::string::npos;
    bool changed = false;
    if (integral && ImStristr(label, nullptr, "color", nullptr) != nullptr)
    {
        // a colour of the scripts is argb() packed into an int
        const auto argb = static_cast<uint32_t>(std::strtoll(value.c_str(), nullptr, 10));
        float rgba[4] = {((argb >> 16) & 0xFF) / 255.0f, ((argb >> 8) & 0xFF) / 255.0f, (argb & 0xFF) / 255.0f,
                         (argb >> 24) / 255.0f};
        if (ImGui::ColorEdit4(label, rgba, ImGuiColorEditFlags_AlphaPreviewHalf))
        {
            const auto channel = [&](int i) { return static_cast<uint32_t>(std::lround(rgba[i] * 255.0f)); };
            const uint32_t packed = channel(3) << 24 | channel(0) << 16 | channel(1) << 8 | channel(2);
            attr.SetValue(std::to_string(static_cast<int32_t>(packed)));
            changed = true;
        }
    }
    else if (numeric)
    {
        const float speed = integral ? 1.0f : std::max(0.001f, std::abs(number) * 0.01f);
        if (ImGui::DragFloat(label, &number, speed, 0.0f, 0.0f, integral ? "%.0f" : "%.3f"))
        {
            attr.SetValue(integral ? std::to_string(std::lround(number)) : fmt::format("{}", number));
            changed = true;
        }
    }
    else
    {
        std::string text = value;
        if (ImGui::InputText(label, &text, ImGuiInputTextFlags_EnterReturnsTrue))
        {
            attr.SetValue(text);
            changed = true;
        }
    }
    if (changed)
    {
        entity.AttributeChanged(&attr);
    }
}

} // namespace

void ShowAttributes(Entity &entity, ATTRIBUTES &attributes, const ImGuiTextFilter &filter)
{
    for (uint32_t i = 0; i < attributes.GetAttributesNum(); ++i)
    {
        ATTRIBUTES *child = attributes.GetAttributeClass(i);
        if (child == nullptr)
        {
            continue;
        }
        ImGui::PushID(child);
        const char *name = child->GetThisName();
        if (child->GetAttributesNum() > 0)
        {
            const ImGuiTreeNodeFlags flags = filter.IsActive() ? ImGuiTreeNodeFlags_DefaultOpen : 0;
            if (ImGui::TreeNodeEx("##node", flags, "%s", name))
            {
                // a branch made by a script assignment to its leaves keeps an empty value
                if (child->HasValue() && !child->GetValue().empty())
                {
                    ShowValue(entity, *child, "value");
                }
                ShowAttributes(entity, *child, filter);
                ImGui::TreePop();
            }
        }
        else if (filter.PassFilter(name))
        {
            if (child->HasValue())
            {
                ShowValue(entity, *child, name);
            }
            else
            {
                ImGui::TextDisabled("%s", name);
            }
        }
        ImGui::PopID();
    }
}

} // namespace storm::editor_tools
