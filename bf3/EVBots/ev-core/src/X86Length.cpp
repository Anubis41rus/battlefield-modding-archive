// X86Length.cpp — декодер длин инструкций x86-32.
//
// Объём: однобайтовые и двухбайтовые (0F) карты, префиксы, ModRM/SIB,
// непосредственные операнды. Инструкции, не встречающиеся в прологах
// функций MSVC, помечены как недопустимые — хук на такой функции мы не
// ставим, вместо того чтобы рисковать порчей кода.
//
// Проверка: C:\\Games\\re\\verify_decoder.py сверяет наш декодер с
// capstone поточечно: в каждой точке, где capstone распознал инструкцию,
// длины обязаны совпасть. Сверка нашла и помогла исправить 10 ошибок
// (полярность F_IZ, SIB при mod==3, RET imm16, групповые F6/F7, чтение
// ModRM после того как его съел ModRmLength, потерянная строка 0F 70-77,
// 0F 01/xgetbv, третий байт opcode 0F 38/3A, 0F A5/AD, 0xFD std).
// Отказ декодера на данных в .text и на VEX/AVX (C4/C5) — нормален:
// хук на таких байтах просто не ставится.

#include "X86Length.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace evcore {
namespace {

// Флаги для таблиц opcode.
enum : uint8_t {
    F_NONE  = 0x00,  ///< ни ModRM, ни непосредственного операнда
    F_MODRM = 0x01,  ///< за ModRM следует ModR/M
    F_IB    = 0x02,  ///< + 1 байт непосредственного
    F_IZ    = 0x04,  ///< + 2 байта (4 при префиксе 0x66)
    F_IW    = 0x08,  ///< + 2 байта ВСЕГДА (RET imm16/RETF imm16: 0x66 не влияет)
    F_BAD   = 0x10,  ///< недопустим в 32-битном режиме
};

// ── однобайтовая карта ────────────────────────────────────────────────────
// Размер не задаём вручную: пусть компилятор выведет его из числа записей,
// а static_assert ниже проверит, что их ровно 256. Так опечатка (лишняя или
// потерянная запись) — это ошибка сборки, а не молчаливо сдвинутая таблица.
const uint8_t kOneByte[] = {
    /*00*/ F_MODRM, F_MODRM, F_MODRM, F_MODRM, F_IB, F_IZ, F_BAD, F_BAD,
    /*08*/ F_MODRM, F_MODRM, F_MODRM, F_MODRM, F_IB, F_IZ, F_BAD, F_NONE,  // 0F = отдельно
    /*10*/ F_MODRM, F_MODRM, F_MODRM, F_MODRM, F_IB, F_IZ, F_BAD, F_BAD,
    /*18*/ F_MODRM, F_MODRM, F_MODRM, F_MODRM, F_IB, F_IZ, F_BAD, F_BAD,
    /*20*/ F_MODRM, F_MODRM, F_MODRM, F_MODRM, F_IB, F_IZ, F_NONE, F_BAD,  // 26 = префикс
    /*28*/ F_MODRM, F_MODRM, F_MODRM, F_MODRM, F_IB, F_IZ, F_NONE, F_BAD,  // 2E = префикс
    /*30*/ F_MODRM, F_MODRM, F_MODRM, F_MODRM, F_IB, F_IZ, F_NONE, F_BAD,  // 36 = префикс
    /*38*/ F_MODRM, F_MODRM, F_MODRM, F_MODRM, F_IB, F_IZ, F_NONE, F_BAD,  // 3E = префикс
    /*40*/ F_NONE, F_NONE, F_NONE, F_NONE, F_NONE, F_NONE, F_NONE, F_NONE,
    /*48*/ F_NONE, F_NONE, F_NONE, F_NONE, F_NONE, F_NONE, F_NONE, F_NONE,
    /*50*/ F_NONE, F_NONE, F_NONE, F_NONE, F_NONE, F_NONE, F_NONE, F_NONE,
    /*58*/ F_NONE, F_NONE, F_NONE, F_NONE, F_NONE, F_NONE, F_NONE, F_NONE,
    /*60*/ F_NONE, F_NONE, F_MODRM, F_MODRM, F_NONE, F_NONE, F_NONE, F_NONE,  // 66/67 = префиксы
    /*68*/ F_IZ, F_MODRM | F_IZ, F_IB, F_MODRM | F_IB, F_NONE, F_NONE, F_NONE, F_NONE,
    /*70*/ F_IB, F_IB, F_IB, F_IB, F_IB, F_IB, F_IB, F_IB,
    /*78*/ F_IB, F_IB, F_IB, F_IB, F_IB, F_IB, F_IB, F_IB,
    /*80*/ F_MODRM | F_IB, F_MODRM | F_IZ, F_BAD, F_MODRM | F_IB, F_MODRM, F_MODRM, F_MODRM, F_MODRM,
    /*88*/ F_MODRM, F_MODRM, F_MODRM, F_MODRM, F_MODRM, F_MODRM, F_MODRM, F_MODRM,
    /*90*/ F_NONE, F_NONE, F_NONE, F_NONE, F_NONE, F_NONE, F_NONE, F_NONE,
    /*98*/ F_NONE, F_NONE, F_BAD, F_NONE, F_NONE, F_NONE, F_NONE, F_NONE,
    /*A0*/ F_IZ, F_IZ, F_IZ, F_IZ, F_NONE, F_NONE, F_NONE, F_NONE,
    /*A8*/ F_IB, F_IZ, F_NONE, F_NONE, F_NONE, F_NONE, F_NONE, F_NONE,
    /*B0*/ F_IB, F_IB, F_IB, F_IB, F_IB, F_IB, F_IB, F_IB,
    /*B8*/ F_IW, F_IW, F_IW, F_IW, F_IW, F_IW, F_IW, F_IW,
    /*C0*/ F_MODRM | F_IB, F_MODRM | F_IB, F_IW, F_NONE, F_BAD, F_BAD, F_MODRM | F_IB, F_MODRM | F_IZ,
    /*C8*/ F_IZ, F_NONE, F_IW, F_NONE, F_NONE, F_IB, F_NONE, F_NONE,
    /*D0*/ F_MODRM, F_MODRM, F_MODRM, F_MODRM, F_IB, F_IB, F_BAD, F_NONE,
    /*D8*/ F_MODRM, F_MODRM, F_MODRM, F_MODRM, F_MODRM, F_MODRM, F_MODRM, F_MODRM,
    /*E0*/ F_IB, F_IB, F_IB, F_IB, F_IB, F_IB, F_IB, F_IB,
    /*E8*/ F_IZ, F_IZ, F_BAD, F_IB, F_NONE, F_NONE, F_NONE, F_NONE,
    /*F0*/ F_NONE, F_NONE, F_NONE, F_NONE, F_NONE, F_NONE, F_MODRM, F_MODRM,
    /*F8*/ F_NONE, F_NONE, F_NONE, F_NONE, F_NONE, F_NONE, F_MODRM, F_MODRM,  // F8-FD: clc/stc/cli/sti/cld/std, FE/FF: группы
};

// ── двухбайтовая карта (0F xx) ────────────────────────────────────────────
const uint8_t kTwoByte[] = {
    /*00*/ F_MODRM, F_MODRM, F_MODRM, F_MODRM, F_BAD, F_NONE, F_NONE, F_NONE,
    /*08*/ F_NONE, F_NONE, F_BAD, F_NONE, F_MODRM, F_BAD, F_NONE, F_MODRM | F_IB,  // 0F 0F = 3DNow
    /*10*/ F_MODRM, F_MODRM, F_MODRM, F_MODRM, F_MODRM, F_MODRM, F_MODRM, F_MODRM,
    /*18*/ F_MODRM, F_MODRM, F_MODRM, F_MODRM, F_MODRM, F_MODRM, F_MODRM, F_MODRM,
    /*20*/ F_MODRM, F_MODRM, F_MODRM, F_MODRM, F_BAD, F_BAD, F_BAD, F_BAD,
    /*28*/ F_MODRM, F_MODRM, F_MODRM, F_MODRM, F_MODRM, F_MODRM, F_MODRM, F_MODRM,
    /*30*/ F_NONE, F_NONE, F_NONE, F_NONE, F_NONE, F_NONE, F_NONE, F_NONE,
    /*38*/ F_NONE, F_BAD, F_NONE, F_BAD, F_BAD, F_BAD, F_BAD, F_BAD,  // 38/3A: расширения SSE, разбираются отдельно ниже
    /*40*/ F_MODRM, F_MODRM, F_MODRM, F_MODRM, F_MODRM, F_MODRM, F_MODRM, F_MODRM,
    /*48*/ F_MODRM, F_MODRM, F_MODRM, F_MODRM, F_MODRM, F_MODRM, F_MODRM, F_MODRM,
    /*50*/ F_MODRM, F_MODRM, F_MODRM, F_MODRM, F_MODRM, F_MODRM, F_MODRM, F_MODRM,
    /*58*/ F_MODRM, F_MODRM, F_MODRM, F_MODRM, F_MODRM, F_MODRM, F_MODRM, F_MODRM,
    /*60*/ F_MODRM, F_MODRM, F_MODRM, F_MODRM, F_MODRM, F_MODRM, F_MODRM, F_MODRM,
    /*68*/ F_MODRM, F_MODRM, F_MODRM, F_MODRM, F_MODRM, F_MODRM, F_MODRM, F_MODRM,
    /*70*/ F_MODRM | F_IB, F_MODRM | F_IB, F_MODRM | F_IB, F_MODRM | F_IB, F_MODRM, F_MODRM, F_MODRM, F_NONE,  // 70: pshufw +ib, 71-73: группы +ib, 77: emms
    /*78*/ F_MODRM, F_MODRM, F_BAD, F_BAD, F_MODRM, F_MODRM, F_MODRM, F_MODRM,
    /*80*/ F_IZ, F_IZ, F_IZ, F_IZ, F_IZ, F_IZ, F_IZ, F_IZ,
    /*88*/ F_IZ, F_IZ, F_IZ, F_IZ, F_IZ, F_IZ, F_IZ, F_IZ,
    /*90*/ F_MODRM, F_MODRM, F_MODRM, F_MODRM, F_MODRM, F_MODRM, F_MODRM, F_MODRM,
    /*98*/ F_MODRM, F_MODRM, F_MODRM, F_MODRM, F_MODRM, F_MODRM, F_MODRM, F_MODRM,
    /*A0*/ F_NONE, F_NONE, F_NONE, F_MODRM, F_MODRM | F_IB, F_MODRM, F_BAD, F_BAD,  // A4: shld +ib, A5: shld/cl
    /*A8*/ F_NONE, F_NONE, F_NONE, F_MODRM, F_MODRM | F_IB, F_MODRM, F_MODRM, F_MODRM,  // AC: shrd +ib, AD: shrd/cl, AE: fxsave/lfence, AF: imul
    /*B0*/ F_MODRM, F_MODRM, F_MODRM, F_MODRM, F_MODRM, F_MODRM, F_MODRM, F_MODRM,
    /*B8*/ F_MODRM, F_MODRM, F_MODRM | F_IB, F_MODRM, F_MODRM, F_MODRM, F_MODRM, F_MODRM,
    /*C0*/ F_MODRM, F_MODRM, F_MODRM | F_IB, F_MODRM, F_MODRM | F_IB, F_MODRM | F_IB, F_MODRM, F_MODRM,
    /*C8*/ F_MODRM, F_MODRM, F_MODRM, F_MODRM, F_MODRM, F_MODRM, F_MODRM, F_MODRM,
    /*D0*/ F_MODRM, F_MODRM, F_MODRM, F_MODRM, F_MODRM, F_MODRM, F_MODRM, F_MODRM,
    /*D8*/ F_MODRM, F_MODRM, F_MODRM, F_MODRM, F_MODRM, F_MODRM, F_MODRM, F_MODRM,
    /*E0*/ F_MODRM, F_MODRM, F_MODRM, F_MODRM, F_MODRM, F_MODRM, F_MODRM, F_MODRM,
    /*E8*/ F_MODRM, F_MODRM, F_MODRM, F_MODRM, F_MODRM, F_MODRM, F_MODRM, F_MODRM,
    /*F0*/ F_MODRM, F_MODRM, F_MODRM, F_MODRM, F_MODRM, F_MODRM, F_MODRM, F_MODRM,
    /*F8*/ F_MODRM, F_MODRM, F_MODRM, F_MODRM, F_MODRM, F_MODRM, F_MODRM, F_MODRM,
};

// Обе карты обязаны содержать ровно по 256 записей: индекс — это байт
// opcode. Если записей меньше, недостающие молча обнулятся и инструкции
// начнут разбираться неверно; если больше — это уже ошибка компиляции.
static_assert(sizeof(kOneByte) == 256,
              "однобайтовая карта opcode должна содержать 256 записей");
static_assert(sizeof(kTwoByte) == 256,
              "двухбайтовая карта opcode должна содержать 256 записей");

// ── курсор по байтам с проверкой границ ──────────────────────────────────
class Reader {
public:
    Reader(const uint8_t* code, size_t available)
        : code_(code), available_(available) {}

    bool Has(size_t n) const { return pos_ + n <= available_; }

    uint8_t Peek(size_t ahead = 0) const {
        return (pos_ + ahead < available_) ? code_[pos_ + ahead] : 0;
    }

    uint8_t Get() { return code_[pos_++]; }

    bool Skip(size_t n) {
        if (!Has(n)) {
            return false;
        }
        pos_ += n;
        return true;
    }

    size_t Pos() const { return pos_; }

private:
    const uint8_t* code_;
    size_t available_;
    size_t pos_ = 0;
};

/// Длина ModRM вместе с SIB и смещением.
///
/// mod32 — 32-битная адресация (true) или 16-битная (после 0x67).
bool ModRmLength(Reader& r, bool mod32) {
    if (!r.Has(1)) {
        return false;
    }

    const uint8_t modrm = r.Get();
    const uint8_t mod = static_cast<uint8_t>(modrm >> 6);
    const uint8_t rm = static_cast<uint8_t>(modrm & 7);

    if (!mod32) {
        // 16-битная адресация: mod=0 rm=6 -> disp16, mod=1 -> disp8, mod=2 -> disp16
        if (mod == 0 && rm == 6) {
            return r.Skip(2);
        }
        if (mod == 1) {
            return r.Skip(1);
        }
        if (mod == 2) {
            return r.Skip(2);
        }
        return true;
    }

    if (rm == 4 && mod != 3) {
        // Байт SIB читается только при НЕ регистровой адресации. При mod=3
        // операнд — это регистр, и rm=4 не означает SIB. Из-за пропуска
        // этой проверки `81 EC 0C 01 00 00` (sub esp, 0x10C) укорачивался
        // на байт.
        if (!r.Has(1)) {
            return false;
        }
        const uint8_t sib = r.Get();
        const uint8_t base = static_cast<uint8_t>(sib & 7);

        if (mod == 0 && base == 5) {
            return r.Skip(4);  // смещение 32 бита
        }
    } else if (mod == 0 && rm == 5) {
        return r.Skip(4);  // смещение 32 бита вместо адреса
    }

    if (mod == 1) {
        return r.Skip(1);
    }
    if (mod == 2) {
        return r.Skip(4);
    }

    return true;
}

/// Групповые opcode, где непосредственный операнд зависит от /reg.
///
/// modrm передаётся ЯВНО: к моменту вызова байт ModRM уже прочитан
/// функцией ModRmLength. Если бы GroupOpcode читал его сам через Peek(),
/// он смотрел бы на СЛЕДУЮЩИЙ байт — именно из-за этого `F7 D9` (neg ecx)
/// в одном месте разбирался как 2 байта, а в другом как 6.
bool GroupOpcode(Reader& r, uint8_t op, uint8_t modrm, bool opsize16) {
    // F6 /0,/1 — TEST Eb,Ib ; F7 /0,/1 — TEST Ev,Iz ; остальное — без
    // непосредственного операнда.
    if (op == 0xF6 || op == 0xF7) {
        const uint8_t reg = static_cast<uint8_t>((modrm >> 3) & 7);
        if (reg > 1) {
            return true;  // без непосредственного
        }
        return r.Skip(op == 0xF6 ? 1 : (opsize16 ? 2u : 4u));
    }

    if (op == 0xFE || op == 0xFF) {
        return true;  // непосредственного нет
    }

    return false;
}

}  // namespace

bool InstructionLength(const uint8_t* code, size_t available, size_t* length) {
    if (code == nullptr || length == nullptr || available == 0) {
        return false;
    }

    Reader r(code, available);
    bool opsize16 = false;
    bool addr32 = true;

    // ── префиксы ──────────────────────────────────────────────────────────
    for (;;) {
        const uint8_t b = r.Peek();
        switch (b) {
            case 0x66:  // размер операнда
                opsize16 = true;
                r.Skip(1);
                continue;
            case 0x67:  // размер адреса
                // 16-битная адресация в 32-битном коде не встречается
                // почти никогда, но обрабатываем её честно.
                addr32 = false;
                r.Skip(1);
                continue;
            case 0xF0:  // lock
            case 0xF2:  // repne
            case 0xF3:  // rep
            case 0x2E:  // cs
            case 0x36:  // ss
            case 0x3E:  // ds
            case 0x26:  // es
            case 0x64:  // fs
            case 0x65:  // gs
                r.Skip(1);
                continue;
            default:
                break;
        }
        break;
    }

    if (!r.Has(1)) {
        return false;
    }

    const uint8_t op = r.Get();

    // ── двухбайтовые ──────────────────────────────────────────────────────
    if (op == 0x0F) {
        if (!r.Has(1)) {
            return false;
        }
        const uint8_t op2 = r.Get();
        const uint8_t flags = kTwoByte[op2];

        if ((flags & F_BAD) != 0) {
            return false;
        }

// 0F 38 и 0F 3A — расширения SSE: после 0F идёт байт 38/3A, ЗА НИМ ещё
        // один байт opcode, и только потом ModRM. У 3A ещё и imm8 в конце.
        // Здесь долго забывали про третий байт opcode, из-за чего
        // `66 0F 3A 63 47 F0 40` считался 6 байтами вместо 7.
        if (op2 == 0x38 || op2 == 0x3A) {
            if (!r.Has(1)) {
                return false;
            }
            (void)r.Get();  // байт opcode расширения

            if (!ModRmLength(r, addr32)) {
                return false;
            }
            if (op2 == 0x3A && !r.Skip(1)) {
                return false;
            }
            *length = r.Pos();
            return true;
        }

        // Все кодировки группы 0F 01 (sgdt/lgdt/smsw/lmsw/xgetbv/xsetbv/rdtscp и
        // прочие) имеют байт ModRM, поэтому отдельная обработка не нужна —
        // обычный разбор ModRM справляется. Здесь стояла проверка «reg >= 2 —
        // неизвестно», и она отвергала вполне законный `0F 01 D0` (xgetbv),
        // из-за чего декодер останавливался на нём.

        if ((flags & F_MODRM) != 0) {
            if (!ModRmLength(r, addr32)) {
                return false;
            }
        }

        if ((flags & F_IB) != 0 && !r.Skip(1)) {
            return false;
        }
        // F_IZ — непосредственный размером в операнд: 2 байта при префиксе
        // 0x66, иначе 4. Здесь была перепутана полярность (читалось 4 при
        // 0x66 и 2 без него), из-за чего `sub esp, 0x10C` считался 5-байтовым
        // вместо 6. Поймано сверкой с capstone (C:\Games\re\verify_decoder.py).
        if ((flags & F_IZ) != 0 && !r.Skip(opsize16 ? 2u : 4u)) {
            return false;
        }
        // F_IW — «word»-непосредственный фиксированного размера (RET imm16,
        // RETF imm16): всегда 2 байта, префикс 0x66 его не меняет.
        if ((flags & F_IW) != 0 && !r.Skip(2)) {
            return false;
        }

        *length = r.Pos();
        return true;
    }

    // ── однобайтовые ──────────────────────────────────────────────────────
    const uint8_t flags = kOneByte[op];

    if ((flags & F_BAD) != 0) {
        return false;
    }

    // Групповые opcode F6/F7/FE/FF: наличие непосредственного операнда
    // зависит от поля /reg в ModRM, поэтому их разбираем ЦЕЛИКОМ здесь и
    // сразу выходим. Иначе общий F_IZ добавил бы лишние байты: `F7 D9`
    // (neg ecx, 2 байта) раздувался до 6.
    if (op == 0xF6 || op == 0xF7 || op == 0xFE || op == 0xFF) {
        if (!r.Has(1)) {
            return false;
        }
        // ModRM читаем ДО ModRmLength: дальше его уже не будет.
        const uint8_t modrm = r.Peek();

        if (!ModRmLength(r, addr32)) {
            return false;
        }
        if (!GroupOpcode(r, op, modrm, opsize16)) {
            return false;
        }
        *length = r.Pos();
        return true;
    }

    // CALL/JMP rel32: смещение ВСЕГДА 4 байта, префикс 0x66 на него не влияет.
    if (op == 0xE8 || op == 0xE9) {
        if (!r.Skip(4)) {
            return false;
        }
        *length = r.Pos();
        return true;
    }

    // ENTER: imm16 + imm8, imm16 всегда 2 байта.
    if (op == 0xC8) {
        if (!r.Skip(2) || !r.Skip(1)) {
            return false;
        }
        *length = r.Pos();
        return true;
    }

    // MOV A0..A3 — moffs (4 байта; при 0x66 — 2, но это редкость)
    if (op >= 0xA0 && op <= 0xA3) {
        if (!r.Skip(opsize16 ? 2u : 4u)) {
            return false;
        }
        *length = r.Pos();
        return true;
    }

    // MOV r32, imm — 4 байта, с префиксом 0x66 — 2
    if (op >= 0xB8 && op <= 0xBF) {
        if (!r.Skip(opsize16 ? 2u : 4u)) {
            return false;
        }
        *length = r.Pos();
        return true;
    }

    if ((flags & F_MODRM) != 0) {
        if (!ModRmLength(r, addr32)) {
            return false;
        }
    }

    if ((flags & F_IB) != 0 && !r.Skip(1)) {
        return false;
    }
    if ((flags & F_IZ) != 0 && !r.Skip(opsize16 ? 2u : 4u)) {
        return false;
    }
    // F_IW — «word»-непосредственный фиксированного размера (RET imm16,
    // RETF imm16): всегда 2 байта, префикс 0x66 его не меняет.
    if ((flags & F_IW) != 0 && !r.Skip(2)) {
        return false;
    }

    *length = r.Pos();
    return true;
}

bool StolenSpan(const uint8_t* code, size_t available, size_t minimum,
                size_t* span, size_t* instructionCount, bool allowRel32Calls) {
    if (code == nullptr || span == nullptr || instructionCount == nullptr) {
        return false;
    }

    size_t total = 0;
    size_t count = 0;

    while (total < minimum) {
        size_t len = 0;
        if (!InstructionLength(code + total, available - total, &len)) {
            return false;
        }
        if (len == 0) {
            return false;
        }
        // Сборка 237: относительный переход/вызов, перенесённый в трамплин как есть, прыгнул бы мимо (смещение
        // считается от нового адреса) — тихая порча кода. Такой пролог не крадём: отказ лучше порчи.
        {
            size_t p = 0;
            while (p < len && (code[total + p] == 0x66 || code[total + p] == 0x67 || code[total + p] == 0xF0 ||
                               code[total + p] == 0xF2 || code[total + p] == 0xF3 || code[total + p] == 0x2E ||
                               code[total + p] == 0x36 || code[total + p] == 0x3E || code[total + p] == 0x26 ||
                               code[total + p] == 0x64 || code[total + p] == 0x65)) {
                ++p;
            }
            const uint8_t op = p < len ? code[total + p] : 0;
            const uint8_t op2 = p + 1 < len ? code[total + p + 1] : 0;
            const bool relocatableCall = allowRel32Calls && p == 0 && op == 0xE8 && len == 5;
            if ((!relocatableCall && op == 0xE8) || op == 0xE9 || op == 0xEB || (op >= 0x70 && op <= 0x7F) || (op >= 0xE0 && op <= 0xE3) ||
                (op == 0x0F && op2 >= 0x80 && op2 <= 0x8F)) {
                return false;
            }
        }

        total += len;
        ++count;
    }

    *span = total;
    *instructionCount = count;
    return true;
}

bool RelocatePrologue(const uint8_t* source, size_t span, uint32_t sourceAddress,
                     uint32_t destinationAddress, uint8_t* output) {
    if (!source || !output || span == 0 || span > 32) return false;
    size_t checked = 0, instructions = 0;
    if (!StolenSpan(source, span, span, &checked, &instructions, true) || checked != span) return false;
    uint8_t relocated[32];
    std::memcpy(relocated, source, span);
    for (size_t offset = 0; offset < span;) {
        size_t length = 0;
        if (!InstructionLength(source + offset, span - offset, &length) || length == 0) return false;
        if (source[offset] == 0xE8) {
            uint32_t relative = 0;
            std::memcpy(&relative, source + offset + 1, 4);
            const uint32_t originalNext = sourceAddress + static_cast<uint32_t>(offset + length);
            const uint32_t target = originalNext + relative;
            // A call inside replaced bytes cannot safely return to original code.
            // CALL-next is also used to observe EIP and must not be relocated.
            if (target == originalNext || static_cast<uint32_t>(target - sourceAddress) < span) return false;
            const uint32_t adjusted = target - (destinationAddress + static_cast<uint32_t>(offset + length));
            std::memcpy(relocated + offset + 1, &adjusted, 4);
        }
        offset += length;
    }
    std::memcpy(output, relocated, span);
    return true;
}

std::string HexBytes(const uint8_t* data, size_t count) {
    std::string out;
    char buf[4];

    for (size_t i = 0; i < count; i++) {
        snprintf(buf, sizeof(buf), "%02X", data[i]);
        if (i != 0) {
            out.push_back(' ');
        }
        out.append(buf);
    }

    return out;
}

}  // namespace evcore
