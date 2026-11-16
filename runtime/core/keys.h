#pragma once
// Keyboard play: the game's controls (GameInfo::keys) bound to keys, read into the first controller port while the
// game window has focus. Each control has two key slots; the settings file keeps them under [keys].
#include <cstdint>
#include <string>

namespace input {

struct Pad;
constexpr int kKeySlots = 2;

int KeyControls();                           // GameInfo::keys' count
const char* KeyLabel(int control);           // as the game names it
const char* KeyControlId(int control);       // its key in the settings file
int Key(int control, int slot);              // a Windows virtual key, 0 none
void SetKey(int control, int slot, int vk);  // a key bound elsewhere is taken from there
void ResetKeys();                            // the game's defaults
std::string KeyName(int vk);                 // "W", "Left Shift", "" for none

// settings file: "W, Enter" for a control, "none" for no keys; missing keeps the default
std::string KeysText(int control);
void SetKeysText(int control, const std::string& text);

// while off the keys press nothing (the game's own menu is over the game); a key down when they come back counts
// once it has been let go
void SetKeysOff(bool off);
// the next key pressed in the game window goes to that control and slot; Esc cancels
void CaptureKey(int control, int slot);
bool Capturing();
// window thread, each message: true when a capture took it
bool KeyMessage(uint32_t msg, uintptr_t wp, intptr_t lp);
// a controller on port 0 was used: the keys rest for a while, so a controller that also types (Steam Input's desktop
// layout) doesn't press both
void ControllerUsed();
// what the held keys press, added into port 0's pad
void AddKeys(int port, Pad& p);

}  // namespace input
