#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <chrono>
#include <TlHelp32.h>
#include <vector>
#include <string>
#include <unordered_map>
#include <iostream>
#include <iomanip>
#include <cstdint>

#include "ui.hpp"

using Inspector::InspectorSnapshot;
using Inspector::ProcessInfo;
using Inspector::ProcessWindows;
using Inspector::WindowInfo;

#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((HANDLE)-4)
#endif

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")


extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace
{
    ID3D11Device* gDevice = nullptr;
    ID3D11DeviceContext* gDeviceContext = nullptr;
    IDXGISwapChain* gSwapChain = nullptr;
    ID3D11RenderTargetView* gMainRenderTargetView = nullptr;

    bool CreateDeviceD3D(HWND hWnd);
    void CleanupDeviceD3D();
    void CreateRenderTarget();
    void CleanupRenderTarget();
    LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

    std::vector<ProcessInfo> EnumerateProcesses();
    std::vector<WindowInfo> EnumerateWindows();
    BOOL CALLBACK EnumWindowsThunk(HWND hwnd, LPARAM lParam);
    InspectorSnapshot CollectInspectorSnapshot();
}

static void SetDpiAware()
{
#if defined(_WIN32) && defined(_MSC_VER)
    using SetProcessDpiAwarenessContextFn = BOOL(WINAPI*)(HANDLE);
    if (HMODULE user32 = ::LoadLibraryW(L"User32.dll"))
    {
        if (const auto setContext = reinterpret_cast<SetProcessDpiAwarenessContextFn>(::GetProcAddress(user32, "SetProcessDpiAwarenessContext")))
        {
            setContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        }
        ::FreeLibrary(user32);
    }
#endif
}

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int)
{
    SetDpiAware();

    const wchar_t* windowClassName = L"WindowInspectorClass";
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = ::LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = windowClassName;
    if (!::RegisterClassExW(&wc))
    {
        return 1;
    }

    HWND hwnd = ::CreateWindowW(windowClassName, L"Window Inspector", WS_OVERLAPPEDWINDOW, 100, 100, 1280, 800, nullptr, nullptr, hInstance, nullptr);
    if (!hwnd)
    {
        ::UnregisterClassW(windowClassName, hInstance);
        return 1;
    }

    if (!CreateDeviceD3D(hwnd))
    {
        CleanupDeviceD3D();
        ::DestroyWindow(hwnd);
        ::UnregisterClassW(windowClassName, hInstance);
        return 1;
    }

    ::ShowWindow(hwnd, SW_SHOWDEFAULT);
    ::UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(gDevice, gDeviceContext);

    InspectorSnapshot snapshot = CollectInspectorSnapshot();
    std::wcout << L"[info] Captured " << snapshot.totalProcessCount << L" processes and "
               << snapshot.totalWindowCount << L" windows." << std::endl;

    MSG msg = {};
    auto previousTime = std::chrono::steady_clock::now();

    while (msg.message != WM_QUIT)
    {
        if (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE))
        {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
            continue;
        }

        const auto now = std::chrono::steady_clock::now();
        const float deltaSeconds = std::chrono::duration<float>(now - previousTime).count();
        previousTime = now;

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        const bool shouldRefresh = Inspector::RenderInspectorUi(deltaSeconds, snapshot);
        if (shouldRefresh)
        {
            snapshot = CollectInspectorSnapshot();
            std::wcout << L"[info] Captured " << snapshot.totalProcessCount << L" processes and "
                       << snapshot.totalWindowCount << L" windows." << std::endl;
        }

        ImGui::Render();
        const float clearColor[4] = {0.10f, 0.10f, 0.15f, 1.00f};
        gDeviceContext->OMSetRenderTargets(1, &gMainRenderTargetView, nullptr);
        gDeviceContext->ClearRenderTargetView(gMainRenderTargetView, clearColor);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        gSwapChain->Present(1, 0);
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D();
    ::DestroyWindow(hwnd);
    ::UnregisterClassW(windowClassName, hInstance);

    return 0;
}

int main()
{
    return wWinMain(::GetModuleHandleW(nullptr), nullptr, ::GetCommandLineW(), SW_SHOWDEFAULT);
}

namespace
{
    bool CreateDeviceD3D(HWND hWnd)
    {
        DXGI_SWAP_CHAIN_DESC sd = {};
        sd.BufferCount = 2;
        sd.BufferDesc.Width = 0;
        sd.BufferDesc.Height = 0;
        sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.BufferDesc.RefreshRate.Numerator = 60;
        sd.BufferDesc.RefreshRate.Denominator = 1;
        sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.OutputWindow = hWnd;
        sd.SampleDesc.Count = 1;
        sd.SampleDesc.Quality = 0;
        sd.Windowed = TRUE;
        sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

        UINT createDeviceFlags = 0;
#if defined(_DEBUG)
        createDeviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
        const D3D_FEATURE_LEVEL featureLevelArray[2] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
        D3D_FEATURE_LEVEL featureLevel;

        if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags, featureLevelArray, 2,
                                                 D3D11_SDK_VERSION, &sd, &gSwapChain, &gDevice, &featureLevel, &gDeviceContext)))
        {
            return false;
        }

        CreateRenderTarget();
        return true;
    }

    void CleanupDeviceD3D()
    {
        CleanupRenderTarget();
        if (gSwapChain)
        {
            gSwapChain->Release();
            gSwapChain = nullptr;
        }
        if (gDeviceContext)
        {
            gDeviceContext->Release();
            gDeviceContext = nullptr;
        }
        if (gDevice)
        {
            gDevice->Release();
            gDevice = nullptr;
        }
    }

    void CreateRenderTarget()
    {
        ID3D11Texture2D* backBuffer = nullptr;
        if (gSwapChain && SUCCEEDED(gSwapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer))))
        {
            gDevice->CreateRenderTargetView(backBuffer, nullptr, &gMainRenderTargetView);
            backBuffer->Release();
        }
    }

    void CleanupRenderTarget()
    {
        if (gMainRenderTargetView)
        {
            gMainRenderTargetView->Release();
            gMainRenderTargetView = nullptr;
        }
    }

    LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        {
            return true;
        }

        switch (msg)
        {
        case WM_SIZE:
            if (gDevice != nullptr && wParam != SIZE_MINIMIZED)
            {
                CleanupRenderTarget();
                gSwapChain->ResizeBuffers(0, LOWORD(lParam), HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
                CreateRenderTarget();
            }
            return 0;
        case WM_DPICHANGED:
            if (const RECT* newRect = reinterpret_cast<RECT*>(lParam))
            {
                ::SetWindowPos(hWnd, nullptr, newRect->left, newRect->top,
                                newRect->right - newRect->left, newRect->bottom - newRect->top,
                                SWP_NOZORDER | SWP_NOACTIVATE);
            }
            return 0;
        case WM_SYSCOMMAND:
            if ((wParam & 0xfff0) == SC_KEYMENU)
            {
                return 0;
            }
            break;
        case WM_DESTROY:
            ::PostQuitMessage(0);
            return 0;
        }
        return ::DefWindowProcW(hWnd, msg, wParam, lParam);
    }

    std::vector<ProcessInfo> EnumerateProcesses()
    {
        std::vector<ProcessInfo> processes;
        HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE)
        {
            std::wcerr << L"[error] CreateToolhelp32Snapshot failed (" << ::GetLastError() << L")" << std::endl;
            return processes;
        }

        PROCESSENTRY32W entry = {};
        entry.dwSize = sizeof(PROCESSENTRY32W);
        if (::Process32FirstW(snapshot, &entry))
        {
            do
            {
                ProcessInfo info;
                info.pid = entry.th32ProcessID;
                info.name.assign(entry.szExeFile);
                processes.emplace_back(std::move(info));
            } while (::Process32NextW(snapshot, &entry));
        }

        ::CloseHandle(snapshot);
        return processes;
    }

    BOOL CALLBACK EnumWindowsThunk(HWND hwnd, LPARAM lParam)
    {
        auto* windows = reinterpret_cast<std::vector<WindowInfo>*>(lParam);
        if (!::IsWindow(hwnd))
        {
            return TRUE;
        }

        WindowInfo info;
        info.handle = hwnd;
        info.threadId = ::GetWindowThreadProcessId(hwnd, &info.pid);

        const int length = ::GetWindowTextLengthW(hwnd);
        if (length > 0)
        {
            std::wstring title(length + 1, L'\0');
            const int copied = ::GetWindowTextW(hwnd, title.data(), length + 1);
            if (copied > 0)
            {
                title.resize(static_cast<size_t>(copied));
            }
            else
            {
                title.clear();
            }
            info.title = std::move(title);
        }

        if (info.title.empty())
        {
            info.title = L"<No Title>";
        }

        wchar_t classBuffer[256] = {};
        const int classLen = ::GetClassNameW(hwnd, classBuffer, static_cast<int>(_countof(classBuffer)));
        if (classLen > 0)
        {
            info.className.assign(classBuffer, classLen);
        }
        else
        {
            info.className = L"<UnknownClass>";
        }

        info.style = ::GetWindowLongPtrW(hwnd, GWL_STYLE);
        info.exStyle = ::GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        info.visible = (::IsWindowVisible(hwnd) != FALSE);
        if (!::GetWindowRect(hwnd, &info.bounds))
        {
            info.bounds = RECT{0, 0, 0, 0};
        }

        windows->emplace_back(std::move(info));
        return TRUE;
    }

    std::vector<WindowInfo> EnumerateWindows()
    {
        std::vector<WindowInfo> windows;
        if (::EnumWindows(EnumWindowsThunk, reinterpret_cast<LPARAM>(&windows)) == 0)
        {
            const DWORD error = ::GetLastError();
            if (error != ERROR_SUCCESS)
            {
                std::wcerr << L"[error] EnumWindows failed (" << error << L")" << std::endl;
            }
        }
        return windows;
    }

    InspectorSnapshot CollectInspectorSnapshot()
    {
        auto processes = EnumerateProcesses();
        auto windows = EnumerateWindows();

        InspectorSnapshot snapshot;
        snapshot.totalProcessCount = processes.size();
        snapshot.totalWindowCount = windows.size();
        snapshot.processes.reserve(processes.size());

        std::unordered_map<DWORD, std::vector<WindowInfo>> windowsByPid;
        windowsByPid.reserve(windows.size());
        for (auto& window : windows)
        {
            windowsByPid[window.pid].push_back(std::move(window));
        }

        for (auto& process : processes)
        {
            ProcessWindows entry;
            entry.process = std::move(process);
            if (auto it = windowsByPid.find(entry.process.pid); it != windowsByPid.end())
            {
                entry.windows = std::move(it->second);
            }
            snapshot.processes.emplace_back(std::move(entry));
        }

        ::GetLocalTime(&snapshot.timestamp);
        return snapshot;
    }
}