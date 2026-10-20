// src/image_none.cpp — the null image backend (src/image.hpp; review addendum A4; 2.1.8). Selected with CMake
// -DES_SERREG_IMAGE_FORMAT=none, only together with the static library: the whole program is one image, nothing is
// ever unloaded. Registration, the published index, captures and export_image() work unchanged; every entry
// belongs to the one image, so none needs a descriptor copy. With ES_SERREG_EMBED_NONE the headers emit no `es_serreg_dict` section either.
#include "image.hpp"

namespace es::serreg::image
{
namespace
{
constinit const char the_image = 0; // one id for every address
}

const void* current(const void*) noexcept
{
    return &the_image;
}
const void* main_image() noexcept
{
    return &the_image;
}
const char* name(const void*) noexcept
{
    return "<main program>";
}
bool can_unload(const void*) noexcept
{
    return false;
}
const void* first_registry_marker() noexcept
{
    return nullptr; // one image, one copy
}

} // namespace es::serreg::image
