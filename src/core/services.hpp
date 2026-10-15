#pragma once

#include <anvil/anvil_plugin.h>

#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace anvil {

// Named, versioned services and synchronous events shared by plugins.
class ServiceBus {
public:
    AnvilResult register_service(const std::string& owner, const std::string& name, uint32_t version,
                                 const void* service);
    const void* get_service(const std::string& name, uint32_t version) const;
    std::vector<std::string> describe() const;

    AnvilResult subscribe(const std::string& owner, const std::string& event, AnvilEventCallback callback,
                          void* context);
    // Calls subscribers in subscription order on the calling thread.
    size_t publish(const std::string& event, const void* payload);

    // Drops services and subscriptions of a plugin that failed to load.
    void remove_owner(const std::string& owner);

private:
    struct Service {
        std::string owner;
        const void* pointer;
    };
    struct Subscriber {
        std::string owner;
        AnvilEventCallback callback;
        void* context;
    };
    mutable std::mutex mutex_;
    std::map<std::pair<std::string, uint32_t>, Service> services_;
    std::map<std::string, std::vector<Subscriber>> subscribers_;
};

bool is_valid_service_name(const std::string& owner, const std::string& name);

}  // namespace anvil
