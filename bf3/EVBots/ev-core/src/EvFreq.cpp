// EvFreq.cpp — частота симуляции и сети 30 / 60 / 120 Гц, как у VU (vu.exe -high60 / -high120, RCON vu.FrequencyMode).
// Сборка 238, 26.10.2026. Разбор vu-core.dll (re\vu\pyghidra_mcp, копия проекта): строки -high60/-high120 разбирает
// FUN_1012a040, режим DAT_108cdb84 (0/1/2) пишет установщик FUN_105363a0 — он же правит код bf3.exe (непосредственные значения
// и адреса констант 30.0 / 1/30 / 0.1 / 0.01 / 0.04 в инструкциях); установщик перехватов FUN_10186ef0 ставит:
//   0x01195870 игровой таймер (GameTimeSettings +0x20 MaxSimFps = частота, +0x28 ClampTicks = 2/4/8, частота — аргументом);
//   0x0118D060 таймер с фиксированным шагом (частота аргументом);
//   0x00FB46D0 соединение (частота / половина / полоса 32000·k байт/с), 0x00A53A20 сетевой объект (частота / 10·k / полоса);
//   0x005BB1F0 шаг времени (60/120: 1/60 -> 1/45, 1/120 -> 1/60 — так у VU).
// У VU сервер — тот же клиентский bf3.exe, поэтому для клиента адреса VU берём как есть (сверка байтов), а для нашего
// выделенного сервера bf3_server_Final.exe каждое место найдено отдельно (шаблоны окрестностей, разбор в MODLOG «Сборка 238»).
// ★ 238-3 (проверка 26.10): у сервера ванильные значения сети другие, чем у клиента VU (сетевой объект: полоса 131072,
// частоты 100.0), поэтому сервер при 30 Гц не трогаем вовсе, а при 60/120 поля сети масштабируем от ванильных (см. ниже).
// Всё — «всё или ничего» по стороне: перехваты, код и константы сверяются до первой записи (правило 18).
// ★ 238-3: поправки физики VU (детуры vu-core 10195AA0 / 10195B20 / 10195B80) — при 60 И 120 Гц (ветки cmp 1 / cmp 2; в 238-2
// ошибочно записано «только 120»): 0x005FFA90, 0x005FFA10, 0x005C5720; двойники сервера — по маскированным байтам функций
// (scratchpad twinfind.py, по одному совпадению): 0x006A2A20, 0x006A29A0, 0x0065F560. 0x015FCE30 (частота аргументом) у VU
// дублирует правку 30.0f у всех 6 вызывающих — у нас покрыто правкой кода; 0x015FC570 у VU — пустой детур (только jmp).
// Не перенесено: эмиттеры частиц (vu-core 101CD770/101CD7A0 — число частиц 48·k и отсев, только графика клиента);
// FUN_10537920(частота) ×2 у VU (назначение не разобрано). EVBOTS_FREQ_PATCH=0 — ничего не трогать.

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>

#include "EvCore.h"

extern "C" int __cdecl EvHookInstall(void* target, void* detour, const char* name, void** trampolineOut,
                                     unsigned* stolenOut, unsigned* instructionsOut);
extern "C" int __cdecl EvHookRemove(void* target);
extern "C" int __cdecl EvPatchBytes(void* address, const void* expect, const void* bytes, unsigned len, const char* name);

namespace {

int g_mode = 0;  // 0 — 30 Гц (regular), 1 — 60 (high60), 2 — 120 (high120)
int g_hz = 30;
float g_hzF = 30.0f;
double g_hzD = 30.0;
float g_invF = 1.0f / 30.0f;
double g_invD = 1.0 / 30.0;
double g_tenthD = 0.1;   // у VU DAT_108aee60: 0.1 / 0.2 / 0.4
double g_centD = 0.01;   // DAT_108aee68: 0.01 / 0.005 / 0.0025
float g_f004 = 0.04f;    // 0x021ACAC0: 0.04 / 0.02 / 0.01
float* g_ptrF004 = &g_f004;
int g_clamp = 2;          // ClampTicks 2 / 4 / 8
float g_netHalf = 15.0f;  // соединение: 15 / 30 / 60
float g_netTen = 10.0f;   // сетевой объект: 10 / 20 / 40
uint32_t g_bw = 32000;    // полоса, байт/с: 32000 / 64000 / 128000
float g_physScale = 1.0f; // поправки физики VU: 1 / 0.5 / 0.25 (у VU константы 0x105E22B8 = 0.5, 0x105E26C8 = 0.25)
std::atomic<int> g_timerHits{0};
bool g_server = false;

void SetMode(int mode) {
    g_mode = mode;
    const int k = mode == 2 ? 4 : (mode == 1 ? 2 : 1);
    g_hz = 30 * k;
    g_hzF = static_cast<float>(g_hz);
    g_hzD = static_cast<double>(g_hz);
    g_invF = 1.0f / g_hzF;
    g_invD = 1.0 / g_hzD;
    g_tenthD = 0.1 * k;
    g_centD = 0.01 / k;
    g_f004 = 0.04f / static_cast<float>(k);
    g_clamp = 2 * k;
    g_netHalf = 15.0f * k;
    g_netTen = 10.0f * k;
    g_bw = 32000u * static_cast<uint32_t>(k);
    g_physScale = 1.0f / static_cast<float>(k);
}

int ModeFromEnvironment() {
    char v[16] = {};
    if (::GetEnvironmentVariableA("EVBOTS_FREQUENCY", v, sizeof(v)) > 0) {
        if (std::strcmp(v, "120") == 0 || _stricmp(v, "high120") == 0) return 2;
        if (std::strcmp(v, "60") == 0 || _stricmp(v, "high60") == 0) return 1;
        return 0;
    }
    const char* cl = ::GetCommandLineA();  // как у VU: -high60 / -high120
    if (cl != nullptr) {
        if (std::strstr(cl, "-high120") != nullptr) return 2;
        if (std::strstr(cl, "-high60") != nullptr) return 1;
    }
    return 0;
}

bool SwitchedOff() {
    char v[4] = {};
    return ::GetEnvironmentVariableA("EVBOTS_FREQ_PATCH", v, sizeof(v)) == 1 && v[0] == '0';
}

// ★ 238-4 (поиск «неверной лицензии» на клиенте): EVBOTS_FREQ_CLIENT_PARTS — какие группы правок клиента ставить:
// h — перехваты, c — правки кода, d — константы данных; нет переменной — все («hcd»).
bool ClientPart(char p) {
    char v[8] = {};
    const DWORD n = ::GetEnvironmentVariableA("EVBOTS_FREQ_CLIENT_PARTS", v, sizeof(v));
    if (n == 0 || n >= sizeof(v)) return true;
    return std::strchr(v, p) != nullptr;
}

// ── обработчики перехватов (обе стороны; смещения полей — свои у сервера и клиента) ──────────────────────
using TimerCtorFn = void*(__fastcall*)(void*, void*, uint8_t*, int, int);
using FixedCtorFn = void*(__fastcall*)(void*, void*, int);
using ConnCtorFn = uint8_t*(__fastcall*)(void*, void*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
using NetCtorFn = uint8_t*(__fastcall*)(void*, void*, uint32_t, uint32_t, uint32_t, uint32_t);
using DtFn = void(__fastcall*)(void*, void*, float);
using AccelFn = float(__stdcall*)(void*, void*, const float*);
using MoveFillFn = void(__stdcall*)(uint8_t*, void*, void*, uint32_t);
using PhysDtFn = void(__fastcall*)(void*, void*, float, const void*);
TimerCtorFn g_origTimer = nullptr;
FixedCtorFn g_origFixed = nullptr;
ConnCtorFn g_origConn = nullptr;
NetCtorFn g_origNet = nullptr;
DtFn g_origDt = nullptr;
AccelFn g_origAccel = nullptr;
MoveFillFn g_origMoveFill = nullptr;
PhysDtFn g_origPhysDt = nullptr;
int g_connOff = 0x24;    // клиент +0x24 (полоса, полоса, частота, половина); сервер +0x28
int g_netOff = 0x3004;   // клиент +0x3004 (полоса, полоса, 10·k, частота); сервер +0x3008
std::atomic<int> g_logTimer{0}, g_logConn{0}, g_logNet{0};

// Сервер, 60/120 Гц: у bf3_server_Final.exe ванильные значения сети не как у клиента VU (проверка 26.10: сетевой объект —
// полоса 131072, частоты 100.0; соединение — полоса из настроек), поэтому числа клиента VU не ставим: полоса ×k (как у VU —
// растёт с частотой), поле частоты / доли — только если там значение режима 30 Гц (30 / 15 / 10), иначе оставляем.
void ServerScaleU32(uint8_t* p) {
    const uint64_t v = static_cast<uint64_t>(*reinterpret_cast<uint32_t*>(p)) * static_cast<uint64_t>(g_hz / 30);
    *reinterpret_cast<uint32_t*>(p) = v > 0x7FFFFFFFull ? 0x7FFFFFFFu : static_cast<uint32_t>(v);
}
void ServerSetIf(uint8_t* p, float at30, float now) {
    float* f = reinterpret_cast<float*>(p);
    if (*f == at30) *f = now;
}
struct NetFields { uint32_t bw1, bw2; float f1, f2; };
NetFields ReadFields(const uint8_t* p) {
    NetFields n{};
    std::memcpy(&n, p, sizeof(n));
    return n;
}

// GameTimer(settings, частота, вид): у VU — settings +0x20 MaxSimFps, +0x28 ClampTicks, частота в аргумент.
void* __fastcall TimerCtorDetour(void* self, void*, uint8_t* settings, int hz, int kind) {
    float wasFps = 0.0f;
    int wasClamp = 0;
    bool ok = false;
    if (settings != nullptr) {
        __try {
            wasFps = *reinterpret_cast<float*>(settings + 0x20);
            wasClamp = *reinterpret_cast<int*>(settings + 0x28);
            *reinterpret_cast<float*>(settings + 0x20) = g_hzF;
            *reinterpret_cast<int*>(settings + 0x28) = g_clamp;
            ok = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    g_timerHits.fetch_add(1);
    if (g_logTimer.fetch_add(1) < 4) {
        FileLog("[freq] таймер (вид %d): частота %d -> %d, MaxSimFps %.1f -> %.1f, ClampTicks %d -> %d%s", kind, hz, g_hz,
                (double)wasFps, (double)g_hzF, wasClamp, g_clamp, ok ? "" : " (настройки не читаются — только частота)");
    }
    return g_origTimer(self, nullptr, settings, g_hz, kind);
}

void* __fastcall FixedCtorDetour(void* self, void*, int) { return g_origFixed(self, nullptr, g_hz); }

uint8_t* __fastcall ConnCtorDetour(void* self, void*, uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e,
                                   uint32_t f) {
    uint8_t* r = g_origConn(self, nullptr, a, b, c, d, e, f);
    if (r == nullptr) return r;
    uint8_t* p = r + g_connOff;
    const NetFields was = ReadFields(p);
    if (g_server) {
        ServerScaleU32(p);
        ServerScaleU32(p + 4);
        ServerSetIf(p + 8, 30.0f, g_hzF);
        ServerSetIf(p + 12, 15.0f, g_netHalf);
    } else {  // клиент — как у VU (FUN_10199010)
        *reinterpret_cast<uint32_t*>(p) = g_bw;
        *reinterpret_cast<uint32_t*>(p + 4) = g_bw;
        *reinterpret_cast<float*>(p + 8) = g_hzF;
        *reinterpret_cast<float*>(p + 12) = g_netHalf;
    }
    if (g_logConn.fetch_add(1) < 3) {
        const NetFields now = ReadFields(p);
        FileLog("[freq] соединение: полоса %u/%u -> %u/%u, частота %.1f -> %.1f, половина %.1f -> %.1f", was.bw1, was.bw2,
                now.bw1, now.bw2, (double)was.f1, (double)now.f1, (double)was.f2, (double)now.f2);
    }
    return r;
}

uint8_t* __fastcall NetCtorDetour(void* self, void*, uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    uint8_t* r = g_origNet(self, nullptr, a, b, c, d);
    if (r == nullptr) return r;
    uint8_t* p = r + g_netOff;
    const NetFields was = ReadFields(p);
    if (g_server) {
        ServerScaleU32(p);
        ServerScaleU32(p + 4);
        ServerSetIf(p + 8, 10.0f, g_netTen);
        ServerSetIf(p + 12, 30.0f, g_hzF);
    } else {  // клиент — как у VU (FUN_101A2650)
        *reinterpret_cast<uint32_t*>(p) = g_bw;
        *reinterpret_cast<uint32_t*>(p + 4) = g_bw;
        *reinterpret_cast<float*>(p + 8) = g_netTen;
        *reinterpret_cast<float*>(p + 12) = g_hzF;
    }
    if (g_logNet.fetch_add(1) < 3) {
        const NetFields now = ReadFields(p);
        FileLog("[freq] сетевой объект: полоса %u/%u -> %u/%u, доля %.1f -> %.1f, частота %.1f -> %.1f", was.bw1, was.bw2,
                now.bw1, now.bw2, (double)was.f1, (double)now.f1, (double)was.f2, (double)now.f2);
    }
    return r;
}

// Шаг времени (vu-core 101a2580): 60 Гц — f1 60, f2 45; 120 — f1 120, f2 60. dt == 1/f1 -> 1/f2; dt == 1/(3·f1) -> 1/(3·f2).
void __fastcall DtDetour(void* self, void*, float dt) {
    if (g_mode == 0) {
        g_origDt(self, nullptr, dt);
        return;
    }
    const float f1 = g_mode == 1 ? 60.0f : 120.0f;
    const float f2 = g_mode == 1 ? 45.0f : 60.0f;
    const float inv1 = 1.0f / f1, inv2 = 1.0f / f2;
    float out = dt;
    if (dt == inv1) {
        out = inv2;
    } else if (static_cast<int>(dt * 10000.0f) == static_cast<int>(inv1 / 3.0f * 10000.0f)) {
        out = inv2 / 3.0f;
    }
    g_origDt(self, nullptr, out);
}

// ── поправки физики VU при 60/120 (238-3) ──
// 0x005FFA90 (сервер 0x006A2A20), stdcall(a, b, const float* v) -> float: значение из b (+0x10 / +0x14 / +0x18) по вектору
// ввода v. VU 10195AA0: если v.x² + v.y² != 0 — результат × 1/k.
float __stdcall AccelDetour(void* a, void* b, const float* v) {
    const float r = g_origAccel(a, b, v);
    if (g_mode == 0 || v == nullptr) return r;
    return (v[0] * v[0] + v[1] * v[1] != 0.0f) ? r * g_physScale : r;
}

// 0x005FFA10 (сервер 0x006A29A0), stdcall(out, src, v, f): заполняет структуру движения, out+0x64 = 100.0. VU 10195B20:
// после оригинала out+0x64 × 1/k. Четвёртый аргумент — 4 байта как есть (у VU — float).
void __stdcall MoveFillDetour(uint8_t* out, void* src, void* v, uint32_t f) {
    g_origMoveFill(out, src, v, f);
    if (g_mode != 0 && out != nullptr) *reinterpret_cast<float*>(out + 0x64) *= g_physScale;
}

// 0x005C5720 (сервер 0x0065F560), thiscall(this, float dt, const vec*): шаг в физику (vtable +0x5C у [this+0xA0]+0xE0).
// VU 10195B80: dt == 1/30 (0x3D088889) -> 1/частота, иначе как есть.
void __fastcall PhysDtDetour(void* self, void*, float dt, const void* vec) {
    if (g_mode != 0 && dt == 1.0f / 30.0f) dt = g_invF;
    g_origPhysDt(self, nullptr, dt, vec);
}

// ── таблицы мест ─────────────────────────────────────────────────────────────────────────────────────────
struct HookSite {
    uint32_t va;
    uint8_t prologue[8];
    void* detour;
    void** orig;
    const char* name;
};
enum class Kind { Ptr, Imm32, Imm8 };  // Imm8 — однобайтовое значение (cmp eax, imm8; у VU — запись байта)
struct CodeSite {
    uint32_t va;      // адрес операнда (4 байта)
    uint32_t expect;  // что там сейчас (адрес константы движка или непосредственное значение)
    Kind kind;
    const void* var;  // Ptr: наша переменная (в операнд — её адрес); Imm32: значение int
};

// Выделенный сервер bf3_server_Final.exe (двойники мест VU, найдены 26.10.2026).
const HookSite kServerHooks[] = {
    {0x00849AB0, {0x83, 0xEC, 0x08, 0x53, 0x56, 0x57, 0x8B, 0xF1}, (void*)&TimerCtorDetour, (void**)&g_origTimer, "частота: игровой таймер"},
    {0x0083D600, {0x81, 0xEC, 0x34, 0x01, 0x00, 0x00, 0x8B, 0x84}, (void*)&FixedCtorDetour, (void**)&g_origFixed, "частота: таймер шага"},
    {0x00ED4410, {0x8B, 0x44, 0x24, 0x04, 0x81, 0xEC, 0x34, 0x01}, (void*)&ConnCtorDetour, (void**)&g_origConn, "частота: соединение"},
    {0x00D84810, {0x83, 0xEC, 0x34, 0x53, 0x55, 0x56, 0x57, 0x68}, (void*)&NetCtorDetour, (void**)&g_origNet, "частота: сетевой объект"},
    {0x00651D10, {0x53, 0x8B, 0xDC, 0x83, 0xEC, 0x08, 0x83, 0xE4}, (void*)&DtDetour, (void**)&g_origDt, "частота: шаг времени"},
    // ★ 238-3: поправки физики VU (двойники клиентских 0x005FFA90 / 0x005FFA10 / 0x005C5720)
    {0x006A2A20, {0x8B, 0x44, 0x24, 0x04, 0x80, 0xB8, 0x05, 0x01}, (void*)&AccelDetour, (void**)&g_origAccel, "частота: физика — ускорение по вводу"},
    {0x006A29A0, {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x8B, 0x45}, (void*)&MoveFillDetour, (void**)&g_origMoveFill, "частота: физика — структура движения"},
    {0x0065F560, {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x83, 0xEC}, (void*)&PhysDtDetour, (void**)&g_origPhysDt, "частота: физика — шаг"},
};
const CodeSite kServerCode[] = {
    // 30.0f (0x024690E4) — у VU 0x00AD1E1C, 0x0058C165, 0x0058CFDE, 0x0058D8A3, 0x005CA4E3/5CC/6EB
    {0x00DEF42A, 0x024690E4, Kind::Ptr, &g_hzF}, {0x0061C86E, 0x024690E4, Kind::Ptr, &g_hzF},
    {0x0061DA72, 0x024690E4, Kind::Ptr, &g_hzF}, {0x0061E80A, 0x024690E4, Kind::Ptr, &g_hzF},
    {0x00665C43, 0x024690E4, Kind::Ptr, &g_hzF}, {0x00665D2C, 0x024690E4, Kind::Ptr, &g_hzF},
    {0x00665E4B, 0x024690E4, Kind::Ptr, &g_hzF},
    // 30.0 double (0x02474E58) — у VU 0x0119DACD, 0x0119C3D1/C151/BEE1/BB01, 0x01064E31
    {0x00854D2D, 0x02474E58, Kind::Ptr, &g_hzD}, {0x00852421, 0x02474E58, Kind::Ptr, &g_hzD},
    {0x00852841, 0x02474E58, Kind::Ptr, &g_hzD}, {0x00852AF1, 0x02474E58, Kind::Ptr, &g_hzD},
    {0x00852DA1, 0x02474E58, Kind::Ptr, &g_hzD}, {0x00FB8DBC, 0x02474E58, Kind::Ptr, &g_hzD},
    // 0.1333f (0x0275F01C) -> 1/частота — у VU 0x016EE85D (во всех режимах, и при 30 Гц)
    {0x01A0F4C2, 0x0275F01C, Kind::Ptr, &g_invF},
    // 0.1 double (0x024784C0) — у VU 0x011DCAC1; 0.04 через указатель [0x02B2A8AC] — у VU константа 0x021ACAC0.
    // 0x02B2A8AC (238-3, проверено): указатель на значение настройки ввода «weapon-switching-mouse-wheel-repeat-delay»
    // (регистрирует 0x022681F0, по умолчанию 0.04 = 0x0247DC1C); код 0x00892A40 только `fld [edx]` — подмена на наш 0.04/k.
    {0x008929F5, 0x024784C0, Kind::Ptr, &g_tenthD}, {0x00892A42, 0x02B2A8AC, Kind::Ptr, &g_ptrF004},
    // mov [ecx+..], 30 — у VU 0x005F2458
    {0x00693F08, 30, Kind::Imm32, &g_hz},
    // 1/30 float (0x02437FD0) — у VU константа 0x02082C9C (здесь — только сопоставленные чтения)
    {0x018916C3, 0x02437FD0, Kind::Ptr, &g_invF}, {0x01248972, 0x02437FD0, Kind::Ptr, &g_invF},
    {0x01248A92, 0x02437FD0, Kind::Ptr, &g_invF}, {0x01248C7D, 0x02437FD0, Kind::Ptr, &g_invF},
    {0x01A1DFEB, 0x02437FD0, Kind::Ptr, &g_invF}, {0x00E54ABD, 0x02437FD0, Kind::Ptr, &g_invF},
    {0x00D43B3D, 0x02437FD0, Kind::Ptr, &g_invF}, {0x00F44A99, 0x02437FD0, Kind::Ptr, &g_invF},
    {0x00677093, 0x02437FD0, Kind::Ptr, &g_invF},
    // ★ 238-3 (сопоставление окрестностей, scratchpad freqmatch.py): 0x00DF6BAF/B5 fld+movss — байт в байт клиент
    // 0x00AD8907/0D; 0x00EB0F57 «dt == 0 -> 1/30» — клиент 0x00F72AFC (у сервера встроено в вызывающую); 0x01A0A86D —
    // запасной шаг полей +0x54/+0x58, как клиент 0x016EA4C2 (по полям, не байт в байт). Не трогаем (у клиента VU двойников
    // нет): 0x0059CBA0…0x0059D890 — отладочная отрисовка, «время показа» 1/30; 0x02268E50 — значение по умолчанию настройки
    // WeaponFiring.QuickSwitchDeployTime.
    {0x00DF6BB1, 0x02437FD0, Kind::Ptr, &g_invF}, {0x00DF6BB9, 0x02437FD0, Kind::Ptr, &g_invF},
    {0x00EB0F5B, 0x02437FD0, Kind::Ptr, &g_invF}, {0x01A0A871, 0x02437FD0, Kind::Ptr, &g_invF},
    // 1/30 double (0x02437FD8) — у VU константа 0x02080B80
    {0x00644B43, 0x02437FD8, Kind::Ptr, &g_invD}, {0x00644B91, 0x02437FD8, Kind::Ptr, &g_invD},
    {0x01248AA8, 0x02437FD8, Kind::Ptr, &g_invD}, {0x00ACC6BF, 0x02437FD8, Kind::Ptr, &g_invD},
    {0x00F44AC1, 0x02437FD8, Kind::Ptr, &g_invD},
    // ★ 238-3: только у выделенного сервера (двойника функции в клиенте не нашлось — VU её не правит; само поле
    // ForceDeltaTime в VU-Docs есть: GameTimeSettings) — шаг кадра 0x00412120 при
    // GameTimeSettings.ForceDeltaTime ([0x02877508]+0x2C): копит dt в [this+0x15C] и считает шаги по 1/30 (double), при
    // ForceDeltaTime < 0 отдаёт 1/30 (float). Шаг — 1/частота, как у таймера.
    {0x004121F2, 0x02437FD8, Kind::Ptr, &g_invD}, {0x00412235, 0x02437FD0, Kind::Ptr, &g_invF},
};

// Клиент bf3.exe — адреса VU (тот же exe). Код — после расшифровки.
const HookSite kClientHooks[] = {
    {0x01195870, {0x83, 0xEC, 0x08, 0x53, 0x56, 0x57, 0x8B, 0xF1}, (void*)&TimerCtorDetour, (void**)&g_origTimer, "частота: игровой таймер"},
    {0x0118D060, {0x8B, 0xC1, 0x8B, 0x4C, 0x24, 0x04, 0x33, 0xD2}, (void*)&FixedCtorDetour, (void**)&g_origFixed, "частота: таймер шага"},
    {0x00FB46D0, {0x8B, 0x44, 0x24, 0x04, 0x53, 0x56, 0x8B, 0xF1}, (void*)&ConnCtorDetour, (void**)&g_origConn, "частота: соединение"},
    {0x00A53A20, {0x83, 0xEC, 0x34, 0x53, 0x55, 0x56, 0x57, 0x68}, (void*)&NetCtorDetour, (void**)&g_origNet, "частота: сетевой объект"},
    {0x005BB1F0, {0x53, 0x8B, 0xDC, 0x83, 0xEC, 0x08, 0x83, 0xE4}, (void*)&DtDetour, (void**)&g_origDt, "частота: шаг времени"},
    // ★ 238-3: поправки физики VU (перехваты vu-core 0x5FFA90 / 0x5FFA10 / 0x5C5720)
    {0x005FFA90, {0x8B, 0x44, 0x24, 0x04, 0x80, 0xB8, 0x05, 0x01}, (void*)&AccelDetour, (void**)&g_origAccel, "частота: физика — ускорение по вводу"},
    {0x005FFA10, {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x8B, 0x45}, (void*)&MoveFillDetour, (void**)&g_origMoveFill, "частота: физика — структура движения"},
    {0x005C5720, {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x83, 0xEC}, (void*)&PhysDtDetour, (void**)&g_origPhysDt, "частота: физика — шаг"},
};
const CodeSite kClientCode[] = {
    {0x005E871B, 60, Kind::Imm32, nullptr},  // 2·частота — значение ниже, отдельно
    {0x005F2458, 30, Kind::Imm32, &g_hz},    {0x00A3031B, 30, Kind::Imm32, &g_hz},
    {0x01084220, 30, Kind::Imm8, &g_hz},  // cmp eax, 0x1e — один байт (дальше jcc)
    {0x00AD1E1C, 0x0207E710, Kind::Ptr, &g_hzF}, {0x0058C165, 0x0207E710, Kind::Ptr, &g_hzF},
    {0x0058CFDE, 0x0207E710, Kind::Ptr, &g_hzF}, {0x0058D8A3, 0x0207E710, Kind::Ptr, &g_hzF},
    {0x005CA4E3, 0x0207E710, Kind::Ptr, &g_hzF}, {0x005CA5CC, 0x0207E710, Kind::Ptr, &g_hzF},
    {0x005CA6EB, 0x0207E710, Kind::Ptr, &g_hzF},
    {0x01084BCE, 0x0207B518, Kind::Ptr, &g_hzD}, {0x0119DACD, 0x0207B518, Kind::Ptr, &g_hzD},
    {0x0119C3D1, 0x0207B518, Kind::Ptr, &g_hzD}, {0x0119C151, 0x0207B518, Kind::Ptr, &g_hzD},
    {0x0119BEE1, 0x0207B518, Kind::Ptr, &g_hzD}, {0x0119BB01, 0x0207B518, Kind::Ptr, &g_hzD},
    {0x01064E31, 0x0207B518, Kind::Ptr, &g_hzD},
    {0x016EE85D, 0x02231DA0, Kind::Ptr, &g_invF},
    {0x011DCAC1, 0x0207E8B0, Kind::Ptr, &g_tenthD},
    {0x006FF8DF, 0x020742A8, Kind::Ptr, &g_centD},
};
// Константы данных клиента, которые VU переписывает значением (как FUN_105363a0).
struct DataSite { uint32_t va; uint32_t size; const void* var; uint64_t expect; };

int g_twiceHz = 60;

bool ReadU32(uint32_t va, uint32_t* out) {
    __try {
        *out = *reinterpret_cast<const uint32_t*>(static_cast<uintptr_t>(va));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ReadBytes(uint32_t va, void* out, size_t n) {
    __try {
        std::memcpy(out, reinterpret_cast<const void*>(static_cast<uintptr_t>(va)), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ★ 238-3 (проверка 26.10): «всё или ничего» по стороне — сначала сверка ВСЕХ групп (перехваты, код, константы), потом
// запись. Раньше перехваты ставились до сверки кода: при несовпадении кода оставался полурежим.
bool CheckHooks(const HookSite* hooks, size_t n, const char* who) {
    for (size_t i = 0; i < n; ++i) {
        uint8_t now[8] = {};
        if (!ReadBytes(hooks[i].va, now, 8) || std::memcmp(now, hooks[i].prologue, 8) != 0) {
            FileLog("[freq] %s: пролог %s (0x%08X) не совпал — частота НЕ меняется (ничего не ставлю)", who, hooks[i].name,
                    hooks[i].va);
            return false;
        }
    }
    return true;
}

// Установка после сверки. Трамплин ядро пишет прямо в указатель оригинала ДО записи перехода (EvHook.cpp, 238-3). Не встал
// хоть один — снимаем поставленные: полурежима нет.
bool InstallHooks(const HookSite* hooks, size_t n, const char* who) {
    size_t ok = 0;
    for (; ok < n; ++ok) {
        if (EvHookInstall(reinterpret_cast<void*>(static_cast<uintptr_t>(hooks[ok].va)), hooks[ok].detour, hooks[ok].name,
                          hooks[ok].orig, nullptr, nullptr) != 0) {
            break;
        }
    }
    if (ok != n) {
        FileLog("[freq] %s: перехват %s не встал — снимаю поставленные (%u), частота НЕ меняется", who, hooks[ok].name,
                static_cast<unsigned>(ok));
        for (size_t i = 0; i < ok; ++i) EvHookRemove(reinterpret_cast<void*>(static_cast<uintptr_t>(hooks[i].va)));
        return false;
    }
    FileLog("[freq] %s: перехватов %u из %u", who, static_cast<unsigned>(ok), static_cast<unsigned>(n));
    return true;
}

bool CheckCode(const CodeSite* sites, size_t n, const char* who) {
    for (size_t i = 0; i < n; ++i) {
        uint32_t now = 0;
        if (sites[i].kind == Kind::Imm8) {
            uint8_t b = 0;
            if (ReadBytes(sites[i].va, &b, 1)) now = b;
        } else if (!ReadU32(sites[i].va, &now)) {
            now = 0xFFFFFFFFu;
        }
        if (now != sites[i].expect) {
            FileLog("[freq] %s: место 0x%08X — там 0x%08X, ждали 0x%08X — частота НЕ меняется (ничего не пишу)", who, sites[i].va,
                    now, sites[i].expect);
            return false;
        }
    }
    return true;
}

bool PatchCode(const CodeSite* sites, size_t n, const char* who) {
    int ok = 0;
    for (size_t i = 0; i < n; ++i) {
        uint32_t v = 0;
        if (sites[i].kind == Kind::Ptr) {
            v = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(sites[i].var));
        } else {
            v = sites[i].var != nullptr ? static_cast<uint32_t>(*static_cast<const int*>(sites[i].var))
                                        : static_cast<uint32_t>(g_twiceHz);
        }
        const unsigned len = sites[i].kind == Kind::Imm8 ? 1u : 4u;
        const uint8_t e8 = static_cast<uint8_t>(sites[i].expect);
        const void* expect = sites[i].kind == Kind::Imm8 ? static_cast<const void*>(&e8) : static_cast<const void*>(&sites[i].expect);
        ok += EvPatchBytes(reinterpret_cast<void*>(static_cast<uintptr_t>(sites[i].va)), expect, &v, len, "частота: код");
    }
    FileLog("[freq] %s: правок кода %d из %u", who, ok, static_cast<unsigned>(n));
    return ok == static_cast<int>(n);
}

const char* ModeName() { return g_mode == 2 ? "high120" : (g_mode == 1 ? "high60" : "regular"); }

void ApplyServer() {
    // ★ 238-3: 30 Гц — сервер как есть. Значения VU для режима regular взяты с клиента bf3.exe; у bf3_server_Final.exe
    // ванильная сеть другая (проверка 26.10), а все прежние прогоны стенда шли на ванильном сервере.
    if (g_mode == 0) {
        FileLog("[freq] ★ сервер: 30 Гц (regular) — движок сервера без правок");
        return;
    }
    g_connOff = 0x28;
    g_netOff = 0x3008;
    const size_t nh = sizeof(kServerHooks) / sizeof(kServerHooks[0]);
    const size_t nc = sizeof(kServerCode) / sizeof(kServerCode[0]);
    if (!CheckHooks(kServerHooks, nh, "сервер") || !CheckCode(kServerCode, nc, "сервер")) {
        FileLog("[freq] ★ сервер: остаётся 30 Гц — сверка не прошла, ничего не записано");
        SetMode(0);
        return;
    }
    if (!InstallHooks(kServerHooks, nh, "сервер")) {
        SetMode(0);
        return;
    }
    const bool c = PatchCode(kServerCode, nc, "сервер");
    FileLog("[freq] ★ сервер: %d Гц (%s) — перехваты да, код %s (%u мест)", g_hz, ModeName(), c ? "да" : "НЕ ВСЕ",
            static_cast<unsigned>(nc));
}

// константы данных клиента (у VU FUN_105363a0): 1/частота double, 0.04·k float, 1/частота float ×2
const DataSite kClientData[] = {
    {0x02080B80, 8, &g_invD, 0x3FA1111120000000ULL},  // double 0.033333335 (из float), как в exe
    {0x021ACAC0, 4, &g_f004, 0x3D23D70AULL},
    {0x02082C9C, 4, &g_invF, 0x3D088889ULL},
    {0x021AD55C, 4, &g_invF, 0x3D088889ULL},
};

// ★ 238-4: все места клиента уже расшифрованы (без журнала). Прогон 26.10 21:25 на 60 Гц: пролог таймера появился, а место
// 0x016EE85D было ещё зашифровано (0xA75B86A8; через минуту на живом клиенте — 0x02231DA0, как ждём) — код bf3.exe
// расшифровывается кусками, ждать одного пролога мало.
bool ClientAllPlain() {
    for (const HookSite& h : kClientHooks) {
        uint8_t now[8] = {};
        if (!ReadBytes(h.va, now, 8) || std::memcmp(now, h.prologue, 8) != 0) return false;
    }
    for (const CodeSite& s : kClientCode) {
        uint32_t now = 0;
        if (s.kind == Kind::Imm8) {
            uint8_t b = 0;
            if (!ReadBytes(s.va, &b, 1)) return false;
            now = b;
        } else if (!ReadU32(s.va, &now)) {
            return false;
        }
        if (now != s.expect) return false;
    }
    return true;
}

DWORD WINAPI ClientThread(LPVOID) {
    // Код bf3.exe расшифровывается после старта (activation.dll), кусками. Ждём, пока совпадут ВСЕ места (238-4; раньше —
    // только пролог игрового таймера). Срок 120 с — предел ожидания (не задержка): расшифровку не видно событием; не
    // совпало за срок — ниже сверка назовёт место, частоту не трогаем.
    const ULONGLONG deadline = ::GetTickCount64() + 120000;
    bool decrypted = false;
    bool timerPlain = false;
    while (::GetTickCount64() < deadline) {
        uint8_t now[8] = {};
        if (!timerPlain && ReadBytes(kClientHooks[0].va, now, 8) && std::memcmp(now, kClientHooks[0].prologue, 8) == 0) {
            timerPlain = true;
        }
        if (timerPlain && ClientAllPlain()) {
            decrypted = true;
            break;
        }
        ::Sleep(1);
    }
    if (!decrypted && timerPlain) {
        FileLog("[freq] клиент: за 120 с не все места совпали — сверка ниже назовёт первое");
        decrypted = true;
    }
    // ★ 238-4: правки кода — только когда защита EA (awc) связала импорт bf3.exe, как EVBots.dll (ModdingInterface.cpp,
    // ClientImportsBound, прогон 215 23.10). Перебор 26.10 22:02 (run\freq-bisect.txt): перехваты или правки кода через
    // 0,1 с после расшифровки — игра перечитывает лицензию → «неверная лицензия» / импорт не связан; константы данных — без
    // последствий. Признак — слот KERNEL32!GetProcAddress в .idata (rva 0x1FBD4BC) равен адресу GetProcAddress. Событие
    // защиты, не таймер; срок 120 с — предел ожидания.
    if (decrypted) {
        const uintptr_t want = reinterpret_cast<uintptr_t>(
            ::GetProcAddress(::GetModuleHandleA("kernel32.dll"), "GetProcAddress"));
        const ULONGLONG t0 = ::GetTickCount64();
        bool bound = false;
        while (::GetTickCount64() - t0 < 120000) {
            uint32_t v = 0;
            if (want != 0 && ReadU32(0x00400000u + 0x01FBD4BCu, &v) && v == static_cast<uint32_t>(want)) {
                bound = true;
                break;
            }
            ::Sleep(10);
        }
        if (!bound) {
            FileLog("[freq] ★ клиент: защита не связала импорт bf3.exe за 120 с — правки частоты не ставлю, чтобы не "
                    "мешать проверке лицензии");
            SetMode(0);
            return 0;
        }
        FileLog("[freq] клиент: импорт связан защитой через %llu мс после расшифровки — ставлю правки",
                static_cast<unsigned long long>(::GetTickCount64() - t0));
    }
    if (!decrypted) {
        FileLog("[freq] ★ клиент: за 120 с код игры не расшифровался — частота не меняется (%d Гц не ставлю)", g_hz);
        SetMode(0);
        return 0;
    }
    g_connOff = 0x24;
    g_netOff = 0x3004;
    const size_t nh = sizeof(kClientHooks) / sizeof(kClientHooks[0]);
    const size_t nc = sizeof(kClientCode) / sizeof(kClientCode[0]);
    bool dOk = true;
    for (const DataSite& d : kClientData) {
        uint64_t now = 0;
        if (!ReadBytes(d.va, &now, d.size) || std::memcmp(&now, &d.expect, d.size) != 0) {
            FileLog("[freq] клиент: константа 0x%08X не совпала — частота НЕ меняется (ничего не пишу)", d.va);
            dOk = false;
            break;
        }
    }
    const bool ph = ClientPart('h'), pc = ClientPart('c'), pd = ClientPart('d');
    if (!(ph && pc && pd)) {
        FileLog("[freq] клиент: группы правок по EVBOTS_FREQ_CLIENT_PARTS — перехваты %s, код %s, константы %s",
                ph ? "да" : "нет", pc ? "да" : "нет", pd ? "да" : "нет");
    }
    if (!dOk || !CheckHooks(kClientHooks, nh, "клиент") || !CheckCode(kClientCode, nc, "клиент") ||
        (ph && !InstallHooks(kClientHooks, nh, "клиент"))) {
        FileLog("[freq] ★ клиент: остаётся без правок частоты — сверка не прошла");
        SetMode(0);
        return 0;
    }
    const bool c = pc ? PatchCode(kClientCode, nc, "клиент") : false;
    int dn = 0;
    for (const DataSite& d : kClientData) {
        if (!pd) break;
        dn += EvPatchBytes(reinterpret_cast<void*>(static_cast<uintptr_t>(d.va)), &d.expect, d.var, d.size, "частота: константа");
    }
    FileLog("[freq] ★ клиент: %d Гц (%s) — перехваты да, код %s, констант %d из 4; игровой таймер %s", g_hz, ModeName(),
            c ? "да" : "НЕ ВСЕ", dn, g_timerHits.load() > 0 ? "создан после перехвата" : "ещё не создавался (ждём)");
    return 0;
}

}  // namespace

/// Частота текущего процесса: 30, 60 или 120 (для EVBots.dll: тик Lua, RCON vu.FrequencyMode).
extern "C" __declspec(dllexport) int __cdecl EvFreqHz() { return g_hz; }

namespace evcore {

/// Вызывается из DllMain ядра. Сервер: процесс ещё подвешен загрузчиком (CREATE_SUSPENDED) — ставим сразу, до таймеров.
/// Клиент: код зашифрован — свой поток ждёт расшифровки.
void FreqInit() {
    SetMode(ModeFromEnvironment());
    g_twiceHz = 2 * g_hz;
    if (SwitchedOff()) {
        FileLog("[freq] EVBOTS_FREQ_PATCH=0 — частота движка не трогается (%d Гц)", g_hz);
        return;
    }
    g_server = ::GetModuleHandleA("bf3_server_Final.exe") != nullptr;
    const bool client = !g_server && ::GetModuleHandleA("bf3.exe") != nullptr;
    if (g_server) {
        ApplyServer();
    } else if (client) {
        HANDLE t = ::CreateThread(nullptr, 0, ClientThread, nullptr, 0, nullptr);
        if (t != nullptr) ::CloseHandle(t);
    }
}

}  // namespace evcore
