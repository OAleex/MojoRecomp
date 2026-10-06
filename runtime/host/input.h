#pragma once

#include <cstdint>

struct HostInputState
{
    uint16_t buttons = 0;
    uint8_t leftTrigger = 0;
    uint8_t rightTrigger = 0;
    int16_t thumbLX = 0;
    int16_t thumbLY = 0;
    int16_t thumbRX = 0;
    int16_t thumbRY = 0;
};

constexpr uint32_t kHostInputPlayerCount = 2;

// Poll one Xbox 360-style player slot. Player 0 merges Keyboard 1 with the
// first physical controller; player 1 merges Keyboard 2 with the second.
// Physical input is neutral while the game window is not the foreground
// window. The guest ABI conversion remains in imports.cpp.
void HostInput_Poll(uint32_t playerIndex, HostInputState& state);

// Forward Xbox 360 vibration to the physical controller assigned to a player
// when the active backend supports rumble.
void HostInput_SetVibration(uint32_t playerIndex, uint16_t leftMotor,
                            uint16_t rightMotor);
