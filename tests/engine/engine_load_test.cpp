// Loads the 3D engine built by cmake/Engine.cmake (TESSERACT_WITH_ENGINE) and
// checks that it serves the ABI its headers declare.

#include <engine/engine_abi_version.h>
#include <engine/engine_c.h>
#include <engine/engine_loader.h>

#include <cstdio>

int main()
{
    engine_loader loader{};
    const engine_status status = engine_load(
        TESSERACT_ENGINE_LIBRARY_PATH, ENGINE_ABI_VERSION_MAJOR, ENGINE_ABI_VERSION_MINOR, &loader);
    if (status != ENGINE_OK)
    {
        std::fprintf(stderr, "engine_load(%s) failed: status %d: %s\n",
                     TESSERACT_ENGINE_LIBRARY_PATH, static_cast<int>(status), loader.message);
        return 1;
    }
    std::printf("loaded %s, ABI %u.%u\n", TESSERACT_ENGINE_LIBRARY_PATH,
                static_cast<unsigned>(loader.api->abi_major),
                static_cast<unsigned>(loader.api->abi_minor));
    if (engine_unload(&loader) != ENGINE_OK)
    {
        std::fprintf(stderr, "engine_unload failed\n");
        return 1;
    }
    return 0;
}
