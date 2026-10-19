#include "suspension.hpp"

namespace anvil {

ChangeStatus suspend_plugin(ChangeRegistry& changes, ServiceBus& bus, OwnerId owner, const std::string& id) {
    bool changed = false;
    const ChangeStatus status = changes.suspend(owner, &changed);
    if (changed) bus.publish_to(id, kEventSuspended, nullptr);
    return status;
}

ChangeStatus resume_plugin(ChangeRegistry& changes, ServiceBus& bus, OwnerId owner, const std::string& id) {
    bool changed = false;
    const ChangeStatus status = changes.resume(owner, &changed);
    if (changed) bus.publish_to(id, kEventResumed, nullptr);
    return status;
}

}  // namespace anvil
