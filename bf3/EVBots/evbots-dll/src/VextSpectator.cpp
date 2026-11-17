// Original VU BR camera subset. Native evidence and limits:
// D:/Games-cache/royale/reports/spectator-native-2026-11-11.md
// GameView and FreeCamera are stock BF3 objects; no VU-appended fields are read.
#include "VextSpectator.h"

#include <Windows.h>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "EVBotsLog.h"

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

namespace evbots {
namespace {

#ifndef EVBOTS_SPECTATOR_TEST_MAIN
constexpr uintptr_t kMain = 0x022E3790;
#else
// Standalone read-side lifecycle fixture; native function addresses/signature
// checks stay unchanged, so synthetic objects cannot activate a native camera.
constexpr uintptr_t kMain = EVBOTS_SPECTATOR_TEST_MAIN;
#endif
constexpr uintptr_t kViewVtable = 0x021433D0, kFreeVtable = 0x02146260;
constexpr uintptr_t kEnableFree = 0x00FF6300, kDisableFree = 0x00FF6350;
constexpr uintptr_t kSetView = 0x00FF6370, kSetInput = 0x00FFB700, kSetTransform = 0x011A4560;
constexpr uintptr_t kRenderer = 0x02384D78, kRendererVtable = 0x0213E9C8, kUpdateMatrices = 0x006C3A90;
enum Mode { FreeCamera = 0, FirstPerson = 1, ThirdPerson = 2, Disabled = 3 };

// State belongs to the engine thread. A foreign-thread shutdown only posts a
// request; it never calls a Frostbite camera method or touches an engine pointer.
std::atomic<DWORD> g_owner{0};
std::atomic<bool> g_stop{false};
// Read-only startup callbacks can run on the autostart worker, including after
// a Lua restart. Publish one coherent snapshot without sharing engine pointers.
std::atomic<uint32_t> g_snapshot{static_cast<uint32_t>(Disabled) << 1};
struct Context { uintptr_t view = 0, level = 0, pm = 0, local = 0, free = 0; bool cameraReady = false; };
struct State {
    bool active = false;
    int mode = Disabled;
    uintptr_t view = 0, level = 0, target = 0;
    uint32_t targetId = 0;
} g_state;

void PublishState() {
    uint32_t value = (g_state.active ? 1u : 0u) | (static_cast<uint32_t>(g_state.mode) << 1);
    if (g_state.active && g_state.target) value |= 8u | (g_state.targetId << 16);
    g_snapshot.store(value);
}

void ClearState() {
    g_state = {};
    PublishState();
}

uint32_t U32(uintptr_t p) { return *reinterpret_cast<const uint32_t*>(p); }
bool Matches(uintptr_t p, const unsigned char* bytes, size_t size) {
    __try { return std::memcmp(reinterpret_cast<const void*>(p), bytes, size) == 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool NativeCodeMatches() {
    // All instruction boundaries/signatures verified against zbf3.exe SHA256
    // 8201c5e14667d511f402c424d5c87a3a5ed2d9fdf81c67c914a145a9d3bc42b9.
    static const unsigned char enable[] = {0x55,0x8B,0xEC,0x83,0xE4,0xF0,0x83,0xEC,0x4C,0x56,0x8B,0xF1,0x80,0x7E,0x73,0x00};
    static const unsigned char disable[] = {0x80,0x79,0x73,0x00,0x74,0x0D,0x8B,0x01,0x8B,0x50,0x48,0x6A,0x01,0xC6,0x41,0x73,0x00,0xFF,0xD2,0xC3};
    static const unsigned char setView[] = {0x55,0x8B,0xEC,0x83,0xE4,0xF0,0x83,0xEC,0x48,0x56,0x57,0x8B,0x7D,0x08,0x8B,0xF1};
    static const unsigned char input[] = {0x55,0x8B,0xEC,0x83,0xE4,0xF0,0x8B,0x45,0x08,0x81,0xEC,0xB4,0,0,0,0x53,0x56,0x8B,0xD9};
    static const unsigned char transform[] = {0x56,0x8B,0x74,0x24,0x08,0x57,0x8D,0x79,0x10,0xB9,0x10,0,0,0,0xF3,0xA5,0x5F,0x5E,0xC2,0x04,0};
    return Matches(kEnableFree, enable, sizeof(enable)) && Matches(kDisableFree, disable, sizeof(disable)) &&
        Matches(kSetView, setView, sizeof(setView)) && Matches(kSetInput, input, sizeof(input)) &&
        Matches(kSetTransform, transform, sizeof(transform));
}

bool ReadContext(Context* c) {
    *c = {};
    __try {
        const uintptr_t main = U32(kMain);
        if (!main) return false;
        const uintptr_t client = U32(main + 0xB4);
        if (!client) return false;
        const uintptr_t context = U32(client + 0x0C);
        c->view = U32(client + 0x24);
        c->pm = U32(client + 0x1C);
        if (!context || !c->view || U32(c->view) != kViewVtable) return false;
        if (U32(context + 0x38) != c->view) return false;
        c->free = U32(c->view + 0x64);
        c->cameraReady = c->free && U32(c->free) == kFreeVtable && U32(c->view + 0x60);
        if (!c->cameraReady) return false;
        c->level = U32(context + 0x10);
        c->local = c->pm ? U32(c->pm + 0xBC) : 0;
        return c->level && c->local;
    } __except (EXCEPTION_EXECUTE_HANDLER) { *c = {}; return false; }
}

uintptr_t PlayerById(const Context& c, uint32_t id) {
    __try {
        const uint32_t slots = U32(c.pm + 8);
        const uintptr_t players = U32(c.pm + 0xC0);
        if (slots > 65536 || id >= slots || !players) return 0;
        const uintptr_t player = U32(players + id * 4);
        return player && U32(player + 0x3EC) == id ? player : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

bool FreeIsActive(const Context& c) {
    __try { return *reinterpret_cast<const unsigned char*>(c.view + 0x73) != 0 && U32(c.view + 4) == 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

using NoArg = void(__thiscall*)(void*);
using IntArg = void(__thiscall*)(void*, int);
using TransformArg = void(__thiscall*)(void*, const float*);

bool RestoreView(const Context& c) {
    if (!NativeCodeMatches()) return false;
    __try {
        reinterpret_cast<NoArg>(kDisableFree)(reinterpret_cast<void*>(c.view));
        reinterpret_cast<IntArg>(kSetView)(reinterpret_cast<void*>(c.view), 2);
        return U32(c.view + 4) == 2 && *reinterpret_cast<const unsigned char*>(c.view + 0x73) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool EnableFree(const Context& c) {
    if (!NativeCodeMatches()) return false;
    bool ok = false;
    __try {
        // Same stock methods and order as VeniceEXT 10724CD0 (free/third-person).
        reinterpret_cast<NoArg>(kEnableFree)(reinterpret_cast<void*>(c.view));
        reinterpret_cast<IntArg>(kSetView)(reinterpret_cast<void*>(c.view), 1);
        reinterpret_cast<IntArg>(kSetInput)(reinterpret_cast<void*>(c.view), 0);
        ok = FreeIsActive(c);
    } __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
    if (!ok) RestoreView(c);
    return ok;
}

bool EngineThread() {
    return g_owner.load() == GetCurrentThreadId();
}

void StopOnEngine() {
    Context c;
    // Never dereference a cached view after destruction/replacement.
    ReadContext(&c);
    if (g_state.active && c.cameraReady && c.view == g_state.view) {
        if (!RestoreView(c)) LOG_WARN("EVBots: SpectatorManager: native player view restoration failed");
    }
    ClearState();
}

bool Refresh(Context* c) {
    if (!ReadContext(c)) {
        if (g_state.active && c->cameraReady && c->view == g_state.view) RestoreView(*c);
        ClearState();
        return false;
    }
    if (g_state.active && (c->view != g_state.view || c->level != g_state.level)) StopOnEngine();
    if (g_state.active && (g_state.mode == FreeCamera || g_state.mode == ThirdPerson) && !FreeIsActive(*c)) {
        // An engine view transition took ownership back; do not report stale success.
        ClearState();
    }
    if (g_state.active && g_state.target && PlayerById(*c, g_state.targetId) != g_state.target) {
        g_state.target = 0;
        g_state.targetId = 0;
        if (EnableFree(*c)) g_state.mode = FreeCamera;
        else ClearState();
    }
    PublishState();
    return true;
}

int RequireContext(lua_State* L, Context* c, bool active) {
    if (!EngineThread()) return luaL_error(L, "SpectatorManager must run on the client engine thread");
    if (g_stop.exchange(false)) StopOnEngine();
    if (!Refresh(c)) return luaL_error(L, "SpectatorManager: native client GameView/level/player is unavailable");
    if (active && !g_state.active) return luaL_error(L, "SpectatorManager: spectating is not active");
    return 0;
}

int l_state(lua_State* L) {
    if (EngineThread()) {
        if (g_stop.exchange(false)) StopOnEngine();
        Context c;
        Refresh(&c);
    }
    const uint32_t state = g_snapshot.load();
    lua_pushboolean(L, (state & 1u) != 0);
    lua_pushinteger(L, (state >> 1) & 3u);
    if (state & 8u) lua_pushinteger(L, state >> 16);
    else lua_pushnil(L);
    return 3;
}

int l_enabled(lua_State* L) {
    luaL_checktype(L, 1, LUA_TBOOLEAN);
    if (!g_owner.load() && !lua_toboolean(L, 1)) return 0;
    if (!EngineThread()) return luaL_error(L, "SpectatorManager must run on the client engine thread");
    if (!lua_toboolean(L, 1)) { g_stop = false; StopOnEngine(); return 0; }
    Context c;
    RequireContext(L, &c, false);
    if (g_state.active) return 0;
    if (!EnableFree(c)) return luaL_error(L, "SpectatorManager: native free camera activation failed (client signatures/view)");
    g_state.active = true;
    g_state.mode = FreeCamera;
    g_state.view = c.view;
    g_state.level = c.level;
    PublishState();
    return 0;
}

bool ApplyMode(const Context& c, int mode) {
    const bool ok = mode == FreeCamera || mode == ThirdPerson ? EnableFree(c) : RestoreView(c);
    if (ok) g_state.mode = mode;
    else ClearState();
    return ok;
}

int l_mode(lua_State* L) {
    const lua_Integer mode = luaL_checkinteger(L, 1);
    if (mode < FreeCamera || mode > Disabled) return luaL_error(L, "SpectatorManager: invalid camera mode");
    Context c;
    RequireContext(L, &c, true);
    if (mode == FirstPerson && g_state.target != c.local)
        return luaL_error(L, "SpectatorManager: remote first-person camera is not implemented; native player-camera binding is required");
    if (mode == ThirdPerson && !g_state.target)
        return luaL_error(L, "SpectatorManager: third-person camera requires an existing spectated player");
    if (!ApplyMode(c, static_cast<int>(mode))) return luaL_error(L, "SpectatorManager: native camera mode change failed");
    if (mode == FreeCamera || mode == Disabled) { g_state.target = 0; g_state.targetId = 0; }
    PublishState();
    return 0;
}

int l_target(lua_State* L) {
    const bool hasTarget = !lua_isnil(L, 1);
    const lua_Integer id = hasTarget ? luaL_checkinteger(L, 1) : 0;
    luaL_checktype(L, 2, LUA_TBOOLEAN);
    const bool firstPerson = lua_toboolean(L, 2) != 0;
    if (id < 0 || id > 65535) return luaL_error(L, "SpectatorManager: invalid player id");
    Context c;
    RequireContext(L, &c, true);
    const uintptr_t target = hasTarget ? PlayerById(c, static_cast<uint32_t>(id)) : 0;
    if (hasTarget && !target) return luaL_error(L, "SpectatorManager: player is no longer present in the native client");
    if (target && firstPerson && target != c.local)
        return luaL_error(L, "SpectatorManager: remote first-person camera is not implemented; native player-camera binding is required");
    const int mode = !target ? FreeCamera : firstPerson ? FirstPerson : ThirdPerson;
    if (!ApplyMode(c, mode)) return luaL_error(L, "SpectatorManager: native spectated camera activation failed");
    g_state.target = target;
    g_state.targetId = target ? static_cast<uint32_t>(id) : 0;
    PublishState();
    return 0;
}

bool ReadFree(const Context& c, float matrix[16]) {
    __try { std::memcpy(matrix, reinterpret_cast<const void*>(c.free + 0x10), 64); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool ReadRendered(float matrix[16]) {
    static const unsigned char update[] = {0x55,0x8B,0xEC,0x83,0xE4,0xF0};
    if (!Matches(kUpdateMatrices, update, sizeof(update))) return false;
    __try {
        const uintptr_t renderer = U32(kRenderer);
        if (!renderer || U32(renderer) != kRendererVtable) return false;
        reinterpret_cast<NoArg>(kUpdateMatrices)(reinterpret_cast<void*>(renderer + 0x50));
        std::memcpy(matrix, reinterpret_cast<const void*>(renderer + 0xDE0), 64);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

int PushMatrix(lua_State* L, const float matrix[16]) {
    for (unsigned i = 0; i < 16; ++i) if (!std::isfinite(matrix[i])) { lua_pushnil(L); return 1; }
    for (unsigned row = 0; row < 4; ++row)
        for (unsigned col = 0; col < 3; ++col) lua_pushnumber(L, matrix[row * 4 + col]);
    return 12;
}

int l_transform(lua_State* L) {
    if (!g_owner.load()) { lua_pushnil(L); return 1; }
    if (!EngineThread()) return luaL_error(L, "SpectatorManager must run on the client engine thread");
    Context c;
    alignas(16) float matrix[16] = {};
    if (!Refresh(&c) || !ReadFree(c, matrix)) { lua_pushnil(L); return 1; }
    return PushMatrix(L, matrix);
}

int l_camera(lua_State* L) {
    if (!g_owner.load()) { lua_pushnil(L); return 1; }
    if (!EngineThread()) return luaL_error(L, "SpectatorManager must run on the client engine thread");
    Context c;
    alignas(16) float matrix[16] = {};
    if (!Refresh(&c) || !g_state.active || !ReadRendered(matrix)) { lua_pushnil(L); return 1; }
    return PushMatrix(L, matrix);
}

bool WriteTransform(const Context& c, const float matrix[16]) {
    if (!NativeCodeMatches()) return false;
    __try {
        reinterpret_cast<TransformArg>(kSetTransform)(reinterpret_cast<void*>(c.free), matrix);
        return std::memcmp(reinterpret_cast<const void*>(c.free + 0x10), matrix, 64) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

int l_set_transform(lua_State* L) {
    alignas(16) float matrix[16] = {};
    matrix[15] = 1;
    for (unsigned row = 0; row < 4; ++row) for (unsigned col = 0; col < 3; ++col) {
        const float value = static_cast<float>(luaL_checknumber(L, static_cast<int>(row * 3 + col + 1)));
        if (!std::isfinite(value)) return luaL_error(L, "SpectatorManager: transform contains non-finite coordinates");
        matrix[row * 4 + col] = value;
    }
    Context c;
    RequireContext(L, &c, true);
    if (g_state.mode != FreeCamera && g_state.mode != ThirdPerson)
        return luaL_error(L, "SpectatorManager: free camera must be active before setting its transform");
    if (!WriteTransform(c, matrix)) return luaL_error(L, "SpectatorManager: native free camera transform write failed");
    return 0;
}

const char kBindings[] = R"LUA(
local state, enabled, mode, target = __spectator_state, __spectator_enabled, __spectator_mode, __spectator_target
local getTransform, setTransform = __spectator_transform, __spectator_set_transform
local function lt(a,b,c,d,e,f,g,h,i,j,k,l)
  if l == nil then return nil end
  return LinearTransform(Vec3(a,b,c), Vec3(d,e,f), Vec3(g,h,i), Vec3(j,k,l))
end
local sm = {}
function sm:GetSpectating() local active = state() return active end
function sm:SetSpectating(active) return enabled(active) end
function sm:GetCameraMode() local _, current = state() return current end
function sm:SetCameraMode(current) return mode(current) end
function sm:GetSpectatedPlayer()
  local _, _, id = state()
  return id ~= nil and PlayerManager:GetPlayerById(id) or nil
end
function sm:SpectatePlayer(player, firstPerson)
  return target(player ~= nil and player.id or nil, firstPerson)
end
function sm:GetFreecameraTransform() return lt(getTransform()) end
function sm:SetFreecameraTransform(t)
  if t == nil then error('SpectatorManager: transform is required', 2) end
  return setTransform(t.left.x,t.left.y,t.left.z,t.up.x,t.up.y,t.up.z,
    t.forward.x,t.forward.y,t.forward.z,t.trans.x,t.trans.y,t.trans.z)
end
SpectatorManager = sm
)LUA";

}  // namespace

void TickVextSpectator() {
    DWORD expected = 0;
    g_owner.compare_exchange_strong(expected, GetCurrentThreadId());
    if (!EngineThread()) return;
    if (g_stop.exchange(false)) StopOnEngine();
    Context c;
    Refresh(&c);
}

void StopVextSpectator() {
    const DWORD owner = g_owner.load();
    if (owner && owner != GetCurrentThreadId()) { g_stop = true; return; }
    g_stop = false;
    StopOnEngine();
}

bool RegisterVextSpectator(lua_State* L) {
    if (!L) return false;
    const luaL_Reg funcs[] = {
        {"__spectator_state", l_state}, {"__spectator_enabled", l_enabled},
        {"__spectator_mode", l_mode}, {"__spectator_target", l_target},
        {"__spectator_transform", l_transform}, {"__spectator_set_transform", l_set_transform},
        {"__spectator_camera_transform", l_camera}, {nullptr, nullptr}
    };
    for (const luaL_Reg* f = funcs; f->name; ++f) { lua_pushcfunction(L, f->func); lua_setglobal(L, f->name); }
    if (luaL_loadbufferx(L, kBindings, sizeof(kBindings) - 1, "=[vext-spectator]", "t") != LUA_OK ||
        lua_pcall(L, 0, 0, 0) != LUA_OK) {
        LOG_ERR("EVBots: SpectatorManager registration failed: %s", lua_tostring(L, -1));
        lua_pop(L, 1);
        return false;
    }
    LOG_INFO("EVBots: SpectatorManager: native free/third-person camera; remote first-person unavailable");
    return true;
}

}  // namespace evbots
