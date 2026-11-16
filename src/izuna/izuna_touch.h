// [IzunaRecomp] Android: on-screen touch controls - off by default (the game is
// meant for a controller); the hub's main menu turns them on, after asking
// "Do you REALLY wanna play Ninja Gaiden 2 with touch controls?".
//
// A virtual Xbox 360 controller drawn over the picture: a floating left stick
// (the lower left), a floating right stick (the lower middle - the camera),
// the face buttons, the D-pad, the shoulder buttons and triggers, BACK and
// START, and R3. It reaches the game as one more controller of player 1 (an
// input driver, as the synthetic pad of ng2_autoskip.cpp does) and the menus -
// the hub, the in-game menu - through ng2ps5::Pad.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace rex::input {
class InputDriver;
}

namespace izuna::touch {

// Shown and read only while enabled (the setting touch_controls).
void SetEnabled(bool enabled);
bool Enabled();

// Watches the screen's touches (SDL finger events). Once, early; Android only.
void Install();

// The virtual controller as ng2ps5::Pad's bits (ps5_pad.h), for the menus.
uint32_t PadBits();

// The virtual controller for the game.
std::unique_ptr<rex::input::InputDriver> MakeDriver();

// Draws the controls over everything (ImGui's foreground list), at the
// display's size in pixels. Each frame, from an ImGui dialog.
void Draw(float width, float height);

// The player's own layout (Ng2Settings::touch_layout; "" the default): each
// control's place, in fractions of the screen, and its size.
void SetLayout(const std::string& layout);
// The layout editor (Settings > Controls > Touch layout): the controls over a
// dimmed screen - drag one to move it, SMALLER / BIGGER for the one last
// touched, RESET for the default, DONE to keep it. The game and the menus see
// no touches meanwhile. `save` gets the new layout when DONE is pressed (from
// Draw: the UI thread).
void BeginEdit(std::function<void(const std::string&)> save);
bool Editing();

}  // namespace izuna::touch
