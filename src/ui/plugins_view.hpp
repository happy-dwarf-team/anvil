// The Plugins tab as the Manager and the overlay both draw it (ADR 0013,
// point 9; docs/ui-guidelines.md §5): the list with its search and filters,
// the selected plugin's detail with its modules and settings, and the
// confirmations. The rules come from the view model (plugin_model.hpp); what
// differs between the apps (how a value is edited and written, what else a
// plugin's detail says) comes from a PluginsHost. Each app compiles this file
// with its own Dear ImGui.
#pragma once

#include "ui/plugin_model.hpp"

#include <imgui.h>

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace anvil::ui {

// The list entry's second line in the player's words, and its colour.
std::string entry_line(const EntryLine& line);
ImVec4 entry_color(const EntryLine& line);
// "Where it runs" in a sentence, naming each kept-out mode once.
std::string where_sentence(const Switches& switches, const Offers& offers);
std::string switch_word(bool on);
std::string allow_word(bool on);

class PluginsHost {
public:
    virtual ~PluginsHost() = default;

    // Nothing can be changed now (the Manager while the game runs).
    virtual bool read_only() const = 0;
    // Writes the edits; the host refreshes its snapshot as it needs.
    virtual void apply(const std::vector<Edit>& edits) = 0;
    // The plugin's plugin.toml was read: its switch, modules and settings can be drawn.
    virtual bool known(std::size_t plugin) const = 0;
    // Why the plugin's own switch cannot change here; "" when it can.
    virtual std::string switch_refusal(std::size_t /*plugin*/) const { return {}; }
    // The setting's control, labelled; it writes through apply() or on its own.
    virtual void draw_setting_control(std::size_t plugin, std::size_t setting, bool disabled) = 0;
    // A value of the setting (as the snapshot holds it) as the player reads it.
    virtual std::string shown_value(std::size_t plugin, std::size_t setting, const std::string& value) const = 0;
    // The overlay: the game runs, so a row says what is in effect in it.
    virtual bool in_game() const { return false; }

    // Parts of the detail only the app knows, in their place: buttons after
    // Reset in the header; facts under the header (id, authors, approval);
    // notes after the description (problems, updates).
    virtual bool has_header_buttons(std::size_t /*plugin*/) const { return false; }
    virtual void draw_header_buttons(std::size_t /*plugin*/) {}
    virtual void draw_facts(std::size_t /*plugin*/) {}
    virtual void draw_notes(std::size_t /*plugin*/) {}
    // A section without plugins is shown when this says so (the playset's warnings);
    // its note goes under its header.
    virtual bool show_empty_section(Origin /*origin*/) const { return false; }
    virtual void draw_section_note(Origin /*origin*/, const SectionCounts& /*counts*/) {}
};

// A question before edits or another action (ui-guidelines §4).
struct Confirm {
    std::string title;
    std::string button;
    std::vector<std::string> lines;
    std::vector<Edit> edits;
    std::function<void()> action;  // instead of the edits (the Manager's Remove)
};

class PluginsTab {
public:
    // The selection, the search and the chips stay while the app is open.
    std::string selected;
    std::string find;
    unsigned chips = 0;

    // "Waiting for your approval: [Name] [Other (update to 1.1.0)]".
    void draw_waiting(const Snapshot& model);
    // "n change(s) wait for the next start. [Undo them]".
    void draw_pending(const Snapshot& model, PluginsHost& host);
    // The list and the detail side by side, filling the rest of the window.
    void draw_panes(const Snapshot& model, PluginsHost& host);
    // Once per frame, where the app draws its modals.
    void draw_confirm(PluginsHost& host);

    void ask(Confirm confirm);
    void ask_edits(const Snapshot& model, const PluginsHost& host, std::string title, std::string button, std::vector<Edit> edits);
    void ask_turn_on(const Snapshot& model, const Chain& chain);

private:
    void draw_list(const Snapshot& model, PluginsHost& host);
    void draw_detail(const Snapshot& model, std::size_t i, PluginsHost& host);
    void draw_where(const Snapshot& model, std::size_t i, std::string_view module, const Switches& saved, const Offers& offers,
                    bool without_off, bool disabled, PluginsHost& host);
    void draw_module(const Snapshot& model, std::size_t i, std::size_t m, bool disabled, PluginsHost& host);
    void draw_settings_of(const Snapshot& model, std::size_t i, std::string_view module, bool disabled, PluginsHost& host);
    void draw_setting(const Snapshot& model, std::size_t i, std::size_t s, bool disabled, PluginsHost& host);

    std::optional<Confirm> confirm_;
    bool open_confirm_ = false;
};

}  // namespace anvil::ui
