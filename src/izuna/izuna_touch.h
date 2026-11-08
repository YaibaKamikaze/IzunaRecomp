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
#include <memory>

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

}  // namespace izuna::touch
