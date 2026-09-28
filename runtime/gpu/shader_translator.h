#pragma once
// COT patches vertex-fetch instructions at runtime, so shaders are translated
// in-process from live Xenos microcode instead of relying on a prebuilt cache.
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ShaderTranslator
{
struct Result
{
    std::vector<uint8_t> spirv;
    std::string metaJson;
    std::string hlsl;
};

// Thread-safe. `name` is the runtime cache key; failures identify the rejected stage.
bool Translate(const std::string& name, const uint8_t* ucode, size_t size,
               Result& out, std::string& err);

// Compile host-side helper HLSL directly to Vulkan SPIR-V using the same DXC
// instance/arguments as translated guest shaders. Used by small renderer
// utility passes that don't originate from Xenos microcode.
bool CompileHostHlsl(const std::string& hlsl, bool isVs,
                     std::vector<uint8_t>& spirv, std::string& err,
                     bool enableStencilExport = false);

// Inject the host-only clip-space aspect transform at XenosRecomp's final
// vertex-position seam. Kept as a named/testable step so an upstream emitter
// change cannot silently disable widescreen support.
bool ApplyHostVertexAspectTransform(std::string& hlsl);

// Write the SPIR-V and metadata atomically as a logical pair; remove both if
// either write fails so the cache cannot observe a partial entry.
bool WritePair(const std::filesystem::path& outDir, const std::string& name,
               const Result& r);

// Translate every *.ucode file in parallel. Returns zero only if every shader
// succeeds. MOJORECOMP_TRANSLATE_KEEP_HLSL retains intermediate HLSL.
int TranslateDirectory(const char* ucodeDir, const char* outDir);
} // namespace ShaderTranslator
