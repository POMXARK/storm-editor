// The window "Assets": the files of the game the engine loads, the folders of resource/ and the scripts
// of the program folder, as a tree with the kind and the size of every file. A text one (.ini, .ani, .c)
// shows on the right; the picked file is kept between runs.

#include "core.h"
#include "storm/editor/engine_editor.hpp"
#include "storm/editor/storm_imgui.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace storm::editor_tools
{
namespace
{

using storm::editor::EngineEditor;
namespace fs = std::filesystem;

// the engine runs from the folder of the game
const std::vector<std::string> kRoots = {"resource", "program"};
// a text shown is cut here
constexpr std::uintmax_t kTextLimit = 256 * 1024;

const std::map<std::string, const char *> kKinds = {
    {".gm", "model"},        {".an", "skeleton"},   {".ani", "animation"}, {".tx", "texture"},
    {".tga", "texture"},     {".dds", "texture"},   {".wav", "sound"},     {".ogg", "sound"},
    {".mp3", "sound"},       {".ini", "settings"},  {".c", "script"},      {".h", "script"},
    {".fx", "shader"},       {".ttf", "font"},      {".txt", "text"},      {".col", "collision"},
};

const char *Kind(const fs::path &path)
{
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
    const auto it = kKinds.find(ext);
    return it != kKinds.end() ? it->second : "";
}

bool IsText(const fs::path &path)
{
    const std::string kind = Kind(path);
    return kind == "settings" || kind == "animation" || kind == "script" || kind == "text" || kind == "shader";
}

std::string Size(std::uintmax_t bytes)
{
    if (bytes < 1024)
    {
        return fmt::format("{} B", bytes);
    }
    if (bytes < 1024 * 1024)
    {
        return fmt::format("{:.1f} KB", bytes / 1024.0);
    }
    return fmt::format("{:.1f} MB", bytes / (1024.0 * 1024.0));
}

struct Entry
{
    fs::path path;
    bool folder = false;
    std::uintmax_t size = 0;
    std::vector<Entry> children;
};

// a folder read once, as a tree; folders first, then by name. Links (techniques) are followed.
Entry Read(const fs::path &path)
{
    Entry entry{path, true};
    std::error_code error;
    for (const auto &item : fs::directory_iterator(path, fs::directory_options::skip_permission_denied, error))
    {
        if (item.is_directory(error))
        {
            entry.children.push_back(Read(item.path()));
        }
        else
        {
            entry.children.push_back({item.path(), false, item.file_size(error)});
        }
    }
    std::sort(entry.children.begin(), entry.children.end(), [](const Entry &a, const Entry &b) {
        return a.folder != b.folder ? a.folder : a.path.filename() < b.path.filename();
    });
    for (const Entry &child : entry.children)
    {
        entry.size += child.size;
    }
    return entry;
}

bool Matches(const Entry &entry, const ImGuiTextFilter &filter)
{
    if (!entry.folder)
    {
        return filter.PassFilter(entry.path.filename().string().c_str());
    }
    return std::any_of(entry.children.begin(), entry.children.end(),
                       [&](const Entry &child) { return Matches(child, filter); });
}

void ShowTree(const Entry &entry, const ImGuiTextFilter &filter, const std::string &selected)
{
    for (const Entry &child : entry.children)
    {
        if (filter.IsActive() && !Matches(child, filter))
        {
            continue;
        }
        const std::string name = child.path.filename().string();
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        if (child.folder)
        {
            const ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanFullWidth |
                                             (filter.IsActive() ? ImGuiTreeNodeFlags_DefaultOpen : 0);
            const bool open = ImGui::TreeNodeEx(name.c_str(), flags);
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%zu", child.children.size());
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", Size(child.size).c_str());
            if (open)
            {
                ShowTree(child, filter, selected);
                ImGui::TreePop();
            }
            continue;
        }
        const std::string path = child.path.generic_string();
        ImGui::TreeNodeEx(name.c_str(), ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                            ImGuiTreeNodeFlags_SpanFullWidth |
                                            (path == selected ? ImGuiTreeNodeFlags_Selected : 0));
        if (ImGui::IsItemClicked())
        {
            EngineEditor::SetSetting("Assets.File", path);
        }
        ImGui::TableNextColumn();
        ImGui::TextDisabled("%s", Kind(child.path));
        ImGui::TableNextColumn();
        ImGui::TextDisabled("%s", Size(child.size).c_str());
    }
}

void ShowFile(const std::string &selected)
{
    static std::string shown;
    static std::string text;
    if (selected.empty())
    {
        ImGui::TextDisabled("Pick a file");
        return;
    }
    std::error_code error;
    const fs::path path(selected);
    ImGui::TextUnformatted(path.filename().string().c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("%s, %s", Kind(path), Size(fs::file_size(path, error)).c_str());
    ImGui::TextDisabled("%s", selected.c_str());
    if (ImGui::SmallButton("Copy path"))
    {
        ImGui::SetClipboardText(selected.c_str());
    }
    if (!IsText(path))
    {
        return;
    }
    if (shown != selected)
    {
        shown = selected;
        std::ifstream in(path, std::ios::binary);
        text.assign(kTextLimit, '\0');
        in.read(text.data(), static_cast<std::streamsize>(text.size()));
        text.resize(static_cast<size_t>(in.gcount()));
    }
    ImGui::Separator();
    if (ImGui::BeginChild("text", ImVec2(0.f, 0.f), false, ImGuiWindowFlags_HorizontalScrollbar))
    {
        ImGui::TextUnformatted(text.data(), text.data() + text.size());
    }
    ImGui::EndChild();
}

void ShowAssets(bool &active)
{
    static std::vector<Entry> roots;
    static ImGuiTextFilter filter;
    if (!ImGui::Begin("Assets", &active))
    {
        ImGui::End();
        return;
    }
    if (roots.empty() || ImGui::Button("Rescan"))
    {
        roots.clear();
        for (const std::string &root : kRoots)
        {
            if (fs::is_directory(root))
            {
                roots.push_back(Read(root));
            }
        }
    }
    ImGui::SameLine();
    filter.Draw("Name", 180.f);
    const std::string selected = EngineEditor::GetSetting("Assets.File");

    if (ImGui::BeginChild("tree", ImVec2(ImGui::GetContentRegionAvail().x * 0.55f, 0.f), ImGuiChildFlags_Border))
    {
        constexpr ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY;
        if (ImGui::BeginTable("files", 3, flags))
        {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("kind", ImGuiTableColumnFlags_WidthFixed, 70.f);
            ImGui::TableSetupColumn("size", ImGuiTableColumnFlags_WidthFixed, 70.f);
            ImGui::TableHeadersRow();
            for (const Entry &root : roots)
            {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                const bool open = ImGui::TreeNodeEx(root.path.string().c_str(),
                                                    ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanFullWidth);
                ImGui::TableNextColumn();
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", Size(root.size).c_str());
                if (open)
                {
                    ShowTree(root, filter, selected);
                    ImGui::TreePop();
                }
            }
            ImGui::EndTable();
        }
    }
    ImGui::EndChild();
    ImGui::SameLine();
    if (ImGui::BeginChild("file"))
    {
        ShowFile(selected);
    }
    ImGui::EndChild();
    ImGui::End();
}

// the module registers its window when it is linked into the engine
[[maybe_unused]] const bool registered = [] {
    EngineEditor::RegisterEditorTool("Assets", ShowAssets);
    return true;
}();

} // namespace
} // namespace storm::editor_tools
