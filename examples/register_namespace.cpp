// examples/register_namespace.cpp — register every message type of a namespace before main(), without a
// serialize() call site: the reader-only program, the schema publisher, the plugin whose types are only received.
//   ./register_namespace [dir]     writes <dir>/register_namespace.ser: a capture with no frames whose
//                                  dictionary nevertheless lists every type of app::messages
#include <es/serreg/register_namespace.hpp>
#include <es/serreg/serreg.hpp>

#include <cstdint>
#include <print>
#include <string>

namespace sr = es::serreg;

namespace app::messages
{
struct Order
{
    std::uint64_t id;
    std::int64_t px;
    std::uint32_t qty;
};
struct[[= sr::message]] Fill // the opt-in annotation, for the `annotated<message_t>` selector
{
    std::uint64_t order_id;
    std::uint32_t qty;
};
struct MsgHeartbeat
{
    std::uint32_t seq;
};
enum class Side : std::uint8_t
{
    buy = 1,
    sell = 2
};
struct Helper // not serialisable (a pointer): listed by the selection, skipped by the registration
{
    void* ctx;
};
struct[[= sr::skip]] Scratch // opted out of everything
{
    int x;
};
namespace v2
{
struct MsgOrder // nested namespaces are walked too
{
    std::uint64_t id;
    std::string sym;
};
} // namespace v2
} // namespace app::messages

// One line, once per program: every serialisable type of app::messages (and app::messages::v2) is in the
// registry before main() — in this binary's es_serreg_dict section, in every capture this program opens, in
// export_image(). The selector argument narrows it: ES_SERREG_REGISTER_NAMESPACE(app::messages, sr::name_prefix<"Msg">{}).
ES_SERREG_REGISTER_NAMESPACE(app::messages);
static_assert(sr::registered_type_count<^^app::messages> == 5,
              "Order, Fill, MsgHeartbeat, Side, v2::MsgOrder");

namespace app
{
class program
{
public:
    static int run(const std::string& dir)
    {
        // 1. what the namespace holds — dump it all, then narrow with a pattern or the annotation
        std::println("all types of app::messages:");
        for (const char* const n : sr::selected_type_names<^^app::messages>())
            std::println("  {}", n);
        std::println("name_prefix<\"Msg\">:");
        for (const char* const n : sr::selected_type_names<^^app::messages, sr::name_prefix<"Msg">{
                                                                            }>())
            std::println("  {}", n);
        std::println("annotated<message_t>:");
        for (const char* const n :
             sr::selected_type_names<^^app::messages, sr::annotated<sr::message_t>{
                                                      }>())
            std::println("  {}", n);

        // 2. registered before main(): no serialize() of these types anywhere in this program
        const sr::registry& r = sr::registry::instance();
        std::println("registered: Order {} Side {} v2::MsgOrder {} | {} of {} selected types are "
                     "serialisable "
                     "(Helper has a pointer: listed, not registered)",
                     r.find_type(sr::schema_hash_v<messages::Order>) != nullptr,
                     r.find_type(sr::schema_hash_v<messages::Side>) != nullptr,
                     r.find_type(sr::schema_hash_v<messages::v2::MsgOrder>) != nullptr,
                     sr::registered_type_count<^^app::messages>,
                     sr::selected_type_names<^^app::messages>().size());

        // 3. a capture with no frames still carries the whole dictionary
        const std::string path = dir + "/register_namespace.ser";
        {
            sr::capture_file f(path);
            if (!f.is_open() || !f.close())
                return 1;
        }
        const auto info = sr::tools::info(path);
        if (!info)
            return 1;
        std::println("{}: dictionary {} types, {} calls, 0 frames", "register_namespace.ser",
                     info->dict.types.size(), info->dict.calls.size());
        return 0;
    }
};
} // namespace app

int main(const int argc, char** const argv)
{
    return app::program::run(argc > 1 ? argv[1] : ".");
}
