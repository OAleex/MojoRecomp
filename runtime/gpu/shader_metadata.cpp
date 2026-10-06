#include "shader_metadata.h"

#include <limits>
#include <string>

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-literal-operator"
#endif
#include "../../thirdparty/XenonRecomp/thirdparty/tomlplusplus/vendor/json.hpp"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

namespace mojorecomp::gpu {
namespace {

using Json = nlohmann::json;

bool ReadUint(const Json& object, const char* key, uint32_t& value)
{
    const auto found = object.find(key);
    if (found == object.end() || !found->is_number_integer())
        return false;
    if (found->is_number_unsigned())
    {
        const uint64_t raw = found->get<uint64_t>();
        if (raw > std::numeric_limits<uint32_t>::max())
            return false;
        value = static_cast<uint32_t>(raw);
        return true;
    }
    const int64_t raw = found->get<int64_t>();
    if (raw < 0 || uint64_t(raw) > std::numeric_limits<uint32_t>::max())
        return false;
    value = static_cast<uint32_t>(raw);
    return true;
}

bool ReadInt32(const Json& object, const char* key, int32_t& value)
{
    const auto found = object.find(key);
    if (found == object.end() || !found->is_number_integer())
        return false;
    if (found->is_number_unsigned())
    {
        const uint64_t raw = found->get<uint64_t>();
        if (raw > uint64_t(std::numeric_limits<int32_t>::max()))
            return false;
        value = static_cast<int32_t>(raw);
        return true;
    }
    const int64_t raw = found->get<int64_t>();
    if (raw < std::numeric_limits<int32_t>::min() ||
        raw > std::numeric_limits<int32_t>::max())
        return false;
    value = static_cast<int32_t>(raw);
    return true;
}

bool ReadUintArray(const Json& object, const char* key,
                   std::vector<uint32_t>& values)
{
    const auto found = object.find(key);
    if (found == object.end() || !found->is_array())
        return false;
    values.clear();
    values.reserve(found->size());
    for (const Json& item : *found)
    {
        if (!item.is_number_integer())
            return false;
        uint64_t raw = 0;
        if (item.is_number_unsigned())
            raw = item.get<uint64_t>();
        else
        {
            const int64_t signedRaw = item.get<int64_t>();
            if (signedRaw < 0)
                return false;
            raw = uint64_t(signedRaw);
        }
        if (raw > std::numeric_limits<uint32_t>::max())
            return false;
        values.push_back(static_cast<uint32_t>(raw));
    }
    return true;
}

bool ReadAttributes(const Json& root, bool vertexShader,
                    std::vector<ShaderAttributeMetadata>& attributes)
{
    const auto found = root.find("attributes");
    if (found == root.end())
        return !vertexShader;
    if (!found->is_array())
        return false;

    attributes.clear();
    attributes.reserve(found->size());
    for (const Json& object : *found)
    {
        if (!object.is_object())
            return false;
        ShaderAttributeMetadata attribute{};
        if (!ReadInt32(object, "location", attribute.location) ||
            !ReadUint(object, "fetchSlot", attribute.fetchSlot) ||
            !ReadUint(object, "format", attribute.format) ||
            !ReadUint(object, "signed", attribute.isSigned) ||
            !ReadUint(object, "integer", attribute.isInteger) ||
            !ReadUint(object, "strideDwords", attribute.strideDwords) ||
            !ReadUint(object, "offsetDwords", attribute.offsetDwords) ||
            !ReadUint(object, "indirect", attribute.indirect))
            return false;
        attributes.push_back(attribute);
    }
    return true;
}

} // namespace

bool ParseShaderMetadata(std::string_view text, uint32_t expectedType,
                         ShaderMetadata& out)
{
    out = {};
    try
    {
        const Json root = Json::parse(text.begin(), text.end(), nullptr, false);
        if (root.is_discarded() || !root.is_object())
            return false;

        const auto kind = root.find("kind");
        const auto dynamic = root.find("aluDynamic");
        if (kind == root.end() || !kind->is_string() ||
            dynamic == root.end() || !dynamic->is_boolean())
            return false;
        const std::string shaderKind = kind->get<std::string>();
        if (expectedType > 1 ||
            shaderKind != (expectedType == 0 ? "vs" : "ps"))
            return false;

        ShaderMetadata parsed{};
        if (!ReadUintArray(root, "tfetchConsts", parsed.textureSlots) ||
            !ReadUintArray(root, "tfetchDims", parsed.textureDimensions) ||
            !ReadUintArray(root, "aluConsts", parsed.aluConsts) ||
            parsed.textureSlots.size() != parsed.textureDimensions.size() ||
            !ReadAttributes(root, shaderKind == "vs", parsed.attributes))
            return false;
        parsed.aluDynamic = dynamic->get<bool>();
        out = std::move(parsed);
        return true;
    }
    catch (...)
    {
        out = {};
        return false;
    }
}

} // namespace mojorecomp::gpu
