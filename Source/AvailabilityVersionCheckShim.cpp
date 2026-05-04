#include <cstdint>
#include <dlfcn.h>

struct dyld_build_version_t
{
    uint32_t platform;
    uint32_t version;
};

extern "C" bool _availability_version_check (uint32_t count, dyld_build_version_t versions[])
{
    using CheckFn = bool (*) (uint32_t, dyld_build_version_t*);

    static auto* const systemCheck = reinterpret_cast<CheckFn> (dlsym (RTLD_NEXT, "_availability_version_check"));

    if (systemCheck != nullptr)
        return systemCheck (count, versions);

    return true;
}
