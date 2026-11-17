// The options menu (menu.h): a panel over the dimmed game with tabs across the top and settings as cards in two
// columns. Up/down/left/right move between cards, A flips a switch or starts changing a value (left/right then
// change it, A or B ends), LB/RB or Q/E switch tabs. The mouse clicks tabs and cards, its wheel steps a value.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
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
    for (const app::MenuItem& m : app::kGame.menu) *m.flag = false;
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
        display.cards.push_back(c);
    }
    display.cards.push_back(Switch("VSync", "Waits for the screen: no tearing.", []() -> bool& { return app::Get().vsync; }));
    {
        Card c{"Aspect ratio", "Widescreen, or the 4:3 of an old TV.", kChoice};
        c.value = [] {
            static const bool at_start = app::Get().aspect_4_3;  // read once, at start
            std::string v = app::Get().aspect_4_3 ? "4:3" : "Widescreen";
            return app::Get().aspect_4_3 != at_start ? v + " (restart)" : v;
        };
        c.step = [](int) {
            app::Get().aspect_4_3 = !app::Get().aspect_4_3;
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
        sound.cards.push_back(c);
    }
    tabs.push_back(sound);

    if (input::KeyControls()) {
        Tab keys{"Keyboard", 0xE765, {}};
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
}

void Activate(Card& c, int index) {
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
        g_open = !g_open;
        g_editing = false;
        return;
    }
    if (!g_open || g_tabs.empty() || input::Capturing()) return;
    Tab& t = g_tabs[size_t(g_tab)];
    int sel = g_sel[size_t(g_tab)];
    Card& c = t.cards[size_t(sel)];
    switch (cmd) {
        case kBack:
            if (g_editing) g_editing = false;
            else g_open = false;
            break;
        case kTabPrev: SwitchTab(-1); break;
        case kTabNext: SwitchTab(1); break;
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

    d->AddRectFilled({0, 0}, ds, Fade(Rgba(4, 8, 12, 175), a));
    float pw = std::min(U(1180), ds.x - U(60)), ph = std::min(U(700), ds.y - U(60));
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
        const char* close = "F1  /  Back + Start";
        ImVec2 ts = TextSize(g_body, U(16), close);
        Text(d, g_body, U(16), {br.x - U(40) - ts.x, tl.y + U(42)}, Fade(Rgba(154, 166, 178, 150), a), close);
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
    for (const auto& row : rows) {
        bool info = tab.cards[size_t(row[0])].kind == kInfo;
        float h = info ? U(150) : ch;
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
    if (sel < int(rects.size())) {
        float st = rects[size_t(sel)].first.y, sb = rects[size_t(sel)].second.y;
        if (st - want < 0) want = st;
        if (sb - want > view) want = sb - view;
    }
    if (mouse && io.MouseWheel != 0 && !g_editing && content > view) want -= io.MouseWheel * U(80);
    want = std::clamp(want, 0.0f, std::max(0.0f, content - view));
    Ease(g_scroll, want, 14);

    d->PushClipRect({tl.x, area_t - U(6)}, {br.x, area_b + U(6)}, true);
    for (int i = 0; i < int(tab.cards.size()); i++) {
        Card& c = tab.cards[size_t(i)];
        ImVec2 ctl{rects[size_t(i)].first.x, area_t + rects[size_t(i)].first.y - g_scroll};
        ImVec2 cbr{rects[size_t(i)].second.x, area_t + rects[size_t(i)].second.y - g_scroll};
        bool is_sel = i == sel;
        bool hover = mouse && Focusable(c) && ImGui::IsMouseHoveringRect(ctl, cbr) && ctl.y < area_b && cbr.y > area_t;
        if (hover && mouse_moved && !g_editing && !input::Capturing()) sel = i, is_sel = true;
        d->AddRectFilled(ctl, cbr, Fade(is_sel ? kCardOn : kCard, a), U(14));
        if (c.kind == kInfo) {
            InfoCard(d, ctl, cbr, a);
            continue;
        }
        float left = Widget(d, c, g_tab * 1000 + i, ctl, cbr, is_sel, a);
        float tx2 = ctl.x + U(24), room = left - tx2 - U(16);
        bool danger = c.danger && c.kind == kButton;
        Text(d, g_bold, U(20), {tx2, ctl.y + U(22)}, Fade(danger ? Rgba(255, 140, 120) : kText, a), Fit(g_bold, U(20), c.name, room));
        if (!c.help.empty()) Text(d, g_body, U(16), {tx2, ctl.y + U(52)}, Fade(kSoft, a), Fit(g_body, U(16), c.help, room));
        // mouse: a click flips or presses; on a value the left part steps back, the rest forward; the wheel steps
        if (hover && click) {
            sel = i;
            float mx = io.MousePos.x;
            if (c.kind == kChoice || c.kind == kSlider) {
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
        if (hover && io.MouseWheel != 0 && (c.kind == kChoice || c.kind == kSlider)) c.step(io.MouseWheel > 0 ? 1 : -1);
        if (hover && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && (c.kind == kChoice || c.kind == kSlider)) c.step(-1);
        if (is_sel) {
            if (g_hl_tl.x < 0) g_hl_tl = ctl, g_hl_br = cbr;
            Ease(g_hl_tl.x, ctl.x, 20), Ease(g_hl_tl.y, ctl.y, 20), Ease(g_hl_br.x, cbr.x, 20), Ease(g_hl_br.y, cbr.y, 20);
        }
    }
    d->AddRect(g_hl_tl, g_hl_br, Fade(kAccent, a * (g_editing ? 1.0f : 0.85f)), U(14), 0, g_editing ? U(3) : U(2));
    d->PopClipRect();
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
        fx += Hint(d, {fx, fy}, "B", red, "Close", a);
        fx += Hint(d, {fx, fy}, "LB", grey, "", a) - U(26);
        fx += Hint(d, {fx, fy}, "RB", grey, "Tabs", a);
    }
    const char* saved = "Saved automatically";
    ImVec2 ss = TextSize(g_body, U(16), saved);
    Text(d, g_body, U(16), {br.x - U(40) - ss.x, fy + U(4)}, Fade(Rgba(154, 166, 178, 150), a), saved);
    ImGui::End();
}

}  // namespace

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
    Ease(g_show, g_open ? 1.0f : 0.0f, 16);
    if (g_show <= 0.01f) {
        g_hl_tl = {-1, -1};
        g_tab_x = -1;
        return;
    }
    DrawPanel(fps);
}

}  // namespace menu
