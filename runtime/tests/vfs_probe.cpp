#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "../kernel/vfs.h"

namespace fs = std::filesystem;

int main()
{
    const fs::path root = fs::temp_directory_path() / "mojorecomp-vfs-probe";
    const fs::path overlay = fs::temp_directory_path() / "mojorecomp-vfs-overlay-probe";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::remove_all(overlay, ec);
    fs::create_directories(root / "Movies", ec);
    fs::create_directories(overlay, ec);
    if (ec)
        return 1;

    {
        std::ofstream file(root / "Movies" / "Logo.bik", std::ios::binary);
        file << "probe";
    }
    {
        std::ofstream file(root / "default.rcf", std::ios::binary);
        file << "base";
    }
    {
        std::ofstream file(root / "fallback.txt", std::ios::binary);
        file << "base-only";
    }
    {
        std::ofstream file(overlay / "default.rcf", std::ios::binary);
        file << "overlay";
    }

    VfsSetGameRoot(root.string());
    VfsSetGameOverlay(overlay.string());
    const fs::path savedGames = root / "saved-games";
    const fs::path localData = root / "local-data";
    VfsSetUserRoots(
        savedGames.string(),
        (localData / "content").string(),
        (localData / "cache").string(),
        (localData / "utility").string());

    if (fs::path(VfsTranslate("save:\\slot.dat", true)) != savedGames / "slot.dat")
        return 14;
    if (fs::path(VfsTranslate("cache:\\shader.bin", true)) != localData / "cache" / "shader.bin")
        return 15;
    if (fs::exists(savedGames) || fs::exists(localData / "cache"))
        return 16;

    if (fs::path(VfsResolveExisting("game:\\default.rcf")) != overlay / "default.rcf")
        return 10;
    if (fs::path(VfsResolveExisting("dvd:\\fallback.txt")) != root / "fallback.txt")
        return 11;
    if (!VfsTranslate("game:\\default.rcf", true).empty())
        return 12;
    if (!VfsResolveExisting("game:\\..\\default.rcf").empty())
        return 13;

    if (!VfsCreateSymbolicLink("\\??\\xbmovie:", "dvd:\\Movies"))
        return 2;

    const std::string resolved = VfsResolveExisting("xbmovie:\\logo.bik");
    if (resolved.empty() || !fs::exists(resolved))
    {
        std::cerr << "xbmovie resolve failed: '" << resolved << "'\n";
        return 3;
    }
    if (VfsDeviceWritable("xbmovie:\\logo.bik"))
        return 4;

    if (!VfsCreateSymbolicLink("\\??\\xbsave:", "save:\\profile"))
        return 5;
    if (!VfsDeviceWritable("xbsave:\\slot.dat"))
        return 6;
    const std::string translated = VfsTranslate("xbsave:\\slot.dat", true);
    if (translated.empty() || fs::path(translated).filename() != "slot.dat")
        return 7;

    if (!VfsDeleteSymbolicLink("\\??\\xbmovie:"))
        return 8;
    if (!VfsResolveExisting("xbmovie:\\logo.bik").empty())
        return 9;

    fs::remove_all(root, ec);
    fs::remove_all(overlay, ec);
    std::cout << "VFS symbolic-link probe passed\n";
    return 0;
}
