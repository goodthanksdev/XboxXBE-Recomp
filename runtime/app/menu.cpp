// The options menu (menu.h): a panel over the dimmed game with tabs across the top and settings as cards in two
// columns. Up/down/left/right move between cards, A flips a switch or starts changing a value (left/right then
// change it, A or B ends), LB/RB or Q/E switch tabs. The mouse clicks tabs and cards, its wheel steps a value.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <imgui.h>

#include "app/app.h"
#include "app/menu.h"
#include "audio/audio.h"
#include "core/keys.h"

namespace menu {
namespace {

// ---- look ----

ImFont* g_body = nullptr;  // Segoe UI
ImFont* g_bold = nullptr;  // Segoe UI Semibold or Bold
ImFont* g_icons = nullptr;  // Segoe Fluent Icons or MDL2 Assets, merged into g_body when found

constexpr ImU32 Rgba(int r, int g, int b, int a = 255) { return IM_COL32(r, g, b, a); }
ImU32 Fade(ImU32 c, float alpha) {
    int a = int(((c >> IM_COL32_A_SHIFT) & 0xFF) * std::clamp(alpha, 0.0f, 1.0f));
    return (c & ~IM_COL32_A_MASK) | (ImU32(a) << IM_COL32_A_SHIFT);
}
const ImU32 kAccent = Rgba(46, 217, 199);
const ImU32 kText = Rgba(242, 245, 247);
const ImU32 kSoft = Rgba(154, 166, 178);
const ImU32 kPanel = Rgba(15, 20, 26, 244);
const ImU32 kCard = Rgba(255, 255, 255, 13);
const ImU32 kCardOn = Rgba(255, 255, 255, 26);
const ImU32 kLine = Rgba(255, 255, 255, 22);

struct TierLook {
    const char* name;
    ImU32 color;
    const char* about;
};
TierLook Tier() {
    std::string t = app::kGame.tier ? app::kGame.tier : "";
    if (t == "beta") return {"BETA", Rgba(90, 165, 255), "Plays well from start to finish for most people. Some bugs are left."};
    if (t == "complete") return {"COMPLETE", Rgba(88, 214, 128), "Fully playable. Anything that seems off is worth reporting."};
    if (t == "alpha") return {"ALPHA", Rgba(255, 171, 51), "An early build: expect bugs and the odd crash, and keep saves backed up."};
    return {nullptr, 0, nullptr};
}

std::string Utf8(uint32_t cp) {
    std::string s;
    if (cp < 0x80) s += char(cp);
    else if (cp < 0x800) s += char(0xC0 | cp >> 6), s += char(0x80 | (cp & 0x3F));
    else s += char(0xE0 | cp >> 12), s += char(0x80 | (cp >> 6 & 0x3F)), s += char(0x80 | (cp & 0x3F));
    return s;
}

// ---- input from three threads, run on the render thread ----

enum Cmd { kUp, kDown, kLeft, kRight, kAccept, kBack, kToggle, kClear, kTabPrev, kTabNext };
std::atomic<bool> g_open{false};
bool g_setup = false;  // the panel is the setup window's (SetupUi): one tab, it doesn't close
std::mutex g_cmd_lock;
std::vector<Cmd> g_cmds;
const auto g_start = std::chrono::steady_clock::now();
double Now() { return std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count(); }
void Push(Cmd c) {
    std::lock_guard lock(g_cmd_lock);
    g_cmds.push_back(c);
}

// ---- the settings as cards ----

enum Kind { kChoice, kSlider, kSwitch, kButton, kKey, kInfo };
struct Card {
    std::string name, help;
    Kind kind = kChoice;
    std::function<std::string()> value;  // a choice's or slider's text
    std::function<void(int)> step;       // a choice, slider or switch: a step either way
    std::function<bool()> on;            // a switch
    std::function<float()> fill;         // a slider, 0 to 1
    std::function<void()> press;         // a button
    int key = -1;                        // a keyboard control
    bool wide = false;                   // both columns
    bool danger = false;                 // a button asked twice
    bool primary = false;                // a button filled with the accent (the setup's Play)
    std::function<bool()> enabled;       // a button that can be off for now
    std::function<void(ImDrawList*, ImVec2, ImVec2, float)> draw;  // an info card's own drawing
    float height = 0;                    // its row's height, 1080p pixels; 0: the usual
    bool footer = false;                 // a button in the footer, always in view (the setup's Play)
    // the mouse: a choice opens its list to pick from, a slider goes where it is clicked
    std::function<std::vector<std::string>()> list;
    std::function<int()> index;          // the list's current entry
    std::function<void(int)> pick;
    std::function<void(float)> set;      // a slider, 0 to 1
};
struct Tab {
    std::string name;
    uint32_t icon;
    std::vector<Card> cards;
};

void Changed(bool display = false) {
    app::Save();
    if (display) app::ApplyDisplay();
}
template <typename T>
void Cycle(T& v, const std::vector<T>& choices, int dir) {
    size_t i = std::find(choices.begin(), choices.end(), v) - choices.begin();
    i = i >= choices.size() ? 0 : (i + choices.size() + dir) % choices.size();
    v = choices[i];
}
template <typename T>
float Where(T v, const std::vector<T>& choices) {
    size_t i = std::find(choices.begin(), choices.end(), v) - choices.begin();
    return i >= choices.size() ? 0.0f : float(i) / float(choices.size() - 1);
}

struct Res {
    uint32_t w, h;
};
const std::vector<Res>& Resolutions() {
    static std::vector<Res> list;
    if (!list.empty()) return list;
    DEVMODEW m{};
    m.dmSize = sizeof m;
    for (DWORD i = 0; EnumDisplaySettingsW(nullptr, i, &m); i++) {
        if (m.dmPelsWidth < 800 || m.dmPelsHeight < 600) continue;
        Res r{m.dmPelsWidth, m.dmPelsHeight};
        if (std::none_of(list.begin(), list.end(), [&](const Res& o) { return o.w == r.w && o.h == r.h; })) list.push_back(r);
    }
    std::sort(list.begin(), list.end(), [](const Res& a, const Res& b) { return a.w * a.h < b.w * b.h; });
    return list;
}

const std::vector<uint32_t> kPercents = {50, 75, 100, 125, 150, 200, 300, 400};
const std::vector<uint32_t> kCaps = {30, 60, app::kFpsCapRefresh, 0};
const std::vector<uint32_t> kAniso = {0, 2, 4, 8, 16};

Card Switch(const char* name, const char* help, std::function<bool&()> flag, const char* note = nullptr) {
    Card c{name, help, kSwitch};
    c.on = [flag] { return flag(); };
    c.step = [flag](int) {
        flag() = !flag();
        Changed();
    };
    if (note) c.value = [flag, note] { return std::string(flag() ? note : ""); };
    return c;
}

void ResetAll() {
    app::Get() = app::Settings{};
    for (const app::MenuItem& m : app::kGame.menu) {
        if (m.flag) *m.flag = false;
        if (m.value) *m.value = 0;
    }
    input::ResetKeys();
    audio::SetOutputMode(app::Get().audio_output);
    audio::SetVolume(app::Get().volume);
    Changed(true);
}

std::vector<Tab> Build() {
    std::vector<Tab> tabs;
    Tab display{"Display", 0xE7F4, {}};
    {
        Card c{"Display mode", "How the game window fills the screen.", kChoice};
        c.value = [] {
            const char* n[] = {"Fullscreen", "Borderless", "Window"};
            return std::string(n[uint32_t(app::Get().display_mode) % 3]);
        };
        c.step = [](int dir) {
            app::Get().display_mode = app::DisplayMode((uint32_t(app::Get().display_mode) + 3 + dir) % 3);
            Changed(true);
        };
        c.list = [] { return std::vector<std::string>{"Fullscreen", "Borderless", "Window"}; };
        c.index = [] { return int(uint32_t(app::Get().display_mode) % 3); };
        c.pick = [](int i) {
            app::Get().display_mode = app::DisplayMode(i);
            Changed(true);
        };
        display.cards.push_back(c);
    }
    {
        Card c{"Resolution", "The screen mode. Desktop follows your monitor.", kChoice};
        c.value = [] {
            const auto& s = app::Get();
            if (!s.width || !s.height) return std::string("Desktop");
            return std::to_string(s.width) + " x " + std::to_string(s.height);
        };
        c.step = [](int dir) {
            auto& s = app::Get();
            const auto& list = Resolutions();
            int i = -1;  // the desktop
            for (size_t k = 0; k < list.size(); k++)
                if (list[k].w == s.width && list[k].h == s.height) i = int(k);
            i += dir;
            if (i < -1) i = int(list.size()) - 1;
            if (i >= int(list.size())) i = -1;
            s.width = i < 0 ? 0 : list[size_t(i)].w;
            s.height = i < 0 ? 0 : list[size_t(i)].h;
            Changed(true);
        };
        // the list: the desktop, then the biggest modes first
        c.list = [] {
            std::vector<std::string> v{"Desktop"};
            const auto& list = Resolutions();
            for (auto r = list.rbegin(); r != list.rend(); ++r) v.push_back(std::to_string(r->w) + " x " + std::to_string(r->h));
            return v;
        };
        c.index = [] {
            const auto& s = app::Get();
            const auto& list = Resolutions();
            for (size_t k = 0; k < list.size(); k++)
                if (list[k].w == s.width && list[k].h == s.height) return int(list.size() - k);
            return 0;
        };
        c.pick = [](int i) {
            auto& s = app::Get();
            const auto& list = Resolutions();
            bool desktop = i <= 0 || i > int(list.size());
            s.width = desktop ? 0 : list[list.size() - size_t(i)].w;
            s.height = desktop ? 0 : list[list.size() - size_t(i)].h;
            Changed(true);
        };
        display.cards.push_back(c);
    }
    {
        Card c{"Frame rate cap", "The most frames drawn each second.", kChoice};
        c.value = [] {
            uint32_t v = app::Get().fps_cap;
            return v == 0 ? std::string("Unlimited") : v == app::kFpsCapRefresh ? std::string("Monitor") : std::to_string(v) + " fps";
        };
        c.step = [](int dir) {
            Cycle(app::Get().fps_cap, kCaps, dir);
            Changed();
        };
        c.list = [] { return std::vector<std::string>{"30 fps", "60 fps", "Monitor", "Unlimited"}; };
        c.index = [] { return int(std::find(kCaps.begin(), kCaps.end(), app::Get().fps_cap) - kCaps.begin()) % int(kCaps.size()); };
        c.pick = [](int i) {
            app::Get().fps_cap = kCaps[size_t(i) % kCaps.size()];
            Changed();
        };
        display.cards.push_back(c);
    }
    display.cards.push_back(Switch("VSync", "Waits for the screen: no tearing.", []() -> bool& { return app::Get().vsync; }));
    {
        Card c{"Aspect ratio", app::kGame.widescreen ? "Widescreen, or the 4:3 of an old TV." : "Widescreen shows more of the world; the HUD stretches.", kChoice};
        c.value = [] {
            static const bool at_start = app::Get().aspect_4_3;  // read once, at start
            std::string v = app::Get().aspect_4_3 ? "4:3" : "Widescreen";
            return app::Get().aspect_4_3 != at_start ? v + " (restart)" : v;
        };
        c.step = [](int) {
            app::Get().aspect_4_3 = !app::Get().aspect_4_3;
            Changed();
        };
        c.list = [] { return std::vector<std::string>{"Widescreen", "4:3"}; };
        c.index = [] { return app::Get().aspect_4_3 ? 1 : 0; };
        c.pick = [](int i) {
            app::Get().aspect_4_3 = i == 1;
            Changed();
        };
        display.cards.push_back(c);
    }
    display.cards.push_back(Switch("Show FPS", "A frame rate counter in the corner.", []() -> bool& { return app::Get().show_fps; }));
    tabs.push_back(display);

    Tab graphics{"Graphics", 0xE790, {}};
    {
        Card c{"Render resolution", "Sharper picture for more GPU work.", kSlider};
        c.value = [] {
            const auto& s = app::Get();
            return std::to_string(s.render_percent) + "%  " + std::to_string(app::Output().height * s.render_percent / 100) + "p";
        };
        c.fill = [] { return Where(app::Get().render_percent, kPercents); };
        c.step = [](int dir) {
            auto& v = app::Get().render_percent;
            size_t i = std::find(kPercents.begin(), kPercents.end(), v) - kPercents.begin();
            if (i >= kPercents.size()) i = 2;
            i = size_t(std::clamp(int(i) + dir, 0, int(kPercents.size()) - 1));
            v = kPercents[i];
            Changed();
        };
        c.set = [](float f) {
            uint32_t v = kPercents[size_t(std::lround(f * float(kPercents.size() - 1)))];
            if (v != app::Get().render_percent) app::Get().render_percent = v, Changed();
        };
        graphics.cards.push_back(c);
    }
    {
        Card c{"Texture filtering", "Keeps distant surfaces sharp.", kChoice};
        c.value = [] {
            uint32_t a = app::Get().aniso;
            return a ? std::to_string(a) + "x anisotropic" : std::string("As the game sets");
        };
        c.step = [](int dir) {
            Cycle(app::Get().aniso, kAniso, dir);
            Changed();
        };
        c.list = [] { return std::vector<std::string>{"As the game sets", "2x anisotropic", "4x anisotropic", "8x anisotropic", "16x anisotropic"}; };
        c.index = [] { return int(std::find(kAniso.begin(), kAniso.end(), app::Get().aniso) - kAniso.begin()) % int(kAniso.size()); };
        c.pick = [](int i) {
            app::Get().aniso = kAniso[size_t(i) % kAniso.size()];
            Changed();
        };
        graphics.cards.push_back(c);
    }
    graphics.cards.push_back(Switch("Texture packs", "Images in textures\\replacement replace the game's.",
                                    []() -> bool& { return app::Get().custom_textures; }));
    tabs.push_back(graphics);

    Tab sound{"Audio", 0xE767, {}};
    {
        Card c{"Master volume", "Everything the game plays.", kSlider};
        c.value = [] { return std::to_string(app::Get().volume) + "%"; };
        c.fill = [] { return app::Get().volume / 100.0f; };
        c.step = [](int dir) {
            auto& v = app::Get().volume;
            v = uint32_t(std::clamp(int(v) + dir * 5, 0, 100));
            audio::SetVolume(v);
            Changed();
        };
        c.set = [](float f) {
            uint32_t v = uint32_t(std::lround(f * 20)) * 5;
            if (v != app::Get().volume) app::Get().volume = v, audio::SetVolume(v), Changed();
        };
        sound.cards.push_back(c);
    }
    {
        Card c{"Speakers", "Automatic picks surround when your device has it.", kChoice};
        c.value = [] {
            const char* n[] = {"Automatic", "Stereo", "Surround 5.1", "Mono"};
            return std::string(n[app::Get().audio_output % 4]);
        };
        c.step = [](int dir) {
            auto& s = app::Get();
            s.audio_output = (s.audio_output + 4 + dir) % 4;
            audio::SetOutputMode(s.audio_output);
            Changed();
        };
        c.list = [] { return std::vector<std::string>{"Automatic", "Stereo", "Surround 5.1", "Mono"}; };
        c.index = [] { return int(app::Get().audio_output % 4); };
        c.pick = [](int i) {
            app::Get().audio_output = uint32_t(i);
            audio::SetOutputMode(uint32_t(i));
            Changed();
        };
        sound.cards.push_back(c);
    }
    tabs.push_back(sound);

    if (input::KeyControls()) {
        bool mouse = app::kGame.mouse_stick != nullptr;  // mouse play, or a control on a mouse button by default
        for (const app::KeyControl& k : app::kGame.keys)
            mouse |= k.keys && (std::strstr(k.keys, "Mouse") || std::strstr(k.keys, "Scroll"));
        Tab keys{mouse ? "Mouse & keys" : "Keyboard", 0xE765, {}};
        if (app::kGame.mouse_stick) {
            Card c{"Mouse sensitivity", std::string("How far the mouse moves the ") + (std::strcmp(app::kGame.mouse_stick, "left") ? "right" : "left") + " stick.", kSlider};
            c.value = [] { return std::to_string(app::Get().mouse_sensitivity) + "%"; };
            c.fill = [] { return (app::Get().mouse_sensitivity - 25) / 275.0f; };
            c.step = [](int dir) {
                auto& v = app::Get().mouse_sensitivity;
                v = uint32_t(std::clamp(int(v) + dir * 25, 25, 300));
                Changed();
            };
            c.set = [](float f) {
                uint32_t v = 25 + uint32_t(std::lround(f * 11)) * 25;
                if (v != app::Get().mouse_sensitivity) app::Get().mouse_sensitivity = v, Changed();
            };
            keys.cards.push_back(c);
        }
        for (int i = 0; i < input::KeyControls(); i++) {
            Card c{input::KeyLabel(i), "", kKey};
            c.key = i;
            keys.cards.push_back(c);
        }
        Card reset{"Reset keyboard", "The game's own keys again.", kButton};
        reset.press = [] {
            input::ResetKeys();
            Changed();
        };
        reset.danger = true;
        keys.cards.push_back(reset);
        tabs.push_back(keys);
    }

    // the game's own switches, a tab each in the order first named
    for (const app::MenuItem& m : app::kGame.menu) {
        auto t = std::find_if(tabs.begin(), tabs.end(), [&](const Tab& x) { return x.name == m.tab; });
        if (t == tabs.end()) {
            std::string n = m.tab;
            tabs.push_back({n, n == "Cheats" ? 0xE945u : 0xE7FCu, {}});
            t = tabs.end() - 1;
        }
        if (m.value && !m.choices.empty()) {
            Card c{m.name, m.help ? m.help : "", kChoice};
            uint32_t* v = m.value;
            std::vector<std::string> names(m.choices.begin(), m.choices.end());
            int n = int(names.size());
            c.value = [v, names] { return names[*v % names.size()]; };
            c.step = [v, n](int dir) {
                *v = uint32_t((int(*v) + dir + n) % n);
                Changed();
            };
            c.list = [names] { return names; };
            c.index = [v, n] { return int(*v) % n; };
            c.pick = [v, n](int i) {
                *v = uint32_t(i % n);
                Changed();
            };
            t->cards.push_back(c);
            continue;
        }
        bool* flag = m.flag;
        t->cards.push_back(Switch(m.name, m.help ? m.help : "", [flag]() -> bool& { return *flag; }, m.note));
    }

    Tab about{"About", 0xE946, {}};
    {
        Card info{"", "", kInfo};
        info.wide = true;
        about.cards.push_back(info);
        Card back{"Back to game", "Or press B, Esc or F1.", kButton};
        back.press = [] { g_open = false; };
        about.cards.push_back(back);
        Card again{"Setup at next start", "Pick another copy of the game, or prepare it again.", kSwitch};
        again.on = [] { return !app::Get().setup_done; };
        again.step = [](int) {
            app::Get().setup_done = !app::Get().setup_done;
            Changed();
        };
        about.cards.push_back(again);
        Card reset{"Reset all settings", "Every option to its default.", kButton};
        reset.press = ResetAll;
        reset.danger = true;
        about.cards.push_back(reset);
    }
    tabs.push_back(about);
    return tabs;
}

// ---- navigation ----

std::vector<Tab> g_tabs;
int g_tab = 0;
std::vector<int> g_sel;  // per tab
bool g_editing = false;  // left/right change the card's value
int g_drop = -1;         // a choice card's list open for the mouse, by card
int g_drop_first = 0;    // its first entry shown
bool g_drop_fresh = false;
ImVec2 g_drop_anchor_tl, g_drop_anchor_br;  // the card it opened from
ImVec2 g_drop_tl, g_drop_br;                // the list, last frame
int g_drag = -1;         // a slider held by the mouse
int g_slot = 0;          // a key card's slot while editing
double g_confirm = -10;  // a danger button asks again until then
int g_confirm_card = -1;

// cards laid out in rows of one or two, interactive ones only
std::vector<std::vector<int>> Rows(const Tab& t) {
    std::vector<std::vector<int>> rows;
    for (int i = 0; i < int(t.cards.size()); i++) {
        const Card& c = t.cards[size_t(i)];
        if (c.wide || rows.empty() || rows.back().size() == 2 || t.cards[size_t(rows.back()[0])].wide) rows.push_back({});
        rows.back().push_back(i);
    }
    return rows;
}
bool Focusable(const Card& c) { return c.kind != kInfo; }

void Move(int dr, int dc) {
    const Tab& t = g_tabs[size_t(g_tab)];
    auto rows = Rows(t);
    int& sel = g_sel[size_t(g_tab)];
    int r = 0, c = 0;
    for (int i = 0; i < int(rows.size()); i++)
        for (int j = 0; j < int(rows[size_t(i)].size()); j++)
            if (rows[size_t(i)][size_t(j)] == sel) r = i, c = j;
    if (dc) {
        int nc = c + dc;
        if (nc >= 0 && nc < int(rows[size_t(r)].size()) && Focusable(t.cards[size_t(rows[size_t(r)][size_t(nc)])]))
            sel = rows[size_t(r)][size_t(nc)];
        return;
    }
    for (int nr = r + dr; nr >= 0 && nr < int(rows.size()); nr += dr) {
        const auto& row = rows[size_t(nr)];
        int idx = row[size_t(std::min(c, int(row.size()) - 1))];
        if (Focusable(t.cards[size_t(idx)])) {
            sel = idx;
            return;
        }
    }
}

void SwitchTab(int dir) {
    g_tab = (g_tab + int(g_tabs.size()) + dir) % int(g_tabs.size());
    g_editing = false;
    g_drop = -1;
}

void Activate(Card& c, int index) {
    if (c.enabled && !c.enabled()) return;
    switch (c.kind) {
        case kSwitch: c.step(1); break;
        case kChoice:
        case kSlider: g_editing = !g_editing; break;
        case kKey:
            if (g_editing) input::CaptureKey(c.key, g_slot);
            else g_editing = true, g_slot = 0;
            break;
        case kButton:
            if (c.danger && !(g_confirm_card == index && Now() < g_confirm)) {
                g_confirm = Now() + 3;
                g_confirm_card = index;
            } else {
                g_confirm = -10;
                c.press();
            }
            break;
        case kInfo: break;
    }
}

void Run(Cmd cmd) {
    if (cmd == kToggle) {
        if (g_setup) return;
        g_open = !g_open;
        g_editing = false;
        return;
    }
    if (!g_open || g_tabs.empty() || input::Capturing()) return;
    if (g_drop >= 0) {  // keys and the pad close the mouse's list first
        g_drop = -1;
        if (cmd == kBack) return;
    }
    Tab& t = g_tabs[size_t(g_tab)];
    int sel = g_sel[size_t(g_tab)];
    Card& c = t.cards[size_t(sel)];
    switch (cmd) {
        case kBack:
            if (g_editing) g_editing = false;
            else if (!g_setup) g_open = false;
            break;
        case kTabPrev:
            if (!g_setup) SwitchTab(-1);
            break;
        case kTabNext:
            if (!g_setup) SwitchTab(1);
            break;
        case kUp:
        case kDown:
            g_editing = false;
            Move(cmd == kUp ? -1 : 1, 0);
            break;
        case kLeft:
        case kRight: {
            int dir = cmd == kLeft ? -1 : 1;
            if (g_editing && c.kind == kKey) g_slot = std::clamp(g_slot + dir, 0, input::kKeySlots - 1);
            else if (g_editing && c.step) c.step(dir);
            else Move(0, dir);
            break;
        }
        case kAccept: Activate(c, sel); break;
        case kClear:
            if (c.kind == kKey) {
                input::SetKey(c.key, g_editing ? g_slot : 0, 0);
                Changed();
            }
            break;
        default: break;
    }
}

// ---- drawing ----

float g_u = 1;  // pixels per 1080p pixel
float U(float v) { return v * g_u; }

ImVec2 TextSize(ImFont* f, float size, const std::string& s) { return f->CalcTextSizeA(size, FLT_MAX, 0, s.c_str()); }
void Text(ImDrawList* d, ImFont* f, float size, ImVec2 p, ImU32 col, const std::string& s) {
    d->AddText(f, size, p, col, s.c_str());
}
// a line cut with an ellipsis to fit width
std::string Fit(ImFont* f, float size, std::string s, float width) {
    if (TextSize(f, size, s).x <= width) return s;
    while (!s.empty() && TextSize(f, size, s + "\xE2\x80\xA6").x > width) s.pop_back();
    return s + "\xE2\x80\xA6";
}

// a controller button glyph: a coloured disc with its letter, or a pill for a shoulder
float Glyph(ImDrawList* d, ImVec2 p, const char* label, ImU32 col, float a) {
    float h = U(26);
    float w = std::max(h, TextSize(g_bold, U(15), label).x + U(16));
    ImVec2 b = {p.x + w, p.y + h};
    if (w == h) d->AddCircleFilled({p.x + h / 2, p.y + h / 2}, h / 2, Fade(col, a), 24);
    else d->AddRectFilled(p, b, Fade(col, a), h / 2);
    ImVec2 ts = TextSize(g_bold, U(15), label);
    Text(d, g_bold, U(15), {p.x + (w - ts.x) / 2, p.y + (h - ts.y) / 2}, Fade(kText, a), label);
    return w;
}
float Hint(ImDrawList* d, ImVec2 p, const char* glyph, ImU32 col, const char* what, float a) {
    float x = p.x + Glyph(d, p, glyph, col, a) + U(9);
    Text(d, g_body, U(18), {x, p.y + U(2)}, Fade(kSoft, a), what);
    return x + TextSize(g_body, U(18), what).x + U(26) - p.x;
}

std::map<int, float> g_knob;  // switch knobs, eased
float Ease(float& v, float to, float speed) {
    float dt = ImGui::GetIO().DeltaTime;
    v += (to - v) * std::min(1.0f, dt * speed);
    if (std::fabs(to - v) < 0.001f) v = to;
    return v;
}

// a card's value widget at its right edge; returns the left edge it used
float Widget(ImDrawList* d, Card& c, int id, ImVec2 tl, ImVec2 br, bool sel, float a) {
    float right = br.x - U(22), cy = (tl.y + br.y) / 2;
    bool edit = sel && g_editing;
    switch (c.kind) {
        case kSwitch: {
            bool on = c.on();
            float w = U(54), h = U(30);
            ImVec2 p{right - w, cy - h / 2};
            float& k = g_knob.try_emplace(id, on ? 1.0f : 0.0f).first->second;
            Ease(k, on ? 1.0f : 0.0f, 14);
            ImVec4 off = ImGui::ColorConvertU32ToFloat4(Rgba(255, 255, 255, 40)), lit = ImGui::ColorConvertU32ToFloat4(kAccent);
            ImU32 track = ImGui::ColorConvertFloat4ToU32({off.x + (lit.x - off.x) * k, off.y + (lit.y - off.y) * k,
                                                          off.z + (lit.z - off.z) * k, off.w + (lit.w - off.w) * k});
            d->AddRectFilled(p, {p.x + w, p.y + h}, Fade(track, a), h / 2);
            float r = h / 2 - U(4);
            d->AddCircleFilled({p.x + h / 2 + (w - h) * k, cy}, r, Fade(Rgba(255, 255, 255), a), 24);
            float left = p.x;
            if (c.value) {
                std::string note = c.value();
                if (!note.empty()) {
                    ImVec2 ts = TextSize(g_body, U(16), note);
                    left = p.x - U(12) - ts.x;
                    Text(d, g_body, U(16), {left, cy - ts.y / 2}, Fade(kAccent, a), note);
                }
            }
            return left;
        }
        case kChoice: {
            std::string v = c.value();
            ImVec2 ts = TextSize(g_bold, U(19), v);
            float arrow = U(22);
            float x = right - ts.x - (edit ? arrow : 0);
            ImU32 vc = edit ? kAccent : kText;
            Text(d, g_bold, U(19), {x, cy - ts.y / 2}, Fade(vc, a), v);
            if (edit) {
                ImVec2 as = TextSize(g_bold, U(22), "\xE2\x80\xBA");
                Text(d, g_bold, U(22), {right - as.x, cy - as.y / 2 - U(1)}, Fade(kAccent, a), "\xE2\x80\xBA");
                Text(d, g_bold, U(22), {x - arrow, cy - as.y / 2 - U(1)}, Fade(kAccent, a), "\xE2\x80\xB9");
                return x - arrow;
            }
            return x;
        }
        case kSlider: {
            float tw = U(170), th = U(6);
            ImVec2 p{right - tw, cy - th / 2};
            float f = std::clamp(c.fill(), 0.0f, 1.0f);
            d->AddRectFilled(p, {p.x + tw, p.y + th}, Fade(Rgba(255, 255, 255, 40), a), th / 2);
            d->AddRectFilled(p, {p.x + tw * f, p.y + th}, Fade(kAccent, a), th / 2);
            d->AddCircleFilled({p.x + tw * f, cy}, edit ? U(11) : U(9), Fade(edit ? kAccent : Rgba(255, 255, 255), a), 24);
            std::string v = c.value();
            ImVec2 ts = TextSize(g_bold, U(17), v);
            Text(d, g_bold, U(17), {p.x - U(16) - ts.x, cy - ts.y / 2}, Fade(edit ? kAccent : kText, a), v);
            return p.x - U(16) - ts.x;
        }
        case kKey: {
            float x = right;
            bool capturing = input::Capturing();
            for (int s = input::kKeySlots - 1; s >= 0; s--) {
                bool here = edit && s == g_slot;
                std::string k = here && capturing ? "Press a key" : input::KeyName(input::Key(c.key, s));
                if (k.empty()) k = "\xE2\x80\x94";
                ImVec2 ts = TextSize(g_bold, U(16), k);
                float w = std::max(U(64), ts.x + U(22)), h = U(32);
                ImVec2 p{x - w, cy - h / 2};
                float pulse = here && capturing ? 0.6f + 0.4f * float(std::sin(Now() * 6)) : 1.0f;
                d->AddRectFilled(p, {p.x + w, p.y + h}, Fade(here ? Rgba(46, 217, 199, 50) : Rgba(255, 255, 255, 18), a), U(8));
                if (here) d->AddRect(p, {p.x + w, p.y + h}, Fade(kAccent, a * pulse), U(8), 0, U(2));
                Text(d, g_bold, U(16), {p.x + (w - ts.x) / 2, cy - ts.y / 2}, Fade(here ? kAccent : kText, a), k);
                x = p.x - U(10);
            }
            return x;
        }
        case kButton: {
            bool asking = c.danger && g_confirm_card == id % 1000 && Now() < g_confirm;
            const char* label = asking ? "Press again to confirm" : "\xE2\x80\xBA";
            ImVec2 ts = TextSize(g_bold, asking ? U(16) : U(24), label);
            Text(d, g_bold, asking ? U(16) : U(24), {right - ts.x, cy - ts.y / 2}, Fade(asking ? Rgba(255, 120, 100) : kSoft, a), label);
            return right - ts.x;
        }
        case kInfo: break;
    }
    return right;
}

// the About card: the game, its version and how far along the port is
void InfoCard(ImDrawList* d, ImVec2 tl, ImVec2 br, float a) {
    TierLook tier = Tier();
    float x = tl.x + U(24), y = tl.y + U(20);
    std::string title;
    for (const wchar_t* w = app::kGame.title; w && *w; w++) title += Utf8(uint32_t(*w));
    Text(d, g_bold, U(24), {x, y}, Fade(kText, a), title);
    std::string ver = app::kGame.version ? std::string("Version ") + app::kGame.version : "";
    if (!ver.empty()) Text(d, g_body, U(17), {x, y + U(34)}, Fade(kSoft, a), ver);
    if (tier.name) {
        Text(d, g_body, U(17), {x, y + U(62)}, Fade(kSoft, a), Fit(g_body, U(17), tier.about, br.x - x - U(24)));
    }
    Text(d, g_body, U(15), {x, br.y - U(30)}, Fade(Rgba(154, 166, 178, 170), a),
         "Changes save as you make them. Built on XboxXBE-Recomp.");
}

// The launcher's badge, top right of the header: the OneDeck logo and "A OneDeck port", opening the launcher's page.
// right is its right edge, y the header's text line. The logo is drawn as in its SVG (a 120 box): two cards 56x78
// with corners 9, the back one at 33,17, the front one at 26,24, and a play triangle 47.5,51 67.5,63 47.5,75.
void LauncherBadge(ImDrawList* d, ImVec2 right, bool mouse, bool click, float a) {
    const char* label = "A OneDeck port";
    float fs = U(14), s = U(16);  // the logo's height
    float k = s / 85.0f;          // the cards span x 26-89, y 17-102 of the box
    ImVec2 ts = TextSize(g_body, fs, label);
    float lw = 63 * k, w = lw + U(7) + ts.x, x = right.x - w, y = right.y;
    bool hover = mouse && ImGui::IsMouseHoveringRect({x - U(4), y - U(4)}, {right.x + U(4), y + s + U(4)});
    if (hover) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    if (hover && click) ShellExecuteW(nullptr, L"open", L"https://onedeck.org", nullptr, nullptr, SW_SHOWNORMAL);
    auto at = [&](float bx, float by) { return ImVec2{x + (bx - 26) * k, y + (by - 17) * k}; };
    float fade = hover ? 1.0f : 0.85f;
    d->AddRectFilled(at(33, 17), at(89, 95), Fade(Rgba(70, 181, 170), a * fade), 9 * k);           // back card
    d->AddRectFilled(at(26, 24), at(82, 102), Fade(Rgba(117, 228, 216), a * fade), 9 * k);         // front card
    d->AddTriangleFilled(at(47.5f, 51), at(67.5f, 63), at(47.5f, 75), Fade(Rgba(15, 27, 38), a));  // play
    Text(d, g_body, fs, {x + lw + U(7), y + (s - ts.y) / 2}, Fade(hover ? kText : kSoft, a), label);
}

float g_show = 0;  // panel open, eased 0 to 1
float g_tab_x = -1, g_tab_w = 0;  // underline, eased
ImVec2 g_hl_tl{-1, -1}, g_hl_br{-1, -1};  // selection outline, eased
float g_scroll = 0;
ImVec2 g_last_mouse{-1, -1};

void DrawPanel(double) {
    ImGuiIO& io = ImGui::GetIO();
    float a = g_show;
    ImVec2 ds = io.DisplaySize;
    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize(ds);
    ImGui::Begin("##options", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove);
    ImDrawList* d = ImGui::GetWindowDrawList();
    bool mouse_moved = io.MousePos.x != g_last_mouse.x || io.MousePos.y != g_last_mouse.y;
    g_last_mouse = io.MousePos;
    bool mouse = g_open && ImGui::IsWindowHovered() && !input::Capturing();
    bool click = mouse && ImGui::IsMouseClicked(ImGuiMouseButton_Left);

    if (!g_setup) d->AddRectFilled({0, 0}, ds, Fade(Rgba(4, 8, 12, 175), a));
    float pw = std::min(U(1180), ds.x - U(60)), ph = std::min(U(700), ds.y - U(60));
    if (g_setup) pw = ds.x - U(40), ph = ds.y - U(40);  // the window is the panel
    ImVec2 tl{(ds.x - pw) / 2, (ds.y - ph) / 2 + (1 - a) * U(26)};
    ImVec2 br{tl.x + pw, tl.y + ph};
    d->AddRectFilled({tl.x + U(4), tl.y + U(10)}, {br.x + U(4), br.y + U(14)}, Fade(Rgba(0, 0, 0, 90), a), U(22));
    d->AddRectFilled(tl, br, Fade(kPanel, a), U(22));
    d->AddRect(tl, br, Fade(kLine, a), U(22), 0, U(1));

    // header: the game and its tier
    float px = tl.x + U(40);
    std::string title;
    for (const wchar_t* w = app::kGame.short_title; w && *w; w++) title += Utf8(uint32_t(*w));
    Text(d, g_bold, U(30), {px, tl.y + U(30)}, Fade(kText, a), title);
    float hx = px + TextSize(g_bold, U(30), title).x + U(16);
    TierLook tier = Tier();
    if (tier.name) {
        ImVec2 ts = TextSize(g_bold, U(14), tier.name);
        ImVec2 p{hx, tl.y + U(39)}, q{hx + ts.x + U(20), tl.y + U(39) + U(24)};
        d->AddRectFilled(p, q, Fade((tier.color & ~IM_COL32_A_MASK) | IM_COL32(0, 0, 0, 46), a), U(12));
        d->AddRect(p, q, Fade(tier.color, a), U(12), 0, U(1.5f));
        Text(d, g_bold, U(14), {p.x + U(10), p.y + (U(24) - ts.y) / 2}, Fade(tier.color, a), tier.name);
        hx = q.x + U(12);
    }
    if (app::kGame.version) Text(d, g_body, U(17), {hx, tl.y + U(40)}, Fade(kSoft, a), std::string("v") + app::kGame.version);
    {
        const char* close = g_setup ? "" : "F1  /  Back + Start";
        ImVec2 ts = TextSize(g_body, U(16), close);
        float cx = br.x - U(40) - ts.x;
        Text(d, g_body, U(16), {cx, tl.y + U(42)}, Fade(Rgba(154, 166, 178, 150), a), close);
        LauncherBadge(d, {g_setup ? br.x - U(40) : cx - U(28), tl.y + U(42)}, mouse, click, a);
    }

    // tabs
    float ty = tl.y + U(96), tx = px;
    for (int i = 0; i < int(g_tabs.size()); i++) {
        const Tab& t = g_tabs[size_t(i)];
        std::string icon = g_icons ? Utf8(t.icon) + "  " : "";
        std::string label = icon + t.name;
        ImVec2 ts = TextSize(g_bold, U(20), label);
        bool on = i == g_tab;
        bool hover = mouse && ImGui::IsMouseHoveringRect({tx - U(6), ty - U(8)}, {tx + ts.x + U(6), ty + ts.y + U(12)});
        if (hover && click) g_tab = i, g_editing = false;
        Text(d, g_bold, U(20), {tx, ty}, Fade(on ? kText : hover ? Rgba(210, 216, 222) : kSoft, a), label);
        if (on) {
            if (g_tab_x < 0) g_tab_x = tx, g_tab_w = ts.x;
            Ease(g_tab_x, tx, 16);
            Ease(g_tab_w, ts.x, 16);
        }
        tx += ts.x + U(38);
    }
    float uy = ty + U(36);
    d->AddLine({px, uy + U(1)}, {br.x - U(40), uy + U(1)}, Fade(kLine, a), U(1));
    d->AddRectFilled({g_tab_x, uy - U(1)}, {g_tab_x + g_tab_w, uy + U(2)}, Fade(kAccent, a), U(2));

    // cards
    Tab& tab = g_tabs[size_t(g_tab)];
    int& sel = g_sel[size_t(g_tab)];
    float cx0 = px, cw = (br.x - U(40) - px - U(18)) / 2, ch = U(96), gap = U(18);
    float area_t = uy + U(26), area_b = br.y - U(84);
    auto rows = Rows(tab);
    std::vector<std::pair<ImVec2, ImVec2>> rects(tab.cards.size());
    float y = 0;
    int footer_card = -1;
    for (const auto& row : rows) {
        if (tab.cards[size_t(row[0])].footer) {
            footer_card = row[0];
            continue;
        }
        bool info = tab.cards[size_t(row[0])].kind == kInfo;
        float h = tab.cards[size_t(row[0])].height > 0 ? U(tab.cards[size_t(row[0])].height) : info ? U(150) : ch;
        for (size_t j = 0; j < row.size(); j++) {
            const Card& c = tab.cards[size_t(row[j])];
            float w = c.wide ? cw * 2 + gap : cw;
            float x = cx0 + float(j) * (cw + gap);
            rects[size_t(row[j])] = {{x, y}, {x + w, y + h}};
        }
        y += h + gap;
    }
    float content = y - gap, view = area_b - area_t;
    // keep the selected card in view
    float want = g_scroll;
    if (sel < int(rects.size()) && sel != footer_card) {
        float st = rects[size_t(sel)].first.y, sb = rects[size_t(sel)].second.y;
        if (st - want < 0) want = st;
        if (sb - want > view) want = sb - view;
    }
    if (mouse && io.MouseWheel != 0 && !g_editing && g_drop < 0 && content > view) want -= io.MouseWheel * U(80);
    want = std::clamp(want, 0.0f, std::max(0.0f, content - view));
    Ease(g_scroll, want, 14);

    // with a list open a click is the list's: on an entry it picks, anywhere else it closes the list
    bool drop_open = g_drop >= 0;
    bool card_click = click && !drop_open;
    bool opened_now = false;  // the click that opens a list doesn't close it
    d->PushClipRect({tl.x, area_t - U(6)}, {br.x, area_b + U(6)}, true);
    for (int i = 0; i < int(tab.cards.size()); i++) {
        Card& c = tab.cards[size_t(i)];
        if (c.footer) continue;
        ImVec2 ctl{rects[size_t(i)].first.x, area_t + rects[size_t(i)].first.y - g_scroll};
        ImVec2 cbr{rects[size_t(i)].second.x, area_t + rects[size_t(i)].second.y - g_scroll};
        bool is_sel = i == sel;
        bool hover = mouse && !drop_open && Focusable(c) && ImGui::IsMouseHoveringRect(ctl, cbr) && ctl.y < area_b && cbr.y > area_t;
        if (hover && mouse_moved && !g_editing && !input::Capturing()) sel = i, is_sel = true;
        d->AddRectFilled(ctl, cbr, Fade(is_sel ? kCardOn : kCard, a), U(14));
        bool off = c.enabled && !c.enabled();
        if (c.primary && !off) d->AddRectFilled(ctl, cbr, Fade(is_sel ? Rgba(70, 235, 216) : kAccent, a), U(14));
        if (c.kind == kInfo) {
            if (c.draw) c.draw(d, ctl, cbr, a);
            else InfoCard(d, ctl, cbr, a);
            continue;
        }
        float left = Widget(d, c, g_tab * 1000 + i, ctl, cbr, is_sel, a);
        float tx2 = ctl.x + U(24), room = left - tx2 - U(16);
        bool danger = c.danger && c.kind == kButton;
        // no help line: the name centred
        float ny = c.help.empty() ? (ctl.y + cbr.y) / 2 - TextSize(g_bold, U(20), c.name).y / 2 : ctl.y + U(22);
        ImU32 name_col = off ? Rgba(120, 130, 140) : c.primary ? Rgba(6, 30, 27) : danger ? Rgba(255, 140, 120) : kText;
        Text(d, g_bold, U(20), {tx2, ny}, Fade(name_col, a), Fit(g_bold, U(20), c.name, room));
        if (!c.help.empty()) Text(d, g_body, U(16), {tx2, ctl.y + U(52)}, Fade(kSoft, a), Fit(g_body, U(16), c.help, room));
        // mouse: a click flips or presses; on a value the left part steps back, the rest forward; the wheel steps
        float track_l = cbr.x - U(22) - U(170), track_r = cbr.x - U(22);  // a slider's track (Widget)
        if (hover && card_click) {
            sel = i;
            float mx = io.MousePos.x;
            if (c.kind == kChoice && c.list && !(c.enabled && !c.enabled())) {
                g_editing = false;
                g_drop = i;
                g_drop_fresh = true;
                opened_now = true;
                g_drop_anchor_tl = ctl, g_drop_anchor_br = cbr;
            } else if (c.kind == kSlider && c.set && mx >= track_l - U(14)) {
                g_editing = false;
                g_drag = i;  // set below for as long as it is held
            } else if (c.kind == kChoice || c.kind == kSlider) {
                g_editing = false;
                c.step(mx < (left + cbr.x) / 2 && mx > left - U(30) ? -1 : 1);
            } else if (c.kind == kKey) {
                g_editing = true;
                g_slot = mx > cbr.x - U(100) ? 1 : 0;
                input::CaptureKey(c.key, g_slot);
            } else {
                Activate(c, i);
            }
        }
        if (g_drag == i) {
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) g_drag = -1;
            else c.set(std::clamp((io.MousePos.x - track_l) / (track_r - track_l), 0.0f, 1.0f));
        }
        if (hover && io.MouseWheel != 0 && (c.kind == kChoice || c.kind == kSlider)) c.step(io.MouseWheel > 0 ? 1 : -1);
        if (hover && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && (c.kind == kChoice || c.kind == kSlider)) c.step(-1);
        if (is_sel) {
            if (g_hl_tl.x < 0) g_hl_tl = ctl, g_hl_br = cbr;
            Ease(g_hl_tl.x, ctl.x, 20), Ease(g_hl_tl.y, ctl.y, 20), Ease(g_hl_br.x, cbr.x, 20), Ease(g_hl_br.y, cbr.y, 20);
        }
    }
    if (sel != footer_card)
        d->AddRect(g_hl_tl, g_hl_br, Fade(kAccent, a * (g_editing ? 1.0f : 0.85f)), U(14), 0, g_editing ? U(3) : U(2));
    d->PopClipRect();
    if (g_drop >= 0 && g_drop < int(tab.cards.size()) && tab.cards[size_t(g_drop)].list) {
        // the choice's list under its card (over it near the bottom), the current entry marked, a wheel when long
        Card& c = tab.cards[size_t(g_drop)];
        std::vector<std::string> items = c.list();
        int n = int(items.size()), cur = c.index ? c.index() : -1;
        constexpr int kShown = 8;
        int shown = std::min(n, kShown);
        float ih = U(40), w = U(240);
        for (const std::string& it : items) w = std::max(w, TextSize(g_body, U(18), it).x + U(64));
        float h = ih * float(shown) + U(12);
        float x1 = g_drop_anchor_br.x - U(12), x0 = x1 - w;
        float y0 = g_drop_anchor_br.y + U(6);
        if (y0 + h > br.y - U(12)) y0 = g_drop_anchor_tl.y - U(6) - h;
        ImVec2 dtl{x0, y0}, dbr{x1, y0 + h};
        bool over = ImGui::IsMouseHoveringRect(dtl, dbr);
        if (g_drop_fresh) g_drop_first = std::clamp(cur - shown / 2, 0, std::max(0, n - shown)), g_drop_fresh = false;
        if (over && io.MouseWheel != 0) g_drop_first = std::clamp(g_drop_first - int(io.MouseWheel), 0, std::max(0, n - shown));
        d->AddRectFilled({dtl.x + U(3), dtl.y + U(8)}, {dbr.x + U(3), dbr.y + U(12)}, Fade(Rgba(0, 0, 0, 110), a), U(14));
        d->AddRectFilled(dtl, dbr, Fade(Rgba(24, 31, 39, 252), a), U(12));
        d->AddRect(dtl, dbr, Fade(Rgba(46, 217, 199, 120), a), U(12), 0, U(1.5f));
        int picked = -1;
        for (int k = g_drop_first; k < g_drop_first + shown; k++) {
            ImVec2 rtl{dtl.x + U(6), dtl.y + U(6) + ih * float(k - g_drop_first)}, rbr{dbr.x - U(6), rtl.y + ih};
            bool hot = ImGui::IsMouseHoveringRect(rtl, rbr);
            if (hot) d->AddRectFilled(rtl, rbr, Fade(Rgba(255, 255, 255, 22), a), U(8));
            ImVec2 ts = TextSize(g_body, U(18), items[size_t(k)]);
            Text(d, k == cur ? g_bold : g_body, U(18), {rtl.x + U(14), rtl.y + (ih - ts.y) / 2}, Fade(k == cur ? kAccent : kText, a),
                 items[size_t(k)]);
            if (k == cur) d->AddCircleFilled({rbr.x - U(18), (rtl.y + rbr.y) / 2}, U(4), Fade(kAccent, a), 12);
            if (hot && click) picked = k;
        }
        // more above or below: a small arrow at that edge
        float ax = x1 - U(22), as = U(5);
        if (g_drop_first > 0)
            d->AddTriangleFilled({ax - as, dtl.y + U(10)}, {ax + as, dtl.y + U(10)}, {ax, dtl.y + U(5)}, Fade(kSoft, a));
        if (g_drop_first + shown < n)
            d->AddTriangleFilled({ax - as, dbr.y - U(10)}, {ax + as, dbr.y - U(10)}, {ax, dbr.y - U(5)}, Fade(kSoft, a));
        if (picked >= 0) {
            c.pick(picked);
            g_drop = -1;
        } else if (click && !over && !opened_now) {
            g_drop = -1;
        }
        g_drop_tl = dtl, g_drop_br = dbr;
    }
    // more below or above: a soft edge
    if (g_scroll > 1) d->AddRectFilledMultiColor({tl.x + U(2), area_t - U(6)}, {br.x - U(2), area_t + U(24)}, Fade(kPanel, a), Fade(kPanel, a), 0, 0);
    if (content - g_scroll > view + 1)
        d->AddRectFilledMultiColor({tl.x + U(2), area_b - U(24)}, {br.x - U(2), area_b + U(6)}, 0, 0, Fade(kPanel, a), Fade(kPanel, a));

    // footer: what the buttons do now
    float fy = br.y - U(56);
    d->AddLine({px, fy - U(16)}, {br.x - U(40), fy - U(16)}, Fade(kLine, a), U(1));
    float fx = px;
    const Card& sc = tab.cards[size_t(sel)];
    const ImU32 green = Rgba(76, 175, 80), red = Rgba(229, 72, 60), blue = Rgba(52, 120, 220), grey = Rgba(255, 255, 255, 45);
    if (input::Capturing()) {
        fx += Hint(d, {fx, fy}, "Esc", grey, "Cancel", a);
    } else if (g_editing && sc.kind == kKey) {
        fx += Hint(d, {fx, fy}, "A", green, "Bind", a);
        fx += Hint(d, {fx, fy}, "\xE2\x80\xB9\xE2\x80\xBA", grey, "Key slot", a);
        fx += Hint(d, {fx, fy}, "X", blue, "Clear", a);
        fx += Hint(d, {fx, fy}, "B", red, "Done", a);
    } else if (g_editing) {
        fx += Hint(d, {fx, fy}, "\xE2\x80\xB9\xE2\x80\xBA", grey, "Change", a);
        fx += Hint(d, {fx, fy}, "A", green, "Done", a);
    } else {
        const char* what = sc.kind == kSwitch ? "Switch" : sc.kind == kButton ? "Select" : sc.kind == kKey ? "Rebind" : "Change";
        fx += Hint(d, {fx, fy}, "A", green, what, a);
        if (!g_setup) {
            fx += Hint(d, {fx, fy}, "B", red, "Close", a);
            fx += Hint(d, {fx, fy}, "LB", grey, "", a) - U(26);
            fx += Hint(d, {fx, fy}, "RB", grey, "Tabs", a);
        }
    }
    if (footer_card >= 0) {
        // the footer's button: right-aligned, the accent when it can be pressed
        Card& c = tab.cards[size_t(footer_card)];
        bool off = c.enabled && !c.enabled(), is_sel = sel == footer_card;
        ImVec2 ts = TextSize(g_bold, U(22), c.name);
        float w = std::max(U(200), ts.x + U(80)), h = U(52);
        ImVec2 q{br.x - U(40), fy + U(14) + h / 2 - U(2)}, p{q.x - w, q.y - h};
        bool hover = mouse && ImGui::IsMouseHoveringRect(p, q);
        if (hover && mouse_moved && !g_editing) sel = footer_card, is_sel = true;
        if (hover && click) sel = footer_card, Activate(c, footer_card);
        ImU32 fill = off ? Rgba(255, 255, 255, 18) : is_sel ? Rgba(70, 235, 216) : kAccent;
        d->AddRectFilled(p, q, Fade(fill, a), h / 2);
        if (is_sel && !off) d->AddRect({p.x - U(4), p.y - U(4)}, {q.x + U(4), q.y + U(4)}, Fade(kAccent, a * 0.6f), h / 2 + U(4), 0, U(2));
        std::string label = c.name + "  \xE2\x80\xBA";
        ImVec2 ls = TextSize(g_bold, U(22), label);
        Text(d, g_bold, U(22), {p.x + (w - ls.x) / 2, p.y + (h - ls.y) / 2}, Fade(off ? Rgba(120, 130, 140) : Rgba(6, 30, 27), a), label);
        if (is_sel) g_hl_tl = p, g_hl_br = q;
    } else {
        const char* saved = "Saved automatically";
        ImVec2 ss = TextSize(g_body, U(16), saved);
        Text(d, g_body, U(16), {br.x - U(40) - ss.x, fy + U(4)}, Fade(Rgba(154, 166, 178, 150), a), saved);
    }
    ImGui::End();
}

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

// an info card: a heading in a colour and a line under it, cut to fit
void Headed(ImDrawList* d, ImVec2 tl, ImVec2 br, float a, ImU32 col, const std::string& head, const std::string& line) {
    float x = tl.x + U(24);
    Text(d, g_bold, U(22), {x, tl.y + U(20)}, Fade(col, a), head);
    Text(d, g_body, U(16), {x, tl.y + U(56)}, Fade(kSoft, a), Fit(g_body, U(16), line, br.x - x - U(24)));
}

// the setup window's one tab: the game, preparing it, the main display settings, Play
std::vector<Tab> BuildSetup(SetupModel& m) {
    Tab t{"Setup", 0xE713, {}};
    Card game{"", "", kInfo};
    game.wide = true;
    game.height = 100;
    game.draw = [&m](ImDrawList* d, ImVec2 tl, ImVec2 br, float a) {
        if (m.game_ok) Headed(d, tl, br, a, kAccent, "Found your game", Narrow(m.found));
        else if (m.other_release)
            Headed(d, tl, br, a, Rgba(255, 171, 51), "That is a different release of the game",
                   Narrow(m.found) + " is from another version than this port was made for, and would most likely crash.");
        else Headed(d, tl, br, a, kText, "Choose your game", "Its disc image (.iso), or a folder with default.xbe and the rest of the disc.");
    };
    t.cards.push_back(game);
    Card iso{"Choose disc image", "An .iso of your game disc.", kButton};
    iso.press = [&m] { m.pick_iso = true; };
    iso.enabled = [&m] { return m.code.state != app::GameCodeStatus::kRunning; };  // not while it prepares
    t.cards.push_back(iso);
    Card dir{"Choose game folder", "The disc's files: default.xbe and its folders.", kButton};
    dir.press = [&m] { m.pick_folder = true; };
    dir.enabled = [&m] { return m.code.state != app::GameCodeStatus::kRunning; };
    t.cards.push_back(dir);
    Card prep{"", "", kInfo};
    prep.wide = true;
    prep.height = 100;
    prep.draw = [&m](ImDrawList* d, ImVec2 tl, ImVec2 br, float a) {
        using S = app::GameCodeStatus;
        if (!m.blocker.empty()) return Headed(d, tl, br, a, Rgba(255, 120, 100), "Preparing cannot start", Narrow(m.blocker));
        switch (m.code.state) {
            case S::kRunning: {
                Headed(d, tl, br, a, kText, Narrow(m.code.stage), Narrow(m.code.time_left));
                float x0 = tl.x + U(24), x1 = br.x - U(24), y = br.y - U(22);
                d->AddRectFilled({x0, y}, {x1, y + U(6)}, Fade(Rgba(255, 255, 255, 30), a), U(3));
                d->AddRectFilled({x0, y}, {x0 + (x1 - x0) * std::clamp(m.code.fraction, 0.0f, 1.0f), y + U(6)}, Fade(kAccent, a), U(3));
                break;
            }
            case S::kReady: Headed(d, tl, br, a, kAccent, "Ready to play", "The game's code is built for this PC. Later starts skip this."); break;
            case S::kFailed: Headed(d, tl, br, a, Rgba(255, 120, 100), "Preparing failed", Narrow(m.code.failure)); break;
            default: Headed(d, tl, br, a, kSoft, "Preparing the game", "Starts by itself once your game is found: its code is built for this PC, once."); break;
        }
    };
    t.cards.push_back(prep);
    // the main settings, the menu's own cards
    std::vector<Tab> all = Build();
    for (const Tab& tab : all)
        for (const Card& c : tab.cards)
            if (c.name == "Display mode" || c.name == "Resolution" || c.name == "Frame rate cap" || c.name == "VSync" ||
                c.name == "Aspect ratio" || c.name == "Render resolution")
                t.cards.push_back(c);
    Card rm = Switch("Remove the compiler after", "", [&m]() -> bool& { return m.remove_compiler; });
    rm.help = "Frees its space once the game is ready. A new release brings it back.";
    t.cards.push_back(rm);
    Card play{"Play", "", kButton};
    play.wide = true;
    play.footer = true;
    play.enabled = [&m] { return m.game_ok && m.code.state == app::GameCodeStatus::kReady; };
    play.press = [&m] { m.play = true; };
    t.cards.push_back(play);
    return {t};
}

}  // namespace

void SetupUi(SetupModel& m) {
    if (!g_setup) {
        g_setup = true;
        g_tabs = BuildSetup(m);
        g_tab = 0;
        // first the game's buttons, or Play when it is all there
        g_sel.assign(1, m.game_ok ? int(g_tabs[0].cards.size()) - 1 : 1);
        g_editing = false;
        g_scroll = 0;
        g_hl_tl = {-1, -1};
        g_tab_x = -1;
    }
    std::vector<Cmd> cmds;
    {
        std::lock_guard lock(g_cmd_lock);
        cmds.swap(g_cmds);
    }
    g_open = true;
    for (Cmd c : cmds) Run(c);
    // Play becomes possible (the game found and prepared): it is selected, so A starts the game
    static bool could_play = false;
    bool can_play = m.game_ok && m.code.state == app::GameCodeStatus::kReady;
    if (can_play && !could_play && !g_editing) g_sel[0] = int(g_tabs[0].cards.size()) - 1;
    could_play = can_play;
    g_open = true;
    g_show = 1;
    ImGuiIO& io = ImGui::GetIO();
    g_u = std::min(io.DisplaySize.x / 1260.0f, io.DisplaySize.y / 820.0f);
    DrawPanel(0);
}

void SetupEnd() {
    g_setup = false;
    g_tabs.clear();
    g_sel.clear();
    g_open = false;
    g_show = 0;
    g_editing = false;
}

void Fonts() {
    ImGuiIO& io = ImGui::GetIO();
    char win[MAX_PATH] = {};
    GetWindowsDirectoryA(win, MAX_PATH);
    std::string dir = std::string(win) + "\\Fonts\\";
    auto exists = [](const std::string& p) { return GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES; };
    // baked for the monitor: 1.0 at 1080p
    float s = std::max(1.0f, float(GetSystemMetrics(SM_CYSCREEN)) / 1080.0f);
    static const ImWchar kText[] = {0x20, 0xFF, 0x2013, 0x2014, 0x2018, 0x201F, 0x2022, 0x2022, 0x2026, 0x2026, 0x2039, 0x203A, 0};
    static const ImWchar kIconRange[] = {0xE700, 0xF8B3, 0};
    ImFontConfig cfg;
    cfg.OversampleH = 2;
    cfg.PixelSnapH = false;
    std::string body = dir + "segoeui.ttf";
    if (exists(body)) {
        g_body = io.Fonts->AddFontFromFileTTF(body.c_str(), 24 * s, &cfg, kText);
        for (const char* icons : {"SegoeIcons.ttf", "segmdl2.ttf"}) {
            std::string p = dir + icons;
            if (!exists(p)) continue;
            ImFontConfig merge;
            merge.MergeMode = true;
            merge.GlyphOffset = {0, 3 * s};
            g_icons = io.Fonts->AddFontFromFileTTF(p.c_str(), 22 * s, &merge, kIconRange);
            break;
        }
        for (const char* bold : {"seguisb.ttf", "segoeuib.ttf"}) {
            std::string p = dir + bold;
            if (!exists(p)) continue;
            g_bold = io.Fonts->AddFontFromFileTTF(p.c_str(), 24 * s, &cfg, kText);
            if (g_icons) {
                ImFontConfig merge;
                merge.MergeMode = true;
                merge.GlyphOffset = {0, 3 * s};
                io.Fonts->AddFontFromFileTTF((dir + (exists(dir + "SegoeIcons.ttf") ? "SegoeIcons.ttf" : "segmdl2.ttf")).c_str(), 22 * s,
                                             &merge, kIconRange);
            }
            break;
        }
    }
    if (!g_body) g_body = io.Fonts->AddFontDefault();
    if (!g_bold) g_bold = g_body;
    io.FontDefault = g_body;
}

bool Open() { return g_open; }

bool Message(void*, uint32_t msg, uintptr_t wp, intptr_t, intptr_t* result) {
    if (msg != WM_KEYDOWN && msg != WM_SYSKEYDOWN) return false;
    if (wp == VK_F1) {
        Push(kToggle);
    } else if (g_open) {
        switch (wp) {
            case VK_UP: Push(kUp); break;
            case VK_DOWN: Push(kDown); break;
            case VK_LEFT: Push(kLeft); break;
            case VK_RIGHT: Push(kRight); break;
            case VK_RETURN:
            case VK_SPACE: Push(kAccept); break;
            case VK_ESCAPE:
            case VK_BACK: Push(kBack); break;
            case VK_DELETE: Push(kClear); break;
            case 'Q':
            case VK_PRIOR: Push(kTabPrev); break;
            case 'E':
            case VK_NEXT:
            case VK_TAB: Push(kTabNext); break;
            default: return false;
        }
    } else {
        return false;
    }
    *result = 0;
    return true;
}

void Pad(int port, input::Pad& p) {
    if (port != 0) return;
    input::SetKeysOff(g_open);  // while open the keys drive the menu through window messages, not the game
    constexpr uint16_t kUpB = 1, kDownB = 2, kLeftB = 4, kRightB = 8, kStartB = 16, kBackB = 32;
    static uint16_t prev = 0;
    static uint8_t prev_analog = 0;
    static double next_repeat = 0;
    uint16_t b = p.buttons;
    // the left stick counts as the d-pad in the menu
    if (p.ly > 16000) b |= kUpB;
    if (p.ly < -16000) b |= kDownB;
    if (p.lx < -16000) b |= kLeftB;
    if (p.lx > 16000) b |= kRightB;
    // A B X Y black(RB) white(LB) as bits
    uint8_t an = 0;
    for (int i = 0; i < 6; i++)
        if (p.analog[i] > 0x80) an |= uint8_t(1 << i);
    uint16_t pressed = b & ~prev;
    uint8_t apressed = an & ~prev_analog;
    if ((b & (kStartB | kBackB)) == (kStartB | kBackB) && (pressed & (kStartB | kBackB))) {
        Push(kToggle);
    } else if (g_open) {
        uint16_t dirs = b & (kUpB | kDownB | kLeftB | kRightB);
        double now = Now();
        uint16_t fire = pressed & dirs;
        if (fire) next_repeat = now + 0.4;
        else if (dirs && now >= next_repeat) fire = dirs, next_repeat = now + 0.11;
        if (fire & kUpB) Push(kUp);
        if (fire & kDownB) Push(kDown);
        if (fire & kLeftB) Push(kLeft);
        if (fire & kRightB) Push(kRight);
        if (apressed & 1) Push(kAccept);
        if (apressed & 2) Push(kBack);
        if (apressed & 4) Push(kClear);
        if (apressed & 32) Push(kTabPrev);
        if (apressed & 16) Push(kTabNext);
    }
    prev = b;
    prev_analog = an;
    if (g_open) {
        bool connected = p.connected;
        p = input::Pad{};
        p.connected = connected;
    }
}

void Ui(double fps) {
    std::vector<Cmd> cmds;
    {
        std::lock_guard lock(g_cmd_lock);
        cmds.swap(g_cmds);
    }
    if (g_tabs.empty()) {
        g_tabs = Build();
        g_sel.assign(g_tabs.size(), 0);
        for (size_t t = 0; t < g_tabs.size(); t++)
            while (g_sel[t] < int(g_tabs[t].cards.size()) - 1 && !Focusable(g_tabs[t].cards[size_t(g_sel[t])])) g_sel[t]++;
    }
    for (Cmd c : cmds) Run(c);

    ImGuiIO& io = ImGui::GetIO();
    g_u = io.DisplaySize.y / 1080.0f;
    ImDrawList* fg = ImGui::GetForegroundDrawList();
    if (app::Get().show_fps) {
        char t[32];
        std::snprintf(t, sizeof t, "%.0f FPS", fps);
        ImVec2 ts = TextSize(g_bold, U(17), t);
        ImVec2 p{U(16), U(16)};
        fg->AddRectFilled(p, {p.x + ts.x + U(20), p.y + ts.y + U(8)}, Rgba(10, 14, 18, 170), U(8));
        Text(fg, g_bold, U(17), {p.x + U(10), p.y + U(4)}, kText, t);
    }
    // a hint for the first seconds of a run
    double t = Now();
    if (!g_open && t > 1 && t < 11) {
        float alpha = float(std::min({1.0, t - 1, 11 - t}));
        const char* msg = "Settings";
        float w = U(26) + U(8) + TextSize(g_body, U(17), msg).x + U(20) + TextSize(g_body, U(15), "F1  /  Back + Start").x + U(28);
        ImVec2 br{io.DisplaySize.x - U(20), io.DisplaySize.y - U(20)}, tl{br.x - w, br.y - U(44)};
        ImDrawList* d = fg;
        d->AddRectFilled(tl, br, Fade(Rgba(10, 14, 18, 200), alpha), U(22));
        d->AddRect(tl, br, Fade(kLine, alpha), U(22), 0, U(1));
        float x = tl.x + U(10);
        std::string icon = g_icons ? Utf8(0xE713) : "";
        if (!icon.empty()) {
            Text(d, g_body, U(20), {x + U(3), tl.y + U(9)}, Fade(kAccent, alpha), icon);
            x += U(30);
        }
        Text(d, g_bold, U(17), {x, tl.y + U(11)}, Fade(kText, alpha), msg);
        x += TextSize(g_bold, U(17), msg).x + U(16);
        Text(d, g_body, U(15), {x, tl.y + U(13)}, Fade(kSoft, alpha), "F1  /  Back + Start");
    }
    if (!g_open) g_drop = -1, g_drag = -1;
    else input::FreeCursor();  // the options take the mouse
    Ease(g_show, g_open ? 1.0f : 0.0f, 16);
    if (g_show <= 0.01f) {
        g_hl_tl = {-1, -1};
        g_tab_x = -1;
        return;
    }
    DrawPanel(fps);
}

}  // namespace menu
