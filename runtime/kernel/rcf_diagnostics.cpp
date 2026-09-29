#include "rcf_diagnostics.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <span>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mojorecomp::rcfdiag {
namespace {

constexpr size_t kHeaderSize = 60;
constexpr std::string_view kMagic = "ATG CORE CEMENT LIBRARY";
constexpr std::array<uint8_t, 4> kSupportedFlags{2, 1, 1, 1};
constexpr uint64_t kTable1RecordSize = 12;
constexpr size_t kTable2PrefixSize = 8;
constexpr size_t kTable2RecordHeaderSize = 16;
constexpr size_t kTable2RecordPaddingSize = 3;
constexpr uint32_t kMaxFileCount = 1'000'000;

struct Entry
{
    uint32_t id = 0;
    uint64_t offset = 0;
    uint64_t size = 0;
    std::string name;
};

struct Index
{
    std::vector<Entry> entries;
};

std::mutex g_cacheMutex;
std::unordered_map<std::string, std::shared_ptr<const Index>> g_cache;
std::unordered_map<std::string, std::string> g_failures;

std::mutex g_loadedRangeMutex;
std::deque<LoadedRangeMatch> g_loadedRanges;
constexpr size_t kMaxLoadedRanges = 4096;
constexpr uint64_t kPhysicalAddressSpace = 0x20000000ull;

struct RsdSourceInfo
{
    std::string logicalAsset;
    uint32_t preferredStream = 0;
};

std::mutex g_rsdSourceMutex;
std::unordered_map<std::string, std::unordered_map<uint32_t, RsdSourceInfo>> g_rsdSources;

uint32_t ReadBe32(const uint8_t* bytes)
{
    return (uint32_t(bytes[0]) << 24) | (uint32_t(bytes[1]) << 16) |
           (uint32_t(bytes[2]) << 8) | uint32_t(bytes[3]);
}

uint32_t ReadLe32(const uint8_t* bytes)
{
    return uint32_t(bytes[0]) | (uint32_t(bytes[1]) << 8) |
           (uint32_t(bytes[2]) << 16) | (uint32_t(bytes[3]) << 24);
}

bool RangeFits(uint64_t offset, uint64_t size, uint64_t limit)
{
    return offset <= limit && size <= limit - offset;
}

bool AlignUp(uint64_t value, uint64_t alignment, uint64_t& output)
{
    if (!alignment || (alignment & (alignment - 1u)) != 0 ||
        value > UINT64_MAX - (alignment - 1u))
        return false;
    output = (value + alignment - 1u) & ~(alignment - 1u);
    return true;
}

bool EndsWithIgnoreCase(std::string_view value, std::string_view suffix)
{
    if (value.size() < suffix.size())
        return false;
    const size_t offset = value.size() - suffix.size();
    for (size_t index = 0; index < suffix.size(); ++index)
    {
        if (std::tolower(static_cast<unsigned char>(value[offset + index])) !=
            std::tolower(static_cast<unsigned char>(suffix[index])))
            return false;
    }
    return true;
}

std::string LowerAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return char(std::tolower(ch)); });
    return value;
}

bool ParseRsdSourceInfo(std::span<const uint8_t> bytes, RsdSourceInfo& info)
{
    std::vector<std::string> runs;
    std::string current;
    for (const uint8_t value : bytes)
    {
        if (value >= 32 && value <= 126)
        {
            current.push_back(static_cast<char>(value));
            continue;
        }
        if (current.size() >= 6)
            runs.push_back(std::move(current));
        current.clear();
    }
    if (current.size() >= 6)
        runs.push_back(std::move(current));

    std::string sourcePath;
    for (const auto& run : runs)
    {
        const std::string lowered = LowerAscii(run);
        if (lowered.find(".wav") == std::string::npos)
            continue;
        if (run.size() > sourcePath.size())
            sourcePath = run;
    }
    if (sourcePath.empty())
        return false;

    while (!sourcePath.empty() && sourcePath.front() == '*')
        sourcePath.erase(sourcePath.begin());

    const size_t slash = sourcePath.find_last_of("\\/");
    std::string base = sourcePath.substr(slash == std::string::npos ? 0 : slash + 1);
    if (!EndsWithIgnoreCase(base, ".wav"))
        return false;
    base.resize(base.size() - 4);
    if (EndsWithIgnoreCase(base, "_ch*"))
        base.resize(base.size() - 4);
    if (base.empty())
        return false;

    info.logicalAsset = base + ".rsd";
    const std::string loweredPath = LowerAscii(sourcePath);
    info.preferredStream =
        (loweredPath.find("\\6_channel\\") != std::string::npos ||
         loweredPath.find("/6_channel/") != std::string::npos ||
         loweredPath.find("_ch*.wav") != std::string::npos)
            ? 1u
            : 0u;
    return true;
}

std::shared_ptr<const Index> LoadIndex(const std::string& path, std::string& error)
{
    std::ifstream file(std::filesystem::path(path), std::ios::binary);
    if (!file)
    {
        error = "could not open RCF";
        return {};
    }
    file.seekg(0, std::ios::end);
    const auto length = file.tellg();
    if (length < std::streamoff(kHeaderSize))
    {
        error = "RCF is smaller than its fixed header";
        return {};
    }
    const uint64_t sourceSize = static_cast<uint64_t>(length);
    file.seekg(0, std::ios::beg);

    std::array<uint8_t, kHeaderSize> header{};
    file.read(reinterpret_cast<char*>(header.data()), header.size());
    if (!file)
    {
        error = "could not read RCF header";
        return {};
    }
    if (!std::equal(kMagic.begin(), kMagic.end(), header.begin()) ||
        std::any_of(header.begin() + kMagic.size(), header.begin() + 32,
                    [](uint8_t value) { return value != 0; }))
    {
        error = "unsupported RCF signature";
        return {};
    }
    if (!std::equal(kSupportedFlags.begin(), kSupportedFlags.end(), header.begin() + 32))
    {
        error = "unsupported RCF flags";
        return {};
    }

    const uint64_t table1Offset = ReadBe32(header.data() + 36);
    const uint64_t table1Size = ReadBe32(header.data() + 40);
    const uint64_t table2Offset = ReadBe32(header.data() + 44);
    const uint64_t table2Size = ReadBe32(header.data() + 48);
    const uint32_t reserved = ReadBe32(header.data() + 52);
    const uint32_t fileCount = ReadBe32(header.data() + 56);
    if (reserved != 0 || !fileCount || fileCount > kMaxFileCount ||
        table1Size != uint64_t(fileCount) * kTable1RecordSize ||
        !RangeFits(table1Offset, table1Size, sourceSize) ||
        !RangeFits(table2Offset, table2Size, sourceSize) ||
        table1Offset < kHeaderSize || table2Offset < table1Offset + table1Size ||
        table2Size < kTable2PrefixSize)
    {
        error = "invalid RCF index layout";
        return {};
    }

    std::vector<uint8_t> table1(static_cast<size_t>(table1Size));
    file.seekg(static_cast<std::streamoff>(table1Offset));
    file.read(reinterpret_cast<char*>(table1.data()), static_cast<std::streamsize>(table1.size()));
    if (!file)
    {
        error = "could not read RCF table 1";
        return {};
    }

    auto index = std::make_shared<Index>();
    index->entries.reserve(fileCount);
    for (uint32_t item = 0; item < fileCount; ++item)
    {
        const size_t at = size_t(item) * size_t(kTable1RecordSize);
        index->entries.push_back({
            ReadBe32(table1.data() + at),
            ReadBe32(table1.data() + at + 4),
            ReadBe32(table1.data() + at + 8),
            {},
        });
    }

    std::vector<uint8_t> table2(static_cast<size_t>(table2Size));
    file.seekg(static_cast<std::streamoff>(table2Offset));
    file.read(reinterpret_cast<char*>(table2.data()), static_cast<std::streamsize>(table2.size()));
    if (!file)
    {
        error = "could not read RCF table 2";
        return {};
    }
    const uint64_t alignment = ReadLe32(table2.data());
    if (alignment != 2048 || ReadLe32(table2.data() + 4) != 0)
    {
        error = "unsupported RCF table 2 header";
        return {};
    }

    uint64_t dataStart = 0;
    if (!AlignUp(table2Offset + table2Size, alignment, dataStart))
    {
        error = "RCF data start overflow";
        return {};
    }

    std::vector<size_t> physicalOrder(index->entries.size());
    for (size_t item = 0; item < physicalOrder.size(); ++item)
        physicalOrder[item] = item;
    std::sort(physicalOrder.begin(), physicalOrder.end(), [&](size_t left, size_t right) {
        return index->entries[left].offset < index->entries[right].offset;
    });

    uint64_t previousEnd = dataStart;
    std::unordered_set<uint64_t> seenOffsets;
    for (const size_t item : physicalOrder)
    {
        const auto& entry = index->entries[item];
        if (entry.offset < dataStart || (entry.offset % alignment) != 0 ||
            !seenOffsets.insert(entry.offset).second || entry.offset < previousEnd ||
            !RangeFits(entry.offset, entry.size, sourceSize))
        {
            error = "invalid RCF data entry layout";
            return {};
        }
        previousEnd = entry.offset + entry.size;
    }
    if (physicalOrder.empty() || index->entries[physicalOrder.front()].offset != dataStart ||
        previousEnd != sourceSize)
    {
        error = "RCF data extent does not match archive size";
        return {};
    }

    size_t cursor = kTable2PrefixSize;
    std::unordered_set<std::string> seenNames;
    for (const size_t item : physicalOrder)
    {
        if (cursor + kTable2RecordHeaderSize > table2.size())
        {
            error = "RCF table 2 ended before all names";
            return {};
        }
        const uint32_t nameLength = ReadLe32(table2.data() + cursor + 12);
        const size_t nameStart = cursor + kTable2RecordHeaderSize;
        if (nameLength < 2 || nameStart + nameLength + kTable2RecordPaddingSize > table2.size())
        {
            error = "invalid RCF entry name length";
            return {};
        }
        const size_t nameEnd = nameStart + nameLength;
        if (table2[nameEnd - 1] != 0 ||
            std::find(table2.begin() + nameStart, table2.begin() + nameEnd - 1, uint8_t(0)) !=
                table2.begin() + nameEnd - 1 ||
            std::any_of(table2.begin() + nameEnd,
                        table2.begin() + nameEnd + kTable2RecordPaddingSize,
                        [](uint8_t value) { return value != 0; }))
        {
            error = "invalid RCF entry name record";
            return {};
        }
        std::string name(reinterpret_cast<const char*>(table2.data() + nameStart),
                         nameLength - 1u);
        if (!std::all_of(name.begin(), name.end(), [](unsigned char ch) { return ch < 0x80; }) ||
            !seenNames.insert(name).second)
        {
            error = "invalid or duplicate RCF entry name";
            return {};
        }
        index->entries[item].name = std::move(name);
        cursor = nameEnd + kTable2RecordPaddingSize;
    }
    if (cursor != table2.size())
    {
        error = "RCF table 2 contains trailing data";
        return {};
    }

    std::sort(index->entries.begin(), index->entries.end(),
              [](const Entry& left, const Entry& right) { return left.offset < right.offset; });
    return index;
}

std::shared_ptr<const Index> GetIndex(const std::string& path, std::string& error)
{
    {
        std::lock_guard lock(g_cacheMutex);
        if (const auto found = g_cache.find(path); found != g_cache.end())
            return found->second;
        if (const auto failed = g_failures.find(path); failed != g_failures.end())
        {
            error = failed->second;
            return {};
        }
    }

    std::string loadError;
    auto loaded = LoadIndex(path, loadError);
    std::lock_guard lock(g_cacheMutex);
    if (loaded)
        g_cache.emplace(path, loaded);
    else
        g_failures.emplace(path, loadError);
    error = loadError;
    return loaded;
}

} // namespace

bool FindEntry(const std::string& hostPath, uint64_t fileOffset,
               EntryMatch& match, std::string& error)
{
    match = {};
    error.clear();
    const auto index = GetIndex(hostPath, error);
    if (!index || index->entries.empty())
        return false;

    const auto found = std::upper_bound(
        index->entries.begin(), index->entries.end(), fileOffset,
        [](uint64_t offset, const Entry& entry) { return offset < entry.offset; });
    if (found == index->entries.begin())
        return false;
    const auto& entry = *std::prev(found);
    if (fileOffset < entry.offset || fileOffset - entry.offset >= entry.size)
        return false;
    match.id = entry.id;
    match.offset = entry.offset;
    match.size = entry.size;
    match.name = entry.name;
    return true;
}

bool ResolveRsdSourceInfo(const std::string& hostPath, const EntryMatch& entry,
                          std::string& logicalAsset, uint32_t& preferredStream)
{
    logicalAsset.clear();
    preferredStream = 0;
    if (!EndsWithIgnoreCase(entry.name, ".rsd"))
        return false;

    {
        std::lock_guard lock(g_rsdSourceMutex);
        const auto pathIt = g_rsdSources.find(hostPath);
        if (pathIt != g_rsdSources.end())
        {
            const auto entryIt = pathIt->second.find(entry.id);
            if (entryIt != pathIt->second.end())
            {
                logicalAsset = entryIt->second.logicalAsset;
                preferredStream = entryIt->second.preferredStream;
                return !logicalAsset.empty();
            }
        }
    }

    std::ifstream file(std::filesystem::path(hostPath), std::ios::binary);
    if (!file)
        return false;
    const size_t readSize = static_cast<size_t>(std::min<uint64_t>(entry.size, 4096));
    std::vector<uint8_t> header(readSize);
    file.seekg(static_cast<std::streamoff>(entry.offset));
    file.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()));
    if (!file)
        return false;

    RsdSourceInfo info;
    if (!ParseRsdSourceInfo(header, info))
        return false;

    {
        std::lock_guard lock(g_rsdSourceMutex);
        g_rsdSources[hostPath][entry.id] = info;
    }
    logicalAsset = info.logicalAsset;
    preferredStream = info.preferredStream;
    return true;
}

void RecordLoadedRange(const EntryMatch& entry, uint64_t fileOffset,
                       uint32_t physicalStart,
                       uint32_t loadedSize, std::string logicalAsset,
                       uint32_t preferredStream)
{
    if (!loadedSize || fileOffset < entry.offset || fileOffset >= entry.offset + entry.size)
        return;

    const uint64_t entryRelative = fileOffset - entry.offset;
    const uint64_t entryRemaining = entry.size - entryRelative;
    const uint32_t boundedSize = static_cast<uint32_t>(
        std::min<uint64_t>(loadedSize, entryRemaining));
    if (!boundedSize || uint64_t(physicalStart) + boundedSize > kPhysicalAddressSpace)
        return;

    LoadedRangeMatch loaded;
    loaded.physicalStart = physicalStart;
    loaded.loadedSize = boundedSize;
    loaded.logicalAsset = std::move(logicalAsset);
    loaded.preferredStream = preferredStream;

    std::lock_guard lock(g_loadedRangeMutex);
    g_loadedRanges.push_back(std::move(loaded));
    while (g_loadedRanges.size() > kMaxLoadedRanges)
        g_loadedRanges.pop_front();
}

bool FindRecentLoadedRange(uint32_t physicalAddress, LoadedRangeMatch& match)
{
    std::lock_guard lock(g_loadedRangeMutex);
    for (auto it = g_loadedRanges.rbegin(); it != g_loadedRanges.rend(); ++it)
    {
        if (physicalAddress < it->physicalStart)
            continue;
        const uint64_t relative = uint64_t(physicalAddress) - it->physicalStart;
        if (relative >= it->loadedSize)
            continue;
        match = *it;
        return true;
    }
    match = {};
    return false;
}

void ClearLoadedRanges()
{
    std::lock_guard lock(g_loadedRangeMutex);
    g_loadedRanges.clear();
}

} // namespace mojorecomp::rcfdiag
