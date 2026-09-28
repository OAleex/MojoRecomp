#pragma once

#include <cstdint>

// Flat 4 GB guest mapping used by XenonRecomp. It stays null in the link-only
// smoke executable and is installed by the real host runtime before guest code.
extern uint8_t* g_mojoGuestBase;
