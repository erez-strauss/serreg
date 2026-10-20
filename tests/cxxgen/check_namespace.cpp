// tests/cxxgen/check_namespace.cpp — 2.1.16 (plan A5): the header generated from orders.rri with
// `--namespace corpus=regen` beside the original corpus.hpp (ctest cxxgen_golden_namespace_compiles). The
// regenerated types live in regen:: but keep their canonical names through [[=es::serreg::name("corpus::…")]], so
// their hashes equal the originals' and their native layouts agree.
#include "corpus.hpp"
#include "orders_ns.hpp"

#include <type_traits>

static_assert(es::serreg::schema_hash_v<regen::Order> == es::serreg::schema_hash_v<corpus::Order>);
static_assert(es::serreg::layout_hash_v<regen::Order> == es::serreg::layout_hash_v<corpus::Order>);
static_assert(es::serreg::schema_hash_v<regen::Cancel> ==
              es::serreg::schema_hash_v<corpus::Cancel>);
static_assert(es::serreg::schema_hash_v<regen::Side> == es::serreg::schema_hash_v<corpus::Side>);
static_assert(es::serreg::canonical_name_v<regen::Order> == "corpus::Order");
static_assert(sizeof(regen::Order) == sizeof(corpus::Order) &&
              alignof(regen::Order) == alignof(corpus::Order));
static_assert(offsetof(regen::Order, sym) == offsetof(corpus::Order, sym));
static_assert(
    std::is_same_v<std::underlying_type_t<regen::Side>, std::underlying_type_t<corpus::Side>>);

int main()
{
    es::serreg::frame_router r;
    install_handlers(r);
    return r.size() == 9 ? 0 : 1;
}
