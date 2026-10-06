#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace mojorecomp::gpu {

struct ShaderAttributeMetadata
{
    int32_t location = -1;
    uint32_t fetchSlot = 0;
    uint32_t format = 0;
    uint32_t isSigned = 0;
    uint32_t isInteger = 0;
    uint32_t strideDwords = 0;
    uint32_t offsetDwords = 0;
    uint32_t indirect = 0;
};

struct ShaderMetadata
{
    std::vector<uint32_t> textureSlots;
    std::vector<uint32_t> textureDimensions;
    std::vector<uint32_t> aluConsts;
    std::vector<ShaderAttributeMetadata> attributes;
    bool aluDynamic = false;
};

bool ParseShaderMetadata(std::string_view text, uint32_t expectedType,
                         ShaderMetadata& out);

} // namespace mojorecomp::gpu
