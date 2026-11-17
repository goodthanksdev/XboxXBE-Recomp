#include "core/keys.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstring>
#include <mutex>
#include <vector>

#include "app/app.h"
#include "core/input.h"

namespace input {

namespace {

struct Name {
    int vk;
    const char* name;
};
// the mouse wheel as two keys (codes Windows leaves unassigned): a notch holds one for kWheelHold, a flick of a stick
constexpr int kWheelUp = 0x0E, kWheelDown = 0x0F;
constexpr ULONGLONG kWheelHold = 120;
// F1 opens the game's options and F3 writes the threads to the log: neither binds
const Name kNames[] = {
    {VK_SPACE, "Space"}, {VK_RETURN, "Enter"}, {VK_ESCAPE, "Esc"}, {VK_TAB, "Tab"}, {VK_BACK, "Backspace"},
    {VK_LSHIFT, "Left Shift"}, {VK_RSHIFT, "Right Shift"}, {VK_LCONTROL, "Left Ctrl"}, {VK_RCONTROL, "Right Ctrl"},
    {VK_LMENU, "Left Alt"}, {VK_RMENU, "Right Alt"}, {VK_UP, "Up"}, {VK_DOWN, "Down"}, {VK_LEFT, "Left"},
    {VK_RIGHT, "Right"}, {VK_INSERT, "Insert"}, {VK_DELETE, "Delete"}, {VK_HOME, "Home"}, {VK_END, "End"},
    {VK_PRIOR, "Page Up"}, {VK_NEXT, "Page Down"}, {VK_CAPITAL, "Caps Lock"}, {VK_OEM_1, ";"}, {VK_OEM_PLUS, "="},
    {VK_OEM_COMMA, "Comma"}, {VK_OEM_MINUS, "-"}, {VK_OEM_PERIOD, "."}, {VK_OEM_2, "/"}, {VK_OEM_3, "`"},
    {VK_OEM_4, "["}, {VK_OEM_5, "\\"}, {VK_OEM_6, "]"}, {VK_OEM_7, "'"}, {VK_F2, "F2"}, {VK_F4, "F4"},
    {VK_F5, "F5"}, {VK_F6, "F6"}, {VK_F7, "F7"}, {VK_F8, "F8"}, {VK_F9, "F9"}, {VK_F10, "F10"}, {VK_F11, "F11"},
    {VK_F12, "F12"}, {VK_NUMPAD0, "Num 0"}, {VK_NUMPAD1, "Num 1"}, {VK_NUMPAD2, "Num 2"}, {VK_NUMPAD3, "Num 3"},
    {VK_NUMPAD4, "Num 4"}, {VK_NUMPAD5, "Num 5"}, {VK_NUMPAD6, "Num 6"}, {VK_NUMPAD7, "Num 7"},
    {VK_NUMPAD8, "Num 8"}, {VK_NUMPAD9, "Num 9"}, {VK_ADD, "Num +"}, {VK_SUBTRACT, "Num -"},
    {VK_MULTIPLY, "Num *"}, {VK_DIVIDE, "Num /"}, {VK_DECIMAL, "Num ."},
    {VK_LBUTTON, "Left Mouse"}, {VK_RBUTTON, "Right Mouse"}, {VK_MBUTTON, "Middle Mouse"}, {VK_XBUTTON1, "Mouse 4"},
    {VK_XBUTTON2, "Mouse 5"}, {kWheelUp, "Scroll Up"}, {kWheelDown, "Scroll Down"},
};

int VkFromName(std::string s) {
    while (!s.empty() && s.front() == ' ') s.erase(s.begin());
    while (!s.empty() && s.back() == ' ') s.pop_back();
    if (s.size() == 1 && std::isalnum(uint8_t(s[0]))) return std::toupper(uint8_t(s[0]));
    for (const Name& n : kNames)
        if (_stricmp(n.name, s.c_str()) == 0) return n.vk;
    return 0;
}

// "W, Enter" into slots
std::array<int, kKeySlots> Parse(const std::string& text) {
    std::array<int, kKeySlots> k{};
    int slot = 0;
    for (size_t at = 0; slot < kKeySlots && at <= text.size();) {
        size_t comma = text.find(',', at);
        if (comma == std::string::npos) comma = text.size();
        if (int vk = VkFromName(text.substr(at, comma - at))) k[slot++] = vk;
        at = comma + 1;
    }
    return k;
}

std::mutex g_lock;
std::vector<std::array<int, kKeySlots>> g_keys;  // per GameInfo::keys control
std::atomic<bool> g_off{false};
std::atomic<int> g_capture{-1};  // control * kKeySlots + slot
std::atomic<uint64_t> g_pad_used{0};
std::atomic<bool> g_mouse_held{false};  // mouse play has the cursor  // GetTickCount64 when port 0's controller last had input
uint8_t g_wait[256];             // down when the keys came back on: counts once let go
std::atomic<ULONGLONG> g_wheel_until[2] = {0, 0};  // up, down: held until then (GetTickCount64)

bool Held(int vk) {
    if (vk == kWheelUp || vk == kWheelDown) return GetTickCount64() < g_wheel_until[vk == kWheelDown];
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

void Ensure() {
    if (g_keys.size() == app::kGame.keys.size()) return;
    g_keys.clear();
    for (const app::KeyControl& c : app::kGame.keys) g_keys.push_back(Parse(c.keys ? c.keys : ""));
}

// the left or right Shift, Ctrl and Alt from a key message
int ExactVk(uintptr_t wp, intptr_t lp) {
    UINT scan = (lp >> 16) & 0xFF;
    bool ext = (lp >> 24) & 1;
    switch (wp) {
        case VK_SHIFT: return int(MapVirtualKeyW(scan, MAPVK_VSC_TO_VK_EX));
        case VK_CONTROL: return ext ? VK_RCONTROL : VK_LCONTROL;
        case VK_MENU: return ext ? VK_RMENU : VK_LMENU;
    }
    return int(wp);
}

}  // namespace

int KeyControls() { return int(app::kGame.keys.size()); }
const char* KeyLabel(int c) { return app::kGame.keys[c].label; }
const char* KeyControlId(int c) { return app::kGame.keys[c].control; }

int Key(int c, int slot) {
    std::lock_guard lock(g_lock);
    Ensure();
    return g_keys[c][slot];
}

void SetKey(int c, int slot, int vk) {
    std::lock_guard lock(g_lock);
    Ensure();
    if (vk)
        for (auto& k : g_keys)
            for (int& v : k)
                if (v == vk) v = 0;
    g_keys[c][slot] = vk;
}

void ResetKeys() {
    std::lock_guard lock(g_lock);
    g_keys.clear();
    Ensure();
}

std::string KeyName(int vk) {
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return std::string(1, char(vk));
    for (const Name& n : kNames)
        if (n.vk == vk) return n.name;
    return "";
}

std::string KeysText(int c) {
    std::string s;
    for (int slot = 0; slot < kKeySlots; slot++)
        if (int vk = Key(c, slot)) s += (s.empty() ? "" : ", ") + KeyName(vk);
    return s.empty() ? "none" : s;
}

void SetKeysText(int c, const std::string& text) {
    std::lock_guard lock(g_lock);
    Ensure();
    std::array<int, kKeySlots> k = Parse(text);
    if (k[0] || k[1] || text == "none") g_keys[c] = k;
}

void SetKeysOff(bool off) {
    if (off == g_off) return;
    if (!off)
        for (int vk = 0; vk < 256; vk++) g_wait[vk] = (GetAsyncKeyState(vk) & 0x8000) != 0;
    g_off = off;
}

void CaptureKey(int c, int slot) { g_capture = c * kKeySlots + slot; }
bool Capturing() { return g_capture >= 0; }

bool KeyMessage(uint32_t msg, uintptr_t wp, intptr_t lp) {
    // the window put in the background: the cursor goes free at once, whatever the game is doing
    if ((msg == WM_ACTIVATE && LOWORD(wp) == WA_INACTIVE) || msg == WM_KILLFOCUS) FreeCursor();
    // a held cursor: none shown over the window
    if (msg == WM_SETCURSOR && g_mouse_held && LOWORD(lp) == HTCLIENT) {
        SetCursor(nullptr);
        return true;
    }
    int cap = g_capture;
    // a mouse button while binding
    // a wheel notch: held for a moment, or bound while binding
    if (msg == WM_MOUSEWHEEL) {
        int up = GET_WHEEL_DELTA_WPARAM(wp) > 0 ? kWheelUp : kWheelDown;
        if (cap >= 0) {
            SetKey(cap / kKeySlots, cap % kKeySlots, up);
            g_capture = -1;
            app::Save();
            return true;
        }
        g_wheel_until[up == kWheelDown] = GetTickCount64() + kWheelHold;
        return false;
    }
    int click = msg == WM_LBUTTONDOWN ? VK_LBUTTON : msg == WM_RBUTTONDOWN ? VK_RBUTTON : msg == WM_MBUTTONDOWN ? VK_MBUTTON
              : msg == WM_XBUTTONDOWN ? (HIWORD(wp) == XBUTTON1 ? VK_XBUTTON1 : VK_XBUTTON2) : 0;
    if (cap >= 0 && click) {
        SetKey(cap / kKeySlots, cap % kKeySlots, click);
        g_capture = -1;
        app::Save();
        return true;
    }
    if (cap < 0 || (msg != WM_KEYDOWN && msg != WM_SYSKEYDOWN)) return false;
    if ((lp >> 30) & 1) return true;  // held from before (the key that started the capture): not a new press
    int vk = ExactVk(wp, lp);
    if (vk == VK_ESCAPE) {
        g_capture = -1;
    } else if (!KeyName(vk).empty()) {
        SetKey(cap / kKeySlots, cap % kKeySlots, vk);
        g_capture = -1;
        app::Save();
    }
    return true;  // every key while capturing, F1 and the rest
}

void ControllerUsed() { g_pad_used = GetTickCount64(); }

void FreeCursor() {
    if (g_mouse_held.exchange(false)) ClipCursor(nullptr);
}

namespace {
// The cursor: while the game window has the focus and the options are closed it is hidden and kept inside the window
// (these games have no cursor of their own, and a click can't land on another monitor). The options, another window
// or Alt+Tab free it.
void HoldCursor(bool hold) {
    if (!hold) {
        if (g_mouse_held.exchange(false)) ClipCursor(nullptr);
        return;
    }
    HWND w = static_cast<HWND>(app::Window());
    RECT r;
    GetClientRect(w, &r);
    MapWindowPoints(w, nullptr, reinterpret_cast<POINT*>(&r), 2);
    ClipCursor(&r);  // each time: the window may have moved
    g_mouse_held = true;
}

// Mouse play (GameInfo::mouse_stick): the held cursor goes back to the window's centre and its movement pushes the
// stick. A flick is a full push for a moment; still, the stick eases back to the middle.
float g_mx = 0, g_my = 0;
bool g_centred = false;
void MouseStick(Pad& p, bool on) {
    if (!on || !g_mouse_held) {
        g_mx = g_my = 0;
        g_centred = false;
        return;
    }
    HWND w = static_cast<HWND>(app::Window());
    RECT r;
    GetClientRect(w, &r);
    MapWindowPoints(w, nullptr, reinterpret_cast<POINT*>(&r), 2);
    POINT c{(r.left + r.right) / 2, (r.top + r.bottom) / 2};
    if (!g_centred) {
        SetCursorPos(c.x, c.y);
        g_centred = true;
        return;
    }
    POINT m;
    GetCursorPos(&m);
    SetCursorPos(c.x, c.y);
    // stick units per pixel of movement, 1080p's pixels whatever the window
    float k = 900.0f * app::Get().mouse_sensitivity / 100.0f * 1080.0f / float(std::max<LONG>(r.bottom - r.top, 1));
    g_mx = g_mx * 0.55f + float(m.x - c.x) * k;
    g_my = g_my * 0.55f - float(m.y - c.y) * k;
    auto add = [](int16_t& v, float d) { v = int16_t(std::clamp(float(v) + d, -32767.0f, 32767.0f)); };
    bool right = std::strcmp(app::kGame.mouse_stick, "left") != 0;
    add(right ? p.rx : p.lx, g_mx);
    add(right ? p.ry : p.ly, g_my);
}
}  // namespace

void AddKeys(int port, Pad& p) {
    if (port != 0) return;
    bool focus = !g_off && g_capture < 0 && GetForegroundWindow() == static_cast<HWND>(app::Window());
    HoldCursor(focus);
    bool on = focus && !app::kGame.keys.empty() && GetTickCount64() - g_pad_used >= 2000;  // the pad in use: keys rest
    if (app::kGame.mouse_stick) MouseStick(p, on);
    if (!on) return;
    static const char* kButtons[] = {"dpad_up", "dpad_down", "dpad_left", "dpad_right", "start", "back", "ls", "rs"};
    static const char* kAnalog[] = {"a", "b", "x", "y", "black", "white", "lt", "rt"};
    std::lock_guard lock(g_lock);
    Ensure();
    p.connected = true;
    for (size_t i = 0; i < g_keys.size(); i++) {
        bool down = false;
        for (int vk : g_keys[i]) {
            if (!vk) continue;
            bool held = Held(vk);
            if (!held) g_wait[vk] = 0;
            down |= held && !g_wait[vk];
        }
        if (!down) continue;
        const char* c = app::kGame.keys[i].control;
        for (int k = 0; k < 8; k++) {
            if (!std::strcmp(c, kButtons[k])) p.buttons |= uint16_t(1 << k);
            if (!std::strcmp(c, kAnalog[k])) p.analog[k] = 0xFF;
        }
        if (!std::strcmp(c, "up")) p.ly = 32767;
        if (!std::strcmp(c, "down")) p.ly = -32767;
        if (!std::strcmp(c, "left")) p.lx = -32767;
        if (!std::strcmp(c, "right")) p.lx = 32767;
        // the right stick (a lightsaber, a camera)
        if (!std::strcmp(c, "rs_up")) p.ry = 32767;
        if (!std::strcmp(c, "rs_down")) p.ry = -32767;
        if (!std::strcmp(c, "rs_left")) p.rx = -32767;
        if (!std::strcmp(c, "rs_right")) p.rx = 32767;
    }
}

}  // namespace input
