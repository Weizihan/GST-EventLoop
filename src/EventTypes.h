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

inline Event_type operator|(Event_type lhs, Event_type rhs) noexcept {
    return static_cast<Event_type>(
        static_cast<std::uint32_t>(lhs) | static_cast<std::uint32_t>(rhs)
    );
}

inline Event_type& operator|=(Event_type& lhs, Event_type rhs) noexcept {
    lhs = lhs | rhs;
    return lhs;
}

inline Event_type operator&(Event_type lhs, Event_type rhs) noexcept {
    return static_cast<Event_type>(
        static_cast<std::uint32_t>(lhs) & static_cast<std::uint32_t>(rhs)
    );
}

inline bool has_event(Event_type value, Event_type flag) noexcept {
    return (static_cast<std::uint32_t>(value) & static_cast<std::uint32_t>(flag)) != 0;
}

}
}
