// VextClientRaycast.cpp — лучи клиента из движка bf3.exe (К5 шаг 4, сборка 223): RaycastManager:CollisionRaycast.
//
// Как у VU (декомпилят veniceext 10B49380, ext\CLIENT-SLOTS-2026-10-23.md ★★★):
//   01199D10 — __thiscall(this = физический менеджер, const char* метка, int режим, const Vec4* from, const Vec4* to,
//   Hit* буфер, int maxHits, u32 materialFlags, u32 rayFlags, void* доп) -> число попаданий; ret 0x24 (9 аргументов).
//   VU: режим 1 для CollisionRaycast, доп = 0. this: VU берёт свой объект уровня; сам движок зовёт с [0x023813A0]
//   (0076ECCA: mov ecx,[0x023813A0]) — так и делаем.
//   Попадание — 0x40 байт: +0x00 позиция (Vec4), +0x10 нормаль (Vec4), +0x20 rigidBody (сущность физики), +0x24 материал
//   (геттеры RayCastHit у VU: 10A83D50 [hit+0x20], 109A0B90 hit+0x10, 109A0CE0 hit+0x00, 10A83C90 [hit+0x24]).
//   rigidBody:Is(имя) — по рефлексии Frostbite: vtable[0] = getType «mov eax, ClassInfo; ret» (B8 imm32 C3), имя класса —
//   [[ClassInfo+0]+0]; предок — [ClassInfo+0x0C] (гипотеза о раскладке ClassInfo, проверка — в прогоне по журналу).
// Зовём только из потока движка (Lua клиента — в хуке кадра). Пролог 01199D10 сверяется перед первым вызовом.

#include "VextClientRaycast.h"

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <cmath>
#include <intrin.h>
#include <vector>

#include "EngineResources.h"
#include "EVBotsLog.h"
#include "NativeBlueprint.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

namespace evbots {
void EVBotsPushLiveInstance(lua_State*,unsigned long);
namespace {

constexpr uintptr_t kRayFnVa = 0x01199D10;
constexpr unsigned char kRayFnPrologue[12] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x81, 0xEC, 0x14, 0x02, 0x00, 0x00};
constexpr uintptr_t kPhysicsManagerVarVa = 0x023813A0;
constexpr int kMaxHits = 16;

using RayFn = int(__fastcall*)(void* self, void* edx, const char* label, int mode, const float* from, const float* to,
                               void* hits, int maxHits, uint32_t materialFlags, uint32_t rayFlags, void* extra);

struct alignas(16) Hit {
    float pos[4];
    float normal[4];
    uint32_t rigidBody;
    uint32_t material;
    uint32_t rest[6];
};
static_assert(sizeof(Hit) == 0x40, "попадание движка — 0x40 байт");

int g_prologueOk = -1;  // -1 не проверяли, 0 не совпал, 1 совпал

bool PrologueOk() {
    if (g_prologueOk < 0) {
        unsigned char b[sizeof(kRayFnPrologue)] = {};
        SIZE_T n = 0;
        g_prologueOk = ::ReadProcessMemory(::GetCurrentProcess(), reinterpret_cast<LPCVOID>(kRayFnVa), b, sizeof(b), &n) &&
                               n == sizeof(b) && std::memcmp(b, kRayFnPrologue, sizeof(b)) == 0
                           ? 1
                           : 0;
        if (g_prologueOk == 1) {
            LOG_INFO("EVBots: клиент: ★ лучи — пролог 0x%08X совпал с zbf3.exe, CollisionRaycast из движка",
                     static_cast<unsigned>(kRayFnVa));
        } else {
            LOG_ERR("EVBots: клиент: лучи — пролог 0x%08X не совпал, CollisionRaycast остаётся пустым",
                    static_cast<unsigned>(kRayFnVa));
        }
    }
    return g_prologueOk == 1;
}

// ★ Сборка 226: диагностика — прогон 225: луч зовётся (пролог совпал), но ни одного попадания и код не виден.
// Счётчики по исходу, первые 3 вызова целиком, сводка раз в 20 с. У VU this = [[X+0x10]+0xC0] — тот же объект, что
// глобал [0x023813A0] (разбор 25.10, MODLOG 226: единственный конструктор 00F93C10 пишет себя туда, владелец — в +0xC0).
// Контрольный луч (Lua, раз в 10 с от солдата вниз) идёт с probe = true — не в счётчиках. [pm+0x844] — мир физики,
// который 01199D10 обходит (+0x45CD0/+0x4B820/+0x51370), пишется в журнал.
unsigned g_calls = 0, g_pos = 0, g_zero = 0, g_noPm = 0, g_seh = 0, g_badIn = 0;
unsigned long g_sehCode = 0;
unsigned long long g_nextStats = 0;

int CallRay(const float from[4], const float to[4], Hit* hits, int maxHits, uint32_t matFlags, uint32_t rayFlags) {
    __try {
        void* pm = *reinterpret_cast<void**>(kPhysicsManagerVarVa);
        if (pm == nullptr) return -1;
        const int n = reinterpret_cast<RayFn>(kRayFnVa)(pm, nullptr, "RaycastManager::CollisionRaycast", 1, from, to, hits,
                                                        maxHits, matFlags, rayFlags, nullptr);
        return n < 0 ? 0 : (n > maxHits ? maxHits : n);
    } __except (g_sehCode = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return -2;
    }
}

uintptr_t U32(uintptr_t a) { return *reinterpret_cast<const uint32_t*>(a); }

/// [pm+0x844] — мир физики менеджера (0 — нет менеджера или не прочиталось).
unsigned PmWorld() {
    __try {
        const uintptr_t pm = U32(kPhysicsManagerVarVa);
        return pm != 0 ? static_cast<unsigned>(U32(pm + 0x844)) : 0u;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0u;
    }
}

/// ClassInfo объекта по его getType (vtable[0] = B8 imm32 C3) без вызова. 0 — не вышло.
uintptr_t ClassInfoOf(uintptr_t obj) {
    __try {
        if (obj == 0) return 0;
        const uintptr_t vt = U32(obj);
        if (vt == 0) return 0;
        const uintptr_t fn = U32(vt);
        const unsigned char* c = reinterpret_cast<const unsigned char*>(fn);
        if (c[0] == 0xB8 && c[5] == 0xC3) return U32(fn + 1);
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

bool ClassName(uintptr_t ci, char* buf, size_t cap) {
    __try {
        if (ci == 0) return false;
        const uintptr_t info = U32(ci);
        if (info == 0) return false;
        const char* s = reinterpret_cast<const char*>(U32(info));
        if (s == nullptr) return false;
        size_t i = 0;
        for (; i + 1 < cap && s[i] != '\0'; ++i) {
            const char ch = s[i];
            if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_')) return false;
            buf[i] = ch;
        }
        buf[i] = '\0';
        return i > 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

uintptr_t SuperOf(uintptr_t ci) {
    __try {
        return ci != 0 ? U32(ci + 0x0C) : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

bool Finite(float v) { return v == v && v > -1.0e7f && v < 1.0e7f; }

/// __clr_collision(fx,fy,fz, tx,ty,tz, maxHits, materialFlags, rayFlags[, probe]) -> массив {x,y,z, nx,ny,nz, body}
/// (может быть пуст). probe = true — контрольный луч: не в счётчиках и не в «первых 3».
int l_clr_collision(lua_State* L) {
    // движок читает Vec4 через SSE — выравнивание 16
    alignas(16) float from[4] = {static_cast<float>(luaL_checknumber(L, 1)), static_cast<float>(luaL_checknumber(L, 2)),
                                 static_cast<float>(luaL_checknumber(L, 3)), 0.0f};
    alignas(16) float to[4] = {static_cast<float>(luaL_checknumber(L, 4)), static_cast<float>(luaL_checknumber(L, 5)),
                   static_cast<float>(luaL_checknumber(L, 6)), 0.0f};
    int maxHits = static_cast<int>(luaL_optinteger(L, 7, 1));
    if (maxHits < 1) maxHits = 1;
    if (maxHits > kMaxHits) maxHits = kMaxHits;
    const uint32_t matFlags = static_cast<uint32_t>(luaL_optinteger(L, 8, 0));
    const uint32_t rayFlags = static_cast<uint32_t>(luaL_optinteger(L, 9, 0));
    const bool probe = lua_toboolean(L, 10) != 0;
    lua_newtable(L);
    bool finite = true;
    for (float v : from) finite = finite && Finite(v);
    for (float v : to) finite = finite && Finite(v);
    if (!finite) { if (!probe) ++g_badIn; return 1; }
    if (!PrologueOk()) return 1;
    alignas(16) Hit hits[kMaxHits] = {};
    const int n = CallRay(from, to, hits, maxHits, matFlags, rayFlags);
    if (probe) {
        LOG_INFO("EVBots: клиент: контрольный луч вниз (%.1f %.1f %.1f) -> (%.1f %.1f %.1f) pm %08X [pm+0x844] %08X -> %d%s"
                 " (-1 нет менеджера, -2 исключение 0x%08lX)",
                 from[0], from[1], from[2], to[0], to[1], to[2], *reinterpret_cast<const unsigned*>(kPhysicsManagerVarVa),
                 PmWorld(), n, n > 0 ? "" : " — земля НЕ попалась", g_sehCode);
    } else {
        ++g_calls;
        if (n > 0) ++g_pos; else if (n == 0) ++g_zero; else if (n == -1) ++g_noPm; else ++g_seh;
    }
    if (!probe && g_calls <= 3) {
        LOG_INFO("EVBots: клиент: луч #%u (%.1f %.1f %.1f) -> (%.1f %.1f %.1f) maxHits %d mat 0x%X ray 0x%X pm %08X"
                 " [pm+0x844] %08X -> %d (-1 нет менеджера, -2 исключение 0x%08lX)",
                 g_calls, from[0], from[1], from[2], to[0], to[1], to[2], maxHits, matFlags, rayFlags,
                 *reinterpret_cast<const unsigned*>(kPhysicsManagerVarVa), PmWorld(), n, g_sehCode);
    }
    const unsigned long long now = ::GetTickCount64();
    if (!probe && now >= g_nextStats) {
        if (g_nextStats != 0) {
            LOG_INFO("EVBots: клиент: лучи за 20 с (всего): вызовов %u, с попаданиями %u, пусто %u, нет менеджера %u,"
                     " исключений %u (код 0x%08lX), плохой вход %u",
                     g_calls, g_pos, g_zero, g_noPm, g_seh, g_sehCode, g_badIn);
        }
        g_nextStats = now + 20000;
    }
    for (int i = 0; i < n; ++i) {
        const Hit& h = hits[i];
        lua_createtable(L, 0, 7);
        lua_pushnumber(L, h.pos[0]); lua_setfield(L, -2, "x");
        lua_pushnumber(L, h.pos[1]); lua_setfield(L, -2, "y");
        lua_pushnumber(L, h.pos[2]); lua_setfield(L, -2, "z");
        lua_pushnumber(L, h.normal[0]); lua_setfield(L, -2, "nx");
        lua_pushnumber(L, h.normal[1]); lua_setfield(L, -2, "ny");
        lua_pushnumber(L, h.normal[2]); lua_setfield(L, -2, "nz");
        lua_pushinteger(L, static_cast<lua_Integer>(h.rigidBody)); lua_setfield(L, -2, "body");
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

/// __clr_types(ptr) -> массив имён класса и предков (от самого класса вверх), пусто — не прочиталось.
int l_clr_types(lua_State* L) {
    const uintptr_t obj = static_cast<uintptr_t>(luaL_checkinteger(L, 1));
    lua_newtable(L);
    uintptr_t ci = ClassInfoOf(obj);
    for (int depth = 0; ci != 0 && depth < 24; ++depth) {
        char name[96];
        if (!ClassName(ci, name, sizeof(name))) break;
        lua_pushstring(L, name);
        lua_rawseti(L, -2, depth + 1);
        const uintptr_t up = SuperOf(ci);
        if (up == ci) break;
        ci = up;
    }
    return 1;
}

// VU SpatialRaycast 10B4886D..10B48929: GameWorld+844, stock
// SpatialQueryManager::rayCast 0083B9B0, six stack arguments (ret 18).
// Its output is Entity*, NOT CollisionRaycast's PhysicsEntity hit records.
// Grow 00837830 and allocator/free 00495780/004957E0 prove the fixed-vector ABI.
struct QueryCollector {
    uintptr_t* begin;
    uintptr_t* end;
    uintptr_t* capacity;
    uintptr_t allocator;
    uintptr_t* fixed;
    uintptr_t entries[1024];
};
static_assert(offsetof(QueryCollector, entries) == 0x14, "native x86 query collector ABI");
struct QueryRange { int minimum, maximum; };
struct QueryFilter { QueryRange* range; unsigned reserved; };
using SpatialQueryFn = void(__fastcall*)(void*,void*,const char*,QueryCollector*,const float*,const float*,QueryFilter*,uint32_t);
using QueryFreeFn = void(__fastcall*)(void*,void*,void*,unsigned);

bool QuerySignatures() {
    static int checked = -1;
    if (checked >= 0) return checked == 1;
    struct Signature { uintptr_t address; unsigned char bytes[16]; };
    const Signature required[] = {
        {0x0083B9B0,{0x55,0x8B,0xEC,0x83,0xE4,0xF0,0x8B,0x45,0x14,0x83,0xEC,0x5C,0x56,0x8B,0xF1,0x8B}},
        {0x00837830,{0x53,0x55,0x8B,0x6C,0x24,0x0C,0x56,0x57,0x8B,0xF9,0x85,0xED,0x74,0x16,0x6A,0x00}},
        {0x00495780,{0x56,0x8B,0xF1,0x83,0x3E,0x00,0x75,0x0B,0x56,0xE8,0xB2,0xE5,0x06,0x00,0x83,0xC4}},
        {0x004957E0,{0x8B,0x44,0x24,0x04,0x8B,0x09,0x50,0xE8,0xA4,0x2A,0xF8,0xFF,0xC2,0x08,0x00,0xCC}},
    };
    checked = 1;
    for (const auto& signature : required) {
        unsigned char bytes[16]; SIZE_T read = 0;
        if (!ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<const void*>(signature.address),bytes,sizeof(bytes),&read) ||
            read != sizeof(bytes) || std::memcmp(bytes,signature.bytes,sizeof(bytes)) != 0) {
            checked = 0;
            LOG_ERR("SpatialRaycast: signature mismatch at %08X",static_cast<unsigned>(signature.address));
        }
    }
    return checked == 1;
}

// Same current-level and Havok TLS requirements as NativeBlueprint::Context.
// No cached world pointer survives a level change; all queries run on the real frame.
bool QueryWorld(uintptr_t* world,const char** error) {
    *world=0;
    if (!blueprint::EngineFrameActive()) { *error="query requires the engine frame thread"; return false; }
    if (!blueprint::Ready(error) || !blueprint::IsClient()) return false;
    __try {
        const uintptr_t tlsIndex=U32(0x023979E0), tlsVector=__readfsdword(0x2C);
        if (!tlsVector || tlsIndex>4096) { *error="engine TLS unavailable"; return false; }
        const uintptr_t tls=U32(tlsVector+tlsIndex*4);
        if (!tls || !U32(tls+0x230)) { *error="Havok thread memory unavailable"; return false; }
        const uintptr_t main=U32(0x022E3790);
        const uintptr_t client=main?U32(main+0xB4):0;
        const uintptr_t context=client?U32(client+0xC):0;
        const uintptr_t level=context?U32(context+0x10):0;
        const uintptr_t current=U32(kPhysicsManagerVarVa);
        // No level/world is a legitimate empty result during load/teardown.
        if (!level || !current || *reinterpret_cast<const unsigned char*>(level+0x44)) return true;
        if (U32(level+0xC0)!=current || U32(current)!=0x0213C578 ||
            *reinterpret_cast<const unsigned char*>(current+0x83C)) {
            *error="current entity world unavailable or locked"; return false;
        }
        *world=current; return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) { *error="query context read failed"; return false; }
}

bool CallSpatialQuery(uintptr_t world,const float from[4],const float to[4],uint32_t flags,QueryCollector* out) {
    QueryRange range={0,0x7fffffff}; QueryFilter filter={&range,0};
    __try {
        const uintptr_t spatial=U32(world+0x844);
        if (!spatial) return true;
        reinterpret_cast<SpatialQueryFn>(0x0083B9B0)(reinterpret_cast<void*>(spatial),nullptr,
            "RaycastManager::SpatialRaycast",out,from,to,&filter,flags);
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool FreeQueryCollector(QueryCollector* out) {
    if (!out->begin || out->begin==out->fixed) return true;
    const auto begin=reinterpret_cast<uintptr_t>(out->begin);
    const auto capacity=reinterpret_cast<uintptr_t>(out->capacity);
    if (out->fixed!=out->entries || !out->allocator || capacity<begin || ((capacity-begin)&3)) return false;
    __try {
        reinterpret_cast<QueryFreeFn>(0x004957E0)(&out->allocator,nullptr,out->begin,
            static_cast<unsigned>(capacity-begin));
        out->begin=out->end=out->capacity=nullptr;
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool CaptureRayEntity(uintptr_t pointer,blueprint::Handle* entity,const char* query) {
    // Check the native reflection getter without calling a virtual function on
    // a physics result. WeakAcquire may write to the EntityBusPeer weak slot.
    const uintptr_t type=ClassInfoOf(pointer);
    const bool isEntity=type && eng::IsKindOfName(static_cast<unsigned long>(type),"Entity");
    if (isEntity) {
        const uintptr_t weak=blueprint::CaptureWeakPeer(pointer);
        const bool adopted=weak && blueprint::AdoptWeakPeer(pointer,weak,entity);
        blueprint::DropWeakPeer(weak);
        if (adopted) return true;
    }
    static unsigned logged=0;
    if (logged<8) {
        ++logged;
        char name[96]={};
        LOG_WARN("%s: native hit without Entity handle (%s, %s)",query,
            ClassName(type,name,sizeof(name))?name:"unresolved type",isEntity?"weak lifetime unavailable":"not an Entity");
    }
    return false;
}

bool ReadQueryPeer(uintptr_t address,uintptr_t* peer) {
    __try { *peer=U32(address); return true; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}

const char* ReadSpatialHits(const QueryCollector& collector,std::vector<blueprint::Handle>& handles) {
    const auto first=reinterpret_cast<uintptr_t>(collector.begin);
    const auto end=reinterpret_cast<uintptr_t>(collector.end);
    const auto capacity=reinterpret_cast<uintptr_t>(collector.capacity);
    if (!first || end<first || capacity<end || ((first|end|capacity)&3) || end-first>4*1024*1024 ||
        collector.fixed!=collector.entries)
        return "native collector bounds are invalid";
    for (auto address=first;address!=end;address+=sizeof(uintptr_t)) {
        uintptr_t pointer=0;
        if (!ReadQueryPeer(address,&pointer)) return "native collector entry is unreadable";
        blueprint::Handle entity;
        if (pointer && CaptureRayEntity(pointer,&entity,"SpatialRaycast")) handles.push_back(entity);
    }
    return nullptr;
}

void ReadQueryVector(lua_State* L,int index,float out[4]) {
    if (!lua_istable(L,index) && !lua_isuserdata(L,index)) luaL_argerror(L,index,"Vec3 expected");
    const char* axes[]={"x","y","z"};
    for (int i=0;i<3;++i) {
        lua_getfield(L,index,axes[i]); int numeric=0;
        const double value=lua_tonumberx(L,-1,&numeric); lua_pop(L,1);
        if (!numeric || !std::isfinite(value) || !std::isfinite(static_cast<float>(value)))
            luaL_argerror(L,index,"finite Vec3 expected");
        out[i]=static_cast<float>(value);
    }
    out[3]=0;
}

int NativeSpatialRaycast(lua_State* L) {
    alignas(16) float from[4],to[4]; ReadQueryVector(L,2,from); ReadQueryVector(L,3,to);
    const lua_Integer inputFlags=luaL_checkinteger(L,4);
    if (inputFlags<0 || inputFlags>15) return luaL_argerror(L,4,"SpatialQueryFlags expected");
    const char* error=nullptr; uintptr_t world=0;
    if (!QueryWorld(&world,&error)) return luaL_error(L,"SpatialRaycast: %s",error?error:"client world required");
    if (!QuerySignatures()) return luaL_error(L,"SpatialRaycast: native signature batch unavailable");
    lua_newtable(L);
    if (!world || inputFlags==0 || (from[0]==to[0] && from[1]==to[1] && from[2]==to[2])) return 1;
    {
        QueryCollector collector={}; collector.begin=collector.end=collector.fixed=collector.entries;
        collector.capacity=collector.entries+1024;
        std::vector<blueprint::Handle> handles;
        const bool called=CallSpatialQuery(world,from,to,static_cast<uint32_t>(inputFlags),&collector);
        error=called?ReadSpatialHits(collector,handles):"native query raised an exception";
        if (!FreeQueryCollector(&collector) && !error) error="native collector release failed";
        if (!error) {
            int count=0;
            for (const auto& handle:handles) {
                auto* value=static_cast<blueprint::Handle*>(lua_newuserdatauv(L,sizeof(blueprint::Handle),0));
                *value=handle; luaL_setmetatable(L,"evbots.BlueprintEntity"); lua_rawseti(L,-2,++count);
            }
        }
    }
    return error?luaL_error(L,"SpatialRaycast: %s",error):1;
}

// VU 10B48A99..10B48B2B: active Level+BC is PhysicsManager; +40
// is IPhysicsRayCaster. Virtual slot 0 is bool(this,label,from,to,hit,flags,exclude).
// This is deliberately not the GameWorld CollisionRaycast overload (mode 1).
// RayCastHit allocation 10B40B1D..10B40BAD is 0x40 bytes, aligned to 16.
using PhysicsRayFn=bool(__fastcall*)(void*,void*,const char*,const float*,const float*,Hit*,uint32_t,void*);
bool CallPhysicsRay(uintptr_t world,const float from[4],const float to[4],uint32_t flags,Hit* hit,bool* found,const char** error) {
    *found=false;
    __try {
        const uintptr_t main=U32(0x022E3790),client=main?U32(main+0xB4):0,context=client?U32(client+0xC):0;
        const uintptr_t level=context?U32(context+0x10):0;
        if (!level || U32(level+0xC0)!=world) { *error="level changed before physics query"; return false; }
        const uintptr_t physics=U32(level+0xBC),caster=physics?U32(physics+0x40):0;
        if (!caster) return true;
        const uintptr_t table=U32(caster),fn=table?U32(table):0;
        MEMORY_BASIC_INFORMATION code={},vtable={};
        const uintptr_t module=reinterpret_cast<uintptr_t>(GetModuleHandleA("bf3.exe"));
        if (!fn || !VirtualQuery(reinterpret_cast<void*>(fn),&code,sizeof(code)) ||
            !VirtualQuery(reinterpret_cast<void*>(table),&vtable,sizeof(vtable)) ||
            code.State!=MEM_COMMIT || code.Type!=MEM_IMAGE || reinterpret_cast<uintptr_t>(code.AllocationBase)!=module ||
            !(code.Protect&(PAGE_EXECUTE|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY)) ||
            (code.Protect&(PAGE_GUARD|PAGE_NOACCESS)) || vtable.State!=MEM_COMMIT || vtable.Type!=MEM_IMAGE ||
            reinterpret_cast<uintptr_t>(vtable.AllocationBase)!=module || (vtable.Protect&(PAGE_GUARD|PAGE_NOACCESS))) {
            *error="physics raycaster interface is not in the native client image"; return false;
        }
        *found=reinterpret_cast<PhysicsRayFn>(fn)(reinterpret_cast<void*>(caster),nullptr,
            "RaycastManager::Raycast",from,to,hit,flags,nullptr);
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) { *error="native physics raycast raised an exception"; return false; }
}

void PushRayVec(lua_State* L,const float v[4]) {
    lua_getglobal(L,"Vec3"); lua_pushnumber(L,v[0]); lua_pushnumber(L,v[1]); lua_pushnumber(L,v[2]); lua_call(L,3,1);
}
int PushRayHit(lua_State* L,const Hit& hit) {
    lua_createtable(L,0,6); PushRayVec(L,hit.pos); lua_setfield(L,-2,"position");
    PushRayVec(L,hit.normal); lua_setfield(L,-2,"normal");
    lua_pushinteger(L,static_cast<int32_t>(hit.rest[0])); lua_setfield(L,-2,"part");
    lua_pushinteger(L,static_cast<int32_t>(hit.rest[1])); lua_setfield(L,-2,"bone");
    // RayCastHit.rigidBody is optional. Terrain/geometry can be a real hit
    // even when no Entity handle can be acquired for the collision body.
    blueprint::Handle body;
    if (hit.rigidBody && CaptureRayEntity(hit.rigidBody,&body,"Raycast")) {
        auto* value=static_cast<blueprint::Handle*>(lua_newuserdatauv(L,sizeof(blueprint::Handle),0));
        *value=body; luaL_setmetatable(L,"evbots.BlueprintEntity"); lua_setfield(L,-2,"rigidBody");
    }
    // Direct PhysicsRayCaster finalizes material to a DC pointer; VU getter
    // 10A83CB6 passes hit+24 directly to the DataContainer wrapper.
    if (hit.material) { EVBotsPushLiveInstance(L,hit.material); lua_setfield(L,-2,"material"); }
    return 1;
}

int NativeRaycast(lua_State* L) {
    alignas(16) float from[4],to[4]; ReadQueryVector(L,2,from); ReadQueryVector(L,3,to);
    const lua_Integer inputFlags=luaL_checkinteger(L,4);
    if (inputFlags<0 || static_cast<unsigned long long>(inputFlags)>0xffffffffULL) return luaL_argerror(L,4,"RayCastFlags expected");
    const char* error=nullptr; uintptr_t world=0;
    if (!QueryWorld(&world,&error)) return luaL_error(L,"Raycast: %s",error?error:"client world required");
    if (!world || (from[0]==to[0] && from[1]==to[1] && from[2]==to[2])) { lua_pushnil(L); return 1; }
    alignas(16) Hit hit={}; bool found=false;
    if (!CallPhysicsRay(world,from,to,static_cast<uint32_t>(inputFlags),&hit,&found,&error))
        return luaL_error(L,"Raycast: %s",error?error:"native query failed");
    if (found) return PushRayHit(L,hit);
    lua_pushnil(L);
    return 1;
}

// ★ Сборка 234 (реестр В4): «вид» камеры, как у VU. Факт (26.10, zbf3 + veniceext):
//   - «вид» VU — рендер игры GameRenderer: глобал [0x02384D78] (пишет 00F7382A после создания 00FB7E30, 0xEB0 байт),
//     vtable 0213E9C8 (конструктор 00FB4D20). Движок сам пересчитывает его матрицы: 00FB2ED0 (ecx = рендер) —
//     `test byte [this+0xD0],1` → 006C3A90, матрица `this+0x2D0`;
//   - VU (10ADC0B3: `lea ecx,[вид+0x50]; call [18E7501C]` = 006C3A90) пересчитывает матрицы перед WorldToScreen и
//     GetCameraTransform; трансформ камеры — вид+0xDE0…0xE1F (10ADB530), view·proj — вид+0x410…0x44F (10ADC0E2).
// Зовём из потока движка (Lua клиента — кадр). Пролог 006C3A90 и vtable рендера сверяются перед вызовом.
constexpr uintptr_t kGameRendererVarVa = 0x02384D78;
constexpr uintptr_t kGameRendererVtable = 0x0213E9C8;
constexpr uintptr_t kUpdateMatricesVa = 0x006C3A90;
constexpr unsigned char kUpdateMatricesPrologue[6] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0};
using UpdateMatricesFn = void(__fastcall*)(void* matrices, void* edx);

int g_viewPrologueOk = -1;

bool ReadView(float camera[16], float viewProj[16]) {
    __try {
        const uintptr_t r = U32(kGameRendererVarVa);
        if (r == 0 || U32(r) != kGameRendererVtable) return false;
        reinterpret_cast<UpdateMatricesFn>(kUpdateMatricesVa)(reinterpret_cast<void*>(r + 0x50), nullptr);
        std::memcpy(camera, reinterpret_cast<const void*>(r + 0xDE0), 16 * sizeof(float));
        std::memcpy(viewProj, reinterpret_cast<const void*>(r + 0x410), 16 * sizeof(float));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

/// __cu_view() -> таблица {камера 16 float, view·proj 16 float} | nil (нет рендера, пролог не тот, не числа).
int l_cu_view(lua_State* L) {
    if (g_viewPrologueOk < 0) {
        unsigned char b[sizeof(kUpdateMatricesPrologue)] = {};
        SIZE_T n = 0;
        g_viewPrologueOk = ::ReadProcessMemory(::GetCurrentProcess(), reinterpret_cast<LPCVOID>(kUpdateMatricesVa), b,
                                               sizeof(b), &n) &&
                                   n == sizeof(b) && std::memcmp(b, kUpdateMatricesPrologue, sizeof(b)) == 0
                               ? 1
                               : 0;
        if (g_viewPrologueOk == 1) {
            LOG_INFO("EVBots: клиент: ★ вид камеры — пролог 0x%08X совпал, рендер [0x%08X] (как вид VU)",
                     static_cast<unsigned>(kUpdateMatricesVa), static_cast<unsigned>(kGameRendererVarVa));
        } else {
            LOG_ERR("EVBots: клиент: вид камеры — пролог 0x%08X не совпал, камера и WorldToScreen остаются заглушками",
                    static_cast<unsigned>(kUpdateMatricesVa));
        }
    }
    float cam[16], vp[16];
    if (g_viewPrologueOk != 1 || !ReadView(cam, vp)) {
        lua_pushnil(L);
        return 1;
    }
    for (int i = 0; i < 16; ++i) {
        if (!(cam[i] == cam[i]) || !(vp[i] == vp[i])) {  // NaN — нет вида
            lua_pushnil(L);
            return 1;
        }
    }
    lua_createtable(L, 32, 0);
    for (int i = 0; i < 16; ++i) {
        lua_pushnumber(L, cam[i]);
        lua_rawseti(L, -2, i + 1);
    }
    for (int i = 0; i < 16; ++i) {
        lua_pushnumber(L, vp[i]);
        lua_rawseti(L, -2, 17 + i);
    }
    return 1;
}

// ★ Сборка 234 (реестр В6): DebugRenderer как у VU — отладочный рендер движка. Факт (26.10, zbf3 + veniceext):
//   геттер 004B2EA0 (слот VU 18E74E90: `mov ecx,[0x22FB188]; test ecx,ecx` …) -> рендер (0 — нет);
//   все функции ниже — thiscall, ecx = рендер (VU 10DB0C08 `mov ecx,esi`):
//   DrawText2D 004B7610(int x, int y, const char* text, u32 цвет, float масштаб), ret 0x14 (VU 10DB0BC0);
//   DrawSphere 004B77E0(const Vec4* pos, float радиус, u32 цвет, bool renderLines, bool smallSize), ret 0x14 (VU 10DB0B70);
//   DrawLine2D 004B9980(const float* from[2], const float* to[2], u32 цвет) (VU 10DB03F0);
//   вершины 004B9850(тип, число) -> буфер вершин по 0x20 байт (+0 позиция xyz, +0xC цвет): линия — (1, 2)
//   (VU 10DB0480), треугольник — (0, 3) (VU 10DB0C20).
//   Цвет — VU 10DB16B0: каждый канал Vec4 зажат в 0..1, ×255, байты r, g, b, a.
// Отрисовка игры рисует накопленное, только когда у рендера игры флаг [рендер+0xE5C] != 0 (факт: 00FB9C81 `cmp byte
// [esi+0xE5C],0; je` мимо 004B2EA0/004B98E0; откуда флаг — из настроек Render +0x85, не проверено) — пишем в журнал раз.
// Не проверено: геттер 004B2EA0 → 004B05D0 отдаёт объект ПОТОКА (TLS, [esi+0x224]); если Lua (кадр 0x40A470) и
// отрисовка (00FB97A0) в разных потоках, команды до экрана не дойдут — поток Lua пишем в журнал. VU ставит свои хуки на
// отладочный рендер (004AD920…004FCD00) — зачем, не разобрано.
constexpr uintptr_t kDebugRendererGetVa = 0x004B2EA0;
constexpr unsigned char kDebugRendererGetCode[8] = {0x8B, 0x0D, 0x88, 0xB1, 0x2F, 0x02, 0x85, 0xC9};
constexpr uintptr_t kDrawText2DVa = 0x004B7610;
constexpr uintptr_t kDrawSphereVa = 0x004B77E0;
constexpr uintptr_t kDrawLine2DVa = 0x004B9980;
constexpr uintptr_t kDebugVerticesVa = 0x004B9850;
using DebugGetFn = void* (*)();
using DrawText2DFn = void(__fastcall*)(void* r, void* edx, int x, int y, const char* text, uint32_t color, float scale);
using DrawSphereFn = void(__fastcall*)(void* r, void* edx, const float* pos, float radius, uint32_t color, bool lines,
                                       bool small);
using DrawLine2DFn = void(__fastcall*)(void* r, void* edx, const float* from, const float* to, uint32_t color);
using DebugVerticesFn = float*(__fastcall*)(void* r, void* edx, int type, unsigned count);

int g_drCodeOk = -1;
bool g_drStateLogged = false;

struct CodeCheck {
    uintptr_t va;
    unsigned char bytes[6];
};
// Начала функций по zbf3 (дизассемблер 26.10): всё или ничего — одно не совпало, отрисовку не зовём (правило 18).
constexpr CodeCheck kDrawCode[] = {
    {kDrawText2DVa, {0x56, 0x57, 0x8B, 0x7C, 0x24, 0x14}},    // push esi; push edi; mov edi,[esp+0x14]
    {kDrawSphereVa, {0x6A, 0x40, 0x6A, 0x04, 0xE8, 0x57}},    // push 0x40; push 4; call 004B7540
    {kDrawLine2DVa, {0x6A, 0x02, 0x6A, 0x03, 0xE8, 0xC7}},    // push 2; push 3; call 004B9850
    {kDebugVerticesVa, {0x53, 0x8B, 0x5C, 0x24, 0x0C, 0x56}}, // push ebx; mov ebx,[esp+0xC]; push esi
};

bool CodeMatches(uintptr_t va, const unsigned char* want, size_t len) {
    unsigned char b[8] = {};
    SIZE_T n = 0;
    return len <= sizeof(b) &&
           ::ReadProcessMemory(::GetCurrentProcess(), reinterpret_cast<LPCVOID>(va), b, len, &n) && n == len &&
           std::memcmp(b, want, len) == 0;
}

void* DebugRenderer() {
    if (g_drCodeOk < 0) {
        g_drCodeOk = CodeMatches(kDebugRendererGetVa, kDebugRendererGetCode, sizeof(kDebugRendererGetCode)) ? 1 : 0;
        uintptr_t bad = g_drCodeOk == 1 ? 0 : kDebugRendererGetVa;
        for (const CodeCheck& c : kDrawCode) {
            if (bad == 0 && !CodeMatches(c.va, c.bytes, sizeof(c.bytes))) bad = c.va;
        }
        if (bad != 0) g_drCodeOk = 0;
        if (g_drCodeOk == 1) {
            LOG_INFO("EVBots: клиент: ★ DebugRenderer — код 5 функций совпал с zbf3.exe, поток Lua %lu",
                     ::GetCurrentThreadId());
        } else {
            LOG_ERR("EVBots: клиент: DebugRenderer — код 0x%08X не совпал, отрисовка остаётся заглушкой",
                    static_cast<unsigned>(bad));
        }
    }
    if (g_drCodeOk != 1) return nullptr;
    __try {
        void* r = reinterpret_cast<DebugGetFn>(kDebugRendererGetVa)();
        if (!g_drStateLogged) {
            g_drStateLogged = true;
            const uintptr_t gr = U32(kGameRendererVarVa);
            const int flag = (gr != 0 && U32(gr) == kGameRendererVtable) ? *reinterpret_cast<const uint8_t*>(gr + 0xE5C) : -1;
            LOG_INFO("EVBots: клиент: ★ DebugRenderer движка: рендер %p, флаг отрисовки [рендер игры+0xE5C] = %d", r, flag);
        }
        return r;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

uint32_t PackColor(lua_State* L, int idx) {
    uint32_t c = 0;
    for (int i = 0; i < 4; ++i) {
        float v = static_cast<float>(luaL_optnumber(L, idx + i, 1.0));
        if (!(v <= 1.0f)) v = 1.0f;  // и NaN
        if (!(v >= 0.0f)) v = 0.0f;
        c |= static_cast<uint32_t>(static_cast<int>(v * 255.0f) & 0xFF) << (8 * i);
    }
    return c;
}

float Num(lua_State* L, int idx) {
    const float v = static_cast<float>(luaL_checknumber(L, idx));
    return v == v ? v : 0.0f;
}

bool DrawText2DSafe(void* r, int x, int y, const char* t, uint32_t c, float s) {
    __try {
        reinterpret_cast<DrawText2DFn>(kDrawText2DVa)(r, nullptr, x, y, t, c, s);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool DrawSphereSafe(void* r, const float* p, float rad, uint32_t c, bool lines, bool small) {
    __try {
        reinterpret_cast<DrawSphereFn>(kDrawSphereVa)(r, nullptr, p, rad, c, lines, small);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool DrawLine2DSafe(void* r, const float* a, const float* b, uint32_t c) {
    __try {
        reinterpret_cast<DrawLine2DFn>(kDrawLine2DVa)(r, nullptr, a, b, c);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool DrawVerticesSafe(void* r, int type, unsigned count, const float (*pos)[3], const uint32_t* colors) {
    __try {
        float* v = reinterpret_cast<DebugVerticesFn>(kDebugVerticesVa)(r, nullptr, type, count);
        if (v == nullptr) return false;
        for (unsigned i = 0; i < count; ++i) {
            float* d = v + i * 8;  // 0x20 байт
            d[0] = pos[i][0];
            d[1] = pos[i][1];
            d[2] = pos[i][2];
            *reinterpret_cast<uint32_t*>(d + 3) = colors[i];
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

/// __dr_text(x, y, text, r, g, b, a, scale) -> bool
int l_dr_text(lua_State* L) {
    void* r = DebugRenderer();
    const char* t = luaL_checkstring(L, 3);
    bool ok = r != nullptr && DrawText2DSafe(r, static_cast<int>(Num(L, 1)), static_cast<int>(Num(L, 2)), t,
                                             PackColor(L, 4), static_cast<float>(luaL_optnumber(L, 8, 1.0)));
    lua_pushboolean(L, ok);
    return 1;
}

/// __dr_sphere(x, y, z, radius, r, g, b, a, renderLines, smallSizeSegmentDecrease) -> bool
int l_dr_sphere(lua_State* L) {
    void* r = DebugRenderer();
    alignas(16) float p[4] = {Num(L, 1), Num(L, 2), Num(L, 3), 0.0f};
    bool ok = r != nullptr && DrawSphereSafe(r, p, Num(L, 4), PackColor(L, 5), lua_toboolean(L, 9) != 0,
                                             lua_toboolean(L, 10) != 0);
    lua_pushboolean(L, ok);
    return 1;
}

/// __dr_line2d(x1, y1, x2, y2, r, g, b, a) -> bool
int l_dr_line2d(lua_State* L) {
    void* r = DebugRenderer();
    const float a[2] = {Num(L, 1), Num(L, 2)}, b[2] = {Num(L, 3), Num(L, 4)};
    bool ok = r != nullptr && DrawLine2DSafe(r, a, b, PackColor(L, 5));
    lua_pushboolean(L, ok);
    return 1;
}

/// __dr_verts(тип, x, y, z, r, g, b, a, x, y, z, r, g, b, a, …) -> bool; тип 1 — линия (2 вершины), 0 — треугольник (3).
int l_dr_verts(lua_State* L) {
    const int type = static_cast<int>(luaL_checkinteger(L, 1));
    const unsigned count = type == 1 ? 2u : (type == 0 ? 3u : 0u);
    if (count == 0) return luaL_error(L, "__dr_verts: тип %d не поддержан", type);
    float pos[3][3];
    uint32_t col[3];
    for (unsigned i = 0; i < count; ++i) {
        const int base = 2 + static_cast<int>(i) * 7;
        pos[i][0] = Num(L, base);
        pos[i][1] = Num(L, base + 1);
        pos[i][2] = Num(L, base + 2);
        col[i] = PackColor(L, base + 3);
    }
    void* r = DebugRenderer();
    lua_pushboolean(L, r != nullptr && DrawVerticesSafe(r, type, count, pos, col));
    return 1;
}

}  // namespace

bool RegisterClientRaycast(lua_State* L) {
    lua_pushcfunction(L, l_dr_text);
    lua_setglobal(L, "__dr_text");
    lua_pushcfunction(L, l_dr_sphere);
    lua_setglobal(L, "__dr_sphere");
    lua_pushcfunction(L, l_dr_line2d);
    lua_setglobal(L, "__dr_line2d");
    lua_pushcfunction(L, l_dr_verts);
    lua_setglobal(L, "__dr_verts");
    lua_pushcfunction(L, l_clr_collision);
    lua_setglobal(L, "__clr_collision");
    lua_pushcfunction(L, l_clr_types);
    lua_setglobal(L, "__clr_types");
    lua_pushcfunction(L, l_cu_view);
    lua_setglobal(L, "__cu_view");
    return true;
}

bool InstallClientSpatialRaycast(lua_State* L) {
    // Must run after RegisterVextClient and RegisterVextBlueprintEntities.
    luaL_getmetatable(L,"evbots.BlueprintEntity"); const bool entityReady=lua_istable(L,-1); lua_pop(L,1);
    lua_getglobal(L,"RaycastManager");
    if (!entityReady || !lua_istable(L,-1)) { lua_pop(L,1); return false; }
    lua_pushcfunction(L,NativeSpatialRaycast); lua_setfield(L,-2,"SpatialRaycast"); lua_pop(L,1);
    return true;
}

bool InstallClientPhysicsRaycast(lua_State* L) {
    luaL_getmetatable(L,"evbots.BlueprintEntity"); const bool entityReady=lua_istable(L,-1); lua_pop(L,1);
    lua_getglobal(L,"RaycastManager");
    if (!entityReady || !lua_istable(L,-1)) { lua_pop(L,1); return false; }
    lua_pushcfunction(L,NativeRaycast); lua_setfield(L,-2,"Raycast"); lua_pop(L,1);
    return true;
}

}  // namespace evbots
