#pragma once

#include <cstdint>

namespace GST {
namespace BASE {

enum class Event_type : std::uint32_t {
    NONE        = 0,
    READ_EVENT  = 1u << 0,
    WRITE_EVENT = 1u << 1,
    ERROR_EVENT = 1u << 2
};

Event_type operator|(Event_type lhs, Event_type rhs) noexcept;

Event_type& operator|=(Event_type& lhs, Event_type rhs) noexcept;

Event_type operator&(Event_type lhs, Event_type rhs) noexcept;

bool has_event(Event_type value, Event_type flag) noexcept;

}
}
