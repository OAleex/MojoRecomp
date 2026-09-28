#include <SDL3/SDL.h>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <hidsdi.h>
#include <setupapi.h>

#include <cstdio>
#include <vector>

namespace {

void DumpCounts(const char* phase)
{
    int joystickCount = 0;
    SDL_JoystickID* joystickIds = SDL_GetJoysticks(&joystickCount);
    SDL_free(joystickIds);
    int gamepadCount = 0;
    SDL_JoystickID* gamepadIds = SDL_GetGamepads(&gamepadCount);
    SDL_free(gamepadIds);
    std::printf("%s joysticks=%d gamepads=%d error=%s\n", phase,
                joystickCount, gamepadCount,
                SDL_GetError()[0] ? SDL_GetError() : "<none>");
}

void DumpDs4Hid()
{
    constexpr unsigned short kSony = 0x054C;
    constexpr unsigned short kDs4V2 = 0x09CC;
    const int initResult = SDL_hid_init();
    std::printf("hid_init=%d error=%s\n", initResult,
                SDL_GetError()[0] ? SDL_GetError() : "<none>");
    SDL_hid_device_info* devices = SDL_hid_enumerate(kSony, kDs4V2);
    int count = 0;
    for (SDL_hid_device_info* info = devices; info; info = info->next)
    {
        ++count;
        std::printf("hid[%d] vid=%04X pid=%04X bus=%d usage=%04X/%04X iface=%d "
                    "manufacturer=%ls product=%ls path=%s\n",
                    count - 1, info->vendor_id, info->product_id,
                    int(info->bus_type), info->usage_page, info->usage,
                    info->interface_number,
                    info->manufacturer_string ? info->manufacturer_string : L"<none>",
                    info->product_string ? info->product_string : L"<none>",
                    info->path ? info->path : "<none>");
        if (!info->path)
            continue;
        SDL_hid_device* device = SDL_hid_open_path(info->path);
        if (!device)
        {
            std::printf("  open failed: %s\n", SDL_GetError());
            continue;
        }
        for (int report = 0; report < 3; ++report)
        {
            unsigned char data[128]{};
            const int got = SDL_hid_read_timeout(device, data, sizeof(data), 200);
            std::printf("  report[%d] bytes=%d", report, got);
            if (got < 0)
                std::printf(" error=%s", SDL_GetError());
            else
                for (int i = 0; i < got && i < 40; ++i)
                    std::printf(" %02X", unsigned(data[i]));
            std::printf("\n");
        }
        SDL_hid_close(device);
    }
    std::printf("hid_ds4_count=%d\n", count);
    SDL_hid_free_enumeration(devices);
    SDL_hid_exit();
}

void DumpWindowsHid()
{
    GUID hidGuid{};
    HidD_GetHidGuid(&hidGuid);
    HDEVINFO deviceInfo = SetupDiGetClassDevsW(
        &hidGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (deviceInfo == INVALID_HANDLE_VALUE)
    {
        std::printf("winhid SetupDiGetClassDevs failed=%lu\n", GetLastError());
        return;
    }

    unsigned matching = 0;
    for (DWORD index = 0;; ++index)
    {
        SP_DEVICE_INTERFACE_DATA iface{};
        iface.cbSize = sizeof(iface);
        if (!SetupDiEnumDeviceInterfaces(deviceInfo, nullptr, &hidGuid, index, &iface))
        {
            if (GetLastError() != ERROR_NO_MORE_ITEMS)
                std::printf("winhid enum failed index=%lu error=%lu\n", index, GetLastError());
            break;
        }

        DWORD required = 0;
        SetupDiGetDeviceInterfaceDetailW(deviceInfo, &iface, nullptr, 0, &required, nullptr);
        if (!required)
            continue;
        std::vector<unsigned char> detailStorage(required);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(detailStorage.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        SP_DEVINFO_DATA devInfo{};
        devInfo.cbSize = sizeof(devInfo);
        if (!SetupDiGetDeviceInterfaceDetailW(deviceInfo, &iface, detail, required,
                                               nullptr, &devInfo))
            continue;

        HANDLE handle = CreateFileW(detail->DevicePath, 0,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    nullptr, OPEN_EXISTING, 0, nullptr);
        if (handle == INVALID_HANDLE_VALUE)
            continue;

        HIDD_ATTRIBUTES attributes{};
        attributes.Size = sizeof(attributes);
        const bool haveAttributes = HidD_GetAttributes(handle, &attributes) != FALSE;
        PHIDP_PREPARSED_DATA preparsed = nullptr;
        HIDP_CAPS caps{};
        const bool haveCaps = HidD_GetPreparsedData(handle, &preparsed) != FALSE &&
                              HidP_GetCaps(preparsed, &caps) == HIDP_STATUS_SUCCESS;
        if (haveAttributes && attributes.VendorID == 0x054Cu &&
            attributes.ProductID == 0x09CCu)
        {
            ++matching;
            wchar_t instanceId[512]{};
            SetupDiGetDeviceInstanceIdW(deviceInfo, &devInfo, instanceId,
                                        static_cast<DWORD>(std::size(instanceId)), nullptr);
            std::printf("winhid DS4[%u] vid=%04X pid=%04X ver=%04X usage=%04X/%04X "
                        "in=%u out=%u feature=%u\n",
                        matching - 1, attributes.VendorID, attributes.ProductID,
                        attributes.VersionNumber,
                        haveCaps ? caps.UsagePage : 0u, haveCaps ? caps.Usage : 0u,
                        haveCaps ? caps.InputReportByteLength : 0u,
                        haveCaps ? caps.OutputReportByteLength : 0u,
                        haveCaps ? caps.FeatureReportByteLength : 0u);
            std::wprintf(L"  instance=%ls\n  path=%ls\n", instanceId, detail->DevicePath);
        }
        if (preparsed)
            HidD_FreePreparsedData(preparsed);
        CloseHandle(handle);
    }
    SetupDiDestroyDeviceInfoList(deviceInfo);
    std::printf("winhid_ds4_count=%u\n", matching);
}

} // namespace

int main()
{
    SDL_SetLogPriorities(SDL_LOG_PRIORITY_VERBOSE);
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS4, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_THREAD, "1");
    std::printf("hints HIDAPI=%s PS4=%s RAWINPUT=%s THREAD=%s\n",
                SDL_GetHint(SDL_HINT_JOYSTICK_HIDAPI),
                SDL_GetHint(SDL_HINT_JOYSTICK_HIDAPI_PS4),
                SDL_GetHint(SDL_HINT_JOYSTICK_RAWINPUT),
                SDL_GetHint(SDL_HINT_JOYSTICK_THREAD));

    constexpr SDL_InitFlags kUiFlags = SDL_INIT_VIDEO | SDL_INIT_EVENTS;
    constexpr SDL_InitFlags kInputFlags = SDL_INIT_JOYSTICK | SDL_INIT_GAMEPAD;
    if (!SDL_InitSubSystem(kUiFlags))
    {
        std::fprintf(stderr, "SDL UI init failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window* window = SDL_CreateWindow("MojoRecomp input probe", 320, 200,
                                          SDL_WINDOW_HIDDEN);
    std::printf("hidden_window=%p error=%s\n", static_cast<void*>(window),
                SDL_GetError()[0] ? SDL_GetError() : "<none>");
    SDL_PumpEvents();

    if (!SDL_InitSubSystem(kInputFlags))
    {
        std::fprintf(stderr, "SDL input init failed: %s\n", SDL_GetError());
        if (window)
            SDL_DestroyWindow(window);
        SDL_QuitSubSystem(kUiFlags);
        return 1;
    }

    DumpDs4Hid();
    DumpWindowsHid();

    DumpCounts("immediate");
    for (int i = 0; i < 30; ++i)
    {
        SDL_PumpEvents();
        SDL_UpdateJoysticks();
        SDL_UpdateGamepads();
        SDL_Delay(100);
        if (i == 4 || i == 9 || i == 19 || i == 29)
        {
            char phase[32]{};
            std::snprintf(phase, sizeof(phase), "after-%dms", (i + 1) * 100);
            DumpCounts(phase);
        }
    }

    int joystickCount = 0;
    SDL_JoystickID* joystickIds = SDL_GetJoysticks(&joystickCount);
    std::printf("joysticks=%d\n", joystickCount);
    for (int i = 0; i < joystickCount; ++i)
    {
        const SDL_JoystickID id = joystickIds[i];
        const char* name = SDL_GetJoystickNameForID(id);
        std::printf("joy[%d] id=%u name=%s gamepad=%d\n", i,
                    static_cast<unsigned>(id), name ? name : "unknown",
                    SDL_IsGamepad(id) ? 1 : 0);
        if (SDL_Joystick* joystick = SDL_OpenJoystick(id))
        {
            std::printf("  vid=%04X pid=%04X axes=%d buttons=%d hats=%d\n",
                        SDL_GetJoystickVendor(joystick), SDL_GetJoystickProduct(joystick),
                        SDL_GetNumJoystickAxes(joystick), SDL_GetNumJoystickButtons(joystick),
                        SDL_GetNumJoystickHats(joystick));
            SDL_UpdateJoysticks();
            std::printf("  axis:");
            for (int axis = 0; axis < SDL_GetNumJoystickAxes(joystick); ++axis)
                std::printf(" %d=%d", axis, int(SDL_GetJoystickAxis(joystick, axis)));
            std::printf("\n");
            SDL_CloseJoystick(joystick);
        }
    }
    SDL_free(joystickIds);

    int gamepadCount = 0;
    SDL_JoystickID* gamepadIds = SDL_GetGamepads(&gamepadCount);
    std::printf("gamepads=%d\n", gamepadCount);
    for (int i = 0; i < gamepadCount; ++i)
    {
        const SDL_JoystickID id = gamepadIds[i];
        SDL_Gamepad* gamepad = SDL_OpenGamepad(id);
        if (!gamepad)
        {
            std::printf("pad[%d] id=%u open failed: %s\n", i,
                        static_cast<unsigned>(id), SDL_GetError());
            continue;
        }
        SDL_UpdateGamepads();
        char* mapping = SDL_GetGamepadMapping(gamepad);
        std::printf("pad[%d] id=%u name=%s vid=%04X pid=%04X mapping=%s\n",
                    i, static_cast<unsigned>(id),
                    SDL_GetGamepadName(gamepad) ? SDL_GetGamepadName(gamepad) : "unknown",
                    SDL_GetGamepadVendor(gamepad), SDL_GetGamepadProduct(gamepad),
                    mapping ? mapping : "<none>");
        std::printf("  axes LX=%d LY=%d RX=%d RY=%d LT=%d RT=%d\n",
                    int(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTX)),
                    int(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTY)),
                    int(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHTX)),
                    int(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHTY)),
                    int(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER)),
                    int(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)));
        SDL_free(mapping);
        SDL_CloseGamepad(gamepad);
    }
    SDL_free(gamepadIds);
    SDL_QuitSubSystem(kInputFlags);
    if (window)
        SDL_DestroyWindow(window);
    SDL_QuitSubSystem(kUiFlags);
    return 0;
}
