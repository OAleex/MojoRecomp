#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "../config/runtime_config.h"

namespace {

int Fail(const char* message)
{
    std::fprintf(stderr, "FAIL: %s\n", message);
    return 1;
}

} // namespace

int main()
{
    const mojorecomp::config::RuntimeConfig defaults{};
    if (defaults.resolutionScale != 1 ||
        defaults.aspectRatio != mojorecomp::config::AspectRatio::Native16x9 ||
        defaults.vsync ||
        defaults.antiAliasing != mojorecomp::config::AntiAliasing::FxaaExtreme ||
        defaults.textureFiltering != 8)
        return Fail("balanced graphics defaults changed unexpectedly");

    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "mojorecomp-runtime-config-test.toml";
    {
        std::ofstream file(path, std::ios::trunc);
        file << "schema_version = 1\n"
                "[localization]\n"
                "profile = \"pt-BR\"\n"
                "xbox_language = 1\n"
                "[display]\n"
                "mode = \"fullscreen\"\n"
                "monitor = \"primary\"\n"
                "output_resolution = \"desktop\"\n"
                "resolution_scale = 3\n"
                "aspect_ratio = \"21:9\"\n"
                "vsync = true\n"
                "[graphics]\n"
                "anti_aliasing = \"fxaa_extreme\"\n"
                "texture_filtering = \"8x\"\n";
    }

    mojorecomp::config::RuntimeConfig config{};
    std::string error;
    const bool loaded = mojorecomp::config::LoadFile(path, config, error);
    std::error_code removeError;
    std::filesystem::remove(path, removeError);
    if (!loaded)
        return Fail(error.c_str());
    if (config.displayMode != mojorecomp::config::DisplayMode::Fullscreen)
        return Fail("display mode was not parsed");
    if (config.localizationProfile != "pt-BR" || config.xboxLanguage != 1)
        return Fail("localization profile was not parsed");
    if (config.resolutionScale != 3)
        return Fail("resolution scale was not parsed");
    if (config.aspectRatio != mojorecomp::config::AspectRatio::Ultrawide21x9)
        return Fail("aspect ratio was not parsed");
    if (!config.vsync)
        return Fail("vsync was not parsed");
    if (config.antiAliasing != mojorecomp::config::AntiAliasing::FxaaExtreme)
        return Fail("anti-aliasing was not parsed");
    if (config.textureFiltering != 8)
        return Fail("texture filtering was not parsed");

    mojorecomp::config::RuntimeConfig invalid{};
    if (mojorecomp::config::ParseTextureFiltering("3x", invalid.textureFiltering))
        return Fail("invalid anisotropy level was accepted");

    std::string profile;
    uint32_t language = 0;
    if (!mojorecomp::config::ParseLocalizationProfile("pt-br", profile, language) ||
        profile != "pt-BR" || language != 1)
        return Fail("PT-BR localization profile did not map to English Xbox language");
    if (mojorecomp::config::ParseLocalizationProfile("de", profile, language))
    {
        if (profile != "de" || language != 3)
            return Fail("German localization profile mapped incorrectly");
    }
    else
        return Fail("German localization profile was rejected");
    if (!mojorecomp::config::ParseLocalizationProfile("nl", profile, language) ||
        profile != "nl" || language != 16)
        return Fail("Dutch localization profile mapped incorrectly");
    if (!mojorecomp::config::ParseLocalizationProfile("fr", profile, language) ||
        profile != "fr" || language != 4)
        return Fail("French localization profile mapped incorrectly");
    if (!mojorecomp::config::ParseLocalizationProfile("es", profile, language) ||
        profile != "es" || language != 5)
        return Fail("Spanish localization profile mapped incorrectly");
    if (!mojorecomp::config::ParseLocalizationProfile("it", profile, language) ||
        profile != "it" || language != 6)
        return Fail("Italian localization profile mapped incorrectly");
    if (mojorecomp::config::ParseLocalizationProfile("ja", profile, language))
        return Fail("unsupported localization profile was accepted");

    std::puts("OK: runtime config schema and public graphics settings parsed correctly.");
    return 0;
}
