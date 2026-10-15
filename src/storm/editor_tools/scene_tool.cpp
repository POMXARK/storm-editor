// The window "Scene": a scene made in the editor, as in Unity or Godot. Models are put in from the
// window Assets (a .gm dropped on Viewport lands where the mouse points, on a model or the ground) or from
// the list "Add model"; a click in Viewport picks one (in the edit), the Gizmo moves it. The scene is saved
// to resource/scenes/<name>.scene, a script loads it with LoadScene. Ctrl+Z / Ctrl+Y undo and redo every
// change; Play keeps the scene and Stop gives it back as it was. A model may be animated (its skeleton and
// the action it plays); a marker is a named place for the script (the player, an NPC): Add marker puts one,
// Viewport shows it as a ring with its name and the Gizmo moves it.

#include "selection.hpp"

#include "core.h"
#include "dx9render.h"
#include "model.h"
#include "storm/editor/engine_editor.hpp"
#include "storm/editor/storm_imgui.hpp"
#include "storm/scene/scene.hpp"

#include <ImGuizmo.h>
#include <fmt/format.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

namespace storm::editor_tools
{
namespace
{

using storm::editor::EngineEditor;
using namespace storm::scene;
namespace fs = std::filesystem;

constexpr size_t kUndoDepth = 200;
// where a model goes when the mouse points at nothing: this far ahead of the camera, metres
constexpr float kAhead = 8.f;
constexpr float kRayLength = 1000.f;
// a click this near to a marker on the screen picks it, pixels; its ring is this big, metres
constexpr float kMarkerPick = 14.f;
constexpr float kMarkerRadius = 0.4f;

using Objects = std::vector<SceneObject>;

struct Editor
{
    // the scene as it was after the last change, the undo and the redo of the changes
    Objects committed;
    std::vector<Objects> undo;
    std::vector<Objects> redo;
    // as it is in its file, for the * of the title
    Objects saved;
    // as it was at Play, given back at Stop
    std::optional<Objects> play;
    uint32_t lastId = 0;
    bool started = false;
    std::string error;
};

Editor &State()
{
    static Editor editor;
    return editor;
}

bool Same(const Objects &a, const Objects &b)
{
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](const SceneObject &x, const SceneObject &y) {
               return x.id == y.id && x.SameAs(y);
           });
}

// the same in the file: the ids aside
bool SameFile(const Objects &a, const Objects &b)
{
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](const SceneObject &x, const SceneObject &y) { return x.SameAs(y); });
}

// the marker picked: it has no entity, so it is picked while no entity is
uint32_t &SelectedMarkerId()
{
    static uint32_t id = 0;
    return id;
}

SceneObject *Selected()
{
    const entid_t selected = SelectedEntity();
    for (SceneObject &object : CurrentScene().objects)
    {
        if (object.marker ? selected == invalid_entity && object.id == SelectedMarkerId()
                          : object.entity != invalid_entity && object.entity == selected)
        {
            return &object;
        }
    }
    return nullptr;
}

void Select(entid_t entity)
{
    SelectedEntity() = entity;
    SelectedMarkerId() = 0;
}

SceneObject *FindObject(uint32_t id)
{
    Objects &objects = CurrentScene().objects;
    const auto it = std::find_if(objects.begin(), objects.end(), [id](const SceneObject &o) { return o.id == id; });
    return it != objects.end() ? &*it : nullptr;
}

// A marker goes to the window Gizmo as its target: owned by no entity, it shows while none is picked
void SelectMarker(uint32_t id)
{
    Select(invalid_entity);
    SelectedMarkerId() = id;
    storm::editor::GizmoTarget target;
    target.owner = invalid_entity;
    target.name = "marker";
    target.get = [id](float *matrix) {
        const SceneObject *object = SelectedMarkerId() == id ? FindObject(id) : nullptr;
        if (object != nullptr)
        {
            std::copy(object->matrix, object->matrix + 16, matrix);
        }
        return object != nullptr;
    };
    target.set = [id](const float *matrix) {
        if (SceneObject *object = FindObject(id))
        {
            std::copy(matrix, matrix + 16, object->matrix);
        }
    };
    target.script = [id](const float *m) {
        const SceneObject *object = FindObject(id);
        return fmt::format("GetSceneMarker(\"{}\", &place); // {:.3f}, {:.3f}, {:.3f}, ay {:.4f}",
                           object != nullptr ? object->name : "", m[12], m[13], m[14], std::atan2(m[8], m[10]));
    };
    EngineEditor::SetGizmoTarget(std::move(target));
}

// The scene becomes the one given: the models that are the same stay and move, the others are made anew
void Apply(const Objects &target)
{
    Objects &objects = CurrentScene().objects;
    uint32_t selected_id = 0;
    if (const SceneObject *object = Selected())
    {
        selected_id = object->id;
    }
    Objects next = target;
    for (SceneObject &object : next)
    {
        if (object.marker)
        {
            continue;
        }
        const auto it = std::find_if(objects.begin(), objects.end(), [&](const SceneObject &o) { return o.id == object.id; });
        if (it != objects.end() && it->geometry == object.geometry && it->technique == object.technique &&
            it->textures == object.textures && it->animation == object.animation && it->action == object.action &&
            core.GetEntityPointerSafe(it->entity) != nullptr)
        {
            object.entity = it->entity;
            it->entity = invalid_entity;
            WritePlace(object);
        }
        else
        {
            Spawn(object);
        }
    }
    for (SceneObject &object : objects)
    {
        Despawn(object);
    }
    objects = std::move(next);
    for (const SceneObject &object : objects)
    {
        if (object.id == selected_id)
        {
            object.marker ? SelectMarker(object.id) : Select(object.entity);
        }
    }
}

void Reset(const Objects &objects)
{
    Editor &state = State();
    state.committed = objects;
    state.saved = objects;
    state.undo.clear();
    state.redo.clear();
    state.play.reset();
    state.lastId = 0;
    for (const SceneObject &object : objects)
    {
        state.lastId = std::max(state.lastId, object.id);
    }
}

void Undo()
{
    Editor &state = State();
    if (state.undo.empty())
    {
        return;
    }
    state.redo.push_back(state.committed);
    state.committed = std::move(state.undo.back());
    state.undo.pop_back();
    Apply(state.committed);
}

void Redo()
{
    Editor &state = State();
    if (state.redo.empty())
    {
        return;
    }
    state.undo.push_back(state.committed);
    state.committed = std::move(state.redo.back());
    state.redo.pop_back();
    Apply(state.committed);
}

// a change of the scene, whoever made it (this window, the Gizmo, the numbers of the Gizmo), goes to the
// undo once it is done: no button of the mouse held, no field being edited
void Commit()
{
    Editor &state = State();
    const Objects &objects = CurrentScene().objects;
    if (Same(objects, state.committed) || ImGui::IsAnyItemActive() || ImGui::IsMouseDown(ImGuiMouseButton_Left) ||
        ImGuizmo::IsUsing())
    {
        return;
    }
    state.undo.push_back(state.committed);
    if (state.undo.size() > kUndoDepth)
    {
        state.undo.erase(state.undo.begin());
    }
    state.redo.clear();
    state.committed = objects;
}

void OpenScene(const std::string &name)
{
    if (!LoadScene(name))
    {
        State().error = fmt::format("No scene {} (see compile.log)", name);
        return;
    }
    State().error.clear();
    EngineEditor::SetSetting("Scene.File", name);
    Reset(CurrentScene().objects);
}

void NewScene()
{
    ClearScene();
    Select(invalid_entity);
    EngineEditor::SetSetting("Scene.File", "");
    Reset({});
}

void SaveScene(const std::string &name)
{
    Scene &scene = CurrentScene();
    if (!WriteScene(name, scene.objects))
    {
        State().error = "Cannot write " + ScenePath(name).generic_string();
        return;
    }
    State().error.clear();
    scene.name = name;
    State().saved = scene.objects;
    EngineEditor::SetSetting("Scene.File", name);
    core.Trace("EDITOR: scene saved to %s", ScenePath(name).generic_string().c_str());
}

// "resource/models/demo/crate.gm" as MSG_MODEL_LOAD_GEO takes it: "demo\crate"; empty for no model
std::string Geometry(const std::string &asset)
{
    fs::path path(asset);
    if (path.extension() != ".gm")
    {
        return {};
    }
    const fs::path relative = path.lexically_relative(fs::path("resource") / "models");
    if (relative.empty() || *relative.begin() == "..")
    {
        return {};
    }
    std::string geometry = relative.parent_path().generic_string();
    geometry += geometry.empty() ? "" : "/";
    geometry += path.stem().string();
    std::replace(geometry.begin(), geometry.end(), '/', '\\');
    return geometry;
}

// a name not in the scene yet: crate, crate2, crate3...
std::string FreeName(const std::string &base)
{
    const Objects &objects = CurrentScene().objects;
    auto taken = [&](const std::string &name) {
        return std::any_of(objects.begin(), objects.end(), [&](const SceneObject &o) { return o.name == name; });
    };
    std::string name = base;
    for (int i = 2; taken(name); ++i)
    {
        name = fmt::format("{}{}", base, i);
    }
    return name;
}

struct Ray
{
    CVECTOR from;
    CVECTOR direction;
};

// The ray of the camera through a point of the screen in the frame of the game (Viewport)
std::optional<Ray> ScreenRay(float x, float y)
{
    auto *renderer = core.GetServiceX<VDX9RENDER>();
    const storm::editor::ViewportRect rect = EngineEditor::GetViewportRect();
    if (renderer == nullptr || rect.width <= 0.f || rect.height <= 0.f)
    {
        return std::nullopt;
    }
    D3DMATRIX view, projection;
    renderer->GetTransform(D3DTS_VIEW, &view);
    renderer->GetTransform(D3DTS_PROJECTION, &projection);
    const float nx = (x - rect.x) / rect.width * 2.f - 1.f;
    const float ny = 1.f - (y - rect.y) / rect.height * 2.f;
    // in the camera: x right, y up, z ahead; the view matrix turns the world into it, its transpose back
    const float local[3] = {nx / projection._11, ny / projection._22, 1.f};
    const float *m = &view._11;
    float direction[3], from[3];
    for (int j = 0; j < 3; ++j)
    {
        direction[j] = local[0] * m[j * 4] + local[1] * m[j * 4 + 1] + local[2] * m[j * 4 + 2];
        from[j] = -(m[12] * m[j * 4] + m[13] * m[j * 4 + 1] + m[14] * m[j * 4 + 2]);
    }
    CVECTOR d(direction[0], direction[1], direction[2]);
    d = !d;
    return Ray{CVECTOR(from[0], from[1], from[2]), d};
}

// Where the segment from src to dst enters the boxes of the nodes of a model, as a part of its length,
// 2 if it misses. A model made without the BSP tree does not trace, its boxes are hit instead
float TraceBoxes(MODEL *model, const CVECTOR &src, const CVECTOR &dst)
{
    float best = 2.f;
    for (int32_t i = 0; NODE *node = model->GetNode(i); ++i)
    {
        if (node->geo == nullptr)
        {
            continue;
        }
        GEOS::INFO info;
        node->geo->GetInfo(info);
        CVECTOR from, to;
        node->glob_mtx.MulToInv(src, from);
        node->glob_mtx.MulToInv(dst, to);
        const float start[3] = {from.x, from.y, from.z};
        const float delta[3] = {to.x - from.x, to.y - from.y, to.z - from.z};
        const float center[3] = {info.boxcenter.x, info.boxcenter.y, info.boxcenter.z};
        const float half[3] = {info.boxsize.x * 0.5f, info.boxsize.y * 0.5f, info.boxsize.z * 0.5f};
        float enter = 0.f, leave = 1.f;
        for (int k = 0; k < 3 && enter <= leave; ++k)
        {
            const float low = center[k] - half[k] - start[k], high = center[k] + half[k] - start[k];
            if (std::fabs(delta[k]) < 1e-6f)
            {
                if (low > 0.f || high < 0.f)
                {
                    enter = 2.f;
                }
                continue;
            }
            const float a = low / delta[k], b = high / delta[k];
            enter = std::max(enter, std::min(a, b));
            leave = std::min(leave, std::max(a, b));
        }
        if (enter <= leave && enter < best)
        {
            best = enter;
        }
    }
    return best;
}

// The nearest model along the ray and how far it is
std::optional<std::pair<entid_t, float>> Pick(const Ray &ray, entid_t skip = invalid_entity)
{
    const CVECTOR to = ray.from + ray.direction * kRayLength;
    std::optional<std::pair<entid_t, float>> nearest;
    core.ForEachEntity([&](Entity *entity) {
        auto *model = dynamic_cast<MODEL *>(entity);
        if (model == nullptr || entity->GetId() == skip)
        {
            return;
        }
        float hit = model->Trace(ray.from, to);
        if (hit > 1.f)
        {
            hit = TraceBoxes(model, ray.from, to);
        }
        if (hit >= 0.f && hit <= 1.f && (!nearest || hit * kRayLength < nearest->second))
        {
            nearest = std::make_pair(entity->GetId(), hit * kRayLength);
        }
    });
    return nearest;
}

// Where a model dropped at a point of the screen goes: on the model under the mouse, else on the ground
// (y = 0), else a few metres ahead
CVECTOR DropPoint(float x, float y)
{
    const auto ray = ScreenRay(x, y);
    if (!ray)
    {
        return CVECTOR(0.f, 0.f, 0.f);
    }
    if (const auto hit = Pick(*ray))
    {
        return ray->from + ray->direction * hit->second;
    }
    if (ray->direction.y < -1e-3f)
    {
        const float distance = -ray->from.y / ray->direction.y;
        if (distance < 200.f)
        {
            return ray->from + ray->direction * distance;
        }
    }
    return ray->from + ray->direction * kAhead;
}

// The folder of resource/textures a model's textures are in: that of the model when its .gm names them
// without a folder (a character in models/characters, textures/characters) and they lie there
std::string TextureFolder(const std::string &geometry)
{
    const size_t slash = geometry.find_last_of('\\');
    if (slash == std::string::npos)
    {
        return {};
    }
    std::string path = "resource/models/" + geometry + ".gm";
    std::replace(path.begin(), path.end(), '\\', '/');
    std::ifstream in(path, std::ios::binary);
    const std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    // the names of the .gm are zero-ended strings; the first one of a texture tells how they are given
    for (size_t end = data.find(".tga"); end != std::string::npos; end = data.find(".tga", end + 1))
    {
        size_t begin = end;
        while (begin > 0 && data[begin - 1] >= ' ' && data[begin - 1] < 127)
        {
            --begin;
        }
        const std::string name = data.substr(begin, end + 4 - begin);
        if (name.find('\\') != std::string::npos || name.find('/') != std::string::npos)
        {
            return {};
        }
        const std::string folder = geometry.substr(0, slash);
        std::string texture = "resource/textures/" + folder + "/" + name + ".tx";
        std::replace(texture.begin(), texture.end(), '\\', '/');
        return fs::exists(texture) ? folder : std::string();
    }
    return {};
}

void AddModel(const std::string &geometry, const CVECTOR &at)
{
    SceneObject object;
    object.geometry = geometry;
    std::string base = geometry.substr(geometry.find_last_of('\\') + 1);
    object.name = FreeName(base);
    object.technique = EngineEditor::GetSetting("Scene.Technique");
    object.textures = TextureFolder(geometry);
    object.matrix[12] = at.x;
    object.matrix[13] = at.y;
    object.matrix[14] = at.z;
    object.id = ++State().lastId;
    if (!Spawn(object))
    {
        State().error = fmt::format("The model {} is not loaded (see compile.log)", geometry);
        return;
    }
    CurrentScene().objects.push_back(object);
    Select(object.entity);
}

// the middle of the frame, for what is added from a list
CVECTOR ViewCentre()
{
    const storm::editor::ViewportRect rect = EngineEditor::GetViewportRect();
    return DropPoint(rect.x + rect.width / 2.f, rect.y + rect.height / 2.f);
}

void AddMarker(const CVECTOR &at)
{
    SceneObject object;
    object.marker = true;
    object.name = FreeName("marker");
    object.matrix[12] = at.x;
    object.matrix[13] = at.y;
    object.matrix[14] = at.z;
    object.id = ++State().lastId;
    CurrentScene().objects.push_back(object);
    SelectMarker(object.id);
}

// A point of the world on the screen, nullopt behind the camera
std::optional<ImVec2> ToScreen(const float *point)
{
    auto *renderer = core.GetServiceX<VDX9RENDER>();
    const storm::editor::ViewportRect rect = EngineEditor::GetViewportRect();
    if (renderer == nullptr || rect.width <= 0.f)
    {
        return std::nullopt;
    }
    D3DMATRIX view, projection;
    renderer->GetTransform(D3DTS_VIEW, &view);
    renderer->GetTransform(D3DTS_PROJECTION, &projection);
    // D3D rows: the point times the view, then times the projection
    const float *v = &view._11;
    const float *p = &projection._11;
    float eye[4], clip[4];
    for (int j = 0; j < 4; ++j)
    {
        eye[j] = point[0] * v[j] + point[1] * v[4 + j] + point[2] * v[8 + j] + v[12 + j];
    }
    for (int j = 0; j < 4; ++j)
    {
        clip[j] = eye[0] * p[j] + eye[1] * p[4 + j] + eye[2] * p[8 + j] + eye[3] * p[12 + j];
    }
    if (clip[3] < 0.05f)
    {
        return std::nullopt;
    }
    return ImVec2(rect.x + (clip[0] / clip[3] + 1.f) * 0.5f * rect.width,
                  rect.y + (1.f - clip[1] / clip[3]) * 0.5f * rect.height);
}

// The markers over the frame: a ring on the ground of the marker, an arrow where it looks, its name
void DrawMarkers()
{
    ImDrawList *draw = EngineEditor::GetViewportDrawList();
    const storm::editor::ViewportRect rect = EngineEditor::GetViewportRect();
    // as gizmos of Unity: over the edit, not over the game
    if (draw == nullptr || rect.width <= 0.f || EngineEditor::IsPlaying())
    {
        return;
    }
    const SceneObject *selected = Selected();
    draw->PushClipRect(ImVec2(rect.x, rect.y), ImVec2(rect.x + rect.width, rect.y + rect.height), true);
    for (const SceneObject &object : CurrentScene().objects)
    {
        if (!object.marker)
        {
            continue;
        }
        const float *m = object.matrix;
        const ImU32 colour = &object == selected ? IM_COL32(255, 220, 60, 255) : IM_COL32(80, 230, 255, 230);
        constexpr int kSteps = 24;
        ImVec2 ring[kSteps];
        int shown = 0;
        for (int i = 0; i < kSteps; ++i)
        {
            const float a = 6.2831853f * i / kSteps;
            const float point[3] = {m[12] + std::cos(a) * kMarkerRadius, m[13], m[14] + std::sin(a) * kMarkerRadius};
            if (const auto at = ToScreen(point))
            {
                ring[shown++] = *at;
            }
        }
        const float tip[3] = {m[12] + m[8] * kMarkerRadius * 2.f, m[13], m[14] + m[10] * kMarkerRadius * 2.f};
        const float head[3] = {m[12], m[13] + 1.8f, m[14]};
        const auto centre = ToScreen(m + 12);
        const auto front = ToScreen(tip);
        const auto top = ToScreen(head);
        if (!centre || shown < kSteps)
        {
            continue;
        }
        draw->AddPolyline(ring, kSteps, colour, ImDrawFlags_Closed, 2.f);
        if (front)
        {
            draw->AddLine(*centre, *front, colour, 3.f);
        }
        if (top)
        {
            // a post as high as a man, so the place reads as the place of a character
            draw->AddLine(*centre, *top, colour, 1.f);
            draw->AddText(ImVec2(top->x + 4.f, top->y - 14.f), colour, object.name.c_str());
        }
    }
    draw->PopClipRect();
}

// The marker near a point of the screen
std::optional<uint32_t> PickMarker(float x, float y)
{
    std::optional<uint32_t> nearest;
    float best = kMarkerPick;
    for (const SceneObject &object : CurrentScene().objects)
    {
        const auto at = object.marker ? ToScreen(object.matrix + 12) : std::nullopt;
        if (!at)
        {
            continue;
        }
        const float distance = std::hypot(at->x - x, at->y - y);
        if (distance < best)
        {
            best = distance;
            nearest = object.id;
        }
    }
    return nearest;
}

void Duplicate(const SceneObject &source)
{
    SceneObject copy = source;
    copy.entity = invalid_entity;
    copy.id = ++State().lastId;
    copy.name = FreeName(source.name);
    // a step aside, so the copy is seen
    copy.matrix[12] += 0.5f;
    if (copy.marker)
    {
        CurrentScene().objects.push_back(copy);
        SelectMarker(copy.id);
    }
    else if (Spawn(copy))
    {
        CurrentScene().objects.push_back(copy);
        Select(copy.entity);
    }
}

void Delete(uint32_t id)
{
    Objects &objects = CurrentScene().objects;
    const auto it = std::find_if(objects.begin(), objects.end(), [id](const SceneObject &o) { return o.id == id; });
    if (it != objects.end())
    {
        Despawn(*it);
        objects.erase(it);
        Select(invalid_entity);
    }
}

// The actions of resource/animation/<name>.ani: its [sections]
std::vector<std::string> Actions(const std::string &animation)
{
    std::string path = "resource/animation/" + animation + ".ani";
    std::replace(path.begin(), path.end(), '\\', '/');
    std::ifstream in(path);
    std::vector<std::string> actions;
    for (std::string line; std::getline(in, line);)
    {
        const size_t open = line.find_first_not_of(" \t");
        const size_t close = line.find(']');
        if (open != std::string::npos && line[open] == '[' && close != std::string::npos && close > open + 1)
        {
            actions.push_back(line.substr(open + 1, close - open - 1));
        }
    }
    return actions;
}

std::vector<std::string> Files(const fs::path &root, const char *extension)
{
    std::vector<std::string> files;
    std::error_code error;
    for (fs::recursive_directory_iterator it(root, fs::directory_options::follow_directory_symlink, error), end;
         !error && it != end; it.increment(error))
    {
        if (it->is_regular_file(error) && it->path().extension() == extension)
        {
            files.push_back(it->path().generic_string());
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

// the keys of the scene, unless a field takes the keys or the game has them (Viewport in the game)
void Shortcuts()
{
    const ImGuiIO &io = ImGui::GetIO();
    const ImGuiWindow *focused = ImGui::GetCurrentContext()->NavWindow;
    if (io.WantTextInput || (EngineEditor::IsPlaying() && focused != nullptr && std::string_view(focused->Name) == "Viewport"))
    {
        return;
    }
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false))
    {
        io.KeyShift ? Redo() : Undo();
    }
    else if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, false))
    {
        Redo();
    }
    else if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false) && !CurrentScene().name.empty())
    {
        SaveScene(CurrentScene().name);
    }
    else if (SceneObject *object = Selected())
    {
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D, false))
        {
            Duplicate(*object);
        }
        else if (ImGui::IsKeyPressed(ImGuiKey_Delete, false))
        {
            Delete(object->id);
        }
    }
}

// what came from Viewport: a model dropped from Assets, a click that picks (in the edit; in the game the
// click gives it the input)
void ViewportInput()
{
    std::string asset;
    float x, y;
    if (EngineEditor::TakeViewportDrop(asset, x, y))
    {
        const std::string geometry = Geometry(asset);
        if (geometry.empty())
        {
            State().error = fmt::format("{} is no model of resource/models", asset);
        }
        else
        {
            AddModel(geometry, DropPoint(x, y));
        }
    }
    if (EngineEditor::TakeViewportClick(x, y) && !EngineEditor::IsPlaying() && !ImGuizmo::IsOver() && !ImGuizmo::IsUsing())
    {
        if (const auto marker = PickMarker(x, y))
        {
            SelectMarker(*marker);
            return;
        }
        const auto ray = ScreenRay(x, y);
        const auto hit = ray ? Pick(*ray) : std::nullopt;
        Select(hit ? hit->first : invalid_entity);
    }
}

void FileButtons()
{
    Scene &scene = CurrentScene();
    Editor &state = State();
    if (ImGui::Button("New"))
    {
        NewScene();
    }
    ImGui::SameLine();
    if (ImGui::Button("Open"))
    {
        ImGui::OpenPopup("open");
    }
    if (ImGui::BeginPopup("open"))
    {
        const auto files = Files(fs::path("resource") / "scenes", ".scene");
        if (files.empty())
        {
            ImGui::TextDisabled("No scenes in resource/scenes");
        }
        for (const std::string &file : files)
        {
            const std::string name = fs::path(file).stem().string();
            if (ImGui::Selectable(name.c_str(), name == scene.name))
            {
                OpenScene(name);
            }
        }
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(scene.name.empty());
    if (ImGui::Button("Save"))
    {
        SaveScene(scene.name);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    static char save_as[64] = "";
    if (ImGui::Button("Save as"))
    {
        std::snprintf(save_as, sizeof(save_as), "%s", scene.name.empty() ? "scene" : scene.name.c_str());
        ImGui::OpenPopup("save as");
    }
    if (ImGui::BeginPopup("save as"))
    {
        ImGui::TextUnformatted("resource/scenes/");
        ImGui::SameLine(0.f, 0.f);
        ImGui::SetNextItemWidth(140.f);
        if (ImGui::IsWindowAppearing())
        {
            ImGui::SetKeyboardFocusHere();
        }
        const bool enter = ImGui::InputText("##name", save_as, sizeof(save_as), ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine(0.f, 0.f);
        ImGui::TextUnformatted(".scene");
        if ((enter || ImGui::Button("Save")) && save_as[0] != '\0')
        {
            SaveScene(save_as);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    const bool dirty = !SameFile(scene.objects, state.saved);
    ImGui::TextFmt("{}{}", scene.name.empty() ? "(new scene)" : scene.name + ".scene", dirty ? " *" : "");

    ImGui::BeginDisabled(state.undo.empty());
    if (ImGui::Button("Undo"))
    {
        Undo();
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Ctrl+Z, %zu", state.undo.size());
    ImGui::SameLine();
    ImGui::BeginDisabled(state.redo.empty());
    if (ImGui::Button("Redo"))
    {
        Redo();
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Ctrl+Y or Ctrl+Shift+Z, %zu", state.redo.size());
    ImGui::SameLine();
    if (ImGui::Button("Add model"))
    {
        ImGui::OpenPopup("add");
    }
    ImGui::SetItemTooltip("Or drag a .gm from Assets onto Viewport");
    if (ImGui::BeginPopup("add"))
    {
        static ImGuiTextFilter filter;
        filter.Draw("##filter", 200.f);
        const auto files = Files(fs::path("resource") / "models", ".gm");
        if (files.empty())
        {
            ImGui::TextDisabled("No models in resource/models");
        }
        for (const std::string &file : files)
        {
            const std::string geometry = Geometry(file);
            if (filter.PassFilter(geometry.c_str()) && ImGui::Selectable(geometry.c_str()))
            {
                AddModel(geometry, ViewCentre());
            }
        }
        ImGui::EndPopup();
    }
    // on a line of its own: next to the others it hides behind the edge of a narrow panel
    if (ImGui::Button("Add marker"))
    {
        AddMarker(ViewCentre());
    }
    ImGui::SetItemTooltip("A named place for the script: the player, an NPC (GetSceneMarker)");
}

void ObjectList()
{
    Objects &objects = CurrentScene().objects;
    const SceneObject *selected = Selected();
    std::optional<uint32_t> remove;
    std::optional<SceneObject> duplicate;
    if (ImGui::BeginChild("objects", ImVec2(0.f, ImGui::GetContentRegionAvail().y * 0.55f), ImGuiChildFlags_Border))
    {
        if (objects.empty())
        {
            ImGui::TextDisabled("The scene is empty: drag a model (.gm) from Assets onto Viewport");
        }
        for (SceneObject &object : objects)
        {
            ImGui::PushID(static_cast<int>(object.id));
            const bool missing = !object.marker && core.GetEntityPointerSafe(object.entity) == nullptr;
            if (ImGui::Selectable(object.name.c_str(), &object == selected))
            {
                object.marker ? SelectMarker(object.id) : Select(object.entity);
            }
            if (ImGui::BeginPopupContextItem())
            {
                if (ImGui::MenuItem("Duplicate", "Ctrl+D"))
                {
                    duplicate = object;
                }
                if (ImGui::MenuItem("Delete", "Del"))
                {
                    remove = object.id;
                }
                ImGui::EndPopup();
            }
            ImGui::SameLine();
            if (object.marker)
            {
                ImGui::TextColored(ImVec4(0.3f, 0.9f, 1.f, 1.f), "marker");
            }
            else
            {
                ImGui::TextDisabled("%s%s%s%s", object.geometry.c_str(), object.action.empty() ? "" : "  > ",
                                    object.action.c_str(), missing ? "  (not loaded)" : "");
            }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
    if (duplicate)
    {
        Duplicate(*duplicate);
    }
    if (remove)
    {
        Delete(*remove);
    }
}

void ObjectProperties()
{
    SceneObject *object = Selected();
    if (object == nullptr)
    {
        ImGui::TextDisabled("Pick a model or a marker in the list or with a click in Viewport (in the edit)");
        return;
    }
    char name[64];
    std::snprintf(name, sizeof(name), "%s", object->name.c_str());
    if (ImGui::InputText("Name", name, sizeof(name)))
    {
        object->name = name;
    }
    if (object->marker)
    {
        ImGui::TextDisabled("A place for the script, it puts the player or an NPC here:");
        ImGui::TextDisabled("GetSceneMarker(\"%s\", &place) gives place.x, .y, .z, .ay", object->name.c_str());
        ImGui::TextDisabled("Move and turn it with the window Gizmo; the arrow is where it looks");
        return;
    }
    ImGui::LabelText("Geometry", "%s", object->geometry.c_str());
    // the technique, the skeleton and the action are set as the model is made, so it is made anew
    auto remake = [object](std::string SceneObject::*field, const char *value) {
        SceneObject changed = *object;
        changed.*field = value;
        Despawn(*object);
        *object = changed;
        Spawn(*object);
        Select(object->entity);
    };
    char technique[64];
    std::snprintf(technique, sizeof(technique), "%s", object->technique.c_str());
    if (ImGui::InputText("Technique", technique, sizeof(technique), ImGuiInputTextFlags_EnterReturnsTrue))
    {
        remake(&SceneObject::technique, technique);
    }
    ImGui::SetItemTooltip("The shader technique of the model (resource/techniques), Enter to set; empty for its own");
    char textures[64];
    std::snprintf(textures, sizeof(textures), "%s", object->textures.c_str());
    if (ImGui::InputText("Textures", textures, sizeof(textures), ImGuiInputTextFlags_EnterReturnsTrue))
    {
        remake(&SceneObject::textures, textures);
    }
    ImGui::SetItemTooltip("The folder of resource/textures its texture names are in (characters), Enter to set;\n"
                          "empty when the names carry their folder. Set by itself as the model is added");
    char animation[64];
    std::snprintf(animation, sizeof(animation), "%s", object->animation.c_str());
    if (ImGui::InputText("Animation", animation, sizeof(animation), ImGuiInputTextFlags_EnterReturnsTrue))
    {
        remake(&SceneObject::animation, animation);
    }
    ImGui::SetItemTooltip("The skeleton of a skinned model: resource/animation/<name>.ani, Enter to set; empty for none");
    if (!object->animation.empty())
    {
        const auto actions = Actions(object->animation);
        if (ImGui::BeginCombo("Action", object->action.empty() ? "(none)" : object->action.c_str()))
        {
            for (const std::string &action : actions)
            {
                if (ImGui::Selectable(action.c_str(), action == object->action))
                {
                    remake(&SceneObject::action, action.c_str());
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SetItemTooltip("What it plays from the start (an NPC standing by: idle); the game may play another");
    }
    ImGui::TextDisabled("Move and turn it with the window Gizmo");
}

void ShowScene(bool &active)
{
    Editor &state = State();
    Scene &scene = CurrentScene();
    if (!state.started)
    {
        state.started = true;
        // Play keeps the scene, Stop gives it back
        EngineEditor::RegisterPlayCallback([](bool playing) {
            Editor &state = State();
            if (playing)
            {
                state.play = CurrentScene().objects;
            }
            else if (state.play)
            {
                Apply(*state.play);
                state.committed = *state.play;
                state.play.reset();
            }
        });
        // the scene of the last run, unless the game has loaded one already
        const std::string last = EngineEditor::GetSetting("Scene.File");
        if (scene.name.empty() && scene.objects.empty() && !last.empty())
        {
            OpenScene(last);
        }
        // a scene loaded by the game comes with its ids too (LoadScene numbers its objects)
        Reset(scene.objects);
    }

    // the places the Gizmo has given the models
    for (SceneObject &object : scene.objects)
    {
        ReadPlace(object);
    }
    ViewportInput();
    Shortcuts();
    Commit();
    DrawMarkers();

    if (ImGui::Begin("Scene", &active))
    {
        FileButtons();
        if (!state.error.empty())
        {
            ImGui::TextColored(ImVec4(1.f, 0.45f, 0.45f, 1.f), "%s", state.error.c_str());
        }
        ObjectList();
        ObjectProperties();
    }
    ImGui::End();
}

// the module registers its window when it is linked into the engine
[[maybe_unused]] const bool registered = [] {
    EngineEditor::RegisterEditorTool("Scene", ShowScene);
    return true;
}();

} // namespace
} // namespace storm::editor_tools
