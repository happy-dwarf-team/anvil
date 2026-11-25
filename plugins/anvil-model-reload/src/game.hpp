// The real Game of reload.hpp: the game's gfx type table, the fields of its
// mesh types, a mesh type's reload (mesh_type_reload) and the console, on the main thread
// only. Every read and call runs under a structured exception handler, so a
// bad address ends a run with `faulted` instead of the game.
#pragma once

#include <cstdint>

#include "reload.hpp"

namespace model_reload {

struct GameAddresses {
    std::uintptr_t graphics = 0;  // where the game keeps its CGraphics*
    std::uint32_t bucket_count = 0;
    std::uint32_t buckets = 0;
    std::uint32_t mesh_type_id = 0;
    std::uintptr_t mesh_vtable = 0;
    std::uintptr_t reload_type = 0;
    std::uintptr_t string_assign = 0;
    std::uintptr_t console_manager = 0;  // where the game keeps its console manager
    std::uintptr_t console_dispatch = 0;
    std::uintptr_t string_free = 0;
    // The objects on the map: the game state -> its graphics owner -> the application
    // -> the graphical map -> its province graphics (data, size); the unit slots of
    // each are counted, the clear and the sprites' creation delete the province's objects.
    std::uintptr_t game_state = 0;  // where the game keeps its CGameState*
    std::uint32_t graphics_owner = 0;
    std::uint32_t application = 0;
    std::uint32_t map = 0;
    std::uint32_t provinces = 0;
    std::uint32_t province_count = 0;
    std::uint32_t units = 0;
    std::uint32_t unit_slots = 0;
    std::uintptr_t province_clear = 0;
    std::uintptr_t province_sprites = 0;
    // The objects the clear leaves: each province's envoys (its envoy entries, and
    // the envoy objects of each), the map's trade routes and its visible
    // ambient objects.
    std::uint32_t envoy_graphics = 0;
    std::uint32_t envoys = 0;
    std::uintptr_t envoy_recreate = 0;
    std::uint32_t trade_routes = 0;
    std::uint32_t trade_goods = 0;
    std::uintptr_t trade_goods_recreate = 0;
    std::uint32_t ambient_objects = 0;
};

class GameMemory final : public Game {
public:
    explicit GameMemory(const GameAddresses& a) : a_(a) {}

    bool mesh_types(std::vector<MeshType>& out) override;
    bool set_scale(std::uintptr_t type, float scale) override;
    bool set_cull_square(std::uintptr_t type, float value) override;
    bool set_file(std::uintptr_t type, std::string_view file) override;
    bool set_lookup_name(std::uintptr_t type, std::size_t index, std::string_view name) override;
    bool reload_type(std::uintptr_t type) override;
    ConsoleAnswer console(const std::vector<std::string>& tokens) override;
    MapRefresh refresh_map_objects() override;

private:
    bool recreate_envoys(std::uintptr_t province_graphics, int& count);
    bool recreate_trade_goods(std::uintptr_t map, int& count);
    bool remove_ambient(std::uintptr_t map, int& count);

    GameAddresses a_;
};

}  // namespace model_reload
