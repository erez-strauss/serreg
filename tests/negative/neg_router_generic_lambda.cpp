// negative (2.1.12, review T-10): a generic lambda has no single call signature, so frame_router::on(h) cannot
// read the signature from its parameters; the router says so instead of failing inside its traits.
// expected diagnostic: "the handler has no single call signature"
#include <es/serreg/serreg.hpp>
int main()
{
    es::serreg::frame_router r;
    r.on([](const auto&) {});
    return 0;
}
