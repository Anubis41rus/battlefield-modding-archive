# VEManager: сбой enum при загрузке пресетов, 17.11.2026

## Причина по живому логу

Клиент PID 9380, 05:30:20: `VEManager-client.log` сообщает
`Found unhandled enum, CharacterLightingMode`, затем
`Failed to fetch original value: CharacterLighting | CharacterLightingMode [2]`.
В `EVBots-client.log` исключение `__init__.lua:450`,
`bad argument #3 to 'newindex' (number expected, got nil)`.
Последующие ошибки `Reload:298 entity nil` — следствие незавершённого `LoadPresets`.

`XP5_003_Default.lua` содержит пустой объект `CharacterLighting`.
Старый `GetDefaultValue` немедленно возвращал nil для всех enum кроме Realm,
до обращения к `VisualEnvironmentManager:GetStates()`.
Цикл загрузки после этого присваивал nil числовому enum-полю.

## Проверка контракта VU

Использована локальная `veniceext.dll`, SHA256
`C34AD1823F662B1A7515AE28D0F646D633F9B8F2B7AD6DD1E9DF7922FD912EE8`.

- Регистрация `TypeInformation` — 10A96BC0; `enum` привязан к virtual+14.
  Реальный getter 10A96A20 проверяет `(flags & 0x1F0) == 0x80`.
- `fields` — 10A96A70: перечисляет объявленные FieldInfoData класса,
  число полей берёт из TypeInfoData+0D. Фильтра enum-полей нет.
- Регистрация `characterLightingMode` — 11B1895B:
  getter 11644880, setter 11B193C0, строка 177E34C0.
- Основной property dispatcher 11B13910 выбирает setter adapter
  11B0BAE0 → 11B0BD20 → 11B0B890. Последний вызывает lua_type,
  требует тип 3 (number), иначе вызывает обработчик ошибки 11B17930;
  лишь затем читает integer и вызывает setter.
- Альтернативный callable-property wrapper 11B16230 → 11B0F2C0
  также проверяет number через 11B0CF90.
- Setter пишет enum по ComponentData+80. Runtime CharacterLightingData
  имеет другое смещение +20; наш runtime binding использует +20.

Наши enum/fields и отказ писать nil здесь соответствуют VU.
Разрешение nil→0 в ResourceManager было бы несовместимой маскировкой дефекта.

## Изменение

С разрешения родительского агента изменена только рабочая копия:
`D:/Games-cache/royale/working/Mods/VEManager/ext/client/__init__.lua`.
Исходный архив мода и fun-bots не изменялись.

Backup: `C:/Games/backup/2026-11-17/VEManager-client-before-enum-default.lua`,
SHA256 `AD4F5C73CDA707F573C29F60C21AEE505784100E6474AF7CA4D178D7DD8BBC7B`.
Исправленный SHA256 `BD75E680E123523FE9206540E8589D90661CF708B504E2F53CB0E73EF706829D`.

Вызовы на строках 404 и 443 передают новый `ComponentData` в `GetDefaultValue`.
Функция на строке 511 сохраняет специальный Realm_Client, а другие enum читает
тем же путём из runtime-компонента, что и остальные поля. Отсутствующий field
не завершает поиск преждевременно. Если runtime-значения нет, enum читается
из настоящего нового ComponentData. Отсутствие значения остаётся nil;
числовые константы вместо отсутствующих значений не подставляются.

## Проверки

`test-ve-presets.cmd`: x86 MSVC `/W4 /WX`, PASS.
`ve-preset-audit.py` разобрал 14 локальных JSON-входов (12 пресетов и два
примерных шаблона с одинаковым именем ve_base). Помимо Realm обнаружено 6
enum-полей: CharacterLightingMode, Dof.BlurFilter, два PlanarReflection
BlurFilter, ScreenEffect.FrameType, Tonemap.TonemapMethod.

Не заполнены, кроме Realm:

- OutOfCircleNight: CharacterLightingMode;
- XP5_003_Default: CharacterLightingMode, BlurFilter, TonemapMethod;
- XP5_003_Night_Three и XP5_003_Night_Two: CharacterLightingMode, BlurFilter.

`ve-preset-fixture.cpp/.lua` исполняет настоящий `LoadPresets` из рабочего
файла. Runtime GetStates и чтение enum выполняет настоящий код
NativeVisualEnvironment/VextVisualEnvironment над собственными буферами
тестового процесса. Native enum-значения специально ненулевые: 1/2.
Все 14 входов и 180 результирующих enum-полей прошли, проверены значения
XP5_003_Default, fallback из DC=7, отсутствие значения=nil и Realm_Client.

Ограничение fixture: поля reflection ограничены enum, конструкторы DC и
EntityManager — тестовые doubles со строгой проверкой числового setter.
Это проверка реальной Lua-логики и native runtime enum, не утверждение
о полном живом создании всех пресетов.

Тот же fixture с `VEMANAGER_TEST_SOURCE` равным backup воспроизводит старый
сбой на OutOfCircleNight (`enum rejects nil: characterLightingMode`).
Исправленная версия возвращает exit0, старая — ожидаемый exit1.
Lua-файл целиком загружается и исполняется Lua5.4 fixture, синтаксис проверен.

Игровые процессы не запускались, не останавливались и их память не менялась.
Live-подтверждение PresetsLoaded и создания entity должен выполнить родитель
при следующей штатной загрузке рабочего мода.
