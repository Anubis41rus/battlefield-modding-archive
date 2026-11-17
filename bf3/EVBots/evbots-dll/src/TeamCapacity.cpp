// TeamInfo на 128 команд. Основание: vu-core 10199290/D0/F0;
// статическая сверка server f39f7623 и retail client 8201c5e1, 17.11.2026.
// Полный разбор: D:/Games-cache/re/royale-entry-20261117/team-capacity-report.md.
// Установка исключительно ДО первого Level: переходный layout недопустим.
#include "TeamCapacity.h"
#include "InlineHook.h"
#include "EVBotsLog.h"
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace evbots {
namespace {
static_assert(sizeof(void*) == 4, "TeamCapacity requires x86");
struct TeamInfo128 {
    void* teams[128];
    unsigned count;
};
static_assert(sizeof(TeamInfo128) == 0x204, "native allocation size");
static_assert(offsetof(TeamInfo128, count) == 0x200, "VU count offset");
bool gInstalled = false;
struct TeamSize { unsigned playerCount, squadSize; };
static_assert(sizeof(TeamSize)==8, "GameModeTeamSize native stride");
// Native fixed_vector global originally has only17 inline entries. All native
// accesses use its begin/end pointers; the DLL is pinned before redirecting them.
TeamSize gTeamSizes[128] = {};
using ServerAllocate = void* (__thiscall*)(void*, unsigned, unsigned, unsigned);
using ClientAllocate = void* (__thiscall*)(void*, unsigned, unsigned);
ServerAllocate gServerAllocate = nullptr;
ClientAllocate gClientAllocate = nullptr;

TeamInfo128* __fastcall Construct(TeamInfo128* self, void*) {
    std::memset(self, 0, sizeof(*self));
    return self;
}
void* __fastcall Get(TeamInfo128* self, void*, int id) {
    if (static_cast<unsigned>(id) >= 128) {
        LOG_ERR("TeamCapacity: invalid get id=%d (range 0..127)", id);
        return nullptr;
    }
    return self->teams[id];
}
void __fastcall Set(TeamInfo128* self, void*, void* data, int id) {
    if (!data || static_cast<unsigned>(id) >= 128) {
        LOG_ERR("TeamCapacity: rejected registration id=%d data=%p (range 0..127)", id, data);
        return;
    }
    if (self->teams[id]) {
        LOG_ERR("TeamCapacity: duplicate id=%d old=%p new=%p; retained first registration", id, self->teams[id], data);
        return;
    }
    self->teams[id] = data;
    ++self->count; // VU: число зарегистрированных команд, не max(id)+1.
    if (id == 0 || id == 17 || id == 126 || id == 127)
        LOG_INFO("TeamCapacity: register id=%d count=%u capacity=128", id, self->count);
}
unsigned __fastcall Count(TeamInfo128* self, void*) { return self->count; }

void* __fastcall AllocateServer(void* allocator, void*, unsigned size, unsigned align, unsigned tag) {
    if (size != 0x48 || align != 0x10 || tag != 0) {
        LOG_ERR("TeamCapacity: unexpected server allocation %u/%u/%u", size, align, tag);
        return nullptr;
    }
    void* result = gServerAllocate(allocator, sizeof(TeamInfo128), align, tag);
    LOG_INFO("TeamCapacity: server allocation size=516 align=16 pointer=%p", result);
    return result;
}
void* __fastcall AllocateClient(void* allocator, void*, unsigned size, unsigned align) {
    if (size != 0x48 || align != 0x10) {
        LOG_ERR("TeamCapacity: unexpected client allocation %u/%u", size, align);
        return nullptr;
    }
    void* result = gClientAllocate(allocator, sizeof(TeamInfo128), align);
    LOG_INFO("TeamCapacity: client allocation size=516 align=16 pointer=%p", result);
    return result;
}

struct Patch {
    const char* name;
    uintptr_t va;
    unsigned size;
    unsigned char before[16];
    void* destination;
    bool call;
    unsigned char after[16];
};
bool EqualBytes(const void* address, const void* expected, unsigned size) {
    __try { return std::memcmp(address, expected, size) == 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool NoExistingLevel(bool server, uintptr_t delta) {
    __try {
        const uintptr_t main = *reinterpret_cast<uintptr_t*>((server ? 0x2877210u : 0x22E3790u) + delta);
        if (!main) return true;
        if (server) {
            const uintptr_t host = *reinterpret_cast<uintptr_t*>(main + 0xD0);
            if (!host) return true;
            const uintptr_t current = *reinterpret_cast<uintptr_t*>(host + 0x74);
            const uintptr_t loading = *reinterpret_cast<uintptr_t*>(host + 0x7C);
            if (!current && !loading) return true;
            LOG_ERR("TeamCapacity: refused late install server currentLevel=%p loadingLevel=%p", reinterpret_cast<void*>(current), reinterpret_cast<void*>(loading));
        } else {
            const uintptr_t client = *reinterpret_cast<uintptr_t*>(main + 0xB4);
            if (!client) return true;
            const uintptr_t context = *reinterpret_cast<uintptr_t*>(client + 0xC);
            if (!context) return true;
            const uintptr_t level = *reinterpret_cast<uintptr_t*>(context + 0x10);
            if (!level) return true;
            LOG_ERR("TeamCapacity: refused late install client level=%p", reinterpret_cast<void*>(level));
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG_ERR("TeamCapacity: cannot prove absence of Level; startup chain unreadable");
    }
    return false;
}
}

bool TeamCapacityInstalled() { return gInstalled; }

bool InstallTeamCapacity(bool server) {
    if (gInstalled) return true;
    char flag[16] = {};
    GetEnvironmentVariableA("EVBOTS_TEAM_CAPACITY", flag, sizeof(flag));
    if (flag[0] == '0') {
        LOG_WARN("TeamCapacity: disabled by EVBOTS_TEAM_CAPACITY=0; Royale 126 teams requires expansion");
        return false;
    }
    if (!CoreHooksAvailable()) return false;
    const uintptr_t module = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const uintptr_t delta = module - 0x400000;
    if (!NoExistingLevel(server, delta)) return false;
    // Все границы инструкций ниже проверены Capstone по exe, не по дампу памяти.
    Patch patches[] = {
        {"TeamCapacity.ctor", server ? 0x885370u : 0x11D5960u, 12,
            {0x8B,0xC1,0x56,0x8D,0x70,0x44,0xC7,0x06,0,0,0,0}, reinterpret_cast<void*>(&Construct), false, {}},
        {"TeamCapacity.set", server ? 0x880DD0u : 0x11D06A0u, server ? 14u : 8u,
            {}, reinterpret_cast<void*>(&Set), false, {}},
        {"TeamCapacity.get", server ? 0x880D30u : 0x11D0690u, server ? 14u : 10u,
            {}, reinterpret_cast<void*>(&Get), false, {}},
        {"TeamCapacity.count", server ? 0x87AFD0u : 0x11CC4B0u, 8,
            {0x8B,0x41,0x44,0xC3,0xCC,0xCC,0xCC,0xCC}, reinterpret_cast<void*>(&Count), false, {}},
        {"TeamCapacity.allocate", server ? 0x852000u : 0x119B7BFu, 5,
            {}, server ? reinterpret_cast<void*>(&AllocateServer) : reinterpret_cast<void*>(&AllocateClient), true, {}},
        {"TeamCapacity.teamSizes", server ? 0x285BB38u : 0u, 16,
            {}, nullptr, false, {}},
        // Server selector1175A60: enlarged stack accommodates the team-indexed
        // player-count scratch. Argument locations and both exits move equally.
        // The log buffer overlaps scratch only on a mutually exclusive branch.
        {"TeamCapacity.selectorFrame",server?0x1175A60u:0u,6,
            {0x81,0xEC,0x78,1,0,0},nullptr,false,{0x81,0xEC,0x38,3,0,0}},
        {"TeamCapacity.selectorArg1",server?0x1175A6Bu:0u,7,
            {0x8B,0x8C,0x24,0x7C,1,0,0},nullptr,false,{0x8B,0x8C,0x24,0x3C,3,0,0}},
        {"TeamCapacity.selectorArg2",server?0x1175A8Au:0u,8,
            {0x80,0xBC,0x24,0x90,1,0,0,0},nullptr,false,{0x80,0xBC,0x24,0x50,3,0,0,0}},
        {"TeamCapacity.selectorArg3a",server?0x1175AEFu:0u,7,
            {0x8B,0x9C,0x24,0x94,1,0,0},nullptr,false,{0x8B,0x9C,0x24,0x54,3,0,0}},
        {"TeamCapacity.selectorExit1",server?0x1175C88u:0u,6,
            {0x81,0xC4,0x78,1,0,0},nullptr,false,{0x81,0xC4,0x38,3,0,0}},
        {"TeamCapacity.selectorArg3b",server?0x1175D28u:0u,7,
            {0x8B,0x9C,0x24,0x94,1,0,0},nullptr,false,{0x8B,0x9C,0x24,0x54,3,0,0}},
        {"TeamCapacity.selectorExit2",server?0x1175D8Cu:0u,6,
            {0x81,0xC4,0x78,1,0,0},nullptr,false,{0x81,0xC4,0x38,3,0,0}}
    };
    const unsigned char serverSet[] = {0x81,0xEC,0x30,1,0,0,0x55,0x8B,0xAC,0x24,0x38,1,0,0};
    const unsigned char serverGet[] = {0x81,0xEC,0x30,1,0,0,0x56,0x8B,0xB4,0x24,0x38,1,0,0};
    const unsigned char clientSet[] = {0x8B,0x44,0x24,8,0x8B,0x54,0x24,4};
    const unsigned char clientGet[] = {0x8B,0x44,0x24,4,0x8B,0x04,0x81,0xC2,4,0};
    std::memcpy(patches[1].before, server ? serverSet : clientSet, patches[1].size);
    std::memcpy(patches[2].before, server ? serverGet : clientGet, patches[2].size);
    const uintptr_t allocator = (server ? 0x430700u : 0x41BE10u) + delta;
    gServerAllocate = reinterpret_cast<ServerAllocate>(allocator);
    gClientAllocate = reinterpret_cast<ClientAllocate>(allocator);
    const int32_t originalCall = static_cast<int32_t>(allocator - (patches[4].va + delta + 5));
    patches[4].before[0] = 0xE8;
    std::memcpy(patches[4].before + 1, &originalCall, 4);
    if (server) {
        const uintptr_t original[] = {0x285BB48u+delta,0x285BB48u+delta,0x285BBD0u+delta,0};
        const uintptr_t expanded[] = {reinterpret_cast<uintptr_t>(gTeamSizes),reinterpret_cast<uintptr_t>(gTeamSizes),
            reinterpret_cast<uintptr_t>(gTeamSizes+128),0};
        std::memcpy(patches[5].before,original,sizeof(original));
        std::memcpy(patches[5].after,expanded,sizeof(expanded));
    }
    // Проверяем также аргументы allocation, а не только адрес CALL.
    const unsigned char serverArguments[] = {0x6A,0x10,0x89,0x7E,0x60,0x6A,0x48,0x8B,0xCB,0x89,0x7E,0x64};
    const unsigned char clientArguments[] = {0x6A,0x10,0x89,0x7E,0x5C,0x6A,0x48,0x8B,0xCB,0x89,0x7E,0x60};
    bool verified = EqualBytes(reinterpret_cast<void*>(patches[4].va + delta - 12),
        server ? serverArguments : clientArguments, 12);
    LOG_INFO("TeamCapacity: verify allocation arguments %s", verified ? "PASS" : "FAIL");
    for (auto& patch : patches) {
        if (!patch.va) continue; // server-only fixed_vector
        patch.va += delta;
        const bool ok = EqualBytes(reinterpret_cast<void*>(patch.va), patch.before, patch.size);
        LOG_INFO("TeamCapacity: verify %s at=%p len=%u %s", patch.name,
            reinterpret_cast<void*>(patch.va), patch.size, ok ? "PASS" : "FAIL");
        verified = verified && ok;
        if (patch.destination) {
            std::memset(patch.after, 0x90, patch.size);
            patch.after[0] = patch.call ? 0xE8 : 0xE9;
            const int32_t jump = static_cast<int32_t>(reinterpret_cast<uintptr_t>(patch.destination) - (patch.va + 5));
            std::memcpy(patch.after + 1, &jump, 4);
        }
    }
    if (!verified) {
        LOG_ERR("TeamCapacity: no patches written; executable fingerprint mismatch");
        return false;
    }
    // Byte patches остаются вне HookRegistry. Переходы в выгруженную DLL запрещены.
    HMODULE pinned = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(&InstallTeamCapacity), &pinned)) {
        LOG_ERR("TeamCapacity: module pin failed error=%lu; no patches written", GetLastError());
        return false;
    }
    if (!NoExistingLevel(server, delta)) return false;
    unsigned installed = 0;
    for (auto& patch : patches) {
        if (!patch.va) continue;
        if (!PatchBytes(reinterpret_cast<void*>(patch.va), patch.before, patch.after, patch.size, patch.name)) {
            LOG_ERR("TeamCapacity: failed %s, rolling back %u sites", patch.name, installed);
            bool restored = true;
            while (installed) {
                auto& prior = patches[--installed];
                const bool ok = PatchBytes(reinterpret_cast<void*>(prior.va), prior.after, prior.before, prior.size, "TeamCapacity.rollback");
                LOG_INFO("TeamCapacity: rollback %s %s", prior.name, ok ? "PASS" : "FAIL");
                restored = restored && ok;
            }
            // Startup must stop on false; never create Level with partial layout.
            if (!restored) LOG_ERR("TeamCapacity: FATAL rollback incomplete; engine startup must abort");
            return false;
        }
        ++installed;
        LOG_INFO("TeamCapacity: installed %s", patch.name);
    }
    gInstalled = true;
    LOG_INFO("TeamCapacity: installed role=%s capacity=128 size=516 countOffset=512 native allocator/free retained", server ? "server" : "client");
    if (server) LOG_INFO("TeamCapacity: TeamSize fixed_vector capacity=128 begin=%p bytes=1024",gTeamSizes);
    return true;
}

bool TestTeamCapacityMemory() {
    struct Fixture { unsigned guardBefore; TeamInfo128 table; unsigned guardAfter; } fixture;
    std::memset(&fixture, 0xA5, sizeof(fixture));
    Construct(&fixture.table, nullptr);
    for (int id = 127; id >= 0; --id)
        Set(&fixture.table, nullptr, reinterpret_cast<void*>(static_cast<uintptr_t>(0x1000 + id * 16)), id);
    bool ok = Count(&fixture.table, nullptr) == 128;
    for (int id = 0; id < 128; ++id)
        ok = ok && Get(&fixture.table, nullptr, id) == reinterpret_cast<void*>(static_cast<uintptr_t>(0x1000 + id * 16));
    // Реальные граничные записи не должны менять count/указатели/guard.
    Set(&fixture.table, nullptr, reinterpret_cast<void*>(0xFFFF), 128);
    Set(&fixture.table, nullptr, reinterpret_cast<void*>(0xFFFF), -1);
    Set(&fixture.table, nullptr, reinterpret_cast<void*>(0xFFFF), 17);
    ok = ok && Count(&fixture.table, nullptr) == 128 && Get(&fixture.table, nullptr, 17) == reinterpret_cast<void*>(0x1110);
    ok = ok && !Get(&fixture.table, nullptr, 128) && !Get(&fixture.table, nullptr, -1);
    ok = ok && fixture.guardBefore == 0xA5A5A5A5 && fixture.guardAfter == 0xA5A5A5A5;
    Construct(&fixture.table, nullptr);
    ok = ok && Count(&fixture.table, nullptr) == 0;
    for (int id = 0; id < 128; ++id) ok = ok && !Get(&fixture.table, nullptr, id);
    LOG_INFO("TeamCapacity: memory fixture %s (128 teams, reverse order, bounds, duplicate, guards, reset)", ok ? "PASS" : "FAIL");
    return ok;
}
}
