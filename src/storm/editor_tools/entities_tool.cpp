// The window "Entities": every entity of the engine with its class; the one picked opens in
// "Entity Properties" with its layers, its attributes and the fields its class shows itself
// (Entity::ShowEditor). The pick outlives the run by its class, in imgui.ini.

#include "attributes_view.hpp"
#include "selection.hpp"

#include "core.h"
#include "entity.h"
#include "shared/layers.h"
#include "storm/editor/engine_editor.hpp"
#include "storm/editor/storm_imgui.hpp"
#include "storm/layers.hpp"
#include "vma.hpp"

#include <map>
#include <string>
#include <vector>

namespace storm::editor_tools
{

entid_t &SelectedEntity()
{
    static entid_t selected = invalid_entity;
    return selected;
}

namespace
{

using storm::editor::EngineEditor;

constexpr layer_index_t kLayers = RAIN_DROPS + 1;

const char *ClassName(hash_t hash)
{
    for (const auto &c : __STORM_CLASSES_REGISTRY)
    {
        if (c->GetHash() == hash)
        {
            return c->GetName();
        }
    }
    return "";
}

// In a layer or not, and the priority there: a new priority is a new place in the layer, so the
// entity leaves it and comes back
void ShowLayers(entid_t id)
{
    if (!ImGui::BeginTable("layers", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_Borders))
    {
        return;
    }
    for (layer_index_t i = 0; i < kLayers; ++i)
    {
        ImGui::PushID(i);
        const auto priority = core.GetLayerPriority(i, id);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        bool in_layer = priority.has_value();
        if (ImGui::Checkbox(std::string(storm::GetLayerName(i)).c_str(), &in_layer))
        {
            if (in_layer)
            {
                core.AddToLayer(i, id, 0);
            }
            else
            {
                core.RemoveFromLayer(i, id);
            }
        }
        ImGui::TableNextColumn();
        if (priority)
        {
            priority_t value = *priority;
            if (ImGui::InputScalar("##priority", ImGuiDataType_U32, &value, nullptr, nullptr, nullptr,
                                   ImGuiInputTextFlags_EnterReturnsTrue) && value != *priority)
            {
                core.RemoveFromLayer(i, id);
                core.AddToLayer(i, id, value);
            }
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
}

void ShowProperties(Entity &entity)
{
    ImGui::TextFmt("{} {}", ClassName(core.GetClassCode(entity.GetId())), entity.GetId());
    if (ImGui::TreeNode("Layers"))
    {
        ShowLayers(entity.GetId());
        ImGui::TreePop();
    }
    if (entity.AttributesPointer != nullptr && ImGui::TreeNodeEx("Attributes", ImGuiTreeNodeFlags_DefaultOpen))
    {
        // the filter is kept between runs too, it opens the branches it matches
        static ImGuiTextFilter filter(EngineEditor::GetSetting("Entities.Filter").c_str());
        if (filter.Draw("Name"))
        {
            EngineEditor::SetSetting("Entities.Filter", filter.InputBuf);
        }
        ShowAttributes(entity, *entity.AttributesPointer, filter);
        ImGui::TreePop();
    }
    entity.ShowEditor();
}

void ShowEntities(bool &active)
{
    entid_t &selected = SelectedEntity();
    static ImGuiTextFilter class_filter;

    std::vector<Entity *> entities;
    core.ForEachEntity([&](Entity *entity) { entities.push_back(entity); });

    // the pick of the last run: its class and which one of the class it was, in the order of the list
    const std::string selected_class = EngineEditor::GetSetting("Entities.Class");
    if (core.GetEntityPointer(selected) == nullptr && !selected_class.empty())
    {
        const std::string index_setting = EngineEditor::GetSetting("Entities.Index");
        int index = index_setting.empty() ? 0 : std::stoi(index_setting);
        for (Entity *entity : entities)
        {
            if (selected_class == ClassName(core.GetClassCode(entity->GetId())))
            {
                selected = entity->GetId();
                if (index-- == 0)
                {
                    break;
                }
            }
        }
    }

    if (ImGui::Begin("Entities", &active))
    {
        ImGui::TextFmt("There are currently {} entities", entities.size());
        class_filter.Draw("Class");
        if (ImGui::BeginTable("entities", 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Resizable))
        {
            ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("Class", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();
            // the place of an entity among the ones of its class, the pick is kept by it
            std::map<std::string, int> class_index;
            for (Entity *entity : entities)
            {
                const entid_t id = entity->GetId();
                const char *class_name = ClassName(core.GetClassCode(id));
                ++class_index[class_name];
                if (!class_filter.PassFilter(class_name))
                {
                    continue;
                }
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                if (ImGui::Selectable(std::to_string(id).c_str(), id == selected, ImGuiSelectableFlags_SpanAllColumns))
                {
                    selected = id;
                    EngineEditor::SetSetting("Entities.Class", class_name);
                    EngineEditor::SetSetting("Entities.Index", std::to_string(class_index[class_name] - 1));
                }
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(class_name);
            }
            ImGui::EndTable();
        }
    }
    // End goes with every Begin, also when the window is collapsed and Begin is false
    ImGui::End();

    if (ImGui::Begin("Entity Properties"))
    {
        if (Entity *entity = core.GetEntityPointer(selected))
        {
            ShowProperties(*entity);
        }
        else
        {
            ImGui::TextDisabled("Pick an entity in the window Entities");
        }
    }
    ImGui::End();
}

// the module registers its window when it is linked into the engine
[[maybe_unused]] const bool registered = [] {
    EngineEditor::RegisterEditorTool("Entities", ShowEntities);
    return true;
}();

} // namespace
} // namespace storm::editor_tools
