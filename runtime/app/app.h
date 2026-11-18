#pragma once
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace input {
struct Pad;
}

// the host side: window, settings
namespace app {

enum class DisplayMode : uint32_t { Fullscreen, Borderless, Windowed };
inline constexpr uint32_t kFpsCapRefresh = 1;  // fps_cap: the monitor's refresh rate

struct Settings {
    DisplayMode display_mode = DisplayMode::Borderless;
    uint32_t width = 0, height = 0;  // 0 is the desktop mode
    uint32_t render_percent = 100;   // of the output height
    uint32_t fps_cap = 60;           // 0 unlimited, kFpsCapRefresh the monitor's refresh rate
    bool vsync = true;
    uint32_t aniso = 0;              // 0 as the game asks
    bool custom_textures = true;     // images in textures\replacement\ beside the exe replace the game's
    bool dump_textures = false;      // each texture once into textures\dumps\ beside the exe
    uint32_t texture_dump_format = 0;  // 0 PNG, 1 DDS
    uint32_t audio_output = 0;         // audio::kOutputAuto, Stereo, Surround or Mono
    bool aspect_4_3 = false;
    uint32_t volume = 100;             // master volume, percent
    bool show_fps = false;             // a frame rate counter in the corner
    uint32_t mouse_sensitivity = 100;  // percent, GameInfo::mouse_stick
    std::string game_path;             // a disc image or game folder picked in the setup, looked in before assets
    bool setup_done = false;           // the setup window was finished once: it opens again only when needed           // tell the game it's on a 4:3 TV and show 4:3 (read at start)
    std::string join_ip;                   // co-op: a host past the LAN the system link search also asks
};

Settings& Get();
void Load();
void Save();
void LogSettings();  // every setting, as the settings file has them
void LogSystem();    // the machine and the settings, at a run's start
void ApplyDisplay();  // any thread
struct Mode {
    uint32_t width, height;
};
Mode Desktop();
Mode Output();
float RenderScale(uint32_t guest_height);
// the output is 3:2 or wider: the console reports a widescreen TV and the frame is shown 16:9, else 4:3 as on a 4:3 TV
bool Widescreen();

std::filesystem::path ExeDir();
// the game (game_files.cpp): the setup's game_path, then assets: its folder and this executable's xbe, from the
// extracted files or a disc image (.iso), which it mounts as D:; false after telling the player what is missing.
// Quiet (ask false) it tells nothing and only fills found: where it looked or what it found instead.
struct GameFiles {
    std::filesystem::path dir;
    std::vector<uint8_t> xbe;
};
struct GameFound {
    std::filesystem::path where;  // the folder or disc image the game is in, or the other release found
    bool other_release = false;   // a copy of the game, but not the release this build is made from
};
bool FindGame(GameFiles& game, bool ask = true, GameFound* found = nullptr);
std::string Sha1(const std::vector<uint8_t>& data);  // lower case hex
// the recompiled code, <exe>.dll, built from the game in assets when missing or out of date (game_code.cpp) and
// loaded; false after telling the player why the game cannot start, or when they chose not to start it yet
bool PrepareGameCode(const GameFiles& game);
// the same build driven by the setup window (setup.cpp), without a window of its own
struct GameCodeStatus {
    enum State { kIdle, kRunning, kReady, kFailed } state = kIdle;
    float fraction = 0;
    std::wstring stage, time_left, failure;
};
bool GameCodeNeeded(const GameFiles& game);  // something to build for this game; false: the code is current
std::wstring StartGameCode();                // starts it on a thread; what stops it from starting, else empty
GameCodeStatus GameCodeState();
void StopGameCode();                         // waits for the thread
bool LoadGameCode();                         // after a build or when current; false after telling the player
uint64_t CompilerSize();
void RemoveCompiler();                       // the game runs from its DLL alone
// the setup window (setup.cpp), before the game starts: finds the game, prepares its code, a few settings, Play.
// Opens on the first start, when the game is missing or its code needs building, or with Shift held at launch.
// True: the game is found and its code loaded. False: the player closed it.
bool RunSetup(GameFiles& game);
int Instance();  // --instance N on the command line, 0 without: a second copy on this PC for system link tests
void* Window();  // HWND
// boot splash: GameInfo::splash .bmp or .png beside the exe, up before the game window
bool OpenSplash();        // window thread, at launch; false without one, and the game window opens at once
void SplashFirstFrame();  // the game's first Swap: waits out the splash's minimum, then opens the game window
void CloseSplash();       // window thread
void OpenGameWindow(bool apply = true);  // any thread but the window thread's; returns once the window is up
// relaunched: the previous instance keeps its window up until this one has shown its first frames (handover.cpp)
bool StartHandover();   // window thread, at launch; true: the game window starts hidden
bool HandingOver();
void FinishHandover();  // any thread but the window thread's: opens the game window, lets the old instance go

// held around ImGui: the window thread's messages and the renderer's frame
std::recursive_mutex& UiLock();

// a control the keyboard can press: the pad's "up" "down" "left" "right" (left stick), "rs_up" and the rest (right
// stick), "dpad_up" and the rest,
// "a" "b" "x" "y" "black" "white" "lt" "rt" "start" "back" "ls" "rs"; its name as the game calls it; default keys
struct KeyControl {
    const char* control;
    const char* label;
    const char* keys;  // "W, Enter"
};

// what the game repo defines once
struct Option {  // a setting in the settings file, flag or value
    const char* key;
    bool* flag;
    uint32_t* value;
};
// a game switch in the options menu (app/menu.cpp); keep it in the settings file with an Option
struct MenuItem {
    const char* tab;   // its tab, by name: "Game", "Cheats"...
    const char* name;
    const char* help;  // a line under the name
    bool* flag;        // a switch; or null with value and choices: a list to pick from
    const char* note;  // shown after the value while on, e.g. "from the next race"; may be null
    uint32_t* value = nullptr;              // the chosen entry of choices
    std::vector<const char*> choices = {};
};
struct GameInfo {
    const wchar_t* title;        // window and splash
    const wchar_t* short_title;  // message boxes
    const wchar_t* name;         // window classes, crash dumps, handover event
    const char* log;             // log prefix
    const char* settings;        // settings file beside the exe
    const wchar_t* splash;       // .bmp or .png beside the exe
    const wchar_t* missing;      // message when neither the game files nor a disc image are found
    std::vector<std::pair<const char*, const char*>> images;  // xbe file name, executable name
    std::vector<Option> options;
    // the settings file's toml::table, for what options cannot hold
    void (*load)(const void* table);
    void (*save)(void* table);
    void (*init)();                // after the settings load, before the window and boot
    void (*loaded)();              // image in memory, before its entry runs
    void (*window)(void* hwnd);    // the game window is up, before boot
    // window thread, each message after ImGui's; true returns result instead of the host's handling
    bool (*message)(void* hwnd, uint32_t msg, uintptr_t wp, intptr_t lp, intptr_t* result);
    void (*ui)(double fps);        // each frame inside the ImGui frame, UiLock held
    void (*pad)(int port, input::Pad& pad);  // a polled pad before the game gets it
    void (*rumble)(int port, uint16_t& left, uint16_t& right);
    // xbe file name, SHA-1: other releases the executable runs, each from its own [xbe] named config
    std::vector<std::pair<const char*, const char*>> releases;
    std::vector<KeyControl> keys;  // keyboard play into the first port (core/keys.h); none: no keyboard
    // the options menu: its header and the game's own switches
    const char* version = nullptr;  // "0.1.0"
    const char* tier = nullptr;     // "alpha", "beta" or "complete"
    bool widescreen = true;         // the game draws 16:9 when the console says so; false: made for 4:3, shown
                                    // widescreen by the toolkit's hack (3D squeezed, gpu/d3d12.cpp UploadConstants)
    const char* mouse_stick = nullptr;  // "right" or "left": in keyboard play the mouse moves that stick
    std::vector<MenuItem> menu;
};
extern const GameInfo kGame;

// sleeps out the rest of the frame for the fps cap
void Pace();

}  // namespace app
