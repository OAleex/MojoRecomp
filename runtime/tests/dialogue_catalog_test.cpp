#include "../subtitles/dialogue_catalog.h"

#include <filesystem>
#include <fstream>

int main()
{
    const auto path = std::filesystem::temp_directory_path() /
        "mojorecomp-dialogues-en-us-test.json";
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << R"json({
  "schema_version": 1,
  "locale": "en-US",
  "dialogues": {
    "nis01.rsd": [
      {
        "start_ms": 7759,
        "end_ms": 13367,
        "speaker": "Coco",
        "text": "Crash, help me get this gizmo working!"
      },
      {
        "start_ms": 14022,
        "end_ms": 15041,
        "speaker": "Crash",
        "text": "(belches)"
      }
    ]
  }
})json";
    }

    mojorecomp::subtitles::DialogueCatalog catalog;
    std::string error;
    if (!mojorecomp::subtitles::LoadDialogueCatalog(path, catalog, error))
        return 1;
    std::error_code removeError;
    std::filesystem::remove(path, removeError);

    if (catalog.schemaVersion != 1 || catalog.locale != "en-US" ||
        catalog.dialogues.size() != 2)
        return 2;

    const auto* coco = catalog.Find("nis01.rsd#1");
    if (!coco || coco->asset != "nis01.rsd" || coco->speaker != "Coco" ||
        coco->startMs != 7759 || coco->endMs != 13367 ||
        coco->text != "Crash, help me get this gizmo working!")
        return 3;

    if (!catalog.HasAsset("NIS01.RSD"))
        return 4;
    if (catalog.HasAsset("nis02.rsd"))
        return 5;
    if (catalog.FindCue("nis01.rsd", 7758))
        return 6;
    if (catalog.FindCue("NIS01.RSD", 7759) != coco)
        return 7;
    if (catalog.FindCue("nis01.rsd", 13367))
        return 8;

    const auto* crash = catalog.FindCue("nis01.rsd", 14022);
    if (!crash || crash->speaker != "Crash" || crash->text != "(belches)")
        return 9;
    if (catalog.Find("missing"))
        return 10;
    return 0;
}
