#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace mojorecomp::subtitles {

struct DialogueLine
{
    std::string id;
    std::string asset;
    uint64_t startMs = 0;
    uint64_t endMs = 0;
    std::string speaker;
    std::string text;
};

struct DialogueCatalog
{
    uint32_t schemaVersion = 0;
    std::string locale;
    std::vector<DialogueLine> dialogues;

    const DialogueLine* Find(std::string_view id) const noexcept;
    const DialogueLine* FindCue(std::string_view asset,
                                uint64_t timeMs) const noexcept;
    bool HasAsset(std::string_view asset) const noexcept;
};

bool LoadDialogueCatalog(const std::filesystem::path& path,
                         DialogueCatalog& catalog,
                         std::string& error);

} // namespace mojorecomp::subtitles
