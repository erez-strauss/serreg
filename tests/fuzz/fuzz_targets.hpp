// tests/fuzz/fuzz_targets.hpp — the four fuzz entry points (spec §10.2 #7, FR-D5; cc N2). Each has the
// LLVMFuzzerTestOneInput signature, must return normally for any input, and is driven either by the portable
// driver (fuzz_driver.cpp, GCC has no libFuzzer) or by libFuzzer when built with -DES_SERREG_LIBFUZZER=ON.
#pragma once
#include <cstddef>
#include <cstdint>

extern "C"
{
    int fuzz_rri(const std::uint8_t* data, std::size_t size); // dyn::dictionary::add_rri
    int fuzz_capture(
        const std::uint8_t* data,
        std::size_t size); // tools::verify + tools::dump (text and json, discarding sink)
    int fuzz_elf(const std::uint8_t* data,
                 std::size_t size); // extract_section_from_image, extract_embedded_dictionary
    int fuzz_config(const std::uint8_t* data, std::size_t size); // cli::parse_config
}
