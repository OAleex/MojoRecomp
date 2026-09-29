#include "../kernel/rcf_diagnostics.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

void WriteBe32(std::vector<uint8_t>& bytes, size_t at, uint32_t value)
{
    bytes[at + 0] = uint8_t(value >> 24);
    bytes[at + 1] = uint8_t(value >> 16);
    bytes[at + 2] = uint8_t(value >> 8);
    bytes[at + 3] = uint8_t(value);
}

void WriteLe32(std::vector<uint8_t>& bytes, size_t at, uint32_t value)
{
    bytes[at + 0] = uint8_t(value);
    bytes[at + 1] = uint8_t(value >> 8);
    bytes[at + 2] = uint8_t(value >> 16);
    bytes[at + 3] = uint8_t(value >> 24);
}

void WriteNameRecord(std::vector<uint8_t>& bytes, size_t& cursor,
                     const std::string& name)
{
    const uint32_t nameLength = static_cast<uint32_t>(name.size() + 1u);
    WriteLe32(bytes, cursor + 12, nameLength);
    cursor += 16;
    for (const char ch : name)
        bytes[cursor++] = static_cast<uint8_t>(ch);
    bytes[cursor++] = 0;
    cursor += 3;
}

} // namespace

int main()
{
    constexpr size_t headerSize = 60;
    constexpr size_t table1Offset = headerSize;
    constexpr size_t table1Size = 24;
    constexpr size_t table2Offset = table1Offset + table1Size;
    const std::string firstName = "dialogue\\first.xma";
    const std::string secondName = "dialogue\\second.xma";
    const size_t table2Size = 8 + (16 + firstName.size() + 1 + 3) +
                              (16 + secondName.size() + 1 + 3);
    constexpr uint32_t firstOffset = 2048;
    constexpr uint32_t firstSize = 128;
    constexpr uint32_t secondOffset = 4096;
    constexpr uint32_t secondSize = 64;
    std::vector<uint8_t> bytes(secondOffset + secondSize, 0);

    constexpr char magic[] = "ATG CORE CEMENT LIBRARY";
    for (size_t index = 0; index + 1 < sizeof(magic); ++index)
        bytes[index] = static_cast<uint8_t>(magic[index]);
    bytes[32] = 2;
    bytes[33] = 1;
    bytes[34] = 1;
    bytes[35] = 1;
    WriteBe32(bytes, 36, static_cast<uint32_t>(table1Offset));
    WriteBe32(bytes, 40, static_cast<uint32_t>(table1Size));
    WriteBe32(bytes, 44, static_cast<uint32_t>(table2Offset));
    WriteBe32(bytes, 48, static_cast<uint32_t>(table2Size));
    WriteBe32(bytes, 52, 0);
    WriteBe32(bytes, 56, 2);

    WriteBe32(bytes, table1Offset + 0, 0x11111111u);
    WriteBe32(bytes, table1Offset + 4, firstOffset);
    WriteBe32(bytes, table1Offset + 8, firstSize);
    WriteBe32(bytes, table1Offset + 12, 0x22222222u);
    WriteBe32(bytes, table1Offset + 16, secondOffset);
    WriteBe32(bytes, table1Offset + 20, secondSize);

    WriteLe32(bytes, table2Offset + 0, 2048);
    WriteLe32(bytes, table2Offset + 4, 0);
    size_t cursor = table2Offset + 8;
    WriteNameRecord(bytes, cursor, firstName);
    WriteNameRecord(bytes, cursor, secondName);
    if (cursor != table2Offset + table2Size)
        return 10;

    const auto path = std::filesystem::temp_directory_path() /
        "mojorecomp-rcf-diagnostics-test.rcf";
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
    }

    mojorecomp::rcfdiag::EntryMatch match;
    std::string error;
    if (!mojorecomp::rcfdiag::FindEntry(path.string(), firstOffset + 7, match, error) ||
        match.id != 0x11111111u || match.name != firstName ||
        match.offset != firstOffset || match.size != firstSize)
        return 1;
    mojorecomp::rcfdiag::ClearLoadedRanges();
    mojorecomp::rcfdiag::RecordLoadedRange(
        match, firstOffset + 7, 0x0F100000u, 64, "first.rsd", 1);
    mojorecomp::rcfdiag::LoadedRangeMatch loaded;
    if (!mojorecomp::rcfdiag::FindRecentLoadedRange(0x0F100010u, loaded) ||
        loaded.logicalAsset != "first.rsd" || loaded.preferredStream != 1 ||
        loaded.physicalStart != 0x0F100000u || loaded.loadedSize != 64)
        return 5;
    if (mojorecomp::rcfdiag::FindRecentLoadedRange(0x0F100100u, loaded))
        return 6;
    if (mojorecomp::rcfdiag::FindEntry(path.string(), 3000, match, error) || !error.empty())
        return 2;
    if (!mojorecomp::rcfdiag::FindEntry(path.string(), secondOffset + 1, match, error) ||
        match.id != 0x22222222u || match.name != secondName)
        return 3;

    std::error_code removeError;
    std::filesystem::remove(path, removeError);
    return 0;
}
