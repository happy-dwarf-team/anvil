#pragma once

#include "changes.hpp"
#include "services.hpp"

#include <string>

namespace anvil {

// Sent to the suspended or resumed plugin only (docs/api2/anvil_plugin.h;
// the public header gets the macros with #46). Payload NULL.
inline constexpr const char* kEventSuspended = "anvil.suspended";
inline constexpr const char* kEventResumed = "anvil.resumed";

// The core API behind the gameplay policy (design §8.4, §13; anvil-game uses
// it for ironman and multiplayer sessions). Suspending first takes the
// plugin's changes out of the game, then tells the plugin, so it can stop its
// threads; resuming puts the changes back, then tells it. Calling either in
// the state the plugin is already in does nothing and sends no event. The
// result is the registry's: Internal when some site could not be written (the
// plugin is told anyway, since its state changed).
ChangeStatus suspend_plugin(ChangeRegistry& changes, ServiceBus& bus, OwnerId owner, const std::string& id);
ChangeStatus resume_plugin(ChangeRegistry& changes, ServiceBus& bus, OwnerId owner, const std::string& id);

}  // namespace anvil
