// X86Length.Dump.cpp — проверка нашего декодера длин (src\X86Length.cpp).
//
// Два режима:
//   X86Length.Dump.exe sweep <файл>   — линейный проход по .text: печатает
//                                       границы инструкций, останавливается
//                                       на первой неопознанной (там могут
//                                       оказаться данные — таблицы переходов).
//   X86Length.Dump.exe at <файл]      — читает смещения (по одному в строке)
//                                       со стандартного входа и для КАЖДОГО
//                                       печатает "<смещение> <длина>".
//
// Второй режим — основной для сверки с capstone (C:\Games\re\verify_decoder.py):
// в каждой точке, где эталонный дизассемблер распознал инструкцию, спрашиваем
// наш декодер и сравниваем длину. Поточечная сверка не ломается на данных,
// встроенных в .text (таблицы переходов MSVC).

#include <Windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../src/X86Length.h"

namespace {

/// Находит секцию .text: смещение в файле и размер.
bool FindText(const std::vector<unsigned char>& file, size_t* fileOffset,
              size_t* size) {
    if (file.size() < 0x40) {
        return false;
    }

    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(file.data());
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return false;
    }

    const size_t peOffset = static_cast<size_t>(dos->e_lfanew);
    if (peOffset + sizeof(IMAGE_NT_HEADERS32) > file.size()) {
        return false;
    }

    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(
        file.data() + peOffset);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return false;
    }

    const IMAGE_SECTION_HEADER* sections = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        char name[9] = {};
        std::memcpy(name, sections[i].Name, 8);
        if (std::strcmp(name, ".text") == 0) {
            *fileOffset = sections[i].PointerToRawData;
            *size = sections[i].SizeOfRawData;
            return true;
        }
    }

    return false;
}

}  // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);

    if (argc < 3) {
        printf("использование: %s sweep|at <файл>\n", argv[0]);
        return 1;
    }

    const bool sweep = std::strcmp(argv[1], "sweep") == 0;
    const bool at = std::strcmp(argv[1], "at") == 0;
    if (!sweep && !at) {
        printf("неизвестный режим %s\n", argv[1]);
        return 1;
    }

    FILE* f = fopen(argv[2], "rb");
    if (f == nullptr) {
        printf("не открыть %s\n", argv[2]);
        return 1;
    }

    fseek(f, 0, SEEK_END);
    const long length = ftell(f);
    fseek(f, 0, SEEK_SET);

    std::vector<unsigned char> file(static_cast<size_t>(length));
    if (fread(file.data(), 1, file.size(), f) != file.size()) {
        fclose(f);
        printf("не прочитать файл\n");
        return 1;
    }
    fclose(f);

    size_t textOffset = 0;
    size_t textSize = 0;
    if (!FindText(file, &textOffset, &textSize)) {
        printf("секция .text не найдена\n");
        return 2;
    }

    const unsigned char* text = file.data() + textOffset;

    if (sweep) {
        size_t offset = 0;
        size_t decoded = 0;
        size_t unknown = 0;

        while (offset < textSize) {
            size_t len = 0;
            if (!evcore::InstructionLength(text + offset, textSize - offset,
                                           &len)) {
                printf("НЕОПОЗНАНА %zu %02X\n", offset, text[offset]);
                ++unknown;
                break;
            }

            printf("%zu %zu ", offset, len);
            for (size_t i = 0; i < len && i < 16; i++) {
                printf("%02X", text[offset + i]);
            }
            printf("\n");

            offset += len;
            ++decoded;
        }

        fprintf(stderr,
                "распознано инструкций: %zu, неопознанных: %zu, "
                "покрыто байт: %zu из %zu\n",
                decoded, unknown, offset, textSize);
        return unknown == 0 ? 0 : 3;
    }

    // Режим at: читаем смещения со входа, отвечаем длиной для каждого.
    char line[128] = {};
    while (fgets(line, sizeof(line), stdin) != nullptr) {
        const unsigned long offset = strtoul(line, nullptr, 10);
        if (offset >= textSize) {
            printf("%lu ВНЕ\n", offset);
            continue;
        }

        size_t len = 0;
        if (!evcore::InstructionLength(text + offset, textSize - offset,
                                       &len)) {
            printf("%lu FAIL\n", offset);
            continue;
        }

        printf("%lu %zu\n", offset, len);
    }

    return 0;
}
