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

// Merge the native keyboard and the first XInput-compatible controller into
// one Xbox 360-style state.  The guest ABI conversion remains in imports.cpp;
// this layer deliberately contains no guest pointers or endian types.
void HostInput_Poll(HostInputState& state);

