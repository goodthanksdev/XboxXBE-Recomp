#include "core/keys.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

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
uint8_t g_wait[256];             // down when the keys came back on: counts once let go

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
    int cap = g_capture;
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

void AddKeys(int port, Pad& p) {
    if (port != 0 || app::kGame.keys.empty() || g_off || g_capture >= 0) return;
    if (GetForegroundWindow() != static_cast<HWND>(app::Window())) return;
    static const char* kButtons[] = {"dpad_up", "dpad_down", "dpad_left", "dpad_right", "start", "back", "ls", "rs"};
    static const char* kAnalog[] = {"a", "b", "x", "y", "black", "white", "lt", "rt"};
    std::lock_guard lock(g_lock);
    Ensure();
    p.connected = true;
    for (size_t i = 0; i < g_keys.size(); i++) {
        bool down = false;
        for (int vk : g_keys[i]) {
            if (!vk) continue;
            bool held = (GetAsyncKeyState(vk) & 0x8000) != 0;
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
    }
}

}  // namespace input
