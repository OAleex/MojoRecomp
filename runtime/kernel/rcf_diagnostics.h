#pragma once

#include <cstdint>
#include <string>

namespace mojorecomp::rcfdiag {

struct EntryMatch
{
    uint32_t id = 0;
    uint64_t offset = 0;
    uint64_t size = 0;
    std::string name;
};

struct LoadedRangeMatch
{
    uint32_t physicalStart = 0;
    uint32_t loadedSize = 0;
    std::string logicalAsset;
    uint32_t preferredStream = 0;
};

bool FindEntry(const std::string& hostPath, uint64_t fileOffset,
               EntryMatch& match, std::string& error);
bool ResolveRsdSourceInfo(const std::string& hostPath, const EntryMatch& entry,
                          std::string& logicalAsset, uint32_t& preferredStream);
void RecordLoadedRange(const EntryMatch& entry, uint64_t fileOffset,
                       uint32_t physicalStart,
                       uint32_t loadedSize, std::string logicalAsset = {},
                       uint32_t preferredStream = 0);
bool FindRecentLoadedRange(uint32_t physicalAddress, LoadedRangeMatch& match);
void ClearLoadedRanges();

} // namespace mojorecomp::rcfdiag
