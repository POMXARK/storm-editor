// The window "Animation": the animation of the model picked in the window Entities, or of the model of a
// picked character. Its actions (the .ani file) lie on the frames of the .an, with their events and
// data; the players show what plays now and are played, paused and moved by hand; and the actions the
// model goes through make a graph, an arrow for every change seen while the window is open.

#include "selection.hpp"

#include "animation.h"
#include "core.h"
#include "model.h"
#include "storm/editor/engine_editor.hpp"
#include "storm/editor/storm_imgui.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace storm::editor_tools
{
namespace
{

using storm::editor::EngineEditor;

const ImU32 kBar = IM_COL32(80, 120, 170, 255);
const ImU32 kBarPlaying = IM_COL32(90, 200, 110, 255);
const ImU32 kEvent = IM_COL32(255, 210, 80, 255);
const ImU32 kHead = IM_COL32(255, 255, 255, 255);
const ImU32 kNode = IM_COL32(60, 90, 130, 255);
const ImU32 kNodeHere = IM_COL32(70, 160, 90, 255);
const ImU32 kEdge = IM_COL32(200, 200, 200, 150);
const ImU32 kText = IM_COL32(235, 235, 235, 255);

constexpr const char *kTypes[] = {"static", "normal", "reverse", "pingpong", "rpingpong"};

// the changes of the action of player 0 seen on one model
struct Graph
{
    entid_t model{};
    std::string last;
    // the actions in the order they were first seen: the places of the nodes
    std::vector<std::string> nodes;
    std::map<std::pair<std::string, std::string>, int> edges;

    void Reset(entid_t id)
    {
        *this = Graph();
        model = id;
    }

    void Add(const std::string &node)
    {
        if (std::find(nodes.begin(), nodes.end(), node) == nodes.end())
        {
            nodes.push_back(node);
        }
    }

    void See(const std::string &action)
    {
        if (action.empty() || action == last)
        {
            return;
        }
        Add(action);
        if (!last.empty())
        {
            ++edges[{last, action}];
        }
        last = action;
    }
};

MODEL *PickedModel(entid_t selected)
{
    if (auto *model = dynamic_cast<MODEL *>(core.GetEntityPointer(selected)))
    {
        return model;
    }
    const uint64_t of = EngineEditor::ModelOf(selected);
    return of != 0 ? dynamic_cast<MODEL *>(core.GetEntityPointer(of)) : nullptr;
}

// plays an action on a player from its start, as a script would
void Play(ActionPlayer &player, const std::string &action)
{
    if (player.SetAction(action.c_str()))
    {
        player.Play();
    }
}

void ShowPlayers(Animation &animation, const std::vector<AnimationActionDesc> &actions)
{
    constexpr ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp;
    if (!ImGui::BeginTable("players", 5, flags))
    {
        return;
    }
    ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 18.f);
    ImGui::TableSetupColumn("action");
    ImGui::TableSetupColumn("position");
    ImGui::TableSetupColumn("speed");
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 90.f);
    ImGui::TableHeadersRow();
    for (int32_t i = 0; i < ANI_MAX_ACTIONS; ++i)
    {
        ActionPlayer &player = animation.Player(i);
        const char *current = player.GetAction();
        // the unused players: no action, only the first one is always shown
        if (i > 0 && (current == nullptr || current[0] == '\0'))
        {
            continue;
        }
        ImGui::PushID(i);
        ImGui::TableNextColumn();
        ImGui::Text("%d", i);
        ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo("##action", current != nullptr ? current : ""))
        {
            for (const auto &action : actions)
            {
                if (ImGui::Selectable(action.name.c_str(), current != nullptr && action.name == current))
                {
                    Play(player, action.name);
                }
            }
            ImGui::EndCombo();
        }
        ImGui::TableNextColumn();
        float position = player.GetPosition();
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::SliderFloat("##position", &position, 0.f, 1.f, fmt::format("{:.2f}, blend {:.2f}", position, player.GetBlend()).c_str()))
        {
            // the hand holds the action where it is put
            player.Pause();
            player.SetPosition(position);
        }
        ImGui::TableNextColumn();
        float speed = player.GetSpeed();
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::DragFloat("##speed", &speed, 0.01f, 0.f, 10.f, "%.2f"))
        {
            player.SetSpeed(speed);
        }
        ImGui::TableNextColumn();
        if (ImGui::SmallButton(player.IsPlaying() && !player.IsPause() ? "Pause" : "Play"))
        {
            if (player.IsPlaying() && !player.IsPause())
            {
                player.Pause();
            }
            else
            {
                player.Play();
            }
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Stop"))
        {
            player.Stop();
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
    ImGui::TextDisabled("A character sets player 0 itself, by its state: it takes its own action back");
}

// every action a row: its frames on the frames of the whole .an, its events, the play head
void ShowActions(Animation &animation, const std::vector<AnimationActionDesc> &actions, const std::string &filter)
{
    const char *playing = animation.Player(0).GetAction();
    const float position = animation.Player(0).GetPosition();
    const float frames = static_cast<float>(std::max(animation.GetNumFrames(), 1));
    constexpr ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                      ImGuiTableFlags_Resizable;
    if (!ImGui::BeginTable("actions", 4, flags, ImVec2(0.f, 200.f)))
    {
        return;
    }
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("action", ImGuiTableColumnFlags_WidthFixed, 130.f);
    ImGui::TableSetupColumn("frames", ImGuiTableColumnFlags_WidthFixed, 70.f);
    ImGui::TableSetupColumn("on the .an", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("type, events, data", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    for (const auto &action : actions)
    {
        if (!filter.empty() && action.name.find(filter) == std::string::npos)
        {
            continue;
        }
        const bool here = playing != nullptr && action.name == playing;
        ImGui::PushID(action.name.c_str());
        ImGui::TableNextColumn();
        if (ImGui::Selectable(action.name.c_str(), here, ImGuiSelectableFlags_SpanAllColumns))
        {
            Play(animation.Player(0), action.name);
        }
        ImGui::SetItemTooltip("Plays it on player 0");
        ImGui::TableNextColumn();
        ImGui::Text("%d-%d", action.startFrame, action.endFrame);
        ImGui::TableNextColumn();
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x;
        const float height = ImGui::GetTextLineHeight();
        ImDrawList *draw = ImGui::GetWindowDrawList();
        const float x0 = origin.x + width * action.startFrame / frames;
        const float x1 = std::max(origin.x + width * action.endFrame / frames, x0 + 2.f);
        draw->AddRectFilled(ImVec2(x0, origin.y + 2.f), ImVec2(x1, origin.y + height - 2.f), here ? kBarPlaying : kBar);
        for (const auto &[name, time] : action.events)
        {
            const float x = x0 + (x1 - x0) * time;
            draw->AddLine(ImVec2(x, origin.y), ImVec2(x, origin.y + height), kEvent, 2.f);
        }
        if (here)
        {
            const float x = x0 + (x1 - x0) * position;
            draw->AddLine(ImVec2(x, origin.y - 1.f), ImVec2(x, origin.y + height + 1.f), kHead, 2.f);
        }
        ImGui::Dummy(ImVec2(width, height));
        ImGui::TableNextColumn();
        std::string info = kTypes[std::clamp(static_cast<int>(action.type), 0, 4)];
        info += action.loop ? ", loop" : "";
        if (action.rate != 1.f)
        {
            info += fmt::format(", rate {:.2f}", action.rate);
        }
        for (const auto &[name, time] : action.events)
        {
            info += fmt::format(", \"{}\" {:.0f}%", name, time * 100.f);
        }
        for (const auto &[name, value] : action.data)
        {
            info += fmt::format(", {} = {}", name, value);
        }
        ImGui::TextUnformatted(info.c_str());
        ImGui::PopID();
    }
    ImGui::EndTable();
}

// the actions seen as nodes on a circle, the changes between them as arrows
void ShowGraph(Graph &graph, const std::vector<AnimationActionDesc> &actions)
{
    if (graph.nodes.empty())
    {
        ImGui::TextDisabled("No action played yet");
        return;
    }
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    // an ellipse on the width of the window, the height of what is left of it within limits
    const float side = std::clamp(avail.y - ImGui::GetTextLineHeightWithSpacing(), 180.f, 360.f);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(avail.x, side));
    const ImVec2 centre(origin.x + avail.x / 2.f, origin.y + side / 2.f);
    const ImVec2 radius(std::max(avail.x / 2.f - 70.f, 40.f), side / 2.f - 16.f);
    std::map<std::string, ImVec2> places;
    const auto count = static_cast<float>(graph.nodes.size());
    for (size_t i = 0; i < graph.nodes.size(); ++i)
    {
        const float angle = 6.2831853f * static_cast<float>(i) / count - 1.5707963f;
        places[graph.nodes[i]] = ImVec2(centre.x + radius.x * std::cos(angle), centre.y + radius.y * std::sin(angle));
    }
    ImDrawList *draw = ImGui::GetWindowDrawList();
    for (const auto &[edge, times] : graph.edges)
    {
        const ImVec2 a = places[edge.first];
        const ImVec2 b = places[edge.second];
        const float dx = b.x - a.x, dy = b.y - a.y;
        const float length = std::max(std::sqrt(dx * dx + dy * dy), 1.f);
        const ImVec2 dir(dx / length, dy / length);
        // a pair both ways is two arrows side by side
        const ImVec2 side_shift(-dir.y * 4.f, dir.x * 4.f);
        const ImVec2 from(a.x + dir.x * 22.f + side_shift.x, a.y + dir.y * 22.f + side_shift.y);
        const ImVec2 to(b.x - dir.x * 22.f + side_shift.x, b.y - dir.y * 22.f + side_shift.y);
        const float thickness = std::min(1.f + static_cast<float>(times) * 0.5f, 4.f);
        draw->AddLine(from, to, kEdge, thickness);
        draw->AddTriangleFilled(to, ImVec2(to.x - dir.x * 9.f - dir.y * 5.f, to.y - dir.y * 9.f + dir.x * 5.f),
                                ImVec2(to.x - dir.x * 9.f + dir.y * 5.f, to.y - dir.y * 9.f - dir.x * 5.f), kEdge);
        if (times > 1)
        {
            draw->AddText(ImVec2((from.x + to.x) / 2.f, (from.y + to.y) / 2.f), kText, fmt::format("x{}", times).c_str());
        }
    }
    for (const auto &node : graph.nodes)
    {
        const ImVec2 at = places[node];
        const ImVec2 size = ImGui::CalcTextSize(node.c_str());
        const bool loops = std::any_of(actions.begin(), actions.end(),
                                       [&](const AnimationActionDesc &a) { return a.name == node && a.loop; });
        draw->AddRectFilled(ImVec2(at.x - size.x / 2.f - 6.f, at.y - size.y / 2.f - 3.f),
                            ImVec2(at.x + size.x / 2.f + 6.f, at.y + size.y / 2.f + 3.f),
                            node == graph.last ? kNodeHere : kNode, loops ? 8.f : 0.f);
        draw->AddText(ImVec2(at.x - size.x / 2.f, at.y - size.y / 2.f), kText, node.c_str());
    }
    ImGui::TextDisabled("Rounded: a looped action. An arrow: a change of player 0 seen while the window is open");
}

void ShowAnimation(bool &active)
{
    static Graph graph;
    const entid_t selected = SelectedEntity();
    MODEL *model = PickedModel(selected);
    Animation *animation = model != nullptr ? model->GetAnimation() : nullptr;
    if (animation != nullptr)
    {
        if (graph.model != model->GetId())
        {
            graph.Reset(model->GetId());
        }
        const char *playing = animation->Player(0).GetAction();
        graph.See(playing != nullptr ? playing : "");
    }

    if (!ImGui::Begin("Animation", &active))
    {
        ImGui::End();
        return;
    }
    if (animation == nullptr)
    {
        ImGui::TextDisabled("Pick an animated model or a character in the window Entities");
        ImGui::End();
        return;
    }
    const std::vector<AnimationActionDesc> actions = animation->GetActions();
    ImGui::Text("%s.ani: %zu actions, %d frames at %.0f fps", animation->GetName(), actions.size(),
                animation->GetNumFrames(), animation->GetFPS());

    if (ImGui::CollapsingHeader("Players", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ShowPlayers(*animation, actions);
    }
    if (ImGui::CollapsingHeader("Actions", ImGuiTreeNodeFlags_DefaultOpen))
    {
        std::string filter = EngineEditor::GetSetting("Animation.Filter");
        char buffer[64] = {};
        filter.copy(buffer, sizeof(buffer) - 1);
        ImGui::SetNextItemWidth(160.f);
        if (ImGui::InputText("Name", buffer, sizeof(buffer)))
        {
            EngineEditor::SetSetting("Animation.Filter", buffer);
        }
        ShowActions(*animation, actions, buffer);
    }
    if (ImGui::CollapsingHeader("Graph", ImGuiTreeNodeFlags_DefaultOpen))
    {
        if (ImGui::SmallButton("Clear"))
        {
            graph.Reset(model->GetId());
        }
        ShowGraph(graph, actions);
    }
    ImGui::End();
}

// the module registers its window when it is linked into the engine
[[maybe_unused]] const bool registered = [] {
    EngineEditor::RegisterEditorTool("Animation", ShowAnimation);
    return true;
}();

} // namespace
} // namespace storm::editor_tools
