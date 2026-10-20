#include "control.hpp"

#include "anvil_toml_edit.hpp"
#include "game_build.hpp"
#include "host.hpp"
#include "log.hpp"
#include "plugin_settings.hpp"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstring>

namespace anvil {
namespace {

Control* g_control = nullptr;

constexpr size_t kTailBytes = 64 * 1024;

void copy_error(AnvilControlResult* out, const std::string& error) {
    if (out == nullptr) return;
    const size_t n = std::min(error.size(), sizeof(out->error) - 1);
    std::memcpy(out->error, error.data(), n);
    out->error[n] = '\0';
}

void reset_result(AnvilControlResult* out) {
    if (out == nullptr) return;
    *out = AnvilControlResult{};
    out->size = sizeof(*out);
}

AnvilResult finish_change(const PluginSettings::Change& change, AnvilControlResult* out) {
    if (out != nullptr) {
        out->applied = change.applied ? 1 : 0;
        out->saved = change.saved ? 1 : 0;
        out->notified = static_cast<uint32_t>(change.notified);
        copy_error(out, change.error);
    }
    return change.result;
}

// ---- the C table ---------------------------------------------------------------

void svc_status(AnvilControlStatus* out) {
    if (out == nullptr) return;
    if (g_control == nullptr) {
        *out = AnvilControlStatus{};
        out->size = sizeof(*out);
        return;
    }
    g_control->status(*out);
}

int svc_plugin(uint32_t index, AnvilControlPlugin* out) {
    return g_control != nullptr && out != nullptr && g_control->plugin(index, *out) ? 1 : 0;
}

int svc_warning(uint32_t index, AnvilControlSink sink, void* context) {
    if (g_control == nullptr || sink == nullptr) return 0;
    const auto line = g_control->warning(index);
    if (!line) return 0;
    sink(context, line->data(), line->size());
    return 1;
}

PluginSettings* settings_of(const char* plugin_id) {
    return g_control != nullptr && plugin_id != nullptr ? g_control->settings_of(plugin_id) : nullptr;
}

uint32_t svc_setting_count(const char* plugin_id) {
    const PluginSettings* s = settings_of(plugin_id);
    return s == nullptr ? 0 : static_cast<uint32_t>(s->size());
}

int svc_setting_info(const char* plugin_id, AnvilSetting setting, AnvilControlSetting* out) {
    const PluginSettings* s = settings_of(plugin_id);
    const Setting* d = s == nullptr ? nullptr : s->schema().get(setting);
    if (d == nullptr || out == nullptr) return 0;
    *out = AnvilControlSetting{};
    out->size = sizeof(*out);
    out->name = d->name.c_str();
    out->label = d->label.c_str();
    out->description = d->description.c_str();
    out->group = d->group.c_str();
    out->type = static_cast<uint32_t>(d->type);
    out->restart = d->restart ? 1 : 0;
    const bool is_int = d->type == SettingType::Int;
    const auto number = [&](const SettingValue& v) { return is_int ? static_cast<double>(v.integer) : v.real; };
    if (d->min) out->has_min = 1, out->min = number(*d->min);
    if (d->max) out->has_max = 1, out->max = number(*d->max);
    if (d->step) out->has_step = 1, out->step = number(*d->step);
    out->options = static_cast<uint32_t>(d->options.size());
    return 1;
}

int svc_setting_option(const char* plugin_id, AnvilSetting setting, uint32_t index, const char** name,
                       const char** label) {
    const PluginSettings* s = settings_of(plugin_id);
    const Setting* d = s == nullptr ? nullptr : s->schema().get(setting);
    if (d == nullptr || index >= d->options.size()) return 0;
    const SettingOption& o = d->options[index];
    if (name != nullptr) *name = o.name.c_str();
    if (label != nullptr) *label = o.label.empty() ? o.name.c_str() : o.label.c_str();
    return 1;
}

int svc_setting_value(const char* plugin_id, AnvilSetting setting, uint32_t which, AnvilControlSink sink,
                      void* context) {
    const PluginSettings* s = settings_of(plugin_id);
    const Setting* d = s == nullptr ? nullptr : s->schema().get(setting);
    if (d == nullptr || sink == nullptr) return 0;
    std::optional<SettingValue> value;
    switch (which) {
    case ANVIL_CONTROL_VALUE_CURRENT: value = s->current(setting); break;
    case ANVIL_CONTROL_VALUE_SAVED: value = s->saved(setting); break;
    case ANVIL_CONTROL_VALUE_DEFAULT: value = d->default_value; break;
    default: return 0;
    }
    if (!value) return 0;
    const std::string text = setting_text(*d, *value);
    sink(context, text.data(), text.size());
    return 1;
}

AnvilResult svc_setting_set(const char* plugin_id, AnvilSetting setting, const AnvilControlValue* value,
                            AnvilControlResult* out) {
    reset_result(out);
    PluginSettings* s = settings_of(plugin_id);
    if (s == nullptr) {
        copy_error(out, "no such plugin");
        return ANVIL_ERROR_NOT_FOUND;
    }
    if (value == nullptr) return ANVIL_ERROR_INVALID_ARGUMENT;
    SettingValue v;
    v.integer = value->integer;
    v.real = value->real;
    if (value->text != nullptr) v.text = value->text;
    return finish_change(s->set(setting, v), out);
}

AnvilResult svc_setting_set_text(const char* plugin_id, AnvilSetting setting, const char* text,
                                 AnvilControlResult* out) {
    reset_result(out);
    PluginSettings* s = settings_of(plugin_id);
    if (s == nullptr) {
        copy_error(out, "no such plugin");
        return ANVIL_ERROR_NOT_FOUND;
    }
    if (text == nullptr) return ANVIL_ERROR_INVALID_ARGUMENT;
    return finish_change(s->set_text(setting, text), out);
}

AnvilResult svc_setting_reset(const char* plugin_id, AnvilSetting setting, AnvilControlResult* out) {
    reset_result(out);
    PluginSettings* s = settings_of(plugin_id);
    if (s == nullptr) {
        copy_error(out, "no such plugin");
        return ANVIL_ERROR_NOT_FOUND;
    }
    return finish_change(s->reset(setting), out);
}

int svc_setting_warning(const char* plugin_id, uint32_t index, AnvilControlSink sink, void* context) {
    const PluginSettings* s = settings_of(plugin_id);
    if (s == nullptr || sink == nullptr || index >= s->warnings().size()) return 0;
    const std::string& line = s->warnings()[index];
    sink(context, line.data(), line.size());
    return 1;
}

int svc_previous_crash(AnvilControlCrash* out) {
    if (g_control == nullptr || out == nullptr) return 0;
    const AnvilControlCrash* crash = g_control->previous_crash();
    if (crash == nullptr) return 0;
    *out = *crash;
    return 1;
}

uint32_t svc_log_tail(uint32_t max_lines, AnvilControlSink sink, void* context) {
    if (g_control == nullptr || sink == nullptr) return 0;
    const std::string tail = g_control->log_tail(max_lines);
    sink(context, tail.data(), tail.size());
    return static_cast<uint32_t>(std::count(tail.begin(), tail.end(), '\n') +
                                 (!tail.empty() && tail.back() != '\n' ? 1 : 0));
}

AnvilResult svc_disable_next_start(const char* plugin_id, AnvilControlResult* out) {
    reset_result(out);
    if (g_control == nullptr || plugin_id == nullptr) return ANVIL_ERROR_INVALID_ARGUMENT;
    std::string error;
    const AnvilResult r = g_control->disable_next_start(plugin_id, error);
    if (out != nullptr) out->saved = r == ANVIL_OK ? 1 : 0;
    copy_error(out, error);
    return r;
}

const AnvilControlService kService{
    sizeof(AnvilControlService),
    svc_status,
    svc_plugin,
    svc_warning,
    svc_setting_count,
    svc_setting_info,
    svc_setting_option,
    svc_setting_value,
    svc_setting_set,
    svc_setting_set_text,
    svc_setting_reset,
    svc_setting_warning,
    svc_previous_crash,
    svc_log_tail,
    svc_disable_next_start,
};

const char* skipped_state(const Skipped&) { return "skipped"; }

}  // namespace

// ---- Control ---------------------------------------------------------------------

Control::Control(ControlSources sources) : sources_(std::move(sources)) {
    if (sources_.plugins != nullptr) {
        for (const auto& p : *sources_.plugins) versions_.push_back(p->manifest.version.to_string());
    }
    disabled_next_.insert(sources_.disabled.begin(), sources_.disabled.end());
    g_control = this;
}

Control::~Control() {
    if (g_control == this) g_control = nullptr;
}

const AnvilControlService* Control::service() const { return &kService; }

void Control::loading_done() { loading_done_.store(true, std::memory_order_release); }

void Control::status(AnvilControlStatus& out) const {
    out = AnvilControlStatus{};
    out.size = sizeof(out);
    out.anvil_version = sources_.anvil_version;
    out.game_build = sources_.game_build;
    out.safe_mode = sources_.safe_mode ? 1 : 0;
    out.unfinished_starts = sources_.unfinished_starts;
    const size_t resolved = sources_.plugins ? sources_.plugins->size() : 0;
    const size_t skipped = sources_.skipped ? sources_.skipped->size() : 0;
    out.plugins = static_cast<uint32_t>(resolved + skipped);
    out.skipped = static_cast<uint32_t>(skipped);
    out.warnings = static_cast<uint32_t>(Log::instance().warning_count());
    const bool done = loading_done_.load(std::memory_order_acquire);
    out.loading_done = done ? 1 : 0;
    if (sources_.policy != nullptr) {
        if (const auto session = sources_.policy->session()) {
            out.session = 1;
            out.ironman = session->ironman ? 1 : 0;
            out.multiplayer = session->multiplayer ? 1 : 0;
        }
    }
    if (!done) return;
    std::vector<std::string> active;
    for (const auto& p : *sources_.plugins) {
        if (p->state == "loaded") ++out.loaded;
        else if (p->state == "failed") ++out.failed;
        if (p->state != "loaded" || !p->manifest.affects_gameplay) continue;
        if (sources_.policy != nullptr && sources_.policy->suspended(p->owner)) continue;
        active.push_back(p->manifest.id + " " + p->manifest.version.to_string());
    }
    out.gameplay_active = static_cast<uint32_t>(active.size());
    const std::string hash = plugin_set_hash(std::move(active));
    std::snprintf(out.gameplay_hash, sizeof(out.gameplay_hash), "%s", hash.c_str());
}

bool Control::plugin(uint32_t index, AnvilControlPlugin& out) const {
    const size_t resolved = sources_.plugins ? sources_.plugins->size() : 0;
    const size_t skipped = sources_.skipped ? sources_.skipped->size() : 0;
    if (index >= resolved + skipped) return false;
    out = AnvilControlPlugin{};
    out.size = sizeof(out);
    std::string_view id;
    if (index < resolved) {
        const AnvilPlugin& p = *(*sources_.plugins)[index];
        id = p.manifest.id;
        out.id = p.manifest.id.c_str();
        out.name = p.manifest.name.c_str();
        out.version = versions_[index].c_str();
        out.issues = p.manifest.issues.c_str();
        out.early = p.manifest.phase == Phase::Early ? 1 : 0;
        out.affects_gameplay = p.manifest.affects_gameplay ? 1 : 0;
        out.settings = static_cast<uint32_t>(p.settings.size());
        if (loading_done_.load(std::memory_order_acquire)) {
            out.state = p.state.c_str();
            out.reason = p.reason.c_str();
        } else {
            out.state = "pending";
            out.reason = "";
        }
        out.suspended = sources_.policy != nullptr && sources_.policy->suspended(p.owner) ? 1 : 0;
    } else {
        const Skipped& s = (*sources_.skipped)[index - resolved];
        id = s.id;
        out.id = s.id.c_str();
        out.name = "";
        out.version = "";
        out.issues = "";
        out.state = skipped_state(s);
        out.reason = s.reason.c_str();
        out.disabled = s.disabled ? 1 : 0;
    }
    out.disabled_next_start = disabled_next_start(id) ? 1 : 0;
    return true;
}

std::optional<std::string> Control::warning(uint32_t index) const {
    const auto lines = Log::instance().warnings();
    if (index >= lines.size()) return std::nullopt;
    return lines[index];
}

PluginSettings* Control::settings_of(std::string_view plugin_id) const {
    if (sources_.plugins == nullptr) return nullptr;
    for (const auto& p : *sources_.plugins) {
        if (p->manifest.id == plugin_id) return &p->settings;
    }
    return nullptr;
}

const AnvilControlCrash* Control::previous_crash() {
    std::lock_guard lock(mutex_);
    if (crash_loaded_) return crash_ ? &crash_->view : nullptr;
    crash_loaded_ = true;
    if (sources_.previous_crash.empty()) return nullptr;
    const std::filesystem::path dir = sources_.anvil_dir / "crashes";
    auto summary = load_crash_summary(dir, sources_.previous_crash);
    if (!summary) return nullptr;
    auto cache = std::make_unique<CrashCache>();
    cache->summary = std::move(*summary);
    for (const auto& id : cache->summary.plugins) {
        if (!cache->plugins.empty()) cache->plugins += ", ";
        cache->plugins += id;
    }
    cache->path = (dir / (sources_.previous_crash + ".txt")).wstring();
    cache->report = read_text_file(cache->path, 1 << 20).value_or(std::string());
    // Shown before? The marker holds the name of the last report a start showed.
    const std::filesystem::path shown = sources_.anvil_dir / "data" / "anvil" / "crash-shown.txt";
    std::string marker = read_text_file(shown, 256).value_or(std::string());
    while (!marker.empty() && (marker.back() == '\n' || marker.back() == '\r')) marker.pop_back();
    const bool is_new = marker != sources_.previous_crash;
    if (is_new) {
        std::error_code ignored;
        std::filesystem::create_directories(shown.parent_path(), ignored);
        const std::string text = sources_.previous_crash + "\n";
        const HANDLE h = CreateFileW(shown.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                     nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
            CloseHandle(h);
        }
    }
    AnvilControlCrash& v = cache->view;
    v.size = sizeof(v);
    v.name = sources_.previous_crash.c_str();
    v.level = cache->summary.level.c_str();
    v.summary = cache->summary.summary.c_str();
    v.plugins = cache->plugins.c_str();
    v.issues = cache->summary.issues.c_str();
    v.report = cache->report.c_str();
    v.path = cache->path.c_str();
    v.before_game_ready = cache->summary.before_game_ready ? 1 : 0;
    v.is_new = is_new ? 1 : 0;
    crash_ = std::move(cache);
    return &crash_->view;
}

std::string Control::log_tail(uint32_t max_lines) const {
    return read_text_tail(sources_.anvil_dir / "anvil.log", max_lines, kTailBytes);
}

bool Control::disabled_next_start(std::string_view plugin_id) const {
    std::lock_guard lock(mutex_);
    return disabled_next_.contains(plugin_id);
}

AnvilResult Control::disable_next_start(std::string_view plugin_id, std::string& error) {
    bool known = false;
    if (sources_.plugins != nullptr) {
        for (const auto& p : *sources_.plugins) known = known || p->manifest.id == plugin_id;
    }
    if (sources_.skipped != nullptr) {
        for (const auto& s : *sources_.skipped) known = known || s.id == plugin_id;
    }
    if (!known) {
        error = "no plugin '" + std::string(plugin_id) + "' at this start";
        return ANVIL_ERROR_NOT_FOUND;
    }
    std::lock_guard lock(mutex_);
    if (!add_disabled_plugin(sources_.anvil_dir / "anvil.toml", plugin_id, error)) {
        Log::instance().write(ANVIL_LOG_WARNING, "anvil", "cannot disable " + std::string(plugin_id) +
                                                              " at the next start: " + error);
        return ANVIL_ERROR_INTERNAL;
    }
    disabled_next_.insert(std::string(plugin_id));
    Log::instance().write(ANVIL_LOG_INFO, "anvil",
                          std::string(plugin_id) + " is disabled from the next start (disabled_plugins in anvil.toml)");
    return ANVIL_OK;
}

// ---- helpers ---------------------------------------------------------------------

std::string setting_text(const Setting& setting, const SettingValue& value) {
    switch (setting.type) {
    case SettingType::Bool: return value.integer != 0 ? "true" : "false";
    case SettingType::Int: return std::to_string(value.integer);
    case SettingType::Float: {
        char text[64];
        const auto [end, ec] = std::to_chars(text, text + sizeof(text), value.real);
        return std::string(text, ec == std::errc() ? end : text);
    }
    case SettingType::Enum:
        return value.integer >= 0 && static_cast<size_t>(value.integer) < setting.options.size()
                   ? setting.options[static_cast<size_t>(value.integer)].name
                   : std::string();
    case SettingType::String: return value.text;
    case SettingType::Key: return key_name(value.integer);
    }
    return {};
}

std::string plugin_set_hash(std::vector<std::string> ids_and_versions) {
    if (ids_and_versions.empty()) return {};
    std::sort(ids_and_versions.begin(), ids_and_versions.end());
    std::string text;
    for (const auto& line : ids_and_versions) text += line + "\n";
    const std::string sha = sha256_bytes(text.data(), text.size());
    if (sha.size() < 16) return {};
    return sha.substr(0, 4) + "-" + sha.substr(4, 4) + "-" + sha.substr(8, 4) + "-" + sha.substr(12, 4);
}

}  // namespace anvil
