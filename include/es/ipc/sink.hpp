// es/ipc/sink.hpp — basic_sink<Unit, Align, DataAlign>: a producer seen as a claiming sink (claim(n) → std::byte*,
// commit(n) → bool, write(p, n) → bool), the shape es::serreg::serialize() writes into in place:
//
//   es::ipc::sink out{*prod, my_frame_type};
//   es::serreg::serialize(out, "fill"_lit, fill);        // the frame is built directly in the ring record
//
// No dependency on es::serreg: any code that wants "claim a span, fill it, commit it" can use it.
#pragma once

#include "layout.hpp"
#include "producer.hpp"

#include <cstddef>
#include <cstdint>

namespace es::ipc
{

template<std::uint32_t Unit = dynamic, std::uint32_t Align = 64, std::uint32_t DataAlign = 16>
class basic_sink
{
public:
    // `type`: the message type of every record this sink writes. publish = false defers the cursor store
    // (basic_producer::commit(n, false)); call flush() on the producer, or let a later publishing commit do it.
    explicit basic_sink(basic_producer<Unit, Align, DataAlign>& p,
                        const std::uint32_t type = type_data, const bool publish = true) noexcept
        : p_(&p), type_(type), publish_(publish)
    {
    }
    // A record of up to n bytes, or nullptr (too large, a reserved type, or no room under full_policy::fail).
    std::byte* claim(const std::size_t n) noexcept
    {
        const auto s = p_->claim(n, type_);
        return s ? s->data() : nullptr;
    }
    bool commit(const std::size_t n) noexcept
    {
        return p_->commit(n, publish_);
    }
    bool write(const std::byte* const data, const std::size_t n) noexcept
    {
        const auto s = p_->claim(n, type_);
        if (!s)
            return false;
        if (n)
            __builtin_memcpy(s->data(), data, n);
        return p_->commit(n, publish_);
    }
    void type(const std::uint32_t t) noexcept
    {
        type_ = t;
    }
    std::uint32_t type() const noexcept
    {
        return type_;
    }
    basic_producer<Unit, Align, DataAlign>& producer() const noexcept
    {
        return *p_;
    }

private:
    basic_producer<Unit, Align, DataAlign>* p_;
    std::uint32_t type_;
    bool publish_;
};

using sink = basic_sink<>;

} // namespace es::ipc
