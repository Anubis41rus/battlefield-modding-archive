# Royale: отказ Spawn и стандартное меню Esc, 17.11.2026

## Наблюдение

Прогон70D7, сервер320/клиент9380: после Ready05:31:27 GetIterator возвращает5 ServerCharacterSpawnEntity, но солдата нет. ReadProcessMemory находит нужный GUID67A2C146-9CC0-E7EC-5227-B2DCB9D316C1 у сущностиF17C89F0, данныеF8FF0FF0. Пользователь прислал скриншоты камеры под картой и штатного меню BF3 при Esc.

## Entity.data

NativeBlueprint.ReadEntity ошибочно переносил retail slot+1C на debug dedicated. У dedicated vtable025BBBC0 slot+1C=00E3BD10: `mov eax,[ecx+10];ret` возвращает EntityBus. Slot+20=00E3BD20: `mov eax,[ecx+14];ret` возвращает настоящие CharacterSpawnReferenceObjectData. Type check DataContainer корректно отвергал bus; Lua получалnil, сравнение GUID молча пропускало Spawn.

Исправление: server slot+20, client slot+1C; все прежние проверки lifetime, executable page и точной полной инструкции getter сохранены. Клиентские UIGraph vtable02143F00 и EventSplitter021AD108 проверены по PE: slot+1C читает+10. ДанныеSpatialEntity находятся+14, простогоEntity+10, поэтому единое фиксированное поле не возвращалось.

Доказательства: entity-data-probe.py (только чтение процесса), entity-vtable-proof.py (PE;3 точные проверки accessor). Дополнительно entity-data-test исполняет настоящую функцию ReadEntity и неизменённые getter-инструкции из SEC_IMAGE отображений обоих exe на собственных буферах:21PASS, включая ошибочный bus getter, stale handle и nil data. В NativePlayerEvents добавлен ограниченный лог принятия Spawn native handler; сообщение означает постановку события, не успешное появление солдата.

## EntityEvent.eventId

MathUtils:FNVHash возвращает signedint32 по локальным VU-Docs; Native EntityEvents выдавал uint32 Lua integer. EnterFromGame C5A88275:3316154997 вместо-978812299; ExitUIGraph9B97B3B5:2610410421 вместо-1684556875. Зарегистрированный EscMenu callback не совпадал ни с одной веткой и не отменял штатное меню.

Исправление: int32 при выдачеeventId, принятие signed32 и прежнего uint32 при его изменении и числовом FireEvent. За пределами32бит/дробные значения отвергаются. Внутри native хранится прежняя32-битная последовательность.

entity-event-test: извлекает действительные C++ блоки push/update, действительный MathUtils.FNVHash и EscMenu.OnEscapeMenuCallback;20PASS/0FAIL после включения аналогичного исправления ServerPlayerEvent numeric constructor/getter. Проверены обе ветки меню, cancellation=false, roundtrip, границы и отказ некорректных чисел. Живое меню после новой DLL ещё предстоит проверить.
