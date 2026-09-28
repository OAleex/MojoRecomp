#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct MojoTranslatedShader
{
    uint32_t type = 0; // 0 = vertex, 1 = pixel
    uint64_t hash = 0;
    std::vector<uint8_t> spirv;
    std::string metaJson;
    std::string hlsl;
};

void ShaderCache_SetTranslationEnabled(bool enabled);
bool ShaderCache_TranslationEnabled();

// Called once per distinct stage/hash by PM4. First-use translation is
// synchronous so a failure remains attributable to its exact shader.
void ShaderCache_OnBind(uint32_t type, uint64_t hash,
                        const uint8_t* code, uint32_t sizeDwords);

const MojoTranslatedShader* ShaderCache_Find(uint32_t type, uint64_t hash);
uint64_t ShaderCache_TranslatedCount();
uint64_t ShaderCache_FailedCount();
