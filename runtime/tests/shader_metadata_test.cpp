#include <cstdio>

#include "../gpu/shader_metadata.h"

namespace {

int Fail(const char* message)
{
    std::fprintf(stderr, "FAIL: %s\n", message);
    return 1;
}

} // namespace

int main()
{
    using mojorecomp::gpu::ParseShaderMetadata;
    using mojorecomp::gpu::ShaderMetadata;

    ShaderMetadata metadata{};
    constexpr const char* formatted = R"json({
        "kind":"vs",
        "tfetchConsts" : [ 3, 7 ],
        "tfetchDims": [2, 1],
        "aluConsts":[],
        "aluDynamic"   :true,
        "attributes" : [
            {"location":0,"fetchSlot":2,"format":57,"signed":1,
             "integer":0,"strideDwords":4,"offsetDwords":1,"indirect":0}
        ]
    })json";
    if (!ParseShaderMetadata(formatted, 0, metadata))
        return Fail("valid metadata with flexible JSON whitespace was rejected");
    if (metadata.textureSlots != std::vector<uint32_t>({3, 7}) ||
        metadata.textureDimensions != std::vector<uint32_t>({2, 1}) ||
        !metadata.aluDynamic || metadata.attributes.size() != 1 ||
        metadata.attributes[0].fetchSlot != 2)
        return Fail("valid metadata fields were not parsed exactly");

    if (ParseShaderMetadata(
            R"json({"tfetchConsts":[3],"tfetchDims":[],"aluConsts":[],"aluDynamic":false})json",
            0, metadata))
        return Fail("mismatched texture metadata was accepted");
    if (ParseShaderMetadata(
            R"json({"tfetchConsts":[3,],"tfetchDims":[2],"aluConsts":[],"aluDynamic":false})json",
            0, metadata))
        return Fail("malformed metadata array was accepted");
    if (ParseShaderMetadata(
            R"json({"tfetchConsts":[],"tfetchDims":[],"aluConsts":[]})json",
            0, metadata))
        return Fail("metadata missing a required boolean was accepted");
    if (ParseShaderMetadata(
            R"json({"kind":"vs","tfetchConsts":[],"tfetchDims":[],"aluConsts":[],"aluDynamic":false,"attributes":[7]})json",
            0, metadata))
        return Fail("non-object vertex attribute was accepted");
    if (ParseShaderMetadata(
            R"json({"kind":"vs","tfetchConsts":[],"tfetchDims":[],"aluConsts":[],"aluDynamic":false,"attributes":[{"location":0}]})json",
            0, metadata))
        return Fail("incomplete vertex attribute was accepted");
    if (ParseShaderMetadata(
            R"json({"kind":"ps","tfetchConsts":[],"tfetchDims":[],"aluConsts":[],"aluDynamic":false} trailing)json",
            1, metadata))
        return Fail("trailing malformed JSON was accepted");
    if (!ParseShaderMetadata(
            R"json({"kind":"ps","tfetchConsts":[],"tfetchDims":[],"aluConsts":[],"aluDynamic":false})json",
            1, metadata))
        return Fail("valid pixel metadata without vertex attributes was rejected");
    if (ParseShaderMetadata(
            R"json({"kind":"ps","tfetchConsts":[],"tfetchDims":[],"aluConsts":[],"aluDynamic":false})json",
            0, metadata))
        return Fail("pixel metadata was accepted for a vertex shader");

    std::puts("PASS: shader metadata parsing is formatting-independent and fails closed.");
    return 0;
}
