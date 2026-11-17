// X86Length.h — длина инструкций x86-32.
//
// Зачем. Перехват функции начинается с того, что мы заменяем начало
// пролога прыжком. Чтобы трамплин потом мог выполнить украденные байты и
// вернуться в исходный код по адресу «цель + сколько украли», число
// украденных байтов обязано совпадать с ГРАНИЦЕЙ ИНСТРУКЦИИ.
//
// Кража ровно 5 байт (как в botlayer) на это не рассчитана. Реальный
// пример из нашей самопроверки:
//
//     0F AF CA   imul ecx, edx    3 байта
//     8B C1      mov  eax, ecx    2 байта
//     C3         ret               1 байт
//
// Крадём 5 байт → попадаем на последний байт `mov` → падение 0xC0000005
// при первом же вызове через трамплин.
//
// Поэтому здесь считаем длины инструкций и крадём ЦЕЛОЕ ЧИСЛО инструкций,
// суммарно не меньше 5 байт.
//
// Принцип: лучше отказаться от перехвата, чем испортить код. Нераспознанный
// opcode возвращает false — хук не ставится, остальное продолжает работать.

#pragma once

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace evcore {

/// Длина инструкции по адресу.
///
/// available — сколько байт реально можно читать по code (обычно размер
/// секции или остаток страницы), чтобы декодер не вышел за границу.
///
/// Возвращает true и записывает длину в length. При неизвестной инструкции
/// или нехватке байт возвращает false.
bool InstructionLength(const uint8_t* code, size_t available, size_t* length);

/// Суммарная длина первых инструкций, пока не наберётся не меньше minimum.
///
/// Это ровно то, что нужно перехвату: сколько байт безопасно украсть.
///
/// wholeInstructions — не пересекать границу инструкций (для перехвата
/// всегда true).
bool StolenSpan(const uint8_t* code, size_t available, size_t minimum,
                size_t* span, size_t* instructionCount, bool allowRel32Calls = false);

// Copies a validated prologue and fixes unprefixed CALL rel32 operands. Other
// relative control flow, internal calls and CALL-next (get-PC idiom) are refused.
// Output is unchanged on failure. Addresses are explicitly x86 modulo-2^32.
bool RelocatePrologue(const uint8_t* source, size_t span, uint32_t sourceAddress,
                     uint32_t destinationAddress, uint8_t* output);

/// Человекочитаемый байт-код для диагностики: "51 8D 04 11 89".
std::string HexBytes(const uint8_t* data, size_t count);

}  // namespace evcore
