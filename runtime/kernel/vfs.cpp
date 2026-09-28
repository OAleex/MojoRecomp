#include "vfs.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <unordered_map>

#include "klog.h"

namespace fs = std::filesystem;

namespace {

struct Mount
{
    fs::path root;
    bool writable = false;
};

std::mutex g_vfsMutex;
std::map<std::string, Mount> g_mounts;
std::map<std::string, std::string> g_symbolicLinks;
std::unordered_map<std::string, std::string> g_existingCache;
std::optional<fs::path> g_gameOverlay;

std::string Lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool SplitGuestPath(const std::string& guestPath, std::string& device,
                    std::string& relative)
{
    const size_t colon = guestPath.find(':');
    if (colon == std::string::npos)
        return false;

    device = Lower(guestPath.substr(0, colon));
    if (const size_t slash = device.find_last_of("\\/"); slash != std::string::npos)
        device.erase(0, slash + 1);

    relative = guestPath.substr(colon + 1);
    std::replace(relative.begin(), relative.end(), '\\', '/');
    while (!relative.empty() && relative.front() == '/')
        relative.erase(relative.begin());

    // A guest path must never be able to escape a mounted host directory.
    fs::path clean;
    for (const auto& part : fs::path(relative))
    {
        if (part == "." || part.empty())
            continue;
        if (part == "..")
            return false;
        clean /= part;
    }
    relative = clean.generic_string();
    return true;
}

std::string ResolveCaseInsensitive(const fs::path& root, const std::string& relative)
{
    fs::path current = root;
    for (const auto& component : fs::path(relative))
    {
        if (component.empty())
            continue;
        std::error_code ec;
        const fs::path exact = current / component;
        if (fs::exists(exact, ec))
        {
            current = exact;
            continue;
        }

        const std::string wanted = Lower(component.string());
        bool found = false;
        for (const auto& entry : fs::directory_iterator(current, ec))
        {
            if (Lower(entry.path().filename().string()) == wanted)
            {
                current = entry.path();
                found = true;
                break;
            }
        }
        if (!found)
            return {};
    }
    return current.string();
}

bool IsGameDevice(const std::string& device)
{
    return device == "game" || device == "d" || device == "dvd";
}

std::string ResolveExistingWithin(const fs::path& root, const std::string& relative)
{
    const fs::path direct = relative.empty() ? root : root / fs::path(relative);
    std::error_code ec;
    if (fs::exists(direct, ec))
        return direct.string();
    return ResolveCaseInsensitive(root, relative);
}

} // namespace

void VfsMountDevice(const std::string& device, const std::string& hostPath, bool writable)
{
    std::lock_guard lock(g_vfsMutex);
    g_mounts[Lower(device)] = {fs::path(hostPath), writable};
    g_existingCache.clear();
}

void VfsUnmountDevice(const std::string& device)
{
    std::lock_guard lock(g_vfsMutex);
    g_mounts.erase(Lower(device));
    g_existingCache.clear();
}

bool VfsCreateSymbolicLink(const std::string& linkName, const std::string& targetPath)
{
    std::string device, relative;
    if (!SplitGuestPath(linkName, device, relative) || !relative.empty() || targetPath.empty())
        return false;
    std::lock_guard lock(g_vfsMutex);
    g_symbolicLinks[device] = targetPath;
    g_existingCache.clear();
    return true;
}

bool VfsDeleteSymbolicLink(const std::string& linkName)
{
    std::string device, relative;
    if (!SplitGuestPath(linkName, device, relative) || !relative.empty())
        return false;
    std::lock_guard lock(g_vfsMutex);
    const bool removed = g_symbolicLinks.erase(device) != 0;
    if (removed)
        g_existingCache.clear();
    return removed;
}

void VfsSetGameRoot(const std::string& hostPath)
{
    VfsMountDevice("game", hostPath, false);
    VfsMountDevice("d", hostPath, false);
    VfsMountDevice("dvd", hostPath, false);
    KLOG("VFS: game:/d:/dvd: -> %s (read-only)\n", hostPath.c_str());
}

void VfsSetGameOverlay(const std::string& hostPath)
{
    std::lock_guard lock(g_vfsMutex);
    if (hostPath.empty())
    {
        g_gameOverlay.reset();
        KLOG("VFS: game overlay disabled\n");
    }
    else
    {
        g_gameOverlay = fs::path(hostPath);
        KLOG("VFS: game overlay enabled (read-only)\n");
    }
    g_existingCache.clear();
}

void VfsSetUserRoot(const std::string& hostPath)
{
    VfsSetUserRoots(hostPath + "/save",
                    hostPath + "/content",
                    hostPath + "/cache",
                    hostPath + "/utility");
    KLOG("VFS: writable devices -> %s\n", hostPath.c_str());
}

void VfsSetUserRoots(const std::string& savePath,
                     const std::string& contentPath,
                     const std::string& cachePath,
                     const std::string& utilityPath)
{
    VfsMountDevice("save", savePath, true);
    VfsMountDevice("content", contentPath, true);
    VfsMountDevice("cache", cachePath, true);
    VfsMountDevice("utility", utilityPath, true);
    KLOG("VFS: save: -> %s\n", savePath.c_str());
    KLOG("VFS: content: -> %s\n", contentPath.c_str());
    KLOG("VFS: cache: -> %s\n", cachePath.c_str());
    KLOG("VFS: utility: -> %s\n", utilityPath.c_str());
}

bool VfsDeviceWritable(const std::string& guestPath)
{
    std::string current = guestPath;
    for (unsigned depth = 0; depth < 8; ++depth)
    {
        std::string device, relative;
        if (!SplitGuestPath(current, device, relative))
            return false;

        std::lock_guard lock(g_vfsMutex);
        if (auto alias = g_symbolicLinks.find(device); alias != g_symbolicLinks.end())
        {
            current = alias->second;
            if (!relative.empty())
            {
                if (!current.empty() && current.back() != '\\' && current.back() != '/')
                    current.push_back('\\');
                current += relative;
            }
            continue;
        }
        auto it = g_mounts.find(device);
        return it != g_mounts.end() && it->second.writable;
    }
    return false;
}

std::string VfsTranslate(const std::string& guestPath, bool forWrite)
{
    std::string current = guestPath;
    for (unsigned depth = 0; depth < 8; ++depth)
    {
        std::string device, relative;
        if (!SplitGuestPath(current, device, relative))
            return {};

        fs::path root;
        std::optional<fs::path> overlay;
        {
            std::lock_guard lock(g_vfsMutex);
            if (auto alias = g_symbolicLinks.find(device); alias != g_symbolicLinks.end())
            {
                current = alias->second;
                if (!relative.empty())
                {
                    if (!current.empty() && current.back() != '\\' && current.back() != '/')
                        current.push_back('\\');
                    current += relative;
                }
                continue;
            }
            auto it = g_mounts.find(device);
            if (it == g_mounts.end() || (forWrite && !it->second.writable))
                return {};
            root = it->second.root;
            if (!forWrite && IsGameDevice(device))
                overlay = g_gameOverlay;
        }

        if (overlay)
        {
            const std::string overlayPath = ResolveExistingWithin(*overlay, relative);
            if (!overlayPath.empty())
                return overlayPath;
        }
        return relative.empty() ? root.string() : (root / fs::path(relative)).string();
    }
    return {};
}

std::string VfsResolveExisting(const std::string& guestPath)
{
    {
        std::lock_guard lock(g_vfsMutex);
        auto cached = g_existingCache.find(guestPath);
        if (cached != g_existingCache.end())
            return cached->second;
    }

    std::string current = guestPath;
    std::string device, relative;
    fs::path root;
    std::optional<fs::path> overlay;
    for (unsigned depth = 0; depth < 8; ++depth)
    {
        if (!SplitGuestPath(current, device, relative))
            return {};
        std::lock_guard lock(g_vfsMutex);
        if (auto alias = g_symbolicLinks.find(device); alias != g_symbolicLinks.end())
        {
            current = alias->second;
            if (!relative.empty())
            {
                if (!current.empty() && current.back() != '\\' && current.back() != '/')
                    current.push_back('\\');
                current += relative;
            }
            continue;
        }
        auto it = g_mounts.find(device);
        if (it == g_mounts.end())
            return {};
        root = it->second.root;
        if (IsGameDevice(device))
            overlay = g_gameOverlay;
        break;
    }
    if (root.empty())
        return {};

    std::string result;
    if (overlay)
        result = ResolveExistingWithin(*overlay, relative);
    if (result.empty())
        result = ResolveExistingWithin(root, relative);

    std::lock_guard lock(g_vfsMutex);
    g_existingCache[guestPath] = result;
    return result;
}

void VfsForget(const std::string& guestPath)
{
    std::lock_guard lock(g_vfsMutex);
    g_existingCache.erase(guestPath);
}
