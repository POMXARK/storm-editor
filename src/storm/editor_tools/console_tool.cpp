// The window "Console": the logs of the engine as they grow, a tab a log (system, errors, the scripts:
// their compilation and Trace), with a filter and the errors in red. The files are read from where the
// engine writes them; the window reads only what was added since its last look.

#include "core.h"
#include "storm/editor/engine_editor.hpp"
#include "storm/editor/storm_imgui.hpp"
#include "storm/engine_settings.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <string>

namespace storm::editor_tools
{
namespace
{

using storm::editor::EngineEditor;

// the lines kept of a log, the older ones go
constexpr size_t kKeep = 5000;
const ImVec4 kError(1.f, 0.45f, 0.45f, 1.f);
const ImVec4 kWarning(1.f, 0.8f, 0.4f, 1.f);

struct Log
{
    const char *title;
    const char *file;
    std::deque<std::string> lines;
    std::uintmax_t read = 0;
    // the lines before it are hidden by Clear; the file stays
    size_t cleared = 0;
    std::string rest;

    void Update()
    {
        const auto path = GetEngineSettings().GetEnginePath(EngineSettingsPathType::Logs) / file;
        std::error_code error;
        const auto size = std::filesystem::file_size(path, error);
        if (error)
        {
            return;
        }
        if (size < read)
        {
            // the log was begun anew (the engine started again)
            lines.clear();
            read = 0;
            cleared = 0;
            rest.clear();
        }
        if (size == read)
        {
            return;
        }
        std::ifstream in(path, std::ios::binary);
        in.seekg(static_cast<std::streamoff>(read));
        std::string chunk(size - read, '\0');
        in.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        chunk.resize(static_cast<size_t>(in.gcount()));
        read += chunk.size();
        rest += chunk;
        size_t start = 0;
        for (size_t end; (end = rest.find('\n', start)) != std::string::npos; start = end + 1)
        {
            std::string line = rest.substr(start, end - start);
            if (!line.empty() && line.back() == '\r')
            {
                line.pop_back();
            }
            lines.push_back(std::move(line));
        }
        rest.erase(0, start);
        while (lines.size() > kKeep)
        {
            lines.pop_front();
            cleared = cleared > 0 ? cleared - 1 : 0;
        }
    }

    size_t Shown() const
    {
        return lines.size() - std::min(cleared, lines.size());
    }
};

bool Contains(const std::string &line, const char *word)
{
    const auto it = std::search(line.begin(), line.end(), word, word + std::strlen(word),
                                [](char a, char b) { return std::tolower(a) == std::tolower(b); });
    return it != line.end();
}

void ShowLog(Log &log, const ImGuiTextFilter &filter, bool errors_only)
{
    if (!ImGui::BeginChild("lines", ImVec2(0.f, 0.f), false, ImGuiWindowFlags_HorizontalScrollbar))
    {
        ImGui::EndChild();
        return;
    }
    // at the end the view follows the new lines, scrolled up it stays
    const bool follow = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.f;
    for (size_t i = std::min(log.cleared, log.lines.size()); i < log.lines.size(); ++i)
    {
        const std::string &line = log.lines[i];
        const bool error = Contains(line, "error") || Contains(line, "fail");
        if ((errors_only && !error) || !filter.PassFilter(line.c_str()))
        {
            continue;
        }
        if (error || Contains(line, "warn"))
        {
            ImGui::TextColored(error ? kError : kWarning, "%s", line.c_str());
        }
        else
        {
            ImGui::TextUnformatted(line.c_str());
        }
    }
    if (follow)
    {
        ImGui::SetScrollHereY(1.f);
    }
    ImGui::EndChild();
}

void ShowConsole(bool &active)
{
    // compile.log takes the script compilation and Trace of the scripts and of the editor
    static std::array<Log, 3> logs = {Log{"System", "system.log"}, Log{"Errors", "error.log"},
                                      Log{"Scripts", "compile.log"}};
    static ImGuiTextFilter filter;
    static double last = -1.0;
    // the files are looked at a few times a second, not every frame
    if (ImGui::GetTime() - last > 0.25)
    {
        last = ImGui::GetTime();
        for (Log &log : logs)
        {
            log.Update();
        }
    }

    if (!ImGui::Begin("Console", &active))
    {
        ImGui::End();
        return;
    }
    filter.Draw("Filter", 180.f);
    ImGui::SameLine();
    bool errors_only = EngineEditor::GetSetting("Console.ErrorsOnly") == "1";
    if (ImGui::Checkbox("Errors only", &errors_only))
    {
        EngineEditor::SetSetting("Console.ErrorsOnly", errors_only ? "1" : "0");
    }
    ImGui::SameLine();
    const bool clear = ImGui::Button("Clear");
    ImGui::SetItemTooltip("Hides the lines read so far; the files stay as they are");

    if (ImGui::BeginTabBar("logs"))
    {
        for (Log &log : logs)
        {
            if (clear)
            {
                log.cleared = log.lines.size();
            }
            const size_t shown = log.Shown();
            // the tab of the errors is red when there are any: error.log is to be empty
            const bool alarm = std::string_view(log.file) == "error.log" && shown > 0;
            if (alarm)
            {
                ImGui::PushStyleColor(ImGuiCol_Text, kError);
            }
            const bool open = ImGui::BeginTabItem(fmt::format("{} ({})###{}", log.title, shown, log.file).c_str());
            if (alarm)
            {
                ImGui::PopStyleColor();
            }
            if (open)
            {
                ShowLog(log, filter, errors_only);
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

// the module registers its window when it is linked into the engine
[[maybe_unused]] const bool registered = [] {
    EngineEditor::RegisterEditorTool("Console", ShowConsole);
    return true;
}();

} // namespace
} // namespace storm::editor_tools
