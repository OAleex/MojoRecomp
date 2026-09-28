#include "klog.h"

#include <cstring>

void MojoRecompKernelCallTrace(const char* name, uint64_t hit)
{
    if (std::strncmp(name, "__imp__", 7) == 0)
        name += 7;

    if (hit == 0)
        std::fprintf(stderr, "[kcall] %s\n", name);
    else
        std::fprintf(stderr, "[kcall+] %s hit %llu times\n", name,
                     static_cast<unsigned long long>(hit));
}
