#include "title_resources.h"

#include <vector>

#include "gpu/pm4.h"
#include "kernel/klog.h"

void CotTitleResources_OnFileOpened(std::string_view guestPath, FILE* file,
                                    uint64_t size, bool truncated)
{
    constexpr uint64_t kMaximumShaderContainerBytes = 1024u * 1024u;
    if (!file || truncated || !size || size > kMaximumShaderContainerBytes ||
        !CotTitleResources_IsBinkShaderPath(guestPath))
        return;

#if defined(_WIN32)
    const int64_t savedPosition = _ftelli64(file);
    const bool rewound = _fseeki64(file, 0, SEEK_SET) == 0;
#else
    const int64_t savedPosition = ftello(file);
    const bool rewound = fseeko(file, 0, SEEK_SET) == 0;
#endif
    std::vector<uint8_t> container(static_cast<size_t>(size));
    const bool registered = rewound &&
        std::fread(container.data(), 1, container.size(), file) == container.size() &&
        Pm4_RegisterBinkPixelShaderContainer(container.data(), container.size());
    if (!registered)
        KLOG("Bink shader container registration failed for '%.*s'\n",
             static_cast<int>(guestPath.size()), guestPath.data());

    if (savedPosition >= 0)
    {
#if defined(_WIN32)
        _fseeki64(file, savedPosition, SEEK_SET);
#else
        fseeko(file, savedPosition, SEEK_SET);
#endif
    }
}
