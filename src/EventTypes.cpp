#include "EventTypes.h"

namespace GST {
namespace BASE {

Event_type operator|(Event_type lhs, Event_type rhs) noexcept {
    return static_cast<Event_type>(static_cast<std::uint32_t>(lhs) | static_cast<std::uint32_t>(rhs));
}

Event_type& operator|=(Event_type& lhs, Event_type rhs) noexcept {
    lhs = lhs | rhs;
    return lhs;
}

Event_type operator&(Event_type lhs, Event_type rhs) noexcept {
    return static_cast<Event_type>(static_cast<std::uint32_t>(lhs) & static_cast<std::uint32_t>(rhs));
}

bool has_event(Event_type value, Event_type flag) noexcept {
    return (static_cast<std::uint32_t>(value) & static_cast<std::uint32_t>(flag)) != 0;
}

}
}
