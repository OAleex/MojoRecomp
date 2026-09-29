#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace mojorecomp::subtitles {

struct SubtitleSnapshot
{
    bool visible = false;
    std::array<char, 64> speaker{};
    std::array<char, 512> text{};
};

void Initialize();
bool Activate(std::string_view dialogueId);
void Clear();
void OnXmaPlaybackPosition(std::string_view logicalAsset,
                           uint32_t stream,
                           uint32_t sampleRate,
                           uint64_t consumedSamples);
void OnXmaPlaybackReset(std::string_view logicalAsset);
void ResetAudioCursors();
bool NeedsAudioSourceTracking(std::string_view archivePath);
bool HasAudioAsset(std::string_view logicalAsset);
bool GetFirstCueTiming(std::string_view logicalAsset,
                       uint64_t& startMs,
                       uint64_t& endMs);
SubtitleSnapshot GetSnapshot();

} // namespace mojorecomp::subtitles
