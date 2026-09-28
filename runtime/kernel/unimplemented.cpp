#include "unimplemented.h"

#include <cstdio>

[[noreturn]] void MojoRecompRaiseUnimplementedImport(const char* name, uint32_t lr)
{
    std::fprintf(stderr, "[unimplemented] %s (guest lr=%08X)\n", name, lr);
    throw MojoRecompUnimplementedImport{name, lr};
}
