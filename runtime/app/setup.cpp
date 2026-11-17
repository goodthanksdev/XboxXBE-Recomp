// The setup window (app.h RunSetup): before the game starts, the player finds the game, its code is prepared, and a
// few settings are picked. It is the options menu's panel (menu.cpp SetupUi) in a window of its own, drawn with
// Direct3D 11: the game's renderer only starts with the game.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <shobjidl.h>

#include <cwchar>
#include <filesystem>
#include <string>

#include <imgui.h>
#include <backends/imgui_impl_dx11.h>
#include <backends/imgui_impl_win32.h>

#include "app/app.h"
#include "app/menu.h"
#include "core/input.h"
#include "core/log.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace app {

namespace {

namespace fs = std::filesystem;

ID3D11Device* g_dev = nullptr;
ID3D11DeviceContext* g_ctx = nullptr;
IDXGISwapChain* g_chain = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
bool g_closed = false;  // the window's close button: the game doesn't start

void MakeTarget() {
    ID3D11Texture2D* back = nullptr;
    if (SUCCEEDED(g_chain->GetBuffer(0, IID_PPV_ARGS(&back)))) {
        g_dev->CreateRenderTargetView(back, nullptr, &g_rtv);
        back->Release();
    }
}

void DropTarget() {
    if (g_rtv) g_rtv->Release();
    g_rtv = nullptr;
}

bool OpenDevice(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    D3D_FEATURE_LEVEL level;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION,
                                             &sd, &g_chain, &g_dev, &level, &g_ctx)))
        return false;
    MakeTarget();
    return true;
}

void CloseDevice() {
    DropTarget();
    if (g_chain) g_chain->Release();
    if (g_ctx) g_ctx->Release();
    if (g_dev) g_dev->Release();
    g_chain = nullptr;
    g_ctx = nullptr;
    g_dev = nullptr;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return 1;
    if (intptr_t result = 0; msg != WM_KEYDOWN || wp != VK_F1)
        if (menu::Message(hwnd, msg, wp, lp, &result)) return result;  // arrows, Enter, Esc drive the panel
    switch (msg) {
        case WM_SIZE:
            if (g_chain && wp != SIZE_MINIMIZED) {
                DropTarget();
                g_chain->ResizeBuffers(0, LOWORD(lp), HIWORD(lp), DXGI_FORMAT_UNKNOWN, 0);
                MakeTarget();
            }
            return 0;
        case WM_CLOSE:
            g_closed = true;
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// centred on the primary monitor, sized for its DPI and kept inside its work area
HWND OpenWindow() {
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));  // IDC_ARROW
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    std::wstring name = std::wstring(kGame.name) + L"Setup";
    wc.lpszClassName = name.c_str();
    RegisterClassExW(&wc);
    UINT dpi = GetDpiForSystem();
    RECT work;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int cw = std::min(MulDiv(1060, dpi, 96), int(work.right - work.left) - 40);
    int ch = std::min(MulDiv(690, dpi, 96), int(work.bottom - work.top) - 60);
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT r{0, 0, cw, ch};
    AdjustWindowRectExForDpi(&r, style, FALSE, 0, dpi);
    int w = r.right - r.left, h = r.bottom - r.top;
    // --window=x,y,w,h (a launcher's, or the game's last window): centred on that instead
    if (const wchar_t* at = std::wcsstr(GetCommandLineW(), L"--window=")) {
        int ax, ay, aw, ah;
        if (swscanf(at + 9, L"%d,%d,%d,%d", &ax, &ay, &aw, &ah) == 4 && aw > 0 && ah > 0)
            work = {ax, ay, ax + aw, ay + ah};
    }
    int x = work.left + (work.right - work.left - w) / 2, y = work.top + (work.bottom - work.top - h) / 2;
    std::wstring title = std::wstring(kGame.title) + L" setup";
    return CreateWindowExW(0, wc.lpszClassName, title.c_str(), style, x, y, w, h, nullptr, nullptr, inst, nullptr);
}

// the system's file picker: a disc image, or with folders set a folder; empty when cancelled
std::wstring Pick(HWND owner, bool folder) {
    std::wstring out;
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return out;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | (folder ? FOS_PICKFOLDERS : 0));
    if (folder) {
        dlg->SetTitle(L"Choose the folder with default.xbe");
    } else {
        COMDLG_FILTERSPEC types[] = {{L"Disc images (*.iso)", L"*.iso"}, {L"All files", L"*.*"}};
        dlg->SetFileTypes(2, types);
        dlg->SetTitle(L"Choose your game's disc image");
    }
    if (SUCCEEDED(dlg->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                out = path;
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dlg->Release();
    return out;
}

std::string Utf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

}  // namespace

bool RunSetup(GameFiles& game) {
    const wchar_t* line = GetCommandLineW();
    bool relaunched = std::wcsstr(line, L"--relaunched") != nullptr;
    bool asked = std::wcsstr(line, L"--setup") != nullptr || (GetAsyncKeyState(VK_SHIFT) & 0x8000);
    // a relaunch into another of the game's images never shows it: the old way, quietly
    if (relaunched) return FindGame(game) && PrepareGameCode(game);
    GameFound found;
    bool have = FindGame(game, false, &found);
    bool needed = have && GameCodeNeeded(game);
    if (have && !needed && Get().setup_done && !asked) return LoadGameCode();

    LOGR("Setup: %s", !have ? "the game is missing" : needed ? "the game's code needs building" : "opened");
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    HWND hwnd = OpenWindow();
    if (!hwnd || !OpenDevice(hwnd)) {
        LOGE("Setup: no Direct3D 11 window, the game starts without it");
        if (hwnd) DestroyWindow(hwnd);
        return FindGame(game) && PrepareGameCode(game);
    }
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    input::Init();
    ImGuiContext* ui = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    menu::Fonts();
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_dev, g_ctx);

    menu::SetupModel m;
    auto look = [&] {
        found = {};
        have = FindGame(game, false, &found);
        m.game_ok = have;
        m.other_release = !have && found.other_release;
        m.found = found.where.wstring();
        m.blocker.clear();
        needed = have && GameCodeNeeded(game);
    };
    look();
    bool play = false;
    while (!g_closed) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (g_closed) break;
        // the controller drives the panel too (menu::Pad reads it while Poll runs)
        if (GetForegroundWindow() == hwnd) input::Poll(0);
        // the game found: its code is prepared by itself
        m.code = GameCodeState();
        if (have && needed && m.code.state == GameCodeStatus::kIdle && m.blocker.empty()) m.blocker = StartGameCode();
        if (have && !needed) m.code.state = GameCodeStatus::kReady;
        if (m.pick_iso || m.pick_folder) {
            bool folder = m.pick_folder;
            m.pick_iso = m.pick_folder = false;
            std::wstring path = Pick(hwnd, folder);
            if (!path.empty()) {
                StopGameCode();
                Get().game_path = Utf8(path);
                Save();
                LOGR("Setup: game picked: %s", Get().game_path.c_str());
                look();
            }
        }
        if (m.play) {
            m.play = false;
            if (m.game_ok && m.code.state == GameCodeStatus::kReady) {
                play = true;
                break;
            }
        }
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        menu::SetupUi(m);
        ImGui::Render();
        const float clear[4] = {0.03f, 0.04f, 0.05f, 1.0f};
        g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_ctx->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_chain->Present(1, 0);
    }
    if (!play) StopGameCode();
    menu::SetupEnd();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext(ui);
    CloseDevice();
    DestroyWindow(hwnd);
    if (!play) {
        LOGR("Setup: closed");
        return false;
    }
    Get().setup_done = true;
    Save();
    if (m.remove_compiler) RemoveCompiler();
    LOGR("Setup: play");
    // mounted again for the game: the quiet look may have run more than once
    return FindGame(game) && LoadGameCode();
}

}  // namespace app
