# Royale: настоящий VisualEnvironmentManager, 17.11.2026

## Результат

Созданы только четыре новых файла EVBots: `src/NativeVisualEnvironment.cpp/.h` и `src/VextVisualEnvironment.cpp/.h`. Интеграцию регистрации и полной сборки выполняет родительский агент. Клиент и сервер этим исследованием не запускались и не останавливались; память игровых процессов не изменялась. Интернет и MCP не использовались.

Реализованы native `GetStates`, `GetDirty`, `SetDirty`, `Get/SetSunRotationX/Y`; именованные состояния с изменяемыми `visibility`, `priority`, `overrideVisibility`, `excluded`, `activeEffectPosition` и неизменяемым `stateId`. Состояния и компоненты представлены отдельными userdata: это **не DataContainer**.

В 21 runtime-компоненте доступны 249 документированных полей. Числа, bool, enum и целые векторы читаются и записываются в реальную память компонента. TextureAsset/SurfaceShaderBaseAsset читаются с удержанием настоящего DataContainer; удержанная ссылка передаётся ResourcePushOwnedLive. Возвращаемый Vec2/Vec3/Vec4 — снимок значения. Для изменения надо присваивать вектор целиком, как делает Royale CircleEffects.

Реализованная часть нужна реальным путям мода:

- VEManager: LoadPresets → GetDefaultValue → runtime state/component defaults; UpdateVisibility → state.visibility + SetDirty.
- Royale CircleEffects.lua: fog.start/endValue/curve + SetDirty. Изменение Vec4 целиком проверено тестом.
- VEManager/modules/time.lua: SetSunRotationX/Y вызываются в том числе при RemoveTime/ResetSunPosition, поэтому включены.

Поиск по Lua/JSON VEManager и vu-battleroyale не нашёл обращений shaderParams/ShaderParams. Ручные конструкторы состояний, AddState/RemoveState, shaderParams, замена component-pointer/asset-pointer и вложенное редактирование `component.color.x` не реализованы. Не выдаются за работающие; неподдерживаемая запись вызывает явную ошибку. Полная эквивалентность всем API VU не заявляется.

## Артефакты и SHA-256

- zbf3.exe: `8201C5E14667D511F402C424D5C87A3A5ED2D9FDF81C67C914A145A9D3BC42B9`.
- veniceext.dll: `C34AD1823F662B1A7515AE28D0F646D633F9B8F2B7AD6DD1E9DF7922FD912EE8`.
- vu-core.dll.ev-orig: `44F4DD2A55362AD63E7B5B4F060C8411A569544C3A0981BEA3591737F0B9F163`.

Адреса ниже — VA исходных PE с ImageBase 00400000 для клиента и 10000000 для DLL. Выполнен статический разбор pefile/capstone, а не наблюдение работы оригинального VU.

## Доказанный native layout и жизненный цикл

- vu-core 101F3650/101F3670 читают global `[023B6844]` и пишут/читают byte manager+18C0 для SetDirty/GetDirty.
- zbf3 01795CF0 добавляет state в vector manager+20/+24; назначает state+0C глобальный ID из 023B68D8. 01785A90 удаляет state из этого vector, заменяя удалённый указатель последним.
- Начало manager — CRITICAL_SECTION. 0046FF80 и 0046FF90 передают ECX в IAT 023BD70C/023BD708, соответствующие EnterCriticalSection/LeaveCriticalSection. Чтение и изменение держат тот же lock.
- State: +00 float visibility, +04 int priority, +08 byte overrideVisibility, +09 byte excluded, +0C int stateId, +10 Vec3 activeEffectPosition. Подтверждение: VEXT getters 1085F350, 1085EC30, 1085EB70, 1085E7F0, 1085F130 и setters, регистрация 1085F410.
- Runtime component pointers: outdoorLight+20, enlighten+24, tonemap+28, colorCorrection+2C, sky+30, fog+34, wind+38, sunFlare+3C, dynamicAO+40, dof+44, vignette+48, filmGrain+4C, lensScope+50, cameraParams+54, screenEffect+58, damageEffect+5C, planarReflection+60, dynamicEnvmap+64, characterLighting+68, motionBlur+6C, debug+B0. Смещения извлечены из именованных VEXT getter bindings, а не из ComponentData.
- VEXT 10B61030 → singleton10C100D0 → 10C10150 копирует собственный vector VEXT wrappers; это не reflection DataContainer.
- VEXT 10C103BA удаляет wrapper с memmove. Поэтому снимок EVBots сортируется по поколению регистрации: native swap-remove не меняет порядок, видимый Lua. Это существенно для CircleEffects:GetVEState()[2].
- vu-core 100F57E0 устанавливает init hook на zbf3 00F77140 и destructor hook на 00F7A7E0. Detour10114A10 вызывает оригинал, затем получает имя из `initParams+80 → asset+08 → const char*`; при отсутствии asset использует точную строку `EffectEntity`. Указатель состояния — `entity+30`. Detour101149E0 удаляет регистрацию до вызова оригинального destructor. Эти ABI и порядок воспроизведены.
- Имена и поколения хранятся независимо. Любой доступ повторно проверяет manager, наличие state в native vector, ID и поколение. Пересоздание по тому же адресу не оживляет прежний Lua handle.
- Все семь сигнатур проверяются одной пачкой до установки первого hook; неудача второго hook откатывает первый. Env-off: `EVBOTS_VISUAL_ENVIRONMENT=0`.
- LuaRuntime регистрирует библиотеку на injection worker до первого кадра; регистрация не зависит от TypeInfo/TLS. Применение runtime API разрешено только из настоящего client EngineFrameActive. По текущему ModdingInterface.cpp первая загрузка ресурсов начинается после этой регистрации.

## Поля компонентов и солнце

`ve_schema.py` разбирает реальные инструкции регистрации свойств, локальные пары getter/setter, строки имён, а затем getters. Типы сверяются с локальными VU-Docs. Результат — `visual-runtime-schema.json`, содержащий имена, offsets, адреса getters/setters и доказательные инструкции. Для 227 полей отдельно разобран setter и подтверждено совпадение offset. У оставшихся 22 полей чтение подтверждено getter; среди них ссылки на assets с иной формой setter. Дополнительный VEXT debug.realm отсутствует в локальном документированном контракте и не включён в таблицу.

Например, Sky.brightnessScale: VEXT1090B5B9 `fld [eax+38]`; sky runtime pointer — state+30. Это разные смещения, их нельзя смешивать с SkyComponentData.

Регистрация библиотеки VEXT10B61150 связывает GetSunRotationX/Y с 10B610F0/10B61120 (+18B0/+18B4); SetSunRotationX/Y с 10B614B0/10B614E0 (прямая запись туда же). Сами эти setters не выставляют dirty. GetSunRotationOffsetX/Y читают +18B8/+18BC. В исследованной DLL Offset setters зарегистрированы на те же функции, что getters; исправление/эмуляция этого странного поведения не выдумывались. Offset API пока не включены.

## Проверки

- `build-visual-environment.cmd`: отдельная компиляция двух production cpp, x86 MSVC `/O2 /MT /W4 /WX /std:c++17`, PASS.
- `ve_verify.py`: настоящие 7/7 PE-сигнатур совпали; все 249 полей/21 компонент совпадают с извлечённым schema; 227 независимых getter/setter offsets совпали, PASS.
- `test-visual-environment.cmd`: fixture включает фактические production cpp; x86 `/W4 /WX`; **42 PASS, 0 FAIL**. Проверяет имя/ID/ABI init и destructor, имя EffectEntity, порядок после native swap, dirty, реальные state/component записи, целый Vec4 как у CircleEffects, sun calls как у Time, asset retain/transfer, устаревшие handles, неправильный кадр, env-off, некорректный vector и несовпадение каждого байта сигнатуры.
- Снимок vector<string> принадлежит Lua userdata с `__gc` до начала заполнения. Lua error, включая allocation longjmp при построении результата, не пропускает освобождение снимка. Native ReadField использует POD Value, а не std::string.

Это офлайн-проверки собственного адаптера. Настоящий клиентский запуск, применённые пресеты и исчезновение синего неба здесь **ещё не наблюдались**. Если GetStates встретит состояние, созданное до начала tracking, возвращается явная ошибка; неизвестное имя или успешный пустой список не подделываются. Следующий обязательный шаг — родительская полная сборка и живой клиент с проверкой семи PASS, имён состояний, Presets loaded, неба и следующей ошибки мода.
