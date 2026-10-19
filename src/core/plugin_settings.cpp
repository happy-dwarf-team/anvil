#include "plugin_settings.hpp"

#include <windows.h>

namespace anvil {
namespace {

std::optional<std::string> process_environment(const std::string& name) {
    const std::wstring wide(name.begin(), name.end());  // ASCII: ANVIL_<ID>_<NAME>
    const DWORD size = GetEnvironmentVariableW(wide.c_str(), nullptr, 0);
    if (size == 0) return std::nullopt;
    std::wstring value(size, L'\0');
    const DWORD length = GetEnvironmentVariableW(wide.c_str(), value.data(), size);
    value.resize(length);
    if (value.empty()) return std::string();
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), out.data(), bytes, nullptr, nullptr);
    return out;
}

// Only the field the type uses, so values compare equal by content.
SettingValue normalized(const Setting& s, const SettingValue& v) {
    SettingValue out;
    if (s.type == SettingType::Float) out.real = v.real;
    else if (s.type == SettingType::String) out.text = v.text;
    else out.integer = v.integer;
    return out;
}

}  // namespace

std::vector<PluginSettings::Note> PluginSettings::init(SettingsFile schema, const std::filesystem::path& player_file,
                                                       std::string_view plugin_id, const Environment& environment) {
    std::vector<Note> notes;
    schema_ = std::move(schema);
    player_file_ = player_file;
    const size_t count = schema_.settings.size();
    current_.resize(count);
    saved_.resize(count);
    slots_.clear();
    for (size_t i = 0; i < count; ++i) slots_.push_back(std::make_unique<Slot>());

    const std::string where = "data/" + std::string(plugin_id) + "/settings.toml";
    const PlayerSettings player = load_player_settings(schema_, player_file_);
    for (const auto& w : player.warnings) {
        warnings_.push_back(where + ", " + w);
        notes.push_back({ANVIL_LOG_WARNING, warnings_.back()});
    }
    for (const auto& s : schema_.settings) {
        saved_[s.id] = normalized(s, player.values[s.id].value_or(s.default_value));
        current_[s.id] = saved_[s.id];
        const std::string variable = setting_environment_name(plugin_id, s.name);
        const auto text = environment ? environment(variable) : process_environment(variable);
        if (text) {
            std::string error;
            if (const auto value = parse_setting_text(s, *text, error)) {
                current_[s.id] = normalized(s, *value);
                notes.push_back({ANVIL_LOG_INFO, "setting " + s.name + " = " + setting_toml(s, current_[s.id]) + " from " +
                                                     variable + " (environment)"});
            } else {
                warnings_.push_back(variable + ": " + error + "; ignored");
                notes.push_back({ANVIL_LOG_WARNING, warnings_.back()});
            }
        }
        store(s, *slots_[s.id], current_[s.id]);
    }
    return notes;
}

void PluginSettings::store(const Setting& setting, Slot& slot, const SettingValue& value) {
    switch (setting.type) {
    case SettingType::Bool:
    case SettingType::Int:
        slot.integer.store(value.integer, std::memory_order_release);
        slot.real.store(static_cast<double>(value.integer), std::memory_order_release);
        break;
    case SettingType::Float:
        slot.real.store(value.real, std::memory_order_release);
        break;
    case SettingType::Enum:
        slot.integer.store(value.integer, std::memory_order_release);
        slot.text.store(setting.options[static_cast<size_t>(value.integer)].name.c_str(), std::memory_order_release);
        break;
    case SettingType::String:
    case SettingType::Key:
        slot.integer.store(value.integer, std::memory_order_release);
        slot.texts.push_back(
            std::make_unique<std::string>(setting.type == SettingType::Key ? key_name(value.integer) : value.text));
        slot.text.store(slot.texts.back()->c_str(), std::memory_order_release);
        break;
    }
}

int PluginSettings::get_bool(AnvilSetting id) const {
    const Setting* s = schema_.get(id);
    return s != nullptr && s->type == SettingType::Bool && slots_[id]->integer.load(std::memory_order_acquire) != 0;
}

int64_t PluginSettings::get_int(AnvilSetting id) const {
    const Setting* s = schema_.get(id);
    if (s == nullptr || s->type == SettingType::Float || s->type == SettingType::String) return 0;
    return slots_[id]->integer.load(std::memory_order_acquire);
}

double PluginSettings::get_float(AnvilSetting id) const {
    const Setting* s = schema_.get(id);
    if (s == nullptr || (s->type != SettingType::Float && s->type != SettingType::Int)) return 0.0;
    return slots_[id]->real.load(std::memory_order_acquire);
}

const char* PluginSettings::get_string(AnvilSetting id) const {
    return id < slots_.size() ? slots_[id]->text.load(std::memory_order_acquire) : "";
}

std::optional<SettingValue> PluginSettings::current(AnvilSetting id) const {
    const std::lock_guard lock(mutex_);
    if (id >= current_.size()) return std::nullopt;
    return current_[id];
}

std::optional<SettingValue> PluginSettings::saved(AnvilSetting id) const {
    const std::lock_guard lock(mutex_);
    if (id >= saved_.size()) return std::nullopt;
    return saved_[id];
}

AnvilResult PluginSettings::subscribe(AnvilSetting id, AnvilSettingCallback callback, void* context) {
    const Setting* s = schema_.get(id);
    if (s == nullptr) return ANVIL_ERROR_NOT_DECLARED;
    if (s->restart) return ANVIL_ERROR_WRONG_KIND;
    if (callback == nullptr) return ANVIL_ERROR_INVALID_ARGUMENT;
    const std::lock_guard lock(mutex_);
    slots_[id]->subscribers.push_back({callback, context});
    return ANVIL_OK;
}

void PluginSettings::drop_subscriptions() {
    const std::lock_guard lock(mutex_);
    for (auto& slot : slots_) slot->subscribers.clear();
}

PluginSettings::Change PluginSettings::set(AnvilSetting id, const SettingValue& value) {
    Change change;
    const Setting* s = schema_.get(id);
    if (s == nullptr) {
        change.result = ANVIL_ERROR_NOT_DECLARED;
        change.error = "setting " + std::to_string(id) + " is not declared";
        return change;
    }
    const SettingValue v = normalized(*s, value);
    if (std::string problem = check_setting_value(*s, v); !problem.empty()) {
        change.result = ANVIL_ERROR_INVALID_ARGUMENT;
        change.error = s->name + ": " + problem;
        return change;
    }
    std::vector<Slot::Subscriber> notify;
    {
        const std::lock_guard lock(mutex_);
        saved_[id] = v;
        if (!player_file_.empty()) change.saved = write_player_settings(schema_, saved_, player_file_, change.error);
        if (!s->restart) {
            current_[id] = v;
            store(*s, *slots_[id], v);
            change.applied = true;
            notify = slots_[id]->subscribers;
        }
    }
    for (const auto& subscriber : notify) subscriber.callback(id, subscriber.context);
    change.notified = notify.size();
    return change;
}

PluginSettings::Change PluginSettings::set_text(AnvilSetting id, std::string_view text) {
    const Setting* s = schema_.get(id);
    if (s == nullptr) return set(id, {});
    std::string error;
    const auto value = parse_setting_text(*s, text, error);
    if (!value) {
        Change change;
        change.result = ANVIL_ERROR_INVALID_ARGUMENT;
        change.error = s->name + ": " + error;
        return change;
    }
    return set(id, *value);
}

PluginSettings::Change PluginSettings::reset(AnvilSetting id) {
    const Setting* s = schema_.get(id);
    return set(id, s != nullptr ? s->default_value : SettingValue{});
}

}  // namespace anvil
