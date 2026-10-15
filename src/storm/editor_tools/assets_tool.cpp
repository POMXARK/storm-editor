// The window "Assets": the files of the game the engine loads, the folders of resource/ and the scripts
// of the program folder, as a tree with the kind and the size of every file. A text one (.ini, .ani, .c)
// shows on the right and is edited there, Ctrl+S saves it; a saved script is compiled again at once
// (Core::ReloadScripts, globals and event handlers are kept), and its errors open the file on their line.
// The picked file is kept between runs. A file is dragged onto Viewport by its path.
// A glTF model (.glb, .gltf, from Blender and others) is imported: the button Import runs the converter
// tools/gltf2gm, put next to the engine by the build, and the new .gm is picked to be dragged.

#include "core.h"
#include "file_service.h"
#include "storm/editor/engine_editor.hpp"
#include "storm/editor/storm_imgui.hpp"

#include <fmt/format.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <regex>
#include <string>
#include <thread>
#include <vector>

namespace storm::editor_tools
{
namespace
{

using storm::editor::EngineEditor;
namespace fs = std::filesystem;

// the engine runs from the folder of the game
const std::vector<std::string> kRoots = {"resource", "program", "source"};
// a bigger text file is not shown
constexpr std::uintmax_t kTextLimit = 256 * 1024;
// the scripts are named in errors from here
const std::string kProgram = "program";

const std::map<std::string, const char *> kKinds = {
    {".gm", "model"},        {".an", "skeleton"},   {".ani", "animation"}, {".tx", "texture"},
    {".tga", "texture"},     {".dds", "texture"},   {".wav", "sound"},     {".ogg", "sound"},
    {".mp3", "sound"},       {".ini", "settings"},  {".c", "script"},      {".h", "script"},
    {".fx", "shader"},       {".ttf", "font"},      {".txt", "text"},      {".col", "collision"},
    {".glb", "glTF"},        {".gltf", "glTF"},
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

// the import of a glTF file, run by the converter in its own thread so the frame goes on
struct Import
{
    std::mutex mutex;
    bool running = false;
    bool done = false;
    std::string file;
    std::string output;
    std::string model;
};

Import import_;

#ifdef _WIN32
#define STORM_POPEN _popen
#define STORM_PCLOSE _pclose
constexpr const char *kPython = "python";
#else
#define STORM_POPEN popen
#define STORM_PCLOSE pclose
constexpr const char *kPython = "python3";
#endif

void RunImport(std::string file, std::string folder)
{
    const fs::path tool = fs::path(fio->_GetExecutableDirectory()) / "tools" / "gltf2gm" / "gltf2gm.py";
    const std::string command = fmt::format("{} -I \"{}\" \"{}\" --out resource --folder \"{}\" 2>&1", kPython,
                                            tool.string(), file, folder);
    std::string output;
    int status = -1;
    if (FILE *pipe = STORM_POPEN(command.c_str(), "r"))
    {
        char buffer[512];
        while (fgets(buffer, sizeof(buffer), pipe))
        {
            output += buffer;
        }
        status = STORM_PCLOSE(pipe);
    }
    if (!fs::exists(tool))
    {
        output = fmt::format("no converter {}: build the engine target to copy it", tool.string());
    }
    const fs::path model = fs::path("resource") / "models" / folder / (fs::path(file).stem().string() + ".gm");
    std::lock_guard lock(import_.mutex);
    import_.running = false;
    import_.done = true;
    import_.output = output;
    import_.model = status == 0 && fs::exists(model) ? model.generic_string() : std::string();
    core.Trace("EDITOR: import %s: %s", file.c_str(), import_.model.empty() ? "failed" : import_.model.c_str());
}

void ShowImport(const std::string &selected)
{
    static char folder[64] = "";
    if (!folder[0])
    {
        const std::string saved = EngineEditor::GetSetting("Assets.ImportFolder");
        fmt::format_to_n(folder, sizeof(folder) - 1, "{}", saved.empty() ? "imported" : saved);
    }
    ImGui::Separator();
    ImGui::SetNextItemWidth(140.f);
    if (ImGui::InputText("Folder", folder, sizeof(folder)))
    {
        EngineEditor::SetSetting("Assets.ImportFolder", folder);
    }
    std::lock_guard lock(import_.mutex);
    ImGui::BeginDisabled(import_.running || !folder[0]);
    if (ImGui::Button(import_.running ? "Importing..." : "Import"))
    {
        import_.running = true;
        import_.done = false;
        import_.file = selected;
        std::thread(RunImport, selected, std::string(folder)).detach();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("to resource/models/%s, textures/%s", folder, folder);
    if (import_.file == selected && !import_.output.empty())
    {
        ImGui::TextWrapped("%s", import_.output.c_str());
    }
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
        // a file is dragged out by its path: a model onto Viewport goes into the scene (window Scene)
        if (ImGui::BeginDragDropSource())
        {
            ImGui::SetDragDropPayload("STORM_ASSET", path.c_str(), path.size() + 1);
            ImGui::Text("%s", name.c_str());
            ImGui::EndDragDropSource();
        }
        ImGui::TableNextColumn();
        ImGui::TextDisabled("%s", Kind(child.path));
        ImGui::TableNextColumn();
        ImGui::TextDisabled("%s", Size(child.size).c_str());
    }
}

// a text file as edited, kept until saved when another file is picked
struct Document
{
    std::string text;
    bool dirty = false;
    // line to select once the editor gets the focus, from 1
    int go_to = 0;
    int line = 1;
};
std::map<std::string, Document> documents_;

// the last reload of the scripts
struct Reload
{
    bool done = false;
    std::vector<std::string> errors;
} reload_;

void Load(const std::string &file, Document &document)
{
    std::ifstream in(file, std::ios::binary);
    document.text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

Document &Open(const std::string &file)
{
    auto [it, opened] = documents_.try_emplace(file);
    if (opened)
    {
        Load(file, it->second);
    }
    return it->second;
}

// the files not edited are read again: they may have been changed outside
void Refresh()
{
    for (auto &[file, document] : documents_)
    {
        if (!document.dirty)
        {
            Load(file, document);
        }
    }
}

bool IsScript(const fs::path &path)
{
    return std::string(Kind(path)) == "script";
}

void ReloadScripts()
{
    reload_.done = true;
    reload_.errors.clear();
    core.ReloadScripts(reload_.errors);
    Refresh();
}

void Save(const std::string &file, Document &document)
{
    std::ofstream(file, std::ios::binary) << document.text;
    document.dirty = false;
    if (IsScript(file))
    {
        ReloadScripts();
    }
}

// "COMPILE ERROR in model.c(12): ..." is shown as "model.c(12): ...", a click opens program/model.c on line 12
void ShowErrors()
{
    static const std::regex source(R"(^.* in (.+)\((\d+)\): (.*)$)");
    if (!reload_.done)
    {
        return;
    }
    if (reload_.errors.empty())
    {
        ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.f), "Scripts reloaded");
        return;
    }
    ImGui::TextColored(ImVec4(1.f, 0.45f, 0.4f, 1.f), "Not reloaded, the old code runs:");
    // a wrong expression is told about at every level of it: the same line is listed once
    std::vector<std::string> shown;
    for (const std::string &error : reload_.errors)
    {
        if (std::find(shown.begin(), shown.end(), error) == shown.end())
        {
            shown.push_back(error);
        }
    }
    const float height = ImGui::GetTextLineHeightWithSpacing() * static_cast<float>(std::min<size_t>(shown.size(), 4));
    if (ImGui::BeginChild("errors", ImVec2(0.f, height)))
    {
        for (size_t n = 0; n < shown.size(); n++)
        {
            std::smatch match;
            const bool linked = std::regex_search(shown[n], match, source);
            const std::string text = linked ? fmt::format("{}({}): {}", match[1].str(), match[2].str(), match[3].str())
                                            : shown[n];
            ImGui::PushID(static_cast<int>(n));
            if (ImGui::Selectable(text.c_str()) && linked)
            {
                std::string file = match[1].str();
                std::replace(file.begin(), file.end(), '\\', '/');
                file = kProgram + "/" + file;
                if (fs::is_regular_file(file))
                {
                    EngineEditor::SetSetting("Assets.File", file);
                    Open(file).go_to = std::stoi(match[2].str());
                }
            }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
}

// keeps the line of the cursor and puts the cursor on the line asked, selected
int EditCallback(ImGuiInputTextCallbackData *data)
{
    auto &document = *static_cast<Document *>(data->UserData);
    const char *text = data->Buf;
    if (document.go_to > 0)
    {
        int start = 0;
        for (int line = 1; line < document.go_to && start < data->BufTextLen; start++)
        {
            line += text[start] == '\n';
        }
        // the code of the line, without its indent
        while (start < data->BufTextLen && (text[start] == ' ' || text[start] == '\t'))
        {
            start++;
        }
        int end = start;
        while (end < data->BufTextLen && text[end] != '\n')
        {
            end++;
        }
        data->SelectionStart = start;
        data->CursorPos = data->SelectionEnd = end;
        document.go_to = 0;
    }
    document.line = 1 + static_cast<int>(std::count(text, text + data->CursorPos, '\n'));
    return 0;
}

void ShowFile(const std::string &selected)
{
    if (selected.empty())
    {
        ImGui::TextDisabled("Pick a file");
        ShowErrors();
        return;
    }
    std::error_code error;
    const fs::path path(selected);
    const auto size = fs::file_size(path, error);
    ImGui::TextUnformatted(path.filename().string().c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("%s, %s", Kind(path), Size(size).c_str());
    ImGui::TextDisabled("%s", selected.c_str());
    if (ImGui::SmallButton("Copy path"))
    {
        ImGui::SetClipboardText(selected.c_str());
    }
    {
        // the model just imported tells what the converter made of it
        std::lock_guard lock(import_.mutex);
        if (!import_.model.empty() && import_.model == selected)
        {
            ImGui::TextWrapped("Imported from %s, drag it onto Viewport:\n%s", import_.file.c_str(),
                               import_.output.c_str());
        }
    }
    if (std::string(Kind(path)) == "glTF")
    {
        ShowImport(selected);
        return;
    }
    const bool script = IsScript(path);
    if (script)
    {
        ImGui::SameLine();
        if (ImGui::SmallButton("Reload scripts"))
        {
            ReloadScripts();
        }
    }
    ShowErrors();
    if (!IsText(path) || size > kTextLimit)
    {
        return;
    }
    Document &document = Open(selected);
    ImGui::Separator();
    if (ImGui::Button(script ? "Save and reload" : "Save"))
    {
        Save(selected, document);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("line %d%s", document.line, document.dirty ? ", modified" : "");
    if (document.go_to > 0)
    {
        ImGui::SetKeyboardFocusHere();
    }
    constexpr ImGuiInputTextFlags flags = ImGuiInputTextFlags_AllowTabInput | ImGuiInputTextFlags_CallbackAlways;
    if (ImGui::InputTextMultiline("##text", &document.text, ImGui::GetContentRegionAvail(), flags, EditCallback,
                                  &document))
    {
        document.dirty = true;
    }
    if (ImGui::IsItemActive() && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false))
    {
        Save(selected, document);
    }
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
    // a finished import shows its model: the tree is read again and the new .gm picked
    bool imported = false;
    {
        std::lock_guard lock(import_.mutex);
        if (import_.done)
        {
            import_.done = false;
            imported = true;
            if (!import_.model.empty())
            {
                EngineEditor::SetSetting("Assets.File", import_.model);
            }
        }
    }
    if (roots.empty() || ImGui::Button("Rescan") || imported)
    {
        roots.clear();
        Refresh();
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

    // the border between the tree and the file is dragged
    if (ImGui::BeginChild("tree", ImVec2(ImGui::GetContentRegionAvail().x * 0.4f, 0.f),
                          ImGuiChildFlags_Border | ImGuiChildFlags_ResizeX))
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
