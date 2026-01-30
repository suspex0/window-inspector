#pragma once
#include <vector>
#include <string>
#include <cstdint>
#include <cstdio>
#include <array>
#include <algorithm>
#include <cctype>

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"
#include "imgui/imgui_impl_dx11.h"
#include "imgui/imgui_impl_win32.h"

namespace Inspector
{
    struct ProcessInfo
    {
        DWORD pid = 0;
        std::wstring name;
    };

    struct WindowInfo
    {
        HWND handle = nullptr;
        DWORD pid = 0;
        DWORD threadId = 0;
        std::wstring title;
        std::wstring className;
        LONG_PTR style = 0;
        LONG_PTR exStyle = 0;
        RECT bounds{0, 0, 0, 0};
        bool visible = false;
    };

    struct ProcessWindows
    {
        ProcessInfo process;
        std::vector<WindowInfo> windows;
    };

    struct InspectorSnapshot
    {
        SYSTEMTIME timestamp{};
        std::vector<ProcessWindows> processes;
        size_t totalProcessCount = 0;
        size_t totalWindowCount = 0;
    };

    inline std::string ToUtf8(const std::wstring& text)
    {
        if (text.empty())
        {
            return {};
        }

        const int required = ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
        if (required <= 0)
        {
            return {};
        }

        std::string utf8(static_cast<size_t>(required), '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), utf8.data(), required, nullptr, nullptr);
        return utf8;
    }

    inline std::string FormatTimestamp(const SYSTEMTIME& time)
    {
        if (time.wYear == 0)
        {
            return {};
        }

        char buffer[64] = {};
        std::snprintf(buffer, sizeof(buffer), "%04u-%02u-%02u %02u:%02u:%02u",
                      static_cast<unsigned>(time.wYear), static_cast<unsigned>(time.wMonth), static_cast<unsigned>(time.wDay),
                      static_cast<unsigned>(time.wHour), static_cast<unsigned>(time.wMinute), static_cast<unsigned>(time.wSecond));
        return std::string(buffer);
    }

    inline bool ContainsCaseInsensitive(const std::string& text, const char* filter)
    {
        if (filter == nullptr || *filter == '\0')
        {
            return true;
        }

        std::string lowerText = text;
        std::string lowerFilter(filter);
        std::transform(lowerText.begin(), lowerText.end(), lowerText.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        std::transform(lowerFilter.begin(), lowerFilter.end(), lowerFilter.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return lowerText.find(lowerFilter) != std::string::npos;
    }

    inline bool RenderInspectorUi(float deltaSeconds, const InspectorSnapshot& snapshot)
    {
        bool refreshRequested = false;
        const float fps = deltaSeconds > 0.0f ? 1.0f / deltaSeconds : 0.0f;
        ImGuiIO& io = ImGui::GetIO();
        if (io.DisplaySize.x <= 0.0f || io.DisplaySize.y <= 0.0f)
        {
            return false;
        }

        static std::array<char, 128> processFilter{};

        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::SetNextWindowFocus();
        constexpr ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoTitleBar |
                                                 ImGuiWindowFlags_NoResize |
                                                 ImGuiWindowFlags_NoMove |
                                                 ImGuiWindowFlags_NoCollapse |
                                                 ImGuiWindowFlags_NoSavedSettings |
                                                 ImGuiWindowFlags_NoBringToFrontOnFocus;
        if (ImGui::Begin("Window Inspector", nullptr, windowFlags))
        {
            //ImGui::Text("Frame time: %.3f ms | FPS: %.1f", deltaSeconds * 1000.0f, fps);
            // ImGui::SameLine();
            if (ImGui::Button("Refresh"))
            {
                refreshRequested = true;
            }

            ImGui::SameLine();
            ImGui::SetNextItemWidth(250.0f);
            ImGui::InputTextWithHint("##ProcessFilter", "Filter by process name", processFilter.data(), processFilter.size());

            const std::string timestamp = FormatTimestamp(snapshot.timestamp);
            if (!snapshot.processes.empty())
            {
                ImGui::Text("Processes: %zu | Windows: %zu | Last refresh: %s",
                            snapshot.totalProcessCount,
                            snapshot.totalWindowCount,
                            timestamp.empty() ? "N/A" : timestamp.c_str());
            }
            else
            {
                ImGui::TextUnformatted("No snapshot collected yet. Press Refresh to gather data.");
            }

            ImGui::Separator();

            size_t visibleCount = 0;
            if (ImGui::BeginChild("ProcessList", ImVec2(0, 0), true))
            {
                for (const auto& entry : snapshot.processes)
                {
                    std::string processName = entry.process.name.empty() ? std::string("<Unknown>") : ToUtf8(entry.process.name);
                    if (!ContainsCaseInsensitive(processName, processFilter.data()))
                    {
                        continue;
                    }

                    ++visibleCount;
                    std::string headerLabel = processName + " [PID " + std::to_string(entry.process.pid) + "]##proc_" + std::to_string(entry.process.pid);

                    if (ImGui::CollapsingHeader(headerLabel.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
                    {
                        ImGui::Text("Windows: %zu", entry.windows.size());
                        if (entry.windows.empty())
                        {
                            ImGui::TextDisabled("No top-level windows.");
                            continue;
                        }

                        const std::string tableId = "##win_table_" + std::to_string(entry.process.pid);
                        if (ImGui::BeginTable(tableId.c_str(), 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp))
                        {
                            ImGui::TableSetupColumn("HWND", ImGuiTableColumnFlags_WidthFixed, 110.0f);
                            ImGui::TableSetupColumn("Title", ImGuiTableColumnFlags_WidthStretch, 0.35f);
                            ImGui::TableSetupColumn("Class", ImGuiTableColumnFlags_WidthStretch, 0.25f);
                            ImGui::TableSetupColumn("Thread/Visible", ImGuiTableColumnFlags_WidthFixed, 130.0f);
                            ImGui::TableSetupColumn("Styles", ImGuiTableColumnFlags_WidthFixed, 170.0f);
                            ImGui::TableSetupColumn("Bounds", ImGuiTableColumnFlags_WidthFixed, 190.0f);
                            ImGui::TableHeadersRow();

                            for (const auto& window : entry.windows)
                            {
                                ImGui::TableNextRow();
                                ImGui::TableSetColumnIndex(0);
                                ImGui::Text("0x%llX", static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(window.handle)));

                                ImGui::TableSetColumnIndex(1);
                                const std::string title = window.title.empty() ? std::string("<No Title>") : ToUtf8(window.title);
                                ImGui::TextUnformatted(title.c_str());

                                ImGui::TableSetColumnIndex(2);
                                const std::string className = window.className.empty() ? std::string("<UnknownClass>") : ToUtf8(window.className);
                                ImGui::TextUnformatted(className.c_str());

                                ImGui::TableSetColumnIndex(3);
                                ImGui::Text("TID %lu\n%s", window.threadId, window.visible ? "Visible" : "Hidden");

                                ImGui::TableSetColumnIndex(4);
                                ImGui::Text("S:0x%08llX\nE:0x%08llX",
                                            static_cast<unsigned long long>(window.style),
                                            static_cast<unsigned long long>(window.exStyle));

                                ImGui::TableSetColumnIndex(5);
                                const LONG width = window.bounds.right - window.bounds.left;
                                const LONG height = window.bounds.bottom - window.bounds.top;
                                ImGui::Text("(%ld,%ld)-(%ld,%ld)\n[%ldx%ld]",
                                            window.bounds.left, window.bounds.top,
                                            window.bounds.right, window.bounds.bottom,
                                            width, height);
                            }

                            ImGui::EndTable();
                        }
                    }
                }

                if (visibleCount == 0 && !snapshot.processes.empty())
                {
                    ImGui::TextDisabled("No processes match the current filter.");
                }
            }
            ImGui::EndChild();
        }
        ImGui::End();

        return refreshRequested;
    }
}

