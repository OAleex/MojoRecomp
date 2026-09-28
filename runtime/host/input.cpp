#include "input.h"
#include "input_merge.h"
#include "../debug_mode.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>

#if defined(MOJORECOMP_HAS_SDL3)
#include <SDL3/SDL.h>
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr uint16_t kDpadUp = 0x0001;
constexpr uint16_t kDpadDown = 0x0002;
constexpr uint16_t kDpadLeft = 0x0004;
constexpr uint16_t kDpadRight = 0x0008;
constexpr uint16_t kStart = 0x0010;
constexpr uint16_t kBack = 0x0020;
constexpr uint16_t kLeftThumb = 0x0040;
constexpr uint16_t kRightThumb = 0x0080;
constexpr uint16_t kLeftShoulder = 0x0100;
constexpr uint16_t kRightShoulder = 0x0200;
constexpr uint16_t kA = 0x1000;
constexpr uint16_t kB = 0x2000;
constexpr uint16_t kX = 0x4000;
constexpr uint16_t kY = 0x8000;

uint32_t InputPollCacheUs()
{
    static const uint32_t value = [] {
        const char* text = std::getenv("MOJORECOMP_INPUT_POLL_CACHE_US");
        if (!text || !*text || text[0] == '0')
            return 0u;
        char* end = nullptr;
        const unsigned long parsed = std::strtoul(text, &end, 10);
        if (end == text)
            return 1000u;
        return static_cast<uint32_t>(std::min<unsigned long>(parsed, 16000ul));
    }();
    return value;
}

bool Down(int key)
{
    return (GetAsyncKeyState(key) & 0x8000) != 0;
}

uint16_t DiagnosticButtonByName(const std::string& name)
{
    if (name == "START") return kStart;
    if (name == "A") return kA;
    if (name == "B") return kB;
    if (name == "X") return kX;
    if (name == "Y") return kY;
    if (name == "UP") return kDpadUp;
    if (name == "DOWN") return kDpadDown;
    if (name == "LEFT") return kDpadLeft;
    if (name == "RIGHT") return kDpadRight;
    return 0;
}

enum class DiagnosticAxisAction : uint8_t
{
    None,
    LUp,
    LDown,
    LLeft,
    LRight,
    LeftTrigger,
    RightTrigger,
};

DiagnosticAxisAction DiagnosticAxisByName(const std::string& name)
{
    if (name == "LUP") return DiagnosticAxisAction::LUp;
    if (name == "LDOWN") return DiagnosticAxisAction::LDown;
    if (name == "LLEFT") return DiagnosticAxisAction::LLeft;
    if (name == "LRIGHT") return DiagnosticAxisAction::LRight;
    if (name == "LT") return DiagnosticAxisAction::LeftTrigger;
    if (name == "RT") return DiagnosticAxisAction::RightTrigger;
    return DiagnosticAxisAction::None;
}

void ApplyDiagnosticAction(HostInputState& state, uint16_t button,
                           DiagnosticAxisAction axis)
{
    if (button)
        state.buttons |= button;
    switch (axis)
    {
    case DiagnosticAxisAction::LUp:    state.thumbLY = INT16_MAX; break;
    case DiagnosticAxisAction::LDown:  state.thumbLY = INT16_MIN; break;
    case DiagnosticAxisAction::LLeft:  state.thumbLX = INT16_MIN; break;
    case DiagnosticAxisAction::LRight: state.thumbLX = INT16_MAX; break;
    case DiagnosticAxisAction::LeftTrigger: state.leftTrigger = 0xFF; break;
    case DiagnosticAxisAction::RightTrigger: state.rightTrigger = 0xFF; break;
    case DiagnosticAxisAction::None: break;
    }
}

// Keep the XInput dependency optional.  This lets the runtime build on a stock
// Windows SDK and still accepts Xbox-compatible pads when the system DLL is
// present.  PlayStation pads exposed through an XInput compatibility layer are
// accepted by the same path.
struct XInputGamepadNative
{
    uint16_t buttons;
    uint8_t leftTrigger;
    uint8_t rightTrigger;
    int16_t thumbLX;
    int16_t thumbLY;
    int16_t thumbRX;
    int16_t thumbRY;
};

struct XInputStateNative
{
    uint32_t packetNumber;
    XInputGamepadNative gamepad;
};

using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XInputStateNative*);

XInputGetStateFn GetXInput()
{
    static XInputGetStateFn fn = []() -> XInputGetStateFn {
        constexpr const wchar_t* dlls[] = {
            L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"
        };
        for (const wchar_t* dll : dlls)
        {
            if (HMODULE module = LoadLibraryW(dll))
                if (auto proc = GetProcAddress(module, "XInputGetState"))
                    return reinterpret_cast<XInputGetStateFn>(proc);
        }
        return nullptr;
    }();
    return fn;
}

void MergeXInputState(HostInputState& state, const XInputStateNative& native)
{
    state.buttons |= native.gamepad.buttons;
    state.leftTrigger = std::max(state.leftTrigger, native.gamepad.leftTrigger);
    state.rightTrigger = std::max(state.rightTrigger, native.gamepad.rightTrigger);
    if (native.gamepad.thumbLX) state.thumbLX = native.gamepad.thumbLX;
    if (native.gamepad.thumbLY) state.thumbLY = native.gamepad.thumbLY;
    if (native.gamepad.thumbRX) state.thumbRX = native.gamepad.thumbRX;
    if (native.gamepad.thumbRY) state.thumbRY = native.gamepad.thumbRY;
}

int16_t NormalizeJoyAxis(DWORD value, UINT minimum, UINT maximum, bool invert = false)
{
    if (maximum <= minimum)
        return 0;

    const double normalized = std::clamp(
        (static_cast<double>(value) - minimum) /
            static_cast<double>(maximum - minimum),
        0.0, 1.0);
    double signedValue = normalized * 2.0 - 1.0;
    if (invert)
        signedValue = -signedValue;

    // HID/DirectInput devices tend to have a little centre jitter. Keep the
    // guest neutral unless the stick has moved by roughly 8% of its range.
    if (std::abs(signedValue) < 0.08)
        return 0;
    return static_cast<int16_t>(std::clamp(
        signedValue * 32767.0, -32768.0, 32767.0));
}

uint8_t NormalizeJoyTrigger(DWORD value, UINT minimum, UINT maximum)
{
    if (maximum <= minimum)
        return 0;
    const double normalized = std::clamp(
        (static_cast<double>(value) - minimum) /
            static_cast<double>(maximum - minimum),
        0.0, 1.0);
    if (normalized < 0.04)
        return 0;
    return static_cast<uint8_t>(std::clamp(normalized * 255.0, 0.0, 255.0));
}

#if defined(MOJORECOMP_HAS_SDL3)
int16_t InvertSdlAxis(int16_t value)
{
    return value == INT16_MIN ? INT16_MAX : static_cast<int16_t>(-value);
}

uint8_t SdlTriggerToByte(int16_t value)
{
    if (value <= 0)
        return 0;
    return static_cast<uint8_t>(std::clamp<int>(
        (static_cast<int>(value) * 255 + 16383) / 32767, 0, 255));
}

bool MergeSdlGamepad(HostInputState& state, SDL_Gamepad* gamepad)
{
    if (!gamepad)
        return false;

    SDL_UpdateGamepads();
    if (!SDL_GamepadConnected(gamepad))
        return false;

    if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_SOUTH)) state.buttons |= kA;
    if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_EAST)) state.buttons |= kB;
    if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_WEST)) state.buttons |= kX;
    if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_NORTH)) state.buttons |= kY;
    if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER)) state.buttons |= kLeftShoulder;
    if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)) state.buttons |= kRightShoulder;
    if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_LEFT_STICK)) state.buttons |= kLeftThumb;
    if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_RIGHT_STICK)) state.buttons |= kRightThumb;
    if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_START)) state.buttons |= kStart;
    if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_BACK)) state.buttons |= kBack;
    if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_DPAD_UP)) state.buttons |= kDpadUp;
    if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_DPAD_DOWN)) state.buttons |= kDpadDown;
    if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_DPAD_LEFT)) state.buttons |= kDpadLeft;
    if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT)) state.buttons |= kDpadRight;

    state.thumbLX = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTX);
    state.thumbLY = InvertSdlAxis(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTY));
    state.thumbRX = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHTX);
    state.thumbRY = InvertSdlAxis(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHTY));
    state.leftTrigger = std::max(
        state.leftTrigger,
        SdlTriggerToByte(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER)));
    state.rightTrigger = std::max(
        state.rightTrigger,
        SdlTriggerToByte(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)));
    return true;
}

SDL_Gamepad* FindSdlGamepad()
{
    static bool initAttempted = false;
    static bool initialized = false;
    if (!initAttempted)
    {
        initAttempted = true;
        initialized = SDL_InitSubSystem(SDL_INIT_GAMEPAD);
        if (!initialized)
            std::fprintf(stderr, "[input] SDL3 gamepad init failed: %s\n", SDL_GetError());
    }
    if (!initialized)
        return nullptr;

    SDL_UpdateGamepads();
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    if (!ids || count <= 0)
    {
        SDL_free(ids);
        return nullptr;
    }

    SDL_Gamepad* gamepad = SDL_OpenGamepad(ids[0]);
    SDL_free(ids);
    return gamepad;
}
#endif

void MergePov(HostInputState& state, DWORD pov)
{
    if (pov == JOY_POVCENTERED || pov == 0xFFFFu)
        return;

    // WinMM reports hundredths of a degree clockwise from up. Treat diagonal
    // sectors as both neighbouring D-pad buttons, matching XInput semantics.
    const DWORD angle = pov % 36000u;
    if (angle >= 31500u || angle < 4500u) state.buttons |= kDpadUp;
    if (angle >= 4500u && angle < 13500u) state.buttons |= kDpadRight;
    if (angle >= 13500u && angle < 22500u) state.buttons |= kDpadDown;
    if (angle >= 22500u && angle < 31500u) state.buttons |= kDpadLeft;
}

bool MergeWinMmController(HostInputState& state, UINT id, const JOYCAPSW& caps)
{
    JOYINFOEX info{};
    info.dwSize = sizeof(info);
    info.dwFlags = JOY_RETURNALL;
    if (joyGetPosEx(id, &info) != JOYERR_NOERROR)
        return false;

    const bool dualShock4 = caps.wMid == 0x054Cu && caps.wPid == 0x09CCu;
    const DWORD buttons = info.dwButtons;
    const auto pressed = [buttons](unsigned oneBased) {
        return oneBased >= 1 && oneBased <= 32 &&
               (buttons & (1u << (oneBased - 1))) != 0;
    };

    if (dualShock4)
    {
        // DirectInput/WinMM DualShock 4 order:
        // Square, Cross, Circle, Triangle, L1, R1, L2, R2,
        // Share, Options, L3, R3, PS, Touchpad.
        if (pressed(1)) state.buttons |= kX;
        if (pressed(2)) state.buttons |= kA;
        if (pressed(3)) state.buttons |= kB;
        if (pressed(4)) state.buttons |= kY;
        if (pressed(5)) state.buttons |= kLeftShoulder;
        if (pressed(6)) state.buttons |= kRightShoulder;
        if (pressed(7)) state.leftTrigger = 0xFF;
        if (pressed(8)) state.rightTrigger = 0xFF;
        if (pressed(9)) state.buttons |= kBack;
        if (pressed(10)) state.buttons |= kStart;
        if (pressed(11)) state.buttons |= kLeftThumb;
        if (pressed(12)) state.buttons |= kRightThumb;
    }
    else
    {
        // Conventional PC gamepad order used by most WinMM HID devices.
        if (pressed(1)) state.buttons |= kA;
        if (pressed(2)) state.buttons |= kB;
        if (pressed(3)) state.buttons |= kX;
        if (pressed(4)) state.buttons |= kY;
        if (pressed(5)) state.buttons |= kLeftShoulder;
        if (pressed(6)) state.buttons |= kRightShoulder;
        if (pressed(7)) state.buttons |= kBack;
        if (pressed(8)) state.buttons |= kStart;
        if (pressed(9)) state.buttons |= kLeftThumb;
        if (pressed(10)) state.buttons |= kRightThumb;
    }

    MergePov(state, info.dwPOV);
    state.thumbLX = NormalizeJoyAxis(info.dwXpos, caps.wXmin, caps.wXmax);
    state.thumbLY = NormalizeJoyAxis(info.dwYpos, caps.wYmin, caps.wYmax, true);

    // Six-axis HID pads (including the DS4) conventionally expose the right
    // stick as Z/R and analogue triggers as U/V through WinMM.
    if (caps.wCaps & JOYCAPS_HASZ)
        state.thumbRX = NormalizeJoyAxis(info.dwZpos, caps.wZmin, caps.wZmax);
    if (caps.wCaps & JOYCAPS_HASR)
        state.thumbRY = NormalizeJoyAxis(info.dwRpos, caps.wRmin, caps.wRmax, true);
    if (caps.wCaps & JOYCAPS_HASU)
        state.leftTrigger = std::max(
            state.leftTrigger,
            NormalizeJoyTrigger(info.dwUpos, caps.wUmin, caps.wUmax));
    if (caps.wCaps & JOYCAPS_HASV)
        state.rightTrigger = std::max(
            state.rightTrigger,
            NormalizeJoyTrigger(info.dwVpos, caps.wVmin, caps.wVmax));
    return true;
}

void MergeController(HostInputState& state)
{
    const auto getState = GetXInput();
    enum class Backend { None, SDL3, XInput, WinMM };
    struct Selection
    {
        Backend backend = Backend::None;
        UINT id = 0;
        JOYCAPSW caps{};
#if defined(MOJORECOMP_HAS_SDL3)
        SDL_Gamepad* gamepad = nullptr;
#endif
        std::chrono::steady_clock::time_point nextScan{};
    };
    static Selection selected;

    // Keep using the currently selected device while it is alive. If it is
    // unplugged, fall through immediately and look for another one.
#if defined(MOJORECOMP_HAS_SDL3)
    if (selected.backend == Backend::SDL3)
    {
        if (MergeSdlGamepad(state, selected.gamepad))
            return;
        if (selected.gamepad)
            SDL_CloseGamepad(selected.gamepad);
        selected.gamepad = nullptr;
        std::fprintf(stderr, "[input] SDL3 controller disconnected\n");
        selected.backend = Backend::None;
        selected.nextScan = {};
    }
    else
#endif
    if (selected.backend == Backend::XInput && getState)
    {
        XInputStateNative native{};
        if (getState(selected.id, &native) == ERROR_SUCCESS)
        {
            MergeXInputState(state, native);
            return;
        }
        std::fprintf(stderr, "[input] XInput controller %u disconnected\n", selected.id);
        selected.backend = Backend::None;
        selected.nextScan = {};
    }
    else if (selected.backend == Backend::WinMM)
    {
        if (MergeWinMmController(state, selected.id, selected.caps))
            return;
        std::fprintf(stderr, "[input] WinMM controller %u disconnected\n", selected.id);
        selected.backend = Backend::None;
        selected.nextScan = {};
    }

    const auto now = std::chrono::steady_clock::now();
    if (now < selected.nextScan)
        return;
    // Disconnected XInput probes can be surprisingly expensive on Windows.
    // A 250 ms rescan still makes hotplug feel immediate without doing four
    // failed DLL calls on every guest input poll.
    selected.nextScan = now + std::chrono::milliseconds(250);

#if defined(MOJORECOMP_HAS_SDL3)
    if (SDL_Gamepad* gamepad = FindSdlGamepad())
    {
        selected.backend = Backend::SDL3;
        selected.gamepad = gamepad;
        const char* name = SDL_GetGamepadName(gamepad);
        std::fprintf(stderr,
                     "[input] SDL3 controller connected: %s (VID=%04X PID=%04X)\n",
                     name ? name : "unknown",
                     SDL_GetGamepadVendor(gamepad), SDL_GetGamepadProduct(gamepad));
        MergeSdlGamepad(state, gamepad);
        return;
    }
#endif

    if (getState)
    {
        for (DWORD id = 0; id < 4; ++id)
        {
            XInputStateNative native{};
            if (getState(id, &native) != ERROR_SUCCESS)
                continue;
            selected.backend = Backend::XInput;
            selected.id = id;
            std::fprintf(stderr, "[input] XInput controller connected on slot %lu\n",
                         static_cast<unsigned long>(id));
            MergeXInputState(state, native);
            return;
        }
    }

    const UINT count = std::min<UINT>(joyGetNumDevs(), 16u);
    for (UINT id = 0; id < count; ++id)
    {
        JOYINFOEX probe{};
        probe.dwSize = sizeof(probe);
        probe.dwFlags = JOY_RETURNALL;
        if (joyGetPosEx(id, &probe) != JOYERR_NOERROR)
            continue;

        JOYCAPSW caps{};
        if (joyGetDevCapsW(id, &caps, sizeof(caps)) != JOYERR_NOERROR)
            continue;

        selected.backend = Backend::WinMM;
        selected.id = id;
        selected.caps = caps;
        std::fwprintf(stderr,
                      L"[input] WinMM/HID controller connected on slot %u: %ls "
                      L"(VID=%04X PID=%04X)\n",
                      id, caps.szPname, caps.wMid, caps.wPid);
        MergeWinMmController(state, id, selected.caps);
        return;
    }
}

void MergeKeyboard(HostInputState& state)
{
    if (Down('J') || Down(VK_SPACE)) state.buttons |= kA;
    if (Down('K')) state.buttons |= kB;
    if (Down('U')) state.buttons |= kX;
    if (Down('I')) state.buttons |= kY;
    if (Down('Z')) state.buttons |= kLeftShoulder;
    if (Down('C')) state.buttons |= kRightShoulder;
    if (Down('F')) state.buttons |= kLeftThumb;
    if (Down('R')) state.buttons |= kRightThumb;
    if (Down(VK_RETURN)) state.buttons |= kStart;
    if (Down(VK_TAB)) state.buttons |= kBack;

    // Arrow keys are the right stick by default. Shift+arrows become the D-pad,
    // matching the old MojoRecomp bindings.
    const bool shift = Down(VK_SHIFT);
    if (shift)
    {
        if (Down(VK_UP)) state.buttons |= kDpadUp;
        if (Down(VK_DOWN)) state.buttons |= kDpadDown;
        if (Down(VK_LEFT)) state.buttons |= kDpadLeft;
        if (Down(VK_RIGHT)) state.buttons |= kDpadRight;
    }
    else
    {
        const bool left = Down(VK_LEFT);
        const bool right = Down(VK_RIGHT);
        const bool down = Down(VK_DOWN);
        const bool up = Down(VK_UP);
        mojorecomp::input::MergeDigitalAxis(state.thumbRX, left, right);
        mojorecomp::input::MergeDigitalAxis(state.thumbRY, down, up);
    }

    const bool moveLeft = Down('A');
    const bool moveRight = Down('D');
    const bool moveDown = Down('S');
    const bool moveUp = Down('W');
    mojorecomp::input::MergeDigitalAxis(state.thumbLX, moveLeft, moveRight);
    mojorecomp::input::MergeDigitalAxis(state.thumbLY, moveDown, moveUp);
    if (Down('Q')) state.leftTrigger = 0xFF;
    if (Down('E')) state.rightTrigger = 0xFF;
}

void MergeDiagnosticAutopilot(HostInputState& state)
{
    struct ScriptEvent
    {
        int64_t atMs = 0;
        int64_t durationMs = 120;
        uint16_t buttons = 0;
        DiagnosticAxisAction axis = DiagnosticAxisAction::None;
    };

    static const std::vector<ScriptEvent> script = [] {
        std::vector<ScriptEvent> events;
        const char* value = std::getenv("MOJORECOMP_AUTOPILOT_SCRIPT");
        if (!value || !*value)
            return events;

        std::string text(value);
        size_t cursor = 0;
        while (cursor < text.size())
        {
            const size_t comma = text.find(',', cursor);
            const std::string item = text.substr(cursor,
                comma == std::string::npos ? std::string::npos : comma - cursor);
            const size_t colon = item.find(':');
            if (colon != std::string::npos)
            {
                char* end = nullptr;
                const long long at = std::strtoll(item.c_str(), &end, 10);
                if (end == item.c_str() + colon)
                {
                    const std::string name = item.substr(colon + 1);
                    const uint16_t button = DiagnosticButtonByName(name);
                    const DiagnosticAxisAction axis = DiagnosticAxisByName(name);
                    if (button || axis != DiagnosticAxisAction::None)
                        events.push_back({static_cast<int64_t>(at), 120, button, axis});
                }
            }
            if (comma == std::string::npos)
                break;
            cursor = comma + 1;
        }
        return events;
    }();

    static const bool enabled = [] {
        const char* value = std::getenv("MOJORECOMP_AUTOPILOT_INPUT");
        return value && value[0] != '0';
    }();
    if (!enabled && script.empty())
        return;

    // Optional diagnostic kill-switch for unattended/headless runs. A helper
    // can create this file as soon as gameplay/cutscene rendering is detected,
    // preventing the repeating Start/A/Y probe from touching the scene itself.
    static const std::string stopFile = [] {
        const char* value = std::getenv("MOJORECOMP_AUTOPILOT_STOP_FILE");
        return value ? std::string(value) : std::string{};
    }();
    if (!stopFile.empty())
    {
        std::ifstream stop(stopFile);
        if (stop.good())
            return;
    }

    static const auto start = std::chrono::steady_clock::now();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();

    if (!script.empty())
    {
        for (const ScriptEvent& event : script)
            if (elapsed >= event.atMs && elapsed < event.atMs + event.durationMs)
                ApplyDiagnosticAction(state, event.buttons, event.axis);
        return;
    }

    const int64_t phase = elapsed % 1800;
    // Short, edge-producing pulses: Start first, then A, then Y. Repetition makes
    // the diagnostic useful across title screens whose first input poll occurs late;
    // Y also accepts the retail name-entry screen's "Done" action. This path is
    // opt-in only and never affects normal keyboard/controller input.
    if (phase < 120)
        state.buttons |= kStart;
    else if (phase >= 700 && phase < 820)
        state.buttons |= kA;
    else if (phase >= 1200 && phase < 1320)
        state.buttons |= kY;
}

void MergeDiagnosticCommandFile(HostInputState& state)
{
    // Optional diagnostic-only command queue. Each line is:
    //   <monotonic-id> <button> [duration-ms]
    // Example: "3 Y 150". The highest benefit over a timed script is that a
    // headless diagnostic run can stay alive while the caller inspects a frame
    // capture and then appends the next input. Normal runtime never touches the
    // filesystem unless MOJORECOMP_AUTOPILOT_FILE is explicitly set.
    static const std::string path = [] {
        const char* value = std::getenv("MOJORECOMP_AUTOPILOT_FILE");
        return value ? std::string(value) : std::string{};
    }();
    static bool pathReported = false;
    if (!pathReported)
    {
        pathReported = true;
        std::fprintf(stderr, "[autopilot file] path='%s'\n", path.c_str());
    }
    if (path.empty())
        return;

    // Use the same kill-switch as the repeating/scripted autopilot. Headless
    // Episode 1 validation creates this file as soon as real indexed 3D draws
    // begin; from that point onward no diagnostic input source may touch the
    // cutscene, otherwise later queued commands can navigate back into menus.
    static const std::string stopFile = [] {
        const char* value = std::getenv("MOJORECOMP_AUTOPILOT_STOP_FILE");
        return value ? std::string(value) : std::string{};
    }();
    if (!stopFile.empty())
    {
        std::ifstream stop(stopFile);
        if (stop.good())
            return;
    }

    using Clock = std::chrono::steady_clock;
    static uint64_t lastCommandId = 0;
    static uint16_t activeButton = 0;
    static DiagnosticAxisAction activeAxis = DiagnosticAxisAction::None;
    static Clock::time_point activeUntil{};
    static Clock::time_point nextPoll{};

    const auto now = Clock::now();
    if ((activeButton || activeAxis != DiagnosticAxisAction::None) && now < activeUntil)
    {
        ApplyDiagnosticAction(state, activeButton, activeAxis);
        return;
    }
    activeButton = 0;
    activeAxis = DiagnosticAxisAction::None;

    if (now < nextPoll)
        return;
    nextPoll = now + std::chrono::milliseconds(40);

    std::ifstream input(path);
    if (!input)
    {
        static bool openFailureReported = false;
        if (!openFailureReported)
        {
            openFailureReported = true;
            std::fprintf(stderr, "[autopilot file] open failed path='%s'\n", path.c_str());
        }
        return;
    }

    uint64_t selectedId = std::numeric_limits<uint64_t>::max();
    uint16_t selectedButton = 0;
    DiagnosticAxisAction selectedAxis = DiagnosticAxisAction::None;
    int64_t selectedDuration = 120;
    std::string selectedName;
    std::string line;
    while (std::getline(input, line))
    {
        std::istringstream command(line);
        uint64_t id = 0;
        std::string name;
        int64_t duration = 120;
        if (!(command >> id >> name))
            continue;
        command >> duration;

        if (id <= lastCommandId || id >= selectedId)
            continue;
        const uint16_t button = DiagnosticButtonByName(name);
        const DiagnosticAxisAction axis = DiagnosticAxisByName(name);
        if (!button && axis == DiagnosticAxisAction::None)
            continue;
        selectedId = id;
        selectedButton = button;
        selectedAxis = axis;
        selectedDuration = std::clamp<int64_t>(duration, 40, 1000);
        selectedName = name;
    }

    if (!selectedButton && selectedAxis == DiagnosticAxisAction::None)
        return;

    lastCommandId = selectedId;
    activeButton = selectedButton;
    activeAxis = selectedAxis;
    activeUntil = now + std::chrono::milliseconds(selectedDuration);
    ApplyDiagnosticAction(state, activeButton, activeAxis);
    std::fprintf(stderr, "[autopilot] command=%llu action=%s duration=%lldms\n",
                 static_cast<unsigned long long>(selectedId), selectedName.c_str(),
                 static_cast<long long>(selectedDuration));
}

} // namespace

void HostInput_Poll(HostInputState& state)
{
    const uint32_t cacheUs = InputPollCacheUs();
    if (cacheUs)
    {
        using Clock = std::chrono::steady_clock;
        struct Cache
        {
            HostInputState state{};
            Clock::time_point timestamp{};
            bool valid = false;
        };
        static thread_local Cache cache;
        const auto now = Clock::now();
        if (cache.valid && now - cache.timestamp < std::chrono::microseconds(cacheUs))
        {
            state = cache.state;
            mojorecomp::debug::ApplyInput(state);
            return;
        }

        state = {};
        MergeController(state);
        MergeKeyboard(state);
        MergeDiagnosticAutopilot(state);
        MergeDiagnosticCommandFile(state);
        mojorecomp::debug::ApplyInput(state);
        cache.state = state;
        cache.timestamp = now;
        cache.valid = true;
        return;
    }

    state = {};
    MergeController(state);
    MergeKeyboard(state);
    MergeDiagnosticAutopilot(state);
    MergeDiagnosticCommandFile(state);
    mojorecomp::debug::ApplyInput(state);
}
