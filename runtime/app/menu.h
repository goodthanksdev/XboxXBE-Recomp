// The options menu every game gets: F1 or Back + Start opens it. Tabs of setting cards, driven by controller,
// keyboard or mouse; each change applies at once and is saved. A game adds its own switches through GameInfo::menu.
#pragma once

#include <cstdint>

#include <string>

#include "app/app.h"
#include "core/input.h"

namespace menu {

void Fonts();  // after ImGui::CreateContext, before the first frame
bool Open();
// window thread: F1 toggles, keys drive the menu while it is open; true takes the message
bool Message(void* hwnd, uint32_t msg, uintptr_t wp, intptr_t lp, intptr_t* result);
// game thread, after the game's own pad hook: Back + Start toggles; while open the game sees nothing pressed
void Pad(int port, input::Pad& p);
// render thread, inside the ImGui frame
void Ui(double fps);

// The setup window's panel (app/setup.cpp): the game, preparing it, a few settings, Play. setup.cpp fills the state
// each frame and reads the player's picks; SetupEnd before the game's own menu is used.
struct SetupModel {
    std::wstring found;  // where the game is (or the other release found)
    bool game_ok = false, other_release = false;
    app::GameCodeStatus code;
    std::wstring blocker;  // what stops the preparing from starting
    bool remove_compiler = false;
    bool pick_iso = false, pick_folder = false, play = false;  // the player's picks, cleared by setup.cpp
};
void SetupUi(SetupModel& m);  // inside an ImGui frame
void SetupEnd();

}  // namespace menu
