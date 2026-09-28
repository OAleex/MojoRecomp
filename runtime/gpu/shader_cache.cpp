#include "shader_cache.h"

#include <atomic>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>

#include "../kernel/klog.h"
#include "../host/host_paths.h"
#include "shader_translator.h"

namespace {

std::atomic<bool> g_enabled{false};
std::atomic<uint64_t> g_translated{0};
std::atomic<uint64_t> g_failed{0};
std::atomic<uint64_t> g_diskHits{0};
std::deque<MojoTranslatedShader> g_shaders;
std::mutex g_mutex;

constexpr const char* kShaderDiskCacheVersion = "v3-aspect-safe-area";
constexpr uint64_t kShaderDiskCacheMagic = 0x32484341434F4A4Dull; // "MJOCACH2"
constexpr uint32_t kShaderDiskCacheSchema = 2;

struct ShaderDiskCacheStamp
{
    uint64_t magic = kShaderDiskCacheMagic;
    uint32_t schema = kShaderDiskCacheSchema;
    uint32_t sizeBytes = 0;
    uint64_t codeHash = 0;
};

uint64_t Fnv1a64(const uint8_t* bytes, size_t size)
{
    uint64_t hash = 0xCBF29CE484222325ull;
    for (size_t i = 0; i < size; ++i)
    {
        hash ^= bytes[i];
        hash *= 0x100000001B3ull;
    }
    return hash;
}

std::filesystem::path ShaderDiskCacheDir()
{
    if (const char* custom = std::getenv("MOJORECOMP_SHADER_CACHE_DIR"); custom && *custom)
        return std::filesystem::path(custom) / kShaderDiskCacheVersion;
    return HostPaths::ExeDir() / "cache" / "shaders" / kShaderDiskCacheVersion;
}

bool ReadBinaryFile(const std::filesystem::path& path, std::vector<uint8_t>& out)
{
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input)
        return false;
    const std::streamsize size = input.tellg();
    if (size <= 0)
        return false;
    input.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(size));
    return bool(input.read(reinterpret_cast<char*>(out.data()), size));
}

bool ReadTextFile(const std::filesystem::path& path, std::string& out)
{
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input)
        return false;
    const std::streamsize size = input.tellg();
    if (size <= 0)
        return false;
    input.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(size));
    return bool(input.read(out.data(), size));
}

bool LoadDiskTranslation(const std::string& name, const uint8_t* code,
                         size_t sizeBytes, ShaderTranslator::Result& out)
{
    const auto dir = ShaderDiskCacheDir();
    std::vector<uint8_t> stampBytes;
    if (!ReadBinaryFile(dir / (name + ".key"), stampBytes) ||
        stampBytes.size() != sizeof(ShaderDiskCacheStamp))
        return false;

    ShaderDiskCacheStamp stamp{};
    std::memcpy(&stamp, stampBytes.data(), sizeof(stamp));
    if (stamp.magic != kShaderDiskCacheMagic ||
        stamp.schema != kShaderDiskCacheSchema ||
        stamp.sizeBytes != sizeBytes ||
        stamp.codeHash != Fnv1a64(code, sizeBytes))
        return false;

    if (!ReadBinaryFile(dir / (name + ".spv"), out.spirv) ||
        !ReadTextFile(dir / (name + ".meta.json"), out.metaJson) ||
        !ReadTextFile(dir / (name + ".hlsl"), out.hlsl))
    {
        out = {};
        return false;
    }
    if ((out.spirv.size() & 3u) != 0 || out.spirv.size() < sizeof(uint32_t))
    {
        out = {};
        return false;
    }
    uint32_t magic = 0;
    std::memcpy(&magic, out.spirv.data(), sizeof(magic));
    if (magic != 0x07230203u)
    {
        out = {};
        return false;
    }
    return true;
}

void StoreDiskTranslation(const std::string& name, const uint8_t* code,
                          size_t sizeBytes,
                          const ShaderTranslator::Result& translated)
{
    if (!code || !sizeBytes || translated.spirv.empty() ||
        translated.metaJson.empty() || translated.hlsl.empty())
        return;
    std::error_code ec;
    const auto dir = ShaderDiskCacheDir();
    std::filesystem::create_directories(dir, ec);
    if (ec)
        return;

    auto writeBinary = [&](const std::filesystem::path& path, const void* data, size_t size) {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (output)
            output.write(reinterpret_cast<const char*>(data),
                         static_cast<std::streamsize>(size));
    };
    writeBinary(dir / (name + ".spv"), translated.spirv.data(), translated.spirv.size());
    writeBinary(dir / (name + ".meta.json"), translated.metaJson.data(), translated.metaJson.size());
    writeBinary(dir / (name + ".hlsl"), translated.hlsl.data(), translated.hlsl.size());

    // Write the validation stamp last. A partially written cache entry is never
    // considered reusable because LoadDiskTranslation requires this exact key.
    ShaderDiskCacheStamp stamp{};
    stamp.sizeBytes = static_cast<uint32_t>(sizeBytes);
    stamp.codeHash = Fnv1a64(code, sizeBytes);
    writeBinary(dir / (name + ".key"), &stamp, sizeof(stamp));
}

const MojoTranslatedShader* FindLocked(uint32_t type, uint64_t hash)
{
    for (const auto& shader : g_shaders)
        if (shader.type == type && shader.hash == hash)
            return &shader;
    return nullptr;
}

} // namespace

void ShaderCache_SetTranslationEnabled(bool enabled)
{
    g_enabled.store(enabled, std::memory_order_release);
    if (enabled)
        KLOG("shader cache: in-process XenosRecomp + DXC translation enabled\n");
}

bool ShaderCache_TranslationEnabled()
{
    return g_enabled.load(std::memory_order_acquire);
}

void ShaderCache_OnBind(uint32_t type, uint64_t hash,
                        const uint8_t* code, uint32_t sizeDwords)
{
    if (!ShaderCache_TranslationEnabled() || type > 1 || !code || !sizeDwords)
        return;

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (FindLocked(type, hash))
            return;
    }

    char name[32]{};
    std::snprintf(name, sizeof(name), "%s_%016llx",
                  type == 0 ? "vs" : "ps",
                  static_cast<unsigned long long>(hash));

    ShaderTranslator::Result translated;
    std::string error;
    const size_t sizeBytes = size_t(sizeDwords) * 4u;
    const bool diskHit = LoadDiskTranslation(name, code, sizeBytes, translated);
    if (!diskHit &&
        !ShaderTranslator::Translate(name, code, sizeBytes,
                                     translated, error))
    {
        g_failed.fetch_add(1, std::memory_order_relaxed);
        KLOG("shader cache: FAILED %s (%u dwords): %s\n",
             name, sizeDwords, error.c_str());
        if (const char* dump = std::getenv("MOJORECOMP_SHADER_DUMP_DIR");
            dump && *dump && !translated.hlsl.empty())
        {
            std::error_code ec;
            const std::filesystem::path dir(dump);
            std::filesystem::create_directories(dir, ec);
            if (!ec)
            {
                std::ofstream hlsl(dir / (std::string(name) + ".failed.hlsl"),
                                   std::ios::binary);
                hlsl.write(translated.hlsl.data(),
                           static_cast<std::streamsize>(translated.hlsl.size()));
            }
        }
        return;
    }
    if (!diskHit)
        StoreDiskTranslation(name, code, sizeBytes, translated);
    else
        g_diskHits.fetch_add(1, std::memory_order_relaxed);

    // Diagnostic-only single-shader SPIR-V override used to A/B optimized
    // structured translations without changing the normal translator/cache.
    // Keep the sidecar/HLSL from the real translation so constant gathering and
    // metadata remain identical; only the host program binary is substituted.
    if (type == 1 && hash == 0x80428150BC1ED3ABull)
    {
        if (const char* overridePath = std::getenv("MOJORECOMP_PS8042_OVERRIDE_SPV");
            overridePath && *overridePath)
        {
            std::ifstream input(overridePath, std::ios::binary | std::ios::ate);
            if (input)
            {
                const std::streamsize bytes = input.tellg();
                if (bytes > 0 && (bytes & 3) == 0)
                {
                    input.seekg(0, std::ios::beg);
                    std::vector<uint8_t> overrideSpv(static_cast<size_t>(bytes));
                    if (input.read(reinterpret_cast<char*>(overrideSpv.data()), bytes))
                    {
                        translated.spirv = std::move(overrideSpv);
                        KLOG("shader cache: diagnostic PS8042 override loaded: %s (%lld bytes)\n",
                             overridePath, static_cast<long long>(bytes));
                    }
                }
            }
        }
    }

    MojoTranslatedShader shader{};
    shader.type = type;
    shader.hash = hash;
    shader.spirv = std::move(translated.spirv);
    shader.metaJson = std::move(translated.metaJson);
    shader.hlsl = std::move(translated.hlsl);

    const size_t spvSize = shader.spirv.size();
    const size_t hlslSize = shader.hlsl.size();

    if (const char* dump = std::getenv("MOJORECOMP_SHADER_DUMP_DIR"); dump && *dump)
    {
        std::error_code ec;
        const std::filesystem::path dir(dump);
        std::filesystem::create_directories(dir, ec);
        if (!ec)
        {
            std::ofstream spv(dir / (std::string(name) + ".spv"), std::ios::binary);
            spv.write(reinterpret_cast<const char*>(shader.spirv.data()),
                      static_cast<std::streamsize>(shader.spirv.size()));
            std::ofstream meta(dir / (std::string(name) + ".meta.json"), std::ios::binary);
            meta.write(shader.metaJson.data(), static_cast<std::streamsize>(shader.metaJson.size()));
            std::ofstream hlsl(dir / (std::string(name) + ".hlsl"), std::ios::binary);
            hlsl.write(shader.hlsl.data(), static_cast<std::streamsize>(shader.hlsl.size()));
        }
    }
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!FindLocked(type, hash))
            g_shaders.push_back(std::move(shader));
    }

    const uint64_t count = g_translated.fetch_add(1, std::memory_order_relaxed) + 1;
    KLOG_DIAG("shader cache: %s %s -> %zu-byte SPIR-V, %zu-byte HLSL (total=%llu)\n",
              diskHit ? "loaded" : "translated", name, spvSize, hlslSize,
              static_cast<unsigned long long>(count));
}

const MojoTranslatedShader* ShaderCache_Find(uint32_t type, uint64_t hash)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return FindLocked(type, hash);
}

uint64_t ShaderCache_TranslatedCount()
{
    return g_translated.load(std::memory_order_relaxed);
}

uint64_t ShaderCache_FailedCount()
{
    return g_failed.load(std::memory_order_relaxed);
}
