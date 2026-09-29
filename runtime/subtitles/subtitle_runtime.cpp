#include "subtitle_runtime.h"

#include "dialogue_catalog.h"
#include "../host/host_paths.h"
#include "../kernel/klog.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>

namespace mojorecomp::subtitles {
namespace {

std::once_flag g_initializeOnce;
std::mutex g_mutex;
DialogueCatalog g_catalog;
SubtitleSnapshot g_snapshot;
bool g_catalogLoaded = false;
bool g_testOverride = false;
std::string g_activeAutoDialogueId;
std::string g_activeAutoAsset;

bool DiagnosticsEnabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("MOJORECOMP_SUBTITLE_DIAGNOSTICS");
        return value && *value && value[0] != '0';
    }();
    return enabled;
}

std::filesystem::path CatalogPath()
{
    if (const char* overridePath = std::getenv("MOJORECOMP_DIALOGUE_CATALOG");
        overridePath && *overridePath)
        return overridePath;
    return HostPaths::ExeDir() / "Dialogues_En-US.json";
}

void CopyText(char* destination, size_t capacity, std::string_view value)
{
    if (!capacity)
        return;
    const size_t count = std::min(capacity - 1u, value.size());
    std::memcpy(destination, value.data(), count);
    destination[count] = '\0';
}

std::string FileNameLower(std::string_view path)
{
    const size_t slash = path.find_last_of("\\/");
    std::string result(path.substr(slash == std::string_view::npos ? 0 : slash + 1));
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char ch) {
        return char(std::tolower(ch));
    });
    return result;
}

bool IsLocalizedVoiceArchive(std::string_view path)
{
    const std::string name = FileNameLower(path);
    static constexpr std::array<std::string_view, 6> archives = {
        "english.rcf", "dutch.rcf", "french.rcf",
        "german.rcf", "italian.rcf", "spanish.rcf",
    };
    return std::find(archives.begin(), archives.end(), name) != archives.end();
}

void SetSnapshot(const DialogueLine* dialogue)
{
    g_snapshot = {};
    if (!dialogue)
        return;
    g_snapshot.visible = true;
    CopyText(g_snapshot.speaker.data(), g_snapshot.speaker.size(), dialogue->speaker);
    CopyText(g_snapshot.text.data(), g_snapshot.text.size(), dialogue->text);
}

} // namespace

void Initialize()
{
    std::call_once(g_initializeOnce, [] {
        const auto path = CatalogPath();
        if (!std::filesystem::exists(path))
        {
            if (DiagnosticsEnabled())
                KLOG("Subtitle catalog not found: %s\n", path.string().c_str());
            return;
        }

        std::string error;
        DialogueCatalog catalog;
        if (!LoadDialogueCatalog(path, catalog, error))
        {
            KLOG("Subtitle catalog load failed: %s\n", error.c_str());
            return;
        }
        {
            std::lock_guard lock(g_mutex);
            g_catalog = std::move(catalog);
            g_catalogLoaded = true;
        }
        if (DiagnosticsEnabled())
        {
            KLOG("Subtitle catalog loaded: locale=%s cues=%zu path=%s\n",
                 g_catalog.locale.c_str(), g_catalog.dialogues.size(), path.string().c_str());
        }

        const char* testId = std::getenv("MOJORECOMP_SUBTITLE_TEST_ID");
        if (testId && *testId)
        {
            g_testOverride = true;
            if (!Activate(testId))
                KLOG("Subtitle test id not found: %s\n", testId);
        }
    });
}

bool Activate(std::string_view dialogueId)
{
    std::lock_guard lock(g_mutex);
    if (!g_catalogLoaded)
        return false;
    const DialogueLine* dialogue = g_catalog.Find(dialogueId);
    if (!dialogue)
        return false;
    SetSnapshot(dialogue);
    if (DiagnosticsEnabled())
        KLOG("Subtitle activated: id=%s speaker=%s\n",
             dialogue->id.c_str(), dialogue->speaker.c_str());
    return true;
}

void Clear()
{
    std::lock_guard lock(g_mutex);
    g_snapshot = {};
}

void OnXmaPlaybackPosition(std::string_view logicalAsset,
                           uint32_t stream,
                           uint32_t sampleRate,
                           uint64_t consumedSamples)
{
    if (logicalAsset.empty() || !sampleRate)
        return;

    std::lock_guard lock(g_mutex);
    if (!g_catalogLoaded || g_testOverride || !g_catalog.HasAsset(logicalAsset))
        return;

    const uint64_t playbackMs = consumedSamples * 1000ull / sampleRate;
    const DialogueLine* dialogue = g_catalog.FindCue(logicalAsset, playbackMs);

    const std::string nextId = dialogue ? dialogue->id : std::string{};
    if (nextId == g_activeAutoDialogueId && g_activeAutoAsset == logicalAsset)
        return;

    g_activeAutoDialogueId = nextId;
    g_activeAutoAsset = std::string(logicalAsset);
    SetSnapshot(dialogue);
    if (DiagnosticsEnabled())
    {
        if (dialogue)
        {
            KLOG("Subtitle cue enter: id=%s asset='%s' stream=%u time_ms=%llu speaker='%s'\n",
                 dialogue->id.c_str(), dialogue->asset.c_str(), stream,
                 static_cast<unsigned long long>(playbackMs),
                 dialogue->speaker.c_str());
        }
        else
        {
            KLOG("Subtitle cue leave: asset='%s' stream=%u time_ms=%llu\n",
                 std::string(logicalAsset).c_str(), stream,
                 static_cast<unsigned long long>(playbackMs));
        }
    }
}

void OnXmaPlaybackReset(std::string_view logicalAsset)
{
    if (logicalAsset.empty())
        return;
    std::lock_guard lock(g_mutex);
    if (g_testOverride || g_activeAutoAsset != logicalAsset)
        return;
    g_activeAutoDialogueId.clear();
    g_activeAutoAsset.clear();
    g_snapshot = {};
    if (DiagnosticsEnabled())
        KLOG("Subtitle playback reset: asset='%s'\n", std::string(logicalAsset).c_str());
}

void ResetAudioCursors()
{
    std::lock_guard lock(g_mutex);
    g_activeAutoDialogueId.clear();
    g_activeAutoAsset.clear();
    if (!g_testOverride)
        g_snapshot = {};
}

bool NeedsAudioSourceTracking(std::string_view archivePath)
{
    std::lock_guard lock(g_mutex);
    return g_catalogLoaded && !g_catalog.dialogues.empty() &&
           IsLocalizedVoiceArchive(archivePath);
}

bool HasAudioAsset(std::string_view logicalAsset)
{
    std::lock_guard lock(g_mutex);
    return g_catalogLoaded && g_catalog.HasAsset(logicalAsset);
}

bool GetFirstCueTiming(std::string_view logicalAsset,
                       uint64_t& startMs,
                       uint64_t& endMs)
{
    std::lock_guard lock(g_mutex);
    if (!g_catalogLoaded)
        return false;

    const DialogueLine* first = nullptr;
    for (const auto& dialogue : g_catalog.dialogues)
    {
        if (dialogue.asset.size() != logicalAsset.size())
            continue;
        bool same = true;
        for (size_t index = 0; index < logicalAsset.size(); ++index)
        {
            if (std::tolower(static_cast<unsigned char>(dialogue.asset[index])) !=
                std::tolower(static_cast<unsigned char>(logicalAsset[index])))
            {
                same = false;
                break;
            }
        }
        if (!same)
            continue;
        if (!first || dialogue.startMs < first->startMs)
            first = &dialogue;
    }
    if (!first)
        return false;
    startMs = first->startMs;
    endMs = first->endMs;
    return true;
}

SubtitleSnapshot GetSnapshot()
{
    std::lock_guard lock(g_mutex);
    return g_snapshot;
}

} // namespace mojorecomp::subtitles
