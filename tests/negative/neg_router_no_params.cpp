// negative (2.1.10): frame_router::on(h) takes the signature from h's parameters, so a handler without parameters
// is rejected at compile time (a frame of literals only is registered with on(lit, h) or on<Sig...>(h)).
// expected diagnostic: "the handler's parameters are the signature"
#include <es/serreg/serreg.hpp>
int main()
{
    es::serreg::frame_router r;
    r.on([] {});
    return 0;
}
