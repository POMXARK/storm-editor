// The window "Gizmo": moves and turns the model picked in the window Entities, or the target its
// module gave (a locator of a location), with a gizmo in the scene (ImGuizmo) and by numbers, and
// copies its place as a line of script. The engine draws a model by its matrix every frame
// (MODEL::mtx), so the model moves at once; the script may place it again though, as a spinning
// one is placed every frame.

#include "selection.hpp"

#include "core.h"
#include "dx9render.h"
#include "model.h"
#include "storm/editor/engine_editor.hpp"
#include "storm/editor/storm_imgui.hpp"

#include <ImGuizmo.h>
#include <fmt/format.h>

#include <algorithm>
#include <optional>
#include <string>

namespace storm::editor_tools
{
namespace
{

using storm::editor::EngineEditor;
using storm::editor::GizmoTarget;

// MSG_MODEL_SET_POSITION: the position, then the axes x, y and z, as model.c of the demos sends it
std::string ScriptLine(const float *m)
{
    return fmt::format("SendMessage(&model, \"lffffffffffff\", MSG_MODEL_SET_POSITION, "
                       "{:.3f}, {:.3f}, {:.3f}, {:.4f}, {:.4f}, {:.4f}, {:.4f}, {:.4f}, {:.4f}, {:.4f}, {:.4f}, {:.4f});",
                       m[12], m[13], m[14], m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10]);
}

// What the gizmo moves this frame: the target a module gave for the picked entity (a locator of a
// location) or the picked model itself
std::optional<GizmoTarget> Target(entid_t selected)
{
    const GizmoTarget *given = EngineEditor::GetGizmoTarget();
    float matrix[16];
    if (given != nullptr && given->owner == selected && given->get(matrix))
    {
        return *given;
    }
    if (dynamic_cast<MODEL *>(core.GetEntityPointer(selected)) == nullptr)
    {
        return std::nullopt;
    }
    auto model = [selected] { return dynamic_cast<MODEL *>(core.GetEntityPointer(selected)); };
    GizmoTarget target;
    target.owner = selected;
    target.name = "model";
    target.get = [model](float *matrix) {
        MODEL *m = model();
        if (m != nullptr)
        {
            std::copy(m->mtx.matrix, m->mtx.matrix + 16, matrix);
        }
        return m != nullptr;
    };
    target.set = [model](const float *matrix) {
        if (MODEL *m = model())
        {
            std::copy(matrix, matrix + 16, m->mtx.matrix);
        }
    };
    target.script = ScriptLine;
    return target;
}

ImGuizmo::OPERATION Operation(const std::string &name)
{
    return name == "rotate" ? ImGuizmo::ROTATE : ImGuizmo::TRANSLATE;
}

void ShowGizmo(bool &active)
{
    // the mode, the space and the snap are kept between runs, in imgui.ini
    std::string operation = EngineEditor::GetSetting("Gizmo.Operation");
    const bool world = EngineEditor::GetSetting("Gizmo.Space") == "world";
    const std::string snap_setting = EngineEditor::GetSetting("Gizmo.Snap");
    float snap = snap_setting.empty() ? 0.f : std::stof(snap_setting);

    const auto target = Target(SelectedEntity());
    float matrix[16];
    const bool has_matrix = target && target->get(matrix);
    auto *renderer = core.GetServiceX<VDX9RENDER>();

    if (ImGui::Begin("Gizmo", &active))
    {
        if (ImGui::RadioButton("Move", Operation(operation) == ImGuizmo::TRANSLATE))
        {
            EngineEditor::SetSetting("Gizmo.Operation", "translate");
        }
        ImGui::SameLine();
        if (ImGui::RadioButton("Rotate", Operation(operation) == ImGuizmo::ROTATE))
        {
            EngineEditor::SetSetting("Gizmo.Operation", "rotate");
        }
        ImGui::SameLine();
        bool world_space = world;
        if (ImGui::Checkbox("World axes", &world_space))
        {
            EngineEditor::SetSetting("Gizmo.Space", world_space ? "world" : "local");
        }
        // metres for a move, degrees for a turn; 0 is no snap
        if (ImGui::DragFloat("Snap", &snap, 0.05f, 0.f, 90.f, "%.2f"))
        {
            EngineEditor::SetSetting("Gizmo.Snap", fmt::format("{:.2f}", snap));
        }

        if (!has_matrix)
        {
            ImGui::TextDisabled("Pick a model (MODELR) in the window Entities,");
            ImGui::TextDisabled("or a locator of a location in Entity Properties");
        }
        else
        {
            ImGui::TextUnformatted(target->name.c_str());
            float position[3], rotation[3], scale[3];
            ImGuizmo::DecomposeMatrixToComponents(matrix, position, rotation, scale);
            bool changed = ImGui::DragFloat3("Position", position, 0.01f, 0.f, 0.f, "%.3f");
            changed |= ImGui::DragFloat3("Rotation", rotation, 0.5f, 0.f, 0.f, "%.1f");
            if (changed)
            {
                ImGuizmo::RecomposeMatrixFromComponents(position, rotation, scale, matrix);
                target->set(matrix);
            }
            if (target->script && ImGui::Button("Copy as script"))
            {
                const std::string line = target->script(matrix);
                ImGui::SetClipboardText(line.c_str());
                core.Trace("EDITOR: %s", line.c_str());
            }
            ImGui::SetItemTooltip("The place as a line of script or of a generator, also into compile.log");
        }
    }
    ImGui::End();

    if (!has_matrix || renderer == nullptr)
    {
        return;
    }
    // the gizmo is drawn over the frame of the game (the window Viewport, or the whole screen under the
    // windows), with the matrices of the camera
    D3DMATRIX view, projection;
    renderer->GetTransform(D3DTS_VIEW, &view);
    renderer->GetTransform(D3DTS_PROJECTION, &projection);
    const storm::editor::ViewportRect rect = EngineEditor::GetViewportRect();
    ImGuizmo::BeginFrame();
    ImGuizmo::SetOrthographic(false);
    ImDrawList *draw = EngineEditor::GetViewportDrawList();
    ImGuizmo::SetDrawlist(draw);
    ImGuizmo::SetRect(rect.x, rect.y, rect.width, rect.height);
    // a model out of the frame keeps its gizmo out of sight too
    draw->PushClipRect(ImVec2(rect.x, rect.y), ImVec2(rect.x + rect.width, rect.y + rect.height), true);
    const float snap_values[3] = {snap, snap, snap};
    if (ImGuizmo::Manipulate(&view._11, &projection._11, Operation(operation), world ? ImGuizmo::WORLD : ImGuizmo::LOCAL,
                             matrix, nullptr, snap > 0.f ? snap_values : nullptr))
    {
        target->set(matrix);
    }
    draw->PopClipRect();
}

// the module registers its window when it is linked into the engine
[[maybe_unused]] const bool registered = [] {
    EngineEditor::RegisterEditorTool("Gizmo", ShowGizmo);
    return true;
}();

} // namespace
} // namespace storm::editor_tools
