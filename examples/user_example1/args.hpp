// examples/user_example1/args.hpp — the command line of the three readers:
//
//   readerN [--blocks|--mapped] [file]    default: user_example1.ser
//
// --blocks is kept from 2.0.6: since 2.1.16 every file is read in windows (10 MiB, bounded memory; also pipes and
// "-") by default, so the flag changes nothing; --mapped maps the whole file instead. The library
// picks the reading strategy from tools::file_options.
#pragma once

#include <es/serreg/serreg.hpp>

#include <string>
#include <string_view>

namespace ue1
{
struct reader_options
{
    std::string path = "user_example1.ser";
    es::serreg::tools::file_options file;

    static reader_options parse(const int argc, char** const argv)
    {
        reader_options o;
        for (int i = 1; i < argc; ++i)
            if (std::string_view(argv[i]) == "--blocks")
                o.file.blocks = true; // the default since 2.1.16
            else if (std::string_view(argv[i]) == "--mapped")
                o.file.blocks = false;
            else
                o.path = argv[i];
        return o;
    }
};
} // namespace ue1
