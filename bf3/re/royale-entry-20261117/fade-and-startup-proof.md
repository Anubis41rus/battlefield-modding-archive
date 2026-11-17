# Royale: загрузка расширения, камера и FadeAll — 17.11.2026

Прогон DLL282BD7C4A506A97B754251739933B04EE140709A3E63ABC0EC72852072CF0D27:
Engine:Init/Extension:Loaded 02:55:19, DebugRenderSettings=0. Первый кадр только
02:55:25. В 02:55:40 настройки уже F91B8BE0. Royale DisableDebugRenderer при nil
возвращается, а AntiCheat позже читает enable=true и посылает {"Wallhack"}.
В 02:57:28 исправленный print впервые показал именно этот аргумент.
Решение: Engine:Init/Extension:Loaded в первом завершённом кадре, перед установкой
resource hooks и Update. В тестовом хосте события остаются синхронными.

GunshipCamera.lua создаёт CameraEntityData, записывает LinearTransform(), затем
передаёт прочитанное data.transform в CreateEntity. Это vext.LiveDataContainer
shape=1, а старая обёртка принимала только table. Добавлены проверенные типы
LinearTransform/Vec3 нативных структур; прочие userdata и формы отвергаются.
transform-test.cmd извлекает фактические IsNativeStruct/ReadTransform из source;
реальный Lua54, моки только reflection/поля userdata: 12 PASS, включая сохранение
стека, native/table, неправильный тип/размер/shape, NaN/Inf.

## FadeAll: статическая сверка Capstone

VU veniceext.dll: FadeOutAll=10B99780, FadeInAll=10B996E0. Обе функции получают
контекст сервера и server=context+34, создают NetworkScreenFadeMessage на стеке
размером40, пишут duration+38 и boolean+3C (out=1/in=0), вызывают слот18E750FC.
Init слотов:18E74FBC=01F7EDB0 (ctor),18E750FC=00A42C00 (broadcast).

Выделенный сервер: ctor022FF0D0, broadcast00D6E9E0. Первый имеет те же id
C67D277B/25E68C3A, поля+38/+3C, vtable024E5648. Broadcast соответствует клиенту:
сначала локальный message manager, затем цикл соединений D6E970 (копирует/передаёт
сообщение в E4DE80); поле менеджера+304C вместо+3044. Контекст02B3EE2C,+34 также
подтверждён используемым исходящим чатом проекта. Базовый деструктор01A0D980:
отпускает ссылку+30, вызывает Message cleanup05306A0, освобождает память только
при flags&1; для стека передаётся0.

В коде проверяются16байт всех трёх entrypoints до любого вызова. Вызов только
на серверном кадре; проверяются duration и база exe. EVBOTS_NATIVE_FADE=0 отключает.
Подмена Lua-заглушкой или NetEvent не используется. Сигнатуры3/3 совпали с exe.
Игровая проверка нового пакета на момент записи ещё не выполнена.
