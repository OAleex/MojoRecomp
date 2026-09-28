#include <cstdio>
#include <string>

#include "../gpu/shader_translator.h"

int main()
{
    std::string hlsl =
        "void first() {\n"
        "\toPos.xy += g_HalfPixelOffset * oPos.w;\n"
        "}\n"
        "void second() {\n"
        "\toPos.xy += g_HalfPixelOffset * oPos.w;\n"
        "}\n";

    if (!ShaderTranslator::ApplyHostVertexAspectTransform(hlsl))
    {
        std::fprintf(stderr, "FAIL: aspect transform seam was not found\n");
        return 1;
    }

    const std::string expected = "\toPos.xy *= MOJORECOMP_ASPECT_SCALE;\n";
    const size_t first = hlsl.find(expected);
    const size_t second = first == std::string::npos
        ? std::string::npos
        : hlsl.find(expected, first + expected.size());
    if (first == std::string::npos || second == std::string::npos ||
        hlsl.find(expected, second + expected.size()) != std::string::npos)
    {
        std::fprintf(stderr, "FAIL: expected aspect transform at both vertex exits\n");
        return 1;
    }

    std::puts("OK: host vertex aspect transform is injected at every final-position seam.");
    return 0;
}
