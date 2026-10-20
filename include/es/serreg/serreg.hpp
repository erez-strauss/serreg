// es/serreg/serreg.hpp — umbrella header.
//
//   #include <es/serreg/serreg.hpp>
//   using namespace es::serreg::literals;
//   struct Order { std::uint64_t id; std::int64_t px; std::uint32_t qty; char sym[8]; };
//
//   std::vector<std::byte> buf; es::serreg::vector_sink out{buf};
//   es::serreg::serialize(out, "new order"_lit, Order{42, 1050000, 100, "AAPL"});   // Order is now in the registry
//   Order o; es::serreg::deserialize(buf, "new order"_lit, o);
//   es::serreg::registry::instance().export_image("registry.rri");                  // or srdump --elf a.out
#pragma once
#include <es/serreg/arg.hpp>
#include <es/serreg/blob.hpp>
#include <es/serreg/capture.hpp>
#include <es/serreg/descriptor.hpp>
#include <es/serreg/deserialize.hpp>
#include <es/serreg/dictionary.hpp>
#include <es/serreg/error.hpp>
#include <es/serreg/format.hpp>
#include <es/serreg/hash.hpp>
#include <es/serreg/limits.hpp> // 2.1.16: decode_limits (FR-D5)
#include <es/serreg/literal.hpp>
#include <es/serreg/log.hpp> // 2.2.1: logger, level, dictionary scopes
#include <es/serreg/print.hpp>
#include <es/serreg/projection.hpp> // schema evolution level 1: renamed_from, tools::project, deserialize(dict, …)
#include <es/serreg/reflect.hpp>
#include <es/serreg/registry.hpp>
#include <es/serreg/router.hpp> // 2.1.10: frame_router
#include <es/serreg/serialize.hpp>
#include <es/serreg/sink.hpp>
#include <es/serreg/tools.hpp>
#include <es/serreg/version.hpp>
#include <es/serreg/walk.hpp> // 2.0.6: tools::walk_values<Ts...>
