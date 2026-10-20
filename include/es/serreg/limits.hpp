// es/serreg/limits.hpp — the reader limits (2.1.16, maintainer answer O2; spec FR-D5).
//
// The contract: for any sequence of input bytes — a capture, a stream, a frame, an RRI, an ELF file, a config
// file, a ring header in shared memory — every public reading API returns a value or an error, and never crashes,
// aborts or asserts, executes undefined behaviour, or allocates or loops beyond these limits. Errors are reported;
// decoding moves on to the next item where the format allows it. "Wrong data" means corrupt or truncated files,
// buggy writers, mismatched versions, a peer that wrote garbage into a ring; it does not mean system out-of-memory
// within the limits, or a policy the user asked for (ES_SERREG_CONFLICT=abort).
//
// The defaults are chosen so that no honest data reaches them. They are process-wide (set_decode_limits) and
// can be narrowed or widened per call where a reader takes a `decode_limits` (tools::file_options,
// tools::block_options, tools::dump/verify over a path). A reader reads the limits once per walk, not per frame.
#pragma once
#include <cstddef>
#include <cstdint>

namespace es::serreg
{

struct decode_limits
{
    // Largest frame a dictionary-driven reader accepts, header included (the writer limit agreed under O2: a
    // frame above it is a problem, `bad frame_len`, and the walk stops because it cannot resynchronise).
    std::uint64_t frame_bytes = std::uint64_t{64} << 20;
    // Largest dictionary (an RRI image, a FILE's embedded dictionary, a dictionary frame) a reader loads.
    std::uint64_t dictionary_bytes = std::uint64_t{256} << 20;
    // Most entries a dictionary may declare (n_entries); more is corrupt_dictionary.
    std::uint32_t dictionary_entries = 1'000'000;
    // Deepest nesting of struct levels the dictionary-driven decoder follows (at most fmt::max_nesting, which
    // bounds the typed decoder at compile time).
    std::uint32_t image_depth = 64;
    // Largest input read whole: an RRI, ELF or config file, or a capture handed to the span-based readers
    // through tools::read_file. Captures of any size go through the windowed readers instead.
    std::uint64_t input_bytes = std::uint64_t{1} << 30;
    // Window of the windowed readers (for_each_frame over a path, verify/dump over a path, srdump): the file is
    // read this many bytes at a time; a frame larger than the window grows it to that frame, within frame_bytes.
    std::size_t window_bytes = std::size_t{10} << 20;
    // Problem lines a walk keeps; after that only a count ("… and N more") so a corrupt multi-terabyte capture
    // does not accumulate unbounded text.
    std::size_t problems = 1000;

    friend constexpr bool operator==(const decode_limits&, const decode_limits&) noexcept = default;
};

// The process-wide limits: what every reader uses unless the call carries its own. Thread-safe.
decode_limits get_decode_limits() noexcept;
void set_decode_limits(const decode_limits& l) noexcept;

} // namespace es::serreg
