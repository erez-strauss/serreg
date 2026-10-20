// src/cli.cpp — es::serreg::cli: the `srdump` command line as a library — parse() (flags + the --config TOML
// subset reader, FR-D9), execute() (dispatch to es::serreg::tools; spec §6.9, FR-D0/D1/D2/D3/D6/D7) and
// run() = both, under the hosting application's name (cli::app). Exit codes: 0 ok, 1 verify problems or
// decode/tool error, 2 usage error. Nothing here does what tools:: cannot do.
#include "internal.hpp"

#include <es/serreg/registry.hpp>
#include <es/serreg/tools.hpp>

#include <sys/stat.h>

#include <chrono>
#include <cstdio>
#include <map>
#include <print>
#include <sstream>

namespace es::serreg::cli
{
namespace
{

using internal::dec;
using internal::hex64;

constexpr std::string_view usage_options =
    "options:\n"
    "  --dict-only          print only the dictionary\n"
    "  --verbose            with the dictionary: its blocks (opening block, every dictionary "
    "frame: offset,\n"
    "                       what it added); with the frames: a marker line per dictionary frame\n"
    "  --text | --json | --toml   output format (default text; records are JSON under --toml)\n"
    "  --verify             check frames/records against the dictionary; exit 1 on problems\n"
    "  --info               1.7: the file's header, dictionary chain, checkpoints and writer "
    "info,\n"
    "                       by seeks (no frame walk); --json for machine reading\n"
    "  --resync             1.7: after an unparsable frame, continue at the next checkpoint frame\n"
    "  --level <lvl>        1.7: only log frames at or above trace|debug|info|warn|error|fatal, "
    "one\n"
    "                       rendered line each (--json: {\"log\": {...}} objects)\n"
    "  --since <t> / --until <t>   1.7: log frames whose timestamp is in range (implies --level "
    "trace); <t> is\n"
    "                       ns since the epoch, or YYYY-MM-DD[THH:MM[:SS[.fff]]] (UTC)\n"
    "  --dict <file.rri>    extra dictionary (repeatable)\n"
    "  --elf <binary>       extra dictionary from the binary's es_serreg_dict section "
    "(repeatable)\n"
    "  --rri-out <file>     with --elf and no capture: write the merged dictionary as an RRI\n"
    "  --wireshark <f.lua>  write a Wireshark dissector for the capture's / --dict / --elf / "
    "--rri\n"
    "                       dictionary, or this program's own registry when none is given\n"
    "                       (docs/wireshark.md); \"-\" writes it to standard output\n"
    "  --cxx <types.hpp>    write C++ definitions of the same dictionary's types, self-checking\n"
    "                       (compiling the file verifies every size, offset and hash); \"-\" "
    "writes\n"
    "                       it to standard output\n"
    "  --cxx-router         with --cxx: add install_handlers(frame_router&), one handler per call\n"
    "  --namespace old=new  with --cxx: declare the types of namespace old in new (repeatable; "
    "the\n"
    "                       canonical name is kept through [[=es::serreg::name(\"...\")]])\n"
    "  --direction <d>      with --diff: which reader the verdict is for — old-data (default: a "
    "reader\n"
    "                       built from the new side reads old data; removals break), new-data (an "
    "old\n"
    "                       reader reads new data; additions break), both, none\n"
    "  --window <size>      read the capture this many bytes at a time (default 10M; K/M/G "
    "suffixes);\n"
    "                       a capture's size is bounded by the file system, not by this tool\n"
    "  --frame-limit <size> largest frame accepted (default 64M); a larger frame_len is a problem\n"
    "  --frames A..B        only frames A..B (inclusive, 0-based)\n"
    "  --type <name|0xhash> only records of this type\n"
    "  --hex                also print raw payload bytes\n"
    "  --raw                show print frames as records, not the rendered message\n"
    "  --flatten            present base-class members inline\n"
    "  --full               do not elide long arrays\n"
    "  --stdin              read the capture from standard input\n"
    "  --config <file.toml> key = value settings (keys = long flag names without dashes)\n"
    "  --program-hash       print the 0x-hex program hash of the capture / --rri / --elf "
    "dictionary and exit\n"
    "  --version            print \"<name> <version> (format <major>.<minor>, hash_algo <n>)\"\n"
    "  --help\n";

// 2.1.16 (A6): a regular file can be read twice (dump's two passes); pipes and devices cannot.
bool is_regular_file(const std::string& path)
{
    struct stat st = {};
    return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

// 2.1.16: a byte size with an optional K/M/G suffix (powers of two), e.g. "10M", "64K", "4096".
bool parse_size(const std::string_view text, std::uint64_t& out)
{
    if (text.empty())
        return false;
    std::uint64_t v = 0;
    std::size_t i = 0;
    for (; i < text.size() && text[i] >= '0' && text[i] <= '9'; ++i)
    {
        if (v > (~std::uint64_t{0} - 9) / 10)
            return false;
        v = v * 10 + static_cast<std::uint64_t>(text[i] - '0');
    }
    if (i == 0)
        return false;
    std::uint64_t mult = 1;
    if (i < text.size())
    {
        switch (text[i])
        {
        case 'k':
        case 'K':
            mult = std::uint64_t{1} << 10;
            break;
        case 'm':
        case 'M':
            mult = std::uint64_t{1} << 20;
            break;
        case 'g':
        case 'G':
            mult = std::uint64_t{1} << 30;
            break;
        default:
            return false;
        }
        if (i + 1 != text.size())
            return false;
    }
    if (v > ~std::uint64_t{0} / mult)
        return false;
    out = v * mult;
    return true;
}

// 2.2.1 (1.7): a --since/--until time: an integer (the writer's clock units, ns since the epoch for the system
// clock) or an ISO-8601 UTC date-time, YYYY-MM-DD[THH:MM[:SS[.fraction]]].
bool parse_time(const std::string_view text, std::uint64_t& out)
{
    if (text.empty())
        return false;
    if (text.find_first_not_of("0123456789") == std::string_view::npos)
        return internal::parse_u64(text, out);
    std::istringstream in{std::string(text)};
    std::chrono::sys_time<std::chrono::nanoseconds> tp;
    const char* const formats[] = {"%Y-%m-%dT%H:%M:%S", "%Y-%m-%dT%H:%M", "%Y-%m-%d"};
    for (const char* const f : formats)
    {
        in.clear();
        in.seekg(0);
        if (std::chrono::from_stream(in, f, tp) && in.peek() == std::char_traits<char>::eof())
        {
            out = static_cast<std::uint64_t>(tp.time_since_epoch().count());
            return true;
        }
    }
    return false;
}

// 2.1.16 (decision E): the --diff direction, from the flag or the config key.
bool set_direction(options& o, const std::string_view d)
{
    if (d == "old-data")
        o.direction = tools::diff_direction::old_data;
    else if (d == "new-data")
        o.direction = tools::diff_direction::new_data;
    else if (d == "both")
        o.direction = tools::diff_direction::both;
    else if (d == "none")
        o.direction = tools::diff_direction::none;
    else
        return false;
    return true;
}

void fail(const app& a, const std::string_view what, const error e)
{
    std::println(stderr, "{}: {}: {}", a.name, what, to_string(e));
}
int usage_error(const app& a, const std::string& msg)
{
    std::println(stderr, "{}: {}", a.name, msg);
    return 2;
}

std::string_view trim(std::string_view s)
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r'))
        s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
        s.remove_suffix(1);
    return s;
}

std::string normalize_key(const std::string_view k)
{
    std::string out;
    for (const char c : k)
        if (c != '-' && c != '_')
            out += c;
    return out;
}

// One config value: TOML basic string, integer, boolean, or array of strings (FR-D9 subset).
struct value
{
    std::string s;
    long long i{};
    bool b{};
    std::vector<std::string> arr;
    enum
    {
        str,
        integer,
        boolean,
        strarray
    } t{str};
};

bool parse_string(std::string_view& s, std::string& out)
{
    if (s.empty() || s.front() != '"')
        return false;
    s.remove_prefix(1);
    while (!s.empty() && s.front() != '"')
    {
        char c = s.front();
        s.remove_prefix(1);
        if (c == '\\')
        {
            if (s.empty())
                return false;
            const char e = s.front();
            s.remove_prefix(1);
            switch (e)
            {
            case 'n':
                c = '\n';
                break;
            case 't':
                c = '\t';
                break;
            case '"':
                c = '"';
                break;
            case '\\':
                c = '\\';
                break;
            default:
                return false;
            }
        }
        out += c;
    }
    if (s.empty())
        return false;
    s.remove_prefix(1);
    return true;
}

bool parse_value(std::string_view s, value& v)
{
    s = trim(s);
    if (s.empty())
        return false;
    if (s.front() == '"')
    {
        v.t = value::str;
        if (!parse_string(s, v.s))
            return false;
        return trim(s).empty() || trim(s).front() == '#';
    }
    if (s.front() == '[')
    {
        v.t = value::strarray;
        s.remove_prefix(1);
        for (;;)
        {
            s = trim(s);
            if (s.empty())
                return false;
            if (s.front() == ']')
            {
                s.remove_prefix(1);
                return trim(s).empty() || trim(s).front() == '#';
            }
            std::string item;
            if (!parse_string(s, item))
                return false;
            v.arr.push_back(std::move(item));
            s = trim(s);
            if (!s.empty() && s.front() == ',')
                s.remove_prefix(1);
            else if (s.empty() || s.front() != ']')
                return false; // 2.1.12 (review D-09): items are separated by commas
        }
    }
    const std::size_t end = std::min(s.find(' '), std::min(s.find('\t'), s.find('#')));
    const std::string_view tok = s.substr(0, end);
    if (!trim(s.substr(tok.size())).empty() && trim(s.substr(tok.size())).front() != '#')
        return false;
    if (tok == "true" || tok == "false")
    {
        v.t = value::boolean;
        v.b = tok == "true";
        return true;
    }
    v.t = value::integer;
    const auto r = std::from_chars(tok.data(), tok.data() + tok.size(), v.i);
    return r.ec == std::errc{} && r.ptr == tok.data() + tok.size();
}

// Applies one `key = value`; returns an error message (empty = ok).
std::string apply(options& o, const std::string& key, const value& v)
{
    const auto want_bool = [&](bool& dst) -> std::string {
        if (v.t != value::boolean)
            return "expected a boolean for " + key;
        dst = v.b;
        return {};
    };
    const auto want_str = [&](std::string& dst) -> std::string {
        if (v.t != value::str)
            return "expected a string for " + key;
        dst = v.s;
        return {};
    };
    const auto want_list = [&](std::vector<std::string>& dst) -> std::string {
        if (v.t == value::str)
        {
            dst.push_back(v.s);
            return {};
        }
        if (v.t == value::strarray)
        {
            dst.insert(dst.end(), v.arr.begin(), v.arr.end());
            return {};
        }
        return "expected a string or an array of strings for " + key;
    };
    if (key == "capture")
        return want_str(o.capture);
    if (key == "stdin")
        return want_bool(o.use_stdin);
    if (key == "dictonly")
        return want_bool(o.dict_only);
    if (key == "verbose") // 2.2.6
        return want_bool(o.verbose);
    if (key == "verify")
        return want_bool(o.verify);
    if (key == "info")
        return want_bool(o.info);
    if (key == "level")
        return want_str(o.level);
    if (key == "since")
        return want_str(o.since);
    if (key == "until")
        return want_str(o.until);
    if (key == "resync")
        return want_bool(o.resync);
    if (key == "hex")
        return want_bool(o.hex);
    if (key == "raw")
        return want_bool(o.raw);
    if (key == "flatten")
        return want_bool(o.flatten);
    if (key == "full")
        return want_bool(o.full);
    if (key == "json" || key == "toml" || key == "text")
    {
        bool b{};
        if (const auto e = want_bool(b); !e.empty())
            return e;
        if (b)
            o.fmt = key == "json"   ? tools::format::json
                    : key == "toml" ? tools::format::toml
                                    : tools::format::text;
        return {};
    }
    if (key == "format")
    {
        if (v.t != value::str)
            return "expected a string for format";
        if (v.s == "json")
            o.fmt = tools::format::json;
        else if (v.s == "toml")
            o.fmt = tools::format::toml;
        else if (v.s == "text")
            o.fmt = tools::format::text;
        else
            return "unknown format \"" + v.s + "\"";
        return {};
    }
    if (key == "dict")
        return want_list(o.dicts);
    if (key == "elf")
        return want_list(o.elfs);
    if (key == "rriout")
        return want_str(o.rri_out);
    if (key == "cxx") // 2.1.16 (A5)
        return want_str(o.cxx);
    if (key == "window") // 2.1.16 (A6): the window of the capture readers
    {
        std::uint64_t n = 0;
        if (v.t == value::integer)
            n = static_cast<std::uint64_t>(v.i);
        else if (v.t != value::str || !parse_size(v.s, n))
            return "window must be a size (bytes, or with a K/M/G suffix)";
        if (n < 4096)
            return "window must be at least 4096 bytes";
        o.window_bytes = static_cast<std::size_t>(n);
        return {};
    }
    if (key == "framelimit") // 2.1.16 (FR-D5): decode_limits::frame_bytes for this run
    {
        std::uint64_t n = 0;
        if (v.t == value::integer)
            n = static_cast<std::uint64_t>(v.i);
        else if (v.t != value::str || !parse_size(v.s, n))
            return "frame-limit must be a size (bytes, or with a K/M/G suffix)";
        if (n < fmt::frame_header_size)
            return "frame-limit must be at least 16 bytes";
        o.frame_limit = n;
        return {};
    }
    if (key == "direction") // 2.1.16 (decision E)
    {
        if (v.t != value::str)
            return "expected a string for " + key;
        if (!set_direction(o, v.s))
            return "direction must be old-data, new-data, both or none";
        return {};
    }
    if (key == "cxxrouter")
        return want_bool(o.cxx_router);
    if (key == "namespace")
        return want_list(o.namespaces);
    if (key == "rri")
        return want_str(o.rri);
    if (key == "frames")
        return want_str(o.frames);
    if (key == "type")
        return want_str(o.type);
    if (key == "diff")
    {
        if (v.t != value::strarray || v.arr.size() != 2)
            return "diff expects an array of two paths";
        o.diff_a = v.arr[0];
        o.diff_b = v.arr[1];
        return {};
    }
    return "unknown key \"" + key + "\"";
}

// Minimal TOML subset (FR-D9): comments, [section] headers (ignored), key = value. Anything else → error + line.
std::string parse_config_text(options& o, const std::string_view text)
{
    std::size_t pos = 0, line_no = 0;
    while (pos < text.size())
    {
        const std::size_t nl = text.find('\n', pos);
        const std::string_view line = trim(
            text.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos));
        pos = nl == std::string_view::npos ? text.size() : nl + 1;
        ++line_no;
        if (line.empty() || line.front() == '#')
            continue;
        if (line.front() == '[')
        { // 2.1.12 (review D-09): a comment may follow the header
            const std::size_t close = line.find(']');
            const std::string_view rest =
                close == std::string_view::npos ? std::string_view{} : trim(line.substr(close + 1));
            if (close == std::string_view::npos || (!rest.empty() && rest.front() != '#'))
                return dec(line_no) + ": malformed table header";
            continue;
        }
        const std::size_t eq = line.find('=');
        if (eq == std::string_view::npos)
            return dec(line_no) + ": expected key = value";
        const std::string_view key = trim(line.substr(0, eq));
        if (key.empty() || key.find('.') != std::string_view::npos || key.front() == '"')
            return dec(line_no) +
                   ": unsupported key syntax (dotted/quoted keys are not in the v1 subset)";
        value v;
        if (!parse_value(line.substr(eq + 1), v))
            return dec(line_no) +
                   ": unsupported value (strings, integers, booleans, arrays of strings only)";
        if (const auto e = apply(o, normalize_key(key), v); !e.empty())
            return dec(line_no) + ": " + e;
    }
    return {};
}

int load_config(const app& ap, options& o, const std::string& path)
{
    const auto bytes = tools::read_file(path);
    if (!bytes)
    {
        fail(ap, "--config " + path, bytes.error());
        return 2;
    }
    const std::string_view text(reinterpret_cast<const char*>(bytes->data()), bytes->size());
    if (const auto e = parse_config_text(o, text); !e.empty())
        return usage_error(ap, path + ":" + e);
    return 0;
}

int parse_args(const app& ap, options& o, const int argc, char** const argv)
{
    for (int i = 1; i < argc; ++i)
    {
        const std::string_view a = argv[i];
        // Anything cli does not own goes to the application first (cli::app::on_extra, v1.0.3).
        const auto extra = [&] {
            if (!ap.on_extra)
                return false;
            std::span<char* const> rest(argv + i + 1, static_cast<std::size_t>(argc - i - 1));
            if (!ap.on_extra(a, rest))
                return false;
            i = argc - static_cast<int>(rest.size()) -
                1; // the loop's ++i lands on the first unconsumed
            return true;
        };
        // gk §2: an operand that looks like an option ("--config --verify") is a usage error, not a file name.
        const auto next = [&](std::string& dst) {
            if (i + 1 >= argc || std::string_view(argv[i + 1]).starts_with("--"))
                return false;
            dst = argv[++i];
            return true;
        };
        std::string tmp;
        if (a == "--help" || a == "-h")
            o.help = true;
        else if (a == "--version")
            o.version = true;
        else if (a == "--program-hash")
            o.program_hash = true;
        else if (a == "--dict-only")
            o.dict_only = true;
        else if (a == "--verbose") // 2.2.6
            o.verbose = true;
        else if (a == "--json")
            o.fmt = tools::format::json;
        else if (a == "--toml")
            o.fmt = tools::format::toml;
        else if (a == "--text")
            o.fmt = tools::format::text;
        else if (a == "--verify")
            o.verify = true;
        else if (a == "--info") // 2.2.1 (1.7)
            o.info = true;
        else if (a == "--resync") // 2.2.1 (1.7)
            o.resync = true;
        else if (a == "--level") // 2.2.1 (1.7)
        {
            if (!next(o.level))
                return usage_error(ap, "--level needs a level name");
        }
        else if (a == "--since")
        {
            if (!next(o.since))
                return usage_error(ap, "--since needs a time");
        }
        else if (a == "--until")
        {
            if (!next(o.until))
                return usage_error(ap, "--until needs a time");
        }
        else if (a == "--hex")
            o.hex = true;
        else if (a == "--raw")
            o.raw = true;
        else if (a == "--flatten")
            o.flatten = true;
        else if (a == "--full")
            o.full = true;
        else if (a == "--stdin")
            o.use_stdin = true;
        else if (a == "--dict")
        {
            if (!next(tmp))
                return usage_error(ap, "--dict needs a file");
            o.dicts.push_back(tmp);
        }
        else if (a == "--elf")
        {
            if (!next(tmp))
                return usage_error(ap, "--elf needs a binary");
            o.elfs.push_back(tmp);
        }
        else if (a == "--rri-out")
        {
            if (!next(o.rri_out))
                return usage_error(ap, "--rri-out needs a file");
        }
        else if (a == "--wireshark")
        {
            if (!next(o.wireshark))
                return usage_error(ap, "--wireshark needs an output file (or -)");
        }
        else if (a == "--cxx") // 2.1.16 (A5)
        {
            if (!next(o.cxx))
                return usage_error(ap, "--cxx needs an output file (or -)");
        }
        else if (a == "--cxx-router")
            o.cxx_router = true;
        else if (a == "--namespace")
        {
            if (!next(tmp) || tmp.find('=') == std::string::npos || tmp.front() == '=')
                return usage_error(ap, "--namespace needs old=new");
            o.namespaces.push_back(tmp);
        }
        else if (a == "--rri")
        {
            if (!next(o.rri))
                return usage_error(ap, "--rri needs an image");
        }
        else if (a == "--frames")
        {
            if (!next(o.frames))
                return usage_error(ap, "--frames needs A..B");
        }
        else if (a == "--type")
        {
            if (!next(o.type))
                return usage_error(ap, "--type needs a name or hash");
        }
        else if (a == "--config")
        {
            if (!next(tmp))
                return usage_error(ap, "--config needs a file");
        } // loaded in a first pass
        else if (a == "--diff")
        {
            if (!next(o.diff_a) || !next(o.diff_b))
                return usage_error(ap, "--diff needs two paths");
        }
        else if (a == "--window") // 2.1.16 (A6)
        {
            std::string sz;
            std::uint64_t n = 0;
            if (!next(sz) || !parse_size(sz, n) || n < 4096)
                return usage_error(ap, "--window needs a size of at least 4096 bytes (e.g. 10M)");
            o.window_bytes = static_cast<std::size_t>(n);
        }
        else if (a == "--frame-limit") // 2.1.16 (FR-D5)
        {
            std::string sz;
            std::uint64_t n = 0;
            if (!next(sz) || !parse_size(sz, n) || n < fmt::frame_header_size)
                return usage_error(ap,
                                   "--frame-limit needs a size of at least 16 bytes (e.g. 64M)");
            o.frame_limit = n;
        }
        else if (a == "--direction") // 2.1.16 (decision E)
        {
            std::string d;
            if (!next(d) || !set_direction(o, d))
                return usage_error(ap, "--direction needs old-data, new-data, both or none");
        }
        else if (a.size() > 1 && a[0] == '-')
        {
            if (!extra())
                return usage_error(ap, "unknown option " + std::string(a) + "\n" + usage(ap));
        }
        else if (o.capture.empty())
            o.capture = std::string(a);
        else if (!extra())
            return usage_error(ap, "unexpected argument " + std::string(a));
    }
    return 0;
}

bool parse_frames(const std::string_view s, tools::dump_options& d)
{
    const std::size_t dots = s.find("..");
    const std::string_view a = s.substr(0, dots), b = dots == std::string_view::npos
                                                          ? std::string_view{}
                                                          : s.substr(dots + 2);
    if (!a.empty() && !internal::parse_u64(a, d.first_frame))
        return false;
    if (dots == std::string_view::npos)
        d.last_frame = d.first_frame;
    else if (!b.empty() && !internal::parse_u64(b, d.last_frame))
        return false;
    return d.first_frame <= d.last_frame;
}

// A dictionary source for --diff: RRI file or ELF binary, detected by magic.
result<dyn::dictionary> load_any(const std::string& path)
{
    const auto bytes = tools::read_file(path);
    if (!bytes)
        return std::unexpected(bytes.error());
    if (bytes->size() >= 4 && std::memcmp(bytes->data(), fmt::rri_magic.data(), 4) == 0)
        return tools::load_dictionary_rri(*bytes);
    if (bytes->size() >= 4 && std::memcmp(bytes->data(),
                                          "\x7f"
                                          "ELF",
                                          4) == 0)
        return tools::extract_registry_from_elf(path);
    return std::unexpected(error::bad_magic);
}

void emit(const std::string& out);

// The dictionary a generator describes (--wireshark, --cxx): the capture's own (the whole stream — a later
// dictionary frame adds types too, 2.1.12 review T-08), then --dict / --elf / --rri merged over it, or, when no
// source was given, the registry of the hosting program (2.1.10). Two sources that disagree about one schema hash
// are refused, as for the dictionary-only dump; so is a dictionary without entries. Returns the exit code (0: `d`
// and `sources` are filled).
int collect_dictionary(const options& o, const app& a, const dyn::dictionary& extra,
                       const bool have_extra, const std::string_view flag, dyn::dictionary& d,
                       std::vector<std::string>& sources)
{
    if (!o.capture.empty() || o.use_stdin)
    {
        const std::string source = o.use_stdin ? "-" : o.capture;
        // 2.2.1 (1.7): a FILE's dictionary is the opening block plus the chain of delta frames, reached by seeks
        // (tools::info) — O(deltas) whatever the capture's size. A 1.6 file, a stream or a pipe is walked:
        // 2.1.16 (A6): the windowed walker reads the FILE header and the dictionary, then every frame through
        // windows of --window bytes, so the dictionary frames a writer appended after its data frames (T-08: a
        // plugin registered late, a bare stream whose dictionary arrives last) are merged too, with memory
        // bounded whatever the capture's size. That pass is O(size) in time.
        bool have = false;
        if (!o.use_stdin && is_regular_file(o.capture))
        {
            if (const auto ci = tools::info(o.capture); ci && ci->minor >= 7)
            {
                d = ci->dict;
                have = true;
            }
        }
        if (!have)
        {
            const auto walk = tools::for_each_frame_in_file(
                source, [](const dyn::dictionary&, const tools::frame_ref&) { return true; },
                tools::block_options{o.window_bytes, nullptr});
            if (!walk)
            {
                fail(a, source, walk.error());
                return 1;
            }
            d = walk->dict;
        }
        sources.push_back("capture " + source);
    }
    for (const auto& p : o.dicts)
        sources.push_back("--dict " + p);
    for (const auto& p : o.elfs)
        sources.push_back("--elf " + p);
    if (!o.rri.empty())
        sources.push_back("--rri " + o.rri);
    if (have_extra)
        d.merge(extra);
    if (sources.empty())
    { // an application embedding the CLI describes its own registry
        if (const auto own = tools::load_dictionary_rri(registry::instance().export_image());
            own && (!own->types.empty() || !own->calls.empty()))
        {
            d = *own;
            sources.push_back("the registry of " + std::string(a.name));
        }
        else
            return usage_error(a, std::string(flag) + " needs a capture, --dict, --elf or --rri");
    }
    if (!d.conflicts.empty())
    {
        for (const auto& c : d.conflicts)
            std::println(stderr, "{}: schema conflict: {} {}", a.name, c.kept.name,
                         hex64(c.kept.schema_hash));
        return 1;
    }
    if (d.types.empty() && d.calls.empty() && d.literals.empty())
    { // a generator that knows nothing is not a result
        std::println(stderr, "{}: {}: the sources hold no dictionary entries", a.name, flag);
        return 1;
    }
    return 0;
}

// The generated text to `path`, or to standard output for "-"; a file gets a one-line summary on stderr.
int write_generated(const app& a, const std::string_view flag, const std::string& path,
                    const std::string& text, const dyn::dictionary& d)
{
    if (path == "-")
    {
        emit(text);
        return 0;
    }
    if (const auto r = tools::write_file(path, std::as_bytes(std::span(text))); !r)
    {
        fail(a, std::string(flag) + " " + path, r.error());
        return 1;
    }
    std::println(stderr, "{}: wrote {}: {} types, {} call signatures, {} literals", a.name, path,
                 d.types.size(), d.calls.size(), d.literals.size());
    return 0;
}

// --wireshark: the dissector for the merged dictionary.
int write_wireshark(const options& o, const app& a, const dyn::dictionary& extra,
                    const bool have_extra)
{
    tools::wireshark_options wo;
    wo.generator = std::string(a.name);
    dyn::dictionary d;
    if (const int rc = collect_dictionary(o, a, extra, have_extra, "--wireshark", d, wo.sources))
        return rc;
    return write_generated(a, "--wireshark", o.wireshark, tools::wireshark_dissector(d, wo), d);
}

// --cxx (2.1.16, A5): C++ definitions of the merged dictionary's types. Two types of one name (two programs'
// dictionaries merged) cannot both be declared: the user picks one source.
int write_cxx(const options& o, const app& a, const dyn::dictionary& extra, const bool have_extra)
{
    dyn::dictionary d;
    std::vector<std::string> sources;
    if (const int rc = collect_dictionary(o, a, extra, have_extra, "--cxx", d, sources))
        return rc;
    std::map<std::string_view, std::uint64_t> names;
    for (const auto& [h, t] : d.types)
        if (!names.emplace(t.name, h).second)
        {
            std::println(
                stderr,
                "{}: --cxx: duplicate type name {}; pick one source (--program-hash) or --dict",
                a.name, t.name);
            return 1;
        }
    tools::cxx_options co;
    co.router = o.cxx_router;
    for (const std::string& m : o.namespaces)
    {
        const std::size_t eq = m.find('=');
        if (eq == std::string::npos || eq == 0)
            return usage_error(a, "--namespace needs old=new, got \"" + m + "\"");
        co.namespaces.emplace_back(m.substr(0, eq), m.substr(eq + 1));
    }
    return write_generated(a, "--cxx", o.cxx, tools::cxx_header(d, co), d);
}

void emit(const std::string& out)
{
    if (!out.empty())
        std::fwrite(out.data(), 1, out.size(), stdout);
    std::fflush(stdout);
}

} // namespace

std::string usage(const app& a)
{
    const std::string n(a.name);
    return "usage: " + n + " [options] <capture.ser>\n" + "       " + n +
           " --elf <binary> [--elf ...] [--rri-out <file>]\n" + "       " + n +
           " --rri <image.rri>\n" + "       " + n +
           " --diff <a> <b>            (each an .rri file or an ELF binary)\n" +
           std::string(usage_options) + std::string(a.extra_usage);
}

int parse(const int argc, char** const argv, options& o, const app& a)
{
    for (int i = 1; i + 1 < argc; ++i) // --config first, so explicit flags override it
        if (std::string_view(argv[i]) == "--config" &&
            !std::string_view(argv[i + 1]).starts_with("--"))
        {
            if (const int rc = load_config(a, o, argv[i + 1]))
                return rc;
        }
    return parse_args(a, o, argc, argv);
}

int run(const int argc, char** const argv, const app& a)
{
    options o;
    if (const int rc = parse(argc, argv, o, a))
        return rc;
    return execute(o, a);
}

namespace
{
int execute_command(const options& o, const app& a);
}

int execute(const options& o, const app& a)
{
    if (o.frame_limit != 0)
    { // 2.1.16 (FR-D5): --frame-limit narrows or widens the process-wide limit for this run
        decode_limits l = get_decode_limits();
        l.frame_bytes = o.frame_limit;
        set_decode_limits(l);
    }
    const int rc = execute_command(o, a);
    // 2.1.12 (review T-07): output that did not reach standard output (a full disk, a closed pipe) is a failure,
    // not exit 0. The error is reported once and cleared, for applications that call execute() again.
    if (std::fflush(stdout) != 0 || std::ferror(stdout))
    {
        std::clearerr(stdout);
        std::println(stderr, "{}: writing standard output failed", a.name);
        return rc == 0 ? 1 : rc;
    }
    return rc;
}

namespace
{
int execute_command(const options& o, const app& a)
{
    if (o.help)
    {
        emit(usage(a));
        return 0;
    }
    if (o.version)
    {
        emit(std::string(a.name) + " " + std::string(a.version) + " (format " +
             dec(fmt::format_major) + "." + dec(fmt::format_minor) + ", hash_algo " +
             dec(fmt::hash_algo_xxh3_64) + ")\n");
        return 0;
    }
    std::string out;

    if (!o.diff_a.empty())
    {
        const auto da = load_any(o.diff_a), db = load_any(o.diff_b);
        if (!da)
        {
            fail(a, o.diff_a, da.error());
            return 1;
        }
        if (!db)
        {
            fail(a, o.diff_b, db.error());
            return 1;
        }
        const tools::diff_report rep = tools::diff(*da, *db, o.direction);
        emit(rep.to_text());
        return rep.breaking ? 1 : 0;
    }

    dyn::dictionary extra;
    bool have_extra = false;
    for (const auto& p : o.dicts)
    {
        const auto d = tools::load_dictionary_file(p);
        if (!d)
        {
            fail(a, "--dict " + p, d.error());
            return 1;
        }
        extra.merge(*d);
        have_extra = true;
    }
    for (const auto& p : o.elfs)
    {
        const auto d = tools::extract_registry_from_elf(p);
        if (!d)
        {
            fail(a, "--elf " + p, d.error());
            return 1;
        }
        extra.merge(*d);
        have_extra = true;
    }
    if (!o.rri.empty())
    {
        const auto d = tools::load_dictionary_file(o.rri);
        if (!d)
        {
            fail(a, "--rri " + o.rri, d.error());
            return 1;
        }
        extra.merge(*d);
        have_extra = true;
    }
    if (!o.rri_out.empty())
    {
        const auto img = dyn::to_rri(extra);
        if (const auto r = tools::write_file(o.rri_out, img); !r)
        {
            fail(a, "--rri-out " + o.rri_out, r.error());
            return 1;
        }
    }
    if (!o.wireshark.empty())
        return write_wireshark(o, a, extra, have_extra);
    if (!o.cxx.empty())
        return write_cxx(o, a, extra, have_extra);
    if (o.capture.empty() && !o.use_stdin)
    {
        if (!have_extra)
            return usage_error(a, "no capture given\n" + usage(a));
        if (o.program_hash)
        {
            emit(hex64(extra.program_hash ? extra.program_hash : extra.computed_program_hash()) +
                 "\n");
            return 0;
        }
        if (const auto r = tools::dump_dictionary(extra, o.fmt, out); !r)
        {
            fail(a, "dictionary", r.error());
            return 1;
        }
        emit(out);
        if (!extra.conflicts.empty())
        { // cc F15/F16: --elf a --elf b disagreeing about a type is an error
            for (const auto& c : extra.conflicts)
                std::println(stderr, "{}: schema conflict: {} {}", a.name, c.kept.name,
                             hex64(c.kept.schema_hash));
            return 1;
        }
        return 0;
    }

    // 2.1.16 (A6): a capture is read in windows (--window, 10 MiB by default), so its size is bounded by the
    // file system, not by this tool; nothing below reads a capture whole. The one exception is a dump of
    // standard input or a pipe: the dictionary is printed first and is "as of the end", which needs a second
    // pass that a pipe cannot give, so that input is slurped (within decode_limits::input_bytes).
    const std::string source = o.use_stdin ? "-" : o.capture;
    const dyn::dictionary* const extra_p = have_extra ? &extra : nullptr;
    tools::block_options window;
    window.block_bytes = o.window_bytes;
    window.resync = o.resync;

    if (o.info)
    { // 2.2.1 (1.7): what the file says about itself — seeks only, whatever its size
        if (o.use_stdin)
            return usage_error(a, "--info needs a file (a pipe cannot be seeked)");
        const auto ci = tools::info(o.capture);
        if (!ci)
        {
            fail(a, o.capture, ci.error());
            return 1;
        }
        emit(o.fmt == tools::format::json ? tools::to_json(*ci) : tools::to_text(*ci));
        return 0;
    }

    if (o.program_hash)
    { // the capture's own header hash, or the merged dictionary's when the capture has none
        const auto rep =
            tools::verify(source, extra_p, window); // the container must at least parse
        if (!rep)
        {
            fail(a, o.use_stdin ? "stdin" : o.capture, rep.error());
            return 1;
        }
        std::uint64_t hash = rep->program_hash;
        if (hash == 0)
            hash = extra.program_hash ? extra.program_hash : extra.computed_program_hash();
        emit(hex64(hash) + "\n");
        return 0;
    }

    if (o.verify)
    {
        const auto rep = tools::verify(source, extra_p, window);
        if (!rep)
        {
            fail(a, o.use_stdin ? "stdin" : o.capture, rep.error());
            return 1;
        }
        if (o.fmt == tools::format::json)
        {
            out += std::string("{\"ok\":") + (rep->ok() ? "true" : "false") +
                   ",\"frames\":" + dec(rep->frames) + ",\"records\":" + dec(rep->records) +
                   ",\"bytes\":" + dec(rep->bytes) + ",\"problems\":[";
            for (std::size_t i = 0; i < rep->problems.size(); ++i)
            {
                if (i)
                    out += ',';
                internal::append_quoted(out, rep->problems[i]);
            }
            out += "]";
            if (rep->problems_omitted) // 2.1.16: beyond decode_limits::problems
                out += ",\"problems_omitted\":" + dec(rep->problems_omitted);
            if (!rep->warnings.empty()) // 2.1.3: present only when there are warnings (G2-12)
            {
                out += ",\"warnings\":[";
                for (std::size_t i = 0; i < rep->warnings.size(); ++i)
                {
                    if (i)
                        out += ',';
                    internal::append_quoted(out, rep->warnings[i]);
                }
                out += "]";
            }
            out += "}\n";
        }
        else
        {
            for (const auto& p : rep->problems)
                out += "problem: " + p + "\n";
            if (rep->problems_omitted)
                out += "problem: ... and " + dec(rep->problems_omitted) + " more\n";
            for (const auto& w : rep->warnings)
                out += "warning: " + w + "\n";
            out += (rep->ok() ? "ok: "
                              : "FAILED (" + dec(rep->problems.size() + rep->problems_omitted) +
                                    " problems): ") +
                   dec(rep->frames) + " frames, " + dec(rep->records) + " records, " +
                   dec(rep->bytes) + " bytes\n";
        }
        emit(out);
        return rep->ok() ? 0 : 1;
    }

    tools::dump_options d;
    d.fmt = o.fmt;
    d.dict_only = o.dict_only;
    d.verbose = o.verbose; // 2.2.6
    d.hex = o.hex;
    d.raw = o.raw;
    d.flatten = o.flatten;
    d.full_arrays = o.full;
    if (!o.frames.empty() && !parse_frames(o.frames, d))
        return usage_error(a, "bad --frames range \"" + o.frames + "\" (expected A..B)");
    if (!o.level.empty())
    { // 2.2.1 (1.7): log frames only, at or above the level
        static constexpr std::string_view names[] = {"trace", "debug", "info",
                                                     "warn",  "error", "fatal"};
        bool found = false;
        for (std::uint8_t i = 0; i < 6; ++i)
            if (o.level == names[i])
            {
                d.min_level = i;
                found = true;
            }
        if (!found)
            return usage_error(a, "bad --level \"" + o.level +
                                      "\" (trace|debug|info|warn|error|fatal)");
        d.logs_only = true;
    }
    if (!o.since.empty() && !parse_time(o.since, d.since))
        return usage_error(a, "bad --since \"" + o.since +
                                  "\" (ns since the epoch, or YYYY-MM-DD[THH:MM[:SS[.fff]]])");
    if (!o.until.empty() && !parse_time(o.until, d.until))
        return usage_error(a, "bad --until \"" + o.until + "\"");
    if ((!o.since.empty() || !o.until.empty()) && !d.logs_only)
        d.logs_only = true; // a time range means log frames
    if (!o.type.empty())
    {
        if (o.type.starts_with("0x"))
        {
            if (!internal::parse_u64(o.type, d.type_filter))
                return usage_error(a, "bad --type hash " + o.type);
        }
        else
            d.type_name_filter = o.type;
    }
    // cc F19: stream to stdout one frame at a time instead of buffering the whole rendering.
    const auto to_stdout = [](const std::string_view chunk, void*) {
        return chunk.empty() || std::fwrite(chunk.data(), 1, chunk.size(), stdout) == chunk.size();
    };
    result<void> r;
    if (o.use_stdin || !is_regular_file(source))
    { // a pipe: no second pass, so the input is read whole (decode_limits::input_bytes)
        const auto capture = tools::read_file(source);
        if (!capture)
        {
            fail(a, o.use_stdin ? "stdin" : o.capture, capture.error());
            return 1;
        }
        r = tools::dump(*capture, extra_p, d, +to_stdout, nullptr);
    }
    else
        r = tools::dump(source, extra_p, d, +to_stdout, nullptr, window);
    std::fflush(stdout);
    if (!r)
    {
        fail(a, o.use_stdin ? "stdin" : o.capture, r.error());
        return 1;
    }
    return 0;
}
} // namespace

bool parse_config(const std::string_view text, std::string& err)
{
    options scratch;
    err = parse_config_text(scratch, text);
    return err.empty();
}

} // namespace es::serreg::cli
