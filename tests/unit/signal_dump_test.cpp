// tests/unit/signal_dump_test.cpp — gk 1.13 / §6.11: ES_SERREG_DUMP_SIGNAL. The parent forks, sets
// ES_SERREG_DUMP_SIGNAL=USR2, ES_SERREG_DUMP_REGISTRY=<file> and ES_SERREG_DUMP_FORMAT=rri in the child and re-execs
// this binary in "child" mode (the environment must be in place before libserreg's priority-101 constructor runs:
// that is where the signal is blocked and the sigwait helper is configured). The child reports readiness on a pipe,
// the parent sends SIGUSR2 with kill(), the child waits for the dump file to appear and leaves through _Exit (so
// the atexit dump cannot be what wrote the file). The parent then parses the file as an RRI and checks it names the
// child's type. Also checks that the same binary exits cleanly with the helper thread running (shutdown path).
#include <es/serreg/serreg.hpp>

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <print>
#include <string>
#include <string_view>

namespace sig
{
struct Heartbeat
{
    std::uint64_t seq;
    std::uint32_t load;
};
} // namespace sig

namespace
{

int fail(const char* const what)
{
    std::println(stderr, "FAIL: {}", what);
    return 1;
}

bool file_exists_nonempty(const char* const path)
{
    struct stat st = {};
    return ::stat(path, &st) == 0 && st.st_size > 0;
}

int child_main(const char* const path, const int ready_fd)
{
    using namespace es::serreg;
    std::byte buf[64];
    span_sink s{buf};
    if (!serialize(s, sig::Heartbeat{1, 2}))
        return 5;
    if (::write(ready_fd, "r", 1) != 1)
        return 6;
    ::close(ready_fd);
    for (int i = 0; i < 500; ++i)
    { // up to 5 s for the helper thread to write the dump
        if (file_exists_nonempty(path))
        {
            // a second request must work too (the helper keeps running)
            ::unlink(path);
            ::kill(::getpid(), SIGUSR2);
            for (int j = 0; j < 500; ++j)
            {
                if (file_exists_nonempty(path))
                    std::_Exit(0);
                ::usleep(10000);
            }
            std::_Exit(4);
        }
        ::usleep(10000);
    }
    std::_Exit(3);
}

} // namespace

namespace signal_dump_test
{
// signal_dump_test <dump-file>                  the test (parent)
// signal_dump_test child <dump-file> <ready-fd>  re-exec'ed with the dump environment set
// signal_dump_test clean-exit x                  re-exec'ed: exits with the helper thread configured but idle
struct options
{
    enum class mode
    {
        parent,
        child,
        clean_exit,
        usage
    };
    mode m = mode::usage;
    const char* self = nullptr;
    const char* path = nullptr;
    int ready_fd = -1;

    static options parse(const int argc, char** const argv)
    {
        options o;
        o.self = argv[0];
        if (argc >= 4 && std::string_view(argv[1]) == "child")
        {
            o.m = mode::child;
            o.path = argv[2];
            o.ready_fd = std::atoi(argv[3]);
        }
        else if (argc >= 3 && std::string_view(argv[1]) == "clean-exit")
            o.m = mode::clean_exit;
        else if (argc >= 2)
        {
            o.m = mode::parent;
            o.path = argv[1];
        }
        return o;
    }
};

class app
{
public:
    explicit app(const options& opts) : opts_{opts}
    {
    }

    int run() const
    {
        switch (opts_.m)
        {
        case options::mode::child:
            return child_main(opts_.path, opts_.ready_fd);
        case options::mode::clean_exit:
            return clean_exit_child();
        case options::mode::usage:
            return fail("usage: signal_dump_test <dump-file>");
        case options::mode::parent:
            break;
        }
        ::unlink(opts_.path);
        if (const int rc = signal_triggered_dump())
            return rc;
        if (const int rc = clean_exit_with_idle_helper())
            return rc;
        return bogus_signal_name_ignored();
    }

private:
    options opts_;

    // helper thread configured (env set by the parent) but never signalled: process exit must join it cleanly
    static int clean_exit_child()
    {
        using namespace es::serreg;
        std::byte buf[64];
        span_sink s{buf};
        return serialize(s, sig::Heartbeat{1, 2}) ? 0 : 5;
    }

    // ---- 1. signal-triggered dump ----
    int signal_triggered_dump() const
    {
        int fds[2];
        if (::pipe(fds) != 0)
            return fail("pipe");
        const pid_t pid = ::fork();
        if (pid < 0)
            return fail("fork");
        if (pid == 0)
        {
            ::close(fds[0]);
            ::setenv("ES_SERREG_DUMP_SIGNAL", "USR2", 1);
            ::setenv("ES_SERREG_DUMP_REGISTRY", opts_.path, 1);
            ::setenv("ES_SERREG_DUMP_FORMAT", "rri", 1);
            const std::string fd = std::to_string(fds[1]);
            ::execl(opts_.self, opts_.self, "child", opts_.path, fd.c_str(),
                    static_cast<char*>(nullptr));
            ::_exit(9);
        }
        ::close(fds[1]);
        char r = 0;
        if (::read(fds[0], &r, 1) != 1 || r != 'r')
            return fail("child did not report readiness");
        ::close(fds[0]);
        if (::kill(pid, SIGUSR2) != 0)
            return fail("kill");
        int status = 0;
        if (::waitpid(pid, &status, 0) != pid)
            return fail("waitpid");
        std::println("child: exited={:d} code={} signaled={:d}", WIFEXITED(status),
                     WIFEXITED(status) ? WEXITSTATUS(status) : -1, WIFSIGNALED(status));
        if (WIFSIGNALED(status))
            return fail(
                "child was killed by the signal: SIGUSR2 was not blocked/handled by the helper");
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
            return fail("child did not see the dump file (helper thread did not dump)");
        const auto bytes = es::serreg::tools::read_file(opts_.path);
        if (!bytes)
            return fail("dump file unreadable");
        es::serreg::dyn::dictionary d;
        if (!d.add_rri(*bytes))
            return fail("dump file is not a valid RRI");
        if (!d.find_type_by_name("sig::Heartbeat"))
            return fail("dump does not name sig::Heartbeat");
        std::println("ok: signal dump parsed, {} types", d.types.size());
        return 0;
    }

    // ---- 2. clean exit with the helper thread configured but idle (join in the atexit hook) ----
    int clean_exit_with_idle_helper() const
    {
        const pid_t pid = ::fork();
        if (pid < 0)
            return fail("fork");
        if (pid == 0)
        {
            ::setenv("ES_SERREG_DUMP_SIGNAL", "USR2", 1);
            ::setenv("ES_SERREG_DUMP_REGISTRY", "/dev/null", 1);
            ::setenv("ES_SERREG_DUMP_FORMAT", "rri", 1);
            ::execl(opts_.self, opts_.self, "clean-exit", "x", static_cast<char*>(nullptr));
            ::_exit(9);
        }
        int status = 0;
        if (::waitpid(pid, &status, 0) != pid)
            return fail("waitpid 2");
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
            return fail("process with an idle dump helper did not exit cleanly");
        std::println("ok: clean exit with the helper thread running");
        return 0;
    }

    // ---- 3. an unknown signal name is rejected without disabling anything else ----
    int bogus_signal_name_ignored() const
    {
        const pid_t pid = ::fork();
        if (pid == 0)
        {
            ::setenv("ES_SERREG_DUMP_SIGNAL", "BOGUS", 1);
            ::setenv("ES_SERREG_DUMP_REGISTRY", "/dev/null", 1);
            ::execl(opts_.self, opts_.self, "clean-exit", "x", static_cast<char*>(nullptr));
            ::_exit(9);
        }
        int status = 0;
        if (::waitpid(pid, &status, 0) != pid)
            return fail("waitpid 3");
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
            return fail("bogus signal name must be ignored, not fatal");
        std::println("ok: bogus signal name ignored");
        return 0;
    }
};
} // namespace signal_dump_test

int main(const int argc, char** const argv)
{
    return signal_dump_test::app{signal_dump_test::options::parse(argc, argv)}.run();
}
