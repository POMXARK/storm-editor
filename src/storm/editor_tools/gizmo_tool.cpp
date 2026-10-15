// The window "Gizmo": moves and turns the model picked in the window Entities with a gizmo in the
// scene (ImGuizmo) and by numbers, and copies its place as a line of script. The engine draws a
// model by its matrix every frame (MODEL::mtx), so the model moves at once; the script may place it
// again though, as a spinning one is placed every frame.

#include "selection.hpp"

#include "core.h"
#include "dx9render.h"
#include "model.h"
#include "storm/editor/engine_editor.hpp"
#include "storm/editor/storm_imgui.hpp"

#include <ImGuizmo.h>
#include <fmt/format.h>

#include <string>

namespace storm::editor_tools
{
namespace
{

using storm::editor::EngineEditor;

// MSG_MODEL_SET_POSITION: the position, then the axes x, y and z, as model.c of the demos sends it
std::string ScriptLine(const CMatrix &m)
{
    return fmt::format("SendMessage(&model, \"lffffffffffff\", MSG_MODEL_SET_POSITION, "
                       "{:.3f}, {:.3f}, {:.3f}, {:.4f}, {:.4f}, {:.4f}, {:.4f}, {:.4f}, {:.4f}, {:.4f}, {:.4f}, {:.4f});",
                       m.m[3][0], m.m[3][1], m.m[3][2], m.m[0][0], m.m[0][1], m.m[0][2], m.m[1][0], m.m[1][1],
                       m.m[1][2], m.m[2][0], m.m[2][1], m.m[2][2]);
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

    auto *model = dynamic_cast<MODEL *>(core.GetEntityPointer(SelectedEntity()));
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

        if (model == nullptr)
        {
            ImGui::TextDisabled("Pick a model (MODELR) in the window Entities");
        }
        else
        {
            CMatrix &m = model->mtx;
            float position[3], rotation[3], scale[3];
            ImGuizmo::DecomposeMatrixToComponents(m.matrix, position, rotation, scale);
            bool changed = ImGui::DragFloat3("Position", position, 0.01f, 0.f, 0.f, "%.3f");
            changed |= ImGui::DragFloat3("Rotation", rotation, 0.5f, 0.f, 0.f, "%.1f");
            if (changed)
            {
                ImGuizmo::RecomposeMatrixFromComponents(position, rotation, scale, m.matrix);
            }
            if (ImGui::Button("Copy as script"))
            {
                const std::string line = ScriptLine(m);
                ImGui::SetClipboardText(line.c_str());
                core.Trace("EDITOR: %s", line.c_str());
            }
            ImGui::SetItemTooltip("MSG_MODEL_SET_POSITION with this place, also into compile.log");
        }
    }
    ImGui::End();

    if (model == nullptr || renderer == nullptr)
    {
        return;
    }
    // the gizmo is drawn under the windows, over the whole frame, with the matrices of the camera
    D3DMATRIX view, projection;
    renderer->GetTransform(D3DTS_VIEW, &view);
    renderer->GetTransform(D3DTS_PROJECTION, &projection);
    const ImGuiIO &io = ImGui::GetIO();
    ImGuizmo::BeginFrame();
    ImGuizmo::SetOrthographic(false);
    ImGuizmo::SetDrawlist(ImGui::GetBackgroundDrawList());
    ImGuizmo::SetRect(0, 0, io.DisplaySize.x, io.DisplaySize.y);
    const float snap_values[3] = {snap, snap, snap};
    ImGuizmo::Manipulate(&view._11, &projection._11, Operation(operation), world ? ImGuizmo::WORLD : ImGuizmo::LOCAL,
                         model->mtx.matrix, nullptr, snap > 0.f ? snap_values : nullptr);
}

// the module registers its window when it is linked into the engine
[[maybe_unused]] const bool registered = [] {
    EngineEditor::RegisterEditorTool("Gizmo", ShowGizmo);
    return true;
}();

} // namespace
} // namespace storm::editor_tools
