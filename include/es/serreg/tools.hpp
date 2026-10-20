// es/serreg/tools.hpp — library services behind `srdump` (spec §6.9, FR-D0). Everything the CLI can do is
// callable from here. No reflection dependency; works on captures/dictionaries from any program.
#pragma once
#include <es/serreg/dictionary.hpp>
#include <es/serreg/error.hpp>
#include <es/serreg/format.hpp>
#include <es/serreg/limits.hpp>
#include <es/serreg/version.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace es::serreg::tools
{

enum class format : std::uint8_t
{
    text,
    json,
    toml
};

struct dump_options
{
    format fmt = format::text;
    bool dict_only = false;
    bool hex = false;         // raw payload bytes alongside decoded values
    bool flatten = false;     // present base-class members inline (§6.1b option d)
    bool full_arrays = false; // do not elide arrays after `array_limit`
    std::uint32_t array_limit = 16;
    std::uint64_t first_frame = 0, last_frame = UINT64_MAX; // --frames A..B (inclusive, 0-based)
    std::uint64_t type_filter = 0;                          // schema hash; 0 = no filter
    std::string type_name_filter;                           // canonical name; empty = no filter
    bool raw = false; // 1.1: show print frames as records instead of the rendered message (--raw)
    // 2.2.1 (1.7) log filters: with `logs_only` only log frames are shown, those at or above `min_level`
    // (0 trace … 5 fatal) and, when set, with a timestamp in [since, until] (the writer's clock units — ns since
    // the epoch for the system clock)
    bool logs_only = false;
    std::uint8_t min_level = 0;
    std::uint64_t since = 0, until = UINT64_MAX;
    // 2.2.6: after the dictionary, one line per dictionary block (the opening block, every dictionary frame:
    // offset and what it added); in the frame dump, a marker line for every dictionary frame (--verbose)
    bool verbose = false;
};

struct verify_report
{
    std::uint64_t frames = 0, records = 0,
                  bytes = 0;        // data frames only (dictionary, checkpoint and
                                    // writer-info frames are consumed, not counted)
    std::uint64_t program_hash = 0; // the capture header's (0 for a bare stream)
    std::size_t problems_omitted = 0;
    std::vector<std::string> problems; // empty ⇒ ok
    // input a reader repairs, e.g. "frame 3 @120: record 0: flags.on: bool byte 0x02 read as
    // true". Warnings do not make the report fail.
    std::vector<std::string> warnings;
    bool ok() const noexcept
    {
        return problems.empty() && problems_omitted == 0;
    }
};

// Type verdicts: identical | names-only | metadata-only | breaking | added | removed | duplicate-name.
// Call/literal verdicts (gk 1.11): "call added" | "call removed" | "call changed" | "literal added" |
// "literal removed" | "literal changed". For enums, any enumerator value change or removal is breaking.
//
// 2.1.16 (decision E, review T-09): whether a removal or an addition breaks anything depends on who reads what, so
// the caller names the direction. `old_data`: a reader built from `b` reads data written by `a` (replay of old
// captures) — removed types, calls and literals are breaking (their old frames cannot be read typed), additions
// are not. `new_data`: a reader built from `a` reads data written by `b` (an old consumer on a live stream) —
// additions are breaking, removals are not. `both` marks both; `none` neither. A layout change, a changed call
// and a changed literal are breaking in every direction. The default is `old_data`.
enum class diff_direction : std::uint8_t
{
    none,
    old_data,
    new_data,
    both
};
constexpr std::string_view to_string(const diff_direction d) noexcept
{
    switch (d)
    {
    case diff_direction::none:
        return "none";
    case diff_direction::old_data:
        return "old-data";
    case diff_direction::new_data:
        return "new-data";
    case diff_direction::both:
        return "both";
    }
    return "?";
}
struct diff_entry
{
    std::string name;
    std::uint64_t schema_a = 0, schema_b = 0;
    std::string verdict;
    bool breaking = false; // breaks the report's direction (annotated and counted by to_text)
    std::string note;
    diff_entry() = default;
    diff_entry(std::string n, const std::uint64_t a, const std::uint64_t b, std::string v)
        : name(std::move(n)), schema_a(a), schema_b(b), verdict(std::move(v))
    {
    }
};
struct diff_report
{
    std::vector<diff_entry> entries;
    bool breaking = false;                               // any entry breaks the direction
    diff_direction direction = diff_direction::old_data; // 2.1.16
    std::size_t breaking_removals = 0, breaking_additions = 0, breaking_changes = 0;
    std::string to_text() const;
};

using es::serreg::
    library_version; // printed by `srdump --version`; defined in version.hpp since 2.2.1

// ---- dictionary sources ----------------------------------------------------------------------------
result<dyn::dictionary> load_dictionary_rri(std::span<const std::byte> rri);
result<dyn::dictionary> load_dictionary_file(std::string_view path); // .rri file
// The dictionary embedded in a program image (FR-D6; A4, 2.1.8). The format is recognised by its magic: ELF
// reads section `es_serreg_dict`; any other format is error::unsupported (no reader), error::truncated below
// 4 bytes, error::not_found when an ELF image has no section (built with ES_SERREG_IMAGE_FORMAT=none).
result<dyn::dictionary> extract_embedded_dictionary(std::string_view path);
result<dyn::dictionary> extract_embedded_dictionary(std::span<const std::byte> image);
// Same as extract_embedded_dictionary(path); kept for 2.1.7 callers and `srdump --elf`.
result<dyn::dictionary> extract_registry_from_elf(std::string_view path);
result<std::vector<std::byte>> extract_section_from_elf(std::string_view path,
                                                        std::string_view section);
// Same, over an ELF image already in memory (no file access; used by the fuzz target and by embedders).
result<std::vector<std::byte>> extract_section_from_image(std::span<const std::byte> image,
                                                          std::string_view section);

// 2.1.10: one scalar value as srdump's text output shows it — integers and floating point in decimal (shortest
// round trip), bool as true/false, char as a quoted character, byte as 0xNN. For decoded_value_visitor::scalar.
std::string value_text(kind k, std::span<const std::byte> bytes);

// ---- Wireshark (2.1.9; docs/wireshark.md) ------------------------------------------------------------------
// A Wireshark Lua dissector generated from `d`: one filterable field per value (`serreg.<type>.<field>`), the types,
// enumerators, call signatures and literals of `d`, and a fixed runtime that reads FILE captures (.ser), USER0
// pcaps and TCP/UDP ports, walks records with the slot rules of src/decoder.cpp, and compares a capture's own
// dictionary with `d` by hash (unknown entries and layout differences are expert infos). `srdump --wireshark`.
struct wireshark_options
{
    std::string generator = "srdump"; // named in the header comment and in SERREG.generator
    std::vector<std::string> sources; // the dictionary sources, listed in the header comment
};
std::string wireshark_dissector(const dyn::dictionary& d, const wireshark_options& o = {});

// ---- C++ definitions (2.1.16, plan A5; `srdump --cxx`) -------------------------------------------------------
// C++26 source that declares every type of `d` so that, compiled with this library, each regenerated type has
// exactly the schema hash and layout hash the dictionary records; the file checks that itself with static_asserts.
// Enums and structs in dependency order inside their namespaces, fixed-width scalars from kinds, std::array /
// std::string / std::vector / std::optional / std::variant members, bases from the <base:…> pseudo-fields, since /
// deprecated carried, a flat type's gaps as [[=es::serreg::skip]] padding members, [[=es::serreg::name("…")]] for
// every name a plain declaration cannot spell (template specialisations, nested classes, remapped namespaces).
// The std vocabulary (std::string, std::pair<…>, std::monostate, …) is spelled back, never defined.
struct cxx_options
{
    bool router =
        false; // also emit install_handlers(frame_router&), one handler per call signature
    // "old" -> "new" namespace prefixes (--namespace old=new); a remapped type keeps its canonical name
    // through the name annotation, so its hashes do not move
    std::vector<std::pair<std::string, std::string>> namespaces;
    std::string_view include = "<es/serreg/serreg.hpp>"; // the library include line
};
std::string cxx_header(const dyn::dictionary& d, const cxx_options& o = {});

// ---- capture inspection -------------------------------------------------------------------------------
// A capture is either a FILE (header + dictionary + frames) or a bare FRAME stream. `extra` dictionaries
// (from --dict/--elf) are merged over the embedded one.
// 2.0.6: the same walk over a file read in windows of `block_bytes` (a frame larger than a window grows the
// buffer to hold it, within decode_limits::frame_bytes), so memory stays bounded whatever the file size — a
// capture is limited by the file system, not by this reader (2.1.16, A6); also reads pipes and "-" (stdin),
// where the dictionary arrives as dictionary frames. A FILE's embedded dictionary is read first (with pread when
// it was back-patched to the end). frame_ref::bytes is valid only during the callback; offsets are file offsets.
// Checked per frame: frame length, alignment, flags (a bad length stops the walk and is listed as a problem);
// records are validated by whatever the callback runs on them (deserialize, dispatch, inspect, walk_values).
struct block_options
{
    std::size_t block_bytes = 0;           // 0: decode_limits::window_bytes (10 MiB by default)
    const decode_limits* limits = nullptr; // nullptr: the process-wide limits (set_decode_limits)
    // 1.7 (2.2.1): after a frame that cannot be parsed (bad frame_len), scan forward 8 bytes at a time for the
    // next checkpoint frame and continue from it; the skipped range is one problem line. Off: the walk stops
    // there, as it always did.
    bool resync = false;
    // 2.2.6: deliver the opening dictionary block and every dictionary / checkpoint / writer-info frame to the
    // callback too (frame_ref::kind says which; `call` is null, `bytes` is the block or frame). Off: data frames only.
    bool meta_frames = false;
};

// `verify` checks the container structure *and* decodes every record through the descriptor walker (cc F2 /
// gk 1.4); literal ids on the wire must equal the call signature's; dictionary conflicts are problems too.
// 2.1.16 (A6): the same over a file (or "-"), read in windows — a capture of any size with bounded memory.
result<verify_report> verify(std::string_view path, const dyn::dictionary* extra = nullptr,
                             const block_options& o = {});
result<verify_report> verify(std::span<const std::byte> capture,
                             const dyn::dictionary* extra = nullptr);

// ---- 1.7 (2.2.1): what a FILE says about itself without walking its frames ---------------------------------
// The writer-info frame's items (key → text; unknown keys kept by number).
struct writer_info
{
    std::vector<std::pair<std::uint16_t, std::string>> items;
    std::string_view get(std::uint16_t key) const noexcept; // empty when absent
};
struct checkpoint_info
{
    std::uint64_t offset{}, prev{}, next{}, data_frames_before{}, bytes_before{}, wall_clock_ns{},
        dictionary_offset{}, registry_version{};
    bool full_dictionary{}; // the checkpoint carries a full RRI
};
struct capture_info
{
    std::uint16_t minor{};
    std::uint32_t flags{};
    std::uint64_t program_hash{}, abi{}, file_size{};
    dyn::dictionary dict; // the opening dictionary merged with every delta the chain reaches
    std::vector<std::uint64_t>
        dictionary_offsets; // the opening block, then each linked dictionary frame
    std::vector<checkpoint_info> checkpoints; // the checkpoint chain, in order
    std::vector<std::pair<std::uint16_t, std::string>>
        writer; // the writer-info items (empty: none)
    // 2.2.6: a chain link that could not be followed — past the end of the file, not increasing, or not pointing
    // at a frame of the expected kind. The chain ends there; `verify` reports what the frame walk finds.
    std::vector<std::string> warnings;
    std::string_view writer_item(std::uint16_t key) const noexcept;
    bool has_writer_info() const noexcept
    {
        return !writer.empty();
    }
};
// Reads the header, follows the dictionary chain and the checkpoint chain with seeks (O(links), never O(frames))
// and reads the writer-info frame when the header announces one. A 1.6 file yields its (possibly back-patched)
// dictionary and no chains. `path` must be a regular file; a pipe is `unsupported`.
result<capture_info> info(std::string_view path);
std::string to_text(const capture_info& i);
std::string to_json(const capture_info& i);

// Streaming dump (cc F19): `out` is called with rendered text once per frame (and once for the dictionary);
// returning false from it stops the walk with error::io_error. A record that fails to decode is reported as a
// problem and makes `dump` return that error after the walk (exit 1 in srdump); the walk itself continues.
using write_fn = bool (*)(std::string_view chunk, void* ctx);
// 1.1 (FR-P4): render one print frame — the call's format literal, formatted with the frame's records the way
// the writer's compile-time check assumed (scalars as numbers/bools/chars, strings and literals as text, enums
// as the enumerator name, structs as their one-line text form). '\n' is appended for println calls. A frame
// whose call is not a print call → error::invalid_argument; an unknown call/literal → unknown_call/unknown_literal.
// `frame` is one frame (header included), as handed out by inspect() or a capture walk.
result<std::string> render_frame(std::span<const std::byte> frame, const dyn::dictionary& d);

// A log frame (log.hpp) taken apart: the envelope's constants and per-message fields, and the message rendered
// *without* the envelope prefix (what `srdump --level` prints after "<time> <LEVEL> <file>:<line> "). `thread`
// and `sequence` are present when the writer's log_options asked for them (mask bits log_layout::mask_thread /
// mask_sequence); `time_ns` is the raw timestamp of the writer's clock policy (log_layout::clock_of(mask)).
struct log_record
{
    std::uint8_t level{};
    std::string file; // the writer's __FILE__, or "-"
    std::uint32_t line{};
    std::uint8_t mask{};
    std::uint64_t time_ns{};
    std::uint32_t thread{};
    std::uint64_t sequence{};
    bool has_thread{}, has_sequence{};
    std::string format;  // the format literal's text
    std::string message; // the rendered line, no envelope, no trailing newline
};
// A frame whose call is not a log call → error::invalid_argument.
result<log_record> render_log(std::span<const std::byte> frame, const dyn::dictionary& d);

result<void> dump(std::span<const std::byte> capture, const dyn::dictionary* extra,
                  const dump_options& opt, write_fn out, void* ctx);
result<void> dump(std::span<const std::byte> capture, const dyn::dictionary* extra,
                  const dump_options& opt, std::string& out);
// 2.1.16 (A6): dump over a file (or "-"), read in windows; two passes over the file (the dictionary is printed
// first and is "as of the end", so the frames are walked once to collect it and once to render them).
result<void> dump(std::string_view path, const dyn::dictionary* extra, const dump_options& opt,
                  write_fn out, void* ctx, const block_options& o = {});
result<void> dump(std::string_view path, const dyn::dictionary* extra, const dump_options& opt,
                  std::string& out, const block_options& o = {});
result<void> dump_dictionary(const dyn::dictionary& d, format fmt, std::string& out);
diff_report diff(const dyn::dictionary& a, const dyn::dictionary& b,
                 diff_direction direction = diff_direction::old_data);

// ---- record decoding (used by dump; exposed for embedding) -------------------------------------------
// 2.0.6: where a struct value sits, for decoded_value_visitor::take_struct. `bytes` are the value's own bytes
// (its fixed part inside a dynamic record). `native` is true when they are the native object representation (a
// flat record: memcpy into the C++ type works); otherwise the value is part of a canonical image and is decoded
// relative to `image` (the whole record image) at `image_offset` (dynamic members reference the image).
struct value_location
{
    std::span<const std::byte> bytes;
    std::span<const std::byte> image;
    std::size_t image_offset{};
    bool native{};
    // 2.1.3 (G2-6): the walk's slot rules and remaining byte budget for `image`; a typed decode of this value
    // charges the same budget. nullptr: a fresh budget of image.size() bytes, fixed part 0.
    fmt::image_budget* budget{};
};

struct decoded_value_visitor
{ // §6.4 style 4: descriptor-driven walk; zero allocations in the walker
    virtual ~decoded_value_visitor() = default;
    virtual void begin_struct(std::string_view type_name, std::string_view field_name) = 0;
    virtual void end_struct() = 0;
    virtual void begin_array(std::string_view field_name, std::uint32_t count) = 0;
    virtual void end_array() = 0;
    virtual void scalar(std::string_view field_name, kind k, std::span<const std::byte> bytes) = 0;
    virtual void enumerator(std::string_view field_name, std::string_view enum_type,
                            std::string_view name, std::int64_t value) = 0;
    virtual void literal(std::string_view field_name, std::uint64_t id,
                         std::string_view text /*empty if unknown*/) = 0;
    // v2 dynamic members (plan v2.0 §4). The defaults express them through the 1.0 events, so a visitor
    // written against 1.x keeps working: a string is an array of chars, a vector an array, an empty optional /
    // valueless variant a literal "null", a variant a struct named variant<i:type> holding "value".
    virtual void string(const std::string_view field_name, const std::string_view s)
    {
        begin_array(field_name, static_cast<std::uint32_t>(s.size()));
        for (const char c : s)
            scalar({}, kind::char_,
                   std::span<const std::byte>(reinterpret_cast<const std::byte*>(&c), 1));
        end_array();
    }
    virtual void begin_vector(const std::string_view field_name, const std::uint32_t count)
    {
        begin_array(field_name, count);
    }
    virtual void end_vector()
    {
        end_array();
    }
    virtual void null_value(const std::string_view field_name) // nullopt / valueless variant
    {
        literal(field_name, 0, "null");
    }
    virtual void begin_variant(const std::string_view field_name, std::uint32_t /*index*/,
                               const std::string_view alt_text /* "variant<1:i32>" */)
    {
        begin_struct(alt_text, field_name);
    }
    // 1.6 (2.2.0): maps and sets. A map is begin_map, then per entry begin_entry / the key as "key" / the value
    // as "value" / end_entry, then end_map; a set is begin_set, its keys unnamed, end_set. The defaults express
    // them through the 1.x events (a vector of "entry" structs; a vector), so older visitors keep working.
    virtual void begin_map(const std::string_view field_name, const std::uint32_t count)
    {
        begin_vector(field_name, count);
    }
    virtual void begin_entry(std::uint32_t /*index*/)
    {
        begin_struct("entry", {});
    }
    virtual void end_entry()
    {
        end_struct();
    }
    virtual void end_map()
    {
        end_vector();
    }
    virtual void begin_set(const std::string_view field_name, const std::uint32_t count)
    {
        begin_vector(field_name, count);
    }
    virtual void end_set()
    {
        end_vector();
    }
    // 1.6: a container argument's transparent type — the record is the container, not a struct around it.
    // The default walks it as a struct named after the container with one unnamed field; renderers override.
    virtual void begin_transparent(const std::string_view type_name,
                                   const std::string_view field_name)
    {
        begin_struct(type_name, field_name);
    }
    virtual void end_transparent()
    {
        end_struct();
    }
    // 2.0.6: called before a struct value (a record, a nested member, a vector/array element, a variant
    // alternative) is walked. Return true to take the value over: its begin_struct / fields / end_struct events
    // are then not emitted. tools::walk_values uses it to decode values of compiled types; the default walks.
    virtual bool take_struct(const std::string_view /*field_name*/, const dyn::type& /*t*/,
                             const value_location& /*where*/)
    {
        return false;
    }
    virtual void end_variant()
    {
        end_struct();
    }
    // 2.1.3 (G2-12): a bool byte other than 0/1 was read; the scalar event that follows carries the repaired
    // byte 1 (true). `raw` is the byte as found. The default ignores it; tools::verify turns it into a warning.
    virtual void noncanonical_bool(const std::string_view /*field_name*/, const std::byte /*raw*/)
    {
    }
};
result<void> visit_record(std::span<const std::byte> payload, const dyn::type& t,
                          const dyn::dictionary& d, decoded_value_visitor& v,
                          std::string_view field_name = {}, bool flatten = false);

// ---- 2.2.1: one value, rendered from a dictionary (DECISIONS A-10: to_string / to_json for any registered
// type) ---------------------------------------------------------------------------------------------------
// `payload` is the record payload as a compact frame carries it (an object's bytes; a dynamic type's
// `u32 len, u32 0, image`; a scalar's bytes; a string's `u32 len, bytes`), `ak`/`k` its category and scalar
// kind, `schema` the type's schema hash (arg_type). Text is the one-line form print uses for a struct
// argument (`Type{field: value, …}`, an enum as its enumerator name, a scalar as a number, a string quoted);
// JSON is the dump's value. A type missing from `d` → unknown_type; a payload that does not decode → the
// decoder's error.
result<std::string> render_value(const dyn::dictionary& d, fmt::arg_kind ak, kind k,
                                 std::uint64_t schema, std::span<const std::byte> payload,
                                 format f);
// The process's own dictionary — registry::instance().export_image() parsed, re-parsed when the registry
// version moved (a dlopen). A shared, lazily built copy: the first call after a change pays the parse (a few
// hundred µs for a thousand entries); the others take a shared lock. The caller holds the parse it got, so a
// re-parse on another thread never pulls it away (the old one is freed with its last holder). Never for the hot
// path.
std::shared_ptr<const dyn::dictionary> process_dictionary();

// ---- walking a capture frame by frame (2.0.6) ------------------------------------------------------------
// One data frame of a capture, as handed to for_each_frame's callback. `bytes` is the whole frame (header
// included): pass it to deserialize / deserialize_println / dispatch / inspect(bytes, &dict) / render_frame.
// 2.2.6: what a frame_ref refers to. Only `data` is delivered unless block_options::meta_frames is set.
enum class frame_kind : std::uint8_t
{
    data,
    opening,    // a FILE's opening dictionary block (not a frame: index 0, bytes = the RRI)
    dictionary, // a dictionary frame (already merged into the dictionary the callback receives)
    checkpoint,
    info
};
struct frame_ref
{
    std::uint64_t index{}; // position among all frames of the capture (dictionary frames count too)
    std::size_t offset{};  // byte offset of the frame header inside the capture
    std::span<const std::byte> bytes; // the frame, header included
    std::uint64_t call_hash{};
    std::uint16_t n_records{};
    bool full_records{};
    const dyn::call*
        call{};        // the call ENTRY in the dictionary; nullptr when the dictionary lacks it
    frame_kind kind{}; // 2.2.6: data unless block_options::meta_frames
    bool is_print() const noexcept
    {
        return call && call->is_print();
    }
};
struct frame_walk
{
    dyn::dictionary dict;   // embedded + dictionary frames + `extra`, as of the end of the walk
    std::uint64_t frames{}; // data frames delivered
    // walk_values / walk_values_in_file (2.1.3, G2-9): values decoded into a C++ type, and values whose schema
    // hash matched one of Ts... but whose typed decode failed (walked generically instead; each one is also a
    // problem). Both 0 for for_each_frame.
    std::uint64_t typed{}, fallbacks{};
    std::vector<std::string>
        problems; // what tools::verify would report ("frame N @off: ..."); empty when clean
    std::size_t
        problems_omitted{}; // 2.1.16: problems beyond decode_limits::problems (counted, not kept)
};
// Walks a capture file (FILE header + embedded dictionary + frames) or a bare frame stream, calling
// f(dictionary, frame) once per data frame in order; the dictionary is the one known at that frame (the embedded
// one, dictionary frames seen so far, and `extra`). Dictionary frames are consumed, not delivered. f returns false
// to stop. Errors: a container that cannot be read at all (bad magic/version, truncated header or dictionary);
// per-frame inconsistencies do not stop the walk and are listed in frame_walk::problems.
using frame_callback = std::function<bool(const dyn::dictionary&, const frame_ref&)>;
result<frame_walk> for_each_frame(std::span<const std::byte> capture, const frame_callback& f,
                                  const dyn::dictionary* extra = nullptr);

result<frame_walk> for_each_frame_in_file(std::string_view path, const frame_callback& f,
                                          const block_options& o = {},
                                          const dyn::dictionary* extra = nullptr);

// 2.0.6: a read-only memory mapping of a whole file (mmap PROT_READ, MAP_PRIVATE, madvise SEQUENTIAL), for
// for_each_frame / walk_values / verify / dump over files of any size without copying them. Move-only.
class mapped_file
{
public:
    // populate: MAP_POPULATE (read everything now) instead of faulting pages in on first touch.
    static result<mapped_file> open(std::string_view path, bool populate = false);
    mapped_file(mapped_file&& o) noexcept;
    mapped_file& operator=(mapped_file&& o) noexcept;
    mapped_file(const mapped_file&) = delete;
    mapped_file& operator=(const mapped_file&) = delete;
    ~mapped_file();
    std::span<const std::byte> bytes() const noexcept
    {
        return {p_, n_};
    }
    std::size_t size() const noexcept
    {
        return n_;
    }

private:
    mapped_file() noexcept = default;
    const std::byte* p_ = nullptr;
    std::size_t n_ = 0;
};

// 2.1.10: a capture on disk in one call. Since 2.1.16 (A6) the default is the windowed reader
// (for_each_frame_in_file: `block_bytes` at a time, 10 MiB unless set, bounded memory for a file of any size);
// `blocks = false` maps the whole file instead (mapped_file: no copying, the page cache decides) — "-" (stdin),
// pipes and devices are always read in windows. Frame offsets are file offsets either way.
//   tools::for_each_frame("capture.ser", router.callback());                 // windows of 10 MiB
//   tools::for_each_frame(path, on_frame, {.block_bytes = std::size_t{64} << 20});
//   tools::for_each_frame(path, on_frame, {.blocks = false});                 // map the whole file
struct file_options
{
    bool blocks = true;
    std::size_t block_bytes = 0;           // 0: decode_limits::window_bytes
    bool populate = false;                 // mapped: MAP_POPULATE
    const decode_limits* limits = nullptr; // nullptr: the process-wide limits
};
result<frame_walk> for_each_frame(std::string_view path, const frame_callback& f,
                                  const file_options& o = {},
                                  const dyn::dictionary* extra = nullptr);

// ---- file helpers ------------------------------------------------------------------------------------
// read_file reads regular files, pipes and "-" (stdin); inputs above 1 GiB are refused with error::unsupported.
result<std::vector<std::byte>> read_file(std::string_view path);
result<void> write_file(std::string_view path, std::span<const std::byte> data);
// The write loop of the tools layer (gk 1.9): retries EINTR, a zero-length write is error::io_error, EAGAIN is
// not retried (blocking descriptors only). Used by write_file, export_image and the env dump; fd_sink has the
// header-only twin detail::write_fd (sink.hpp) with the same rules.
result<void> write_all(int fd, std::span<const std::byte> data) noexcept;

} // namespace es::serreg::tools

namespace es::serreg::cli
{
// The srdump command line as a library (v1.0.3): an application links es::serreg and gets the whole
// `--verify/--json/--elf/--diff/--config/--version` surface under its own name, plus its own flags:
//
//   int main(int argc, char** argv)
//   {
//       int trace = 0;
//       return es::serreg::cli::run(argc, argv, {
//           .name = "myapp", .version = "2.3", .extra_usage = "  --trace     more output\n",
//           .on_extra = [&](std::string_view a, std::span<char* const>&) { return a == "--trace" ? (++trace, true) : false; }});
//   }
//
// Or take the pieces: parse() fills `options` (after any --config file), execute() runs them exactly as srdump
// would. `srdump` itself is `return cli::run(argc, argv);`. Nothing here does what tools:: cannot do.

// Everything srdump understands, after --config and the command line were applied.
struct options
{
    std::string capture, rri, rri_out, diff_a, diff_b, frames, type, wireshark, cxx;
    tools::diff_direction direction{tools::diff_direction::old_data}; // 2.1.16: --diff --direction
    std::size_t window_bytes{}; // 2.1.16: --window (0: decode_limits::window_bytes, 10 MiB)
    std::uint64_t
        frame_limit{}; // 2.1.16: --frame-limit (0: the process-wide decode_limits::frame_bytes)
    std::vector<std::string> dicts, elfs, namespaces; // 2.1.16: --namespace old=new (with --cxx)
    bool use_stdin{}, dict_only{}, verify{}, hex{}, raw{}, flatten{}, full{}, help{}, version{},
        program_hash{}, cxx_router{};
    bool verbose{}; // 2.2.6: --verbose (dictionary blocks, dictionary-frame markers)
    std::string level, since, until; // 2.2.1 (1.7): --level / --since / --until (log frames)
    bool
        info{}; // 2.2.1 (1.7): --info — header, dictionary chain, checkpoints, writer info; no frame walk
    bool
        resync{}; // 2.2.1 (1.7): --resync — after a bad frame, scan forward for the next checkpoint
    tools::format fmt{tools::format::text};
};

// How the hosting application presents itself. Every field defaults to srdump's.
struct app
{
    std::string_view name = "srdump"; // usage/--version banner and the diagnostics prefix
    std::string_view version = tools::library_version;
    std::string_view extra_usage = {}; // appended to --help (the application's own flags)
    // Consulted for every argument cli does not own — an unknown `--flag` or a second operand — before it
    // becomes a usage error. `rest` is the remaining argv after `arg`; consume values by advancing it
    // (rest = rest.subspan(1)). Return true when handled.
    std::function<bool(std::string_view arg, std::span<char* const>& rest)> on_extra;
};

// Loads --config <file> first (explicit flags override it), then the flags. Returns 0, or 2 after printing
// the usage error under `a.name`.
int parse(int argc, char** argv, options& o, const app& a = {});
// Runs the parsed options: --help/--version, --diff, dictionary-only sources (--elf/--rri/--dict, --rri-out,
// --program-hash), --verify, or the dump. Exit codes: 0 ok, 1 verify problems or decode/tool error, 2 usage.
int execute(const options& o, const app& a = {});
// parse() + execute(). Full `srdump` behaviour with the defaults.
int run(int argc, char** argv, const app& a = {});
// The --help text for `a` (usage lines under a.name, the common options, then a.extra_usage).
std::string usage(const app& a = {});
// Parses one --config document (FR-D9 TOML subset) against a scratch option set. Returns false with a
// "<line>: <reason>" message on the first rejected line. Exposed for the config fuzz target.
bool parse_config(std::string_view text, std::string& err);
} // namespace es::serreg::cli
