#include "services.hpp"

#include <algorithm>
#include <iterator>

namespace anvil {

bool is_valid_service_name(const std::string& owner, const std::string& name) {
    // "<owner-id>.<service>" — a plugin can only publish under its own id.
    if (name.size() <= owner.size() + 1 || name.compare(0, owner.size(), owner) != 0 || name[owner.size()] != '.') {
        return false;
    }
    for (size_t i = owner.size() + 1; i < name.size(); ++i) {
        const char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.')) return false;
    }
    return true;
}

AnvilResult ServiceBus::register_service(const std::string& owner, const std::string& name, uint32_t version,
                                         const void* service) {
    if (service == nullptr || version == 0 || !is_valid_service_name(owner, name)) return ANVIL_ERROR_INVALID_ARGUMENT;
    if (*static_cast<const uint32_t*>(service) < sizeof(uint32_t)) return ANVIL_ERROR_INVALID_ARGUMENT;  // size field
    std::lock_guard lock(mutex_);
    const auto key = std::make_pair(name, version);
    if (services_.contains(key)) return ANVIL_ERROR_ALREADY_EXISTS;
    services_.emplace(key, Service{owner, service});
    return ANVIL_OK;
}

const void* ServiceBus::get_service(const std::string& name, uint32_t version) const {
    std::lock_guard lock(mutex_);
    const auto it = services_.find(std::make_pair(name, version));
    return it == services_.end() ? nullptr : it->second.pointer;
}

std::vector<std::string> ServiceBus::describe() const {
    std::lock_guard lock(mutex_);
    std::vector<std::string> out;
    for (const auto& [key, s] : services_) {
        out.push_back(key.first + " v" + std::to_string(key.second) + " (" + s.owner + ")");
    }
    return out;
}

AnvilResult ServiceBus::subscribe(const std::string& owner, const std::string& event, AnvilEventCallback callback,
                                  void* context) {
    if (event.empty() || callback == nullptr) return ANVIL_ERROR_INVALID_ARGUMENT;
    std::lock_guard lock(mutex_);
    subscribers_[event].push_back({owner, callback, context});
    return ANVIL_OK;
}

size_t ServiceBus::publish(const std::string& event, const void* payload) {
    std::vector<Subscriber> copy;
    {
        std::lock_guard lock(mutex_);
        const auto it = subscribers_.find(event);
        if (it != subscribers_.end()) copy = it->second;
    }
    for (const auto& s : copy) s.callback(event.c_str(), payload, s.context);
    return copy.size();
}

size_t ServiceBus::publish_to(const std::string& owner, const std::string& event, const void* payload) {
    std::vector<Subscriber> copy;
    {
        std::lock_guard lock(mutex_);
        const auto it = subscribers_.find(event);
        if (it != subscribers_.end()) {
            std::copy_if(it->second.begin(), it->second.end(), std::back_inserter(copy),
                         [&](const Subscriber& s) { return s.owner == owner; });
        }
    }
    for (const auto& s : copy) s.callback(event.c_str(), payload, s.context);
    return copy.size();
}

void ServiceBus::remove_owner(const std::string& owner) {
    std::lock_guard lock(mutex_);
    std::erase_if(services_, [&](const auto& item) { return item.second.owner == owner; });
    for (auto& [event, list] : subscribers_) {
        std::erase_if(list, [&](const Subscriber& s) { return s.owner == owner; });
    }
}

}  // namespace anvil
