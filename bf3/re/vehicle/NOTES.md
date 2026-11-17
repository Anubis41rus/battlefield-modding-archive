# Техника: разбор для Player:EnterVehicle/ExitVehicle (16.10.2026, сборка 177+)

Источник: биндинги VU (`re\vu\bindings.txt`, `veniceext.dll`), таблица движковых функций VU (слоты `[0x18E74xxx]`
заполняются `mov [слот], адрес_bf3.exe` в 0x107027xx..0x107035xx; сервер VU = клиентский bf3.exe, расшифрованный
`Battlefield 3\zbf3.exe`), наш `bf3_server_Final.exe`. Аналоги найдены по совпадению кода (правило 18), не по адресам.

## Player (смещения одинаковые у клиента и нашего сервера — проверено по коду обоих)
| VU-биндинг | VU (veniceext) | Поле/вызов |
|---|---|---|
| `attachedControllable` | 0x1098BCC0 | `[игрок+0x3D8]` (сущность, в которой сидит; номер места — `[игрок+0x3D4]`) |
| `controlledControllable` | 0x1098BD10 | `[игрок+0x3E8]` (солдат или машина) |
| `controlledEntryId` | 0x1098E0D1 | (смотреть; вероятно `[игрок+0x3D4]`) |
| `EnterVehicle(veh, entryId)` | 0x1098B770 | проверка типа `ServerVehicleEntity`, `entryId < ([veh+0xCC]-[veh+0xC8])/4` (клиент), затем `[0x18E74FD8]` = bf3.exe **0x00AB4EA0** (thiscall игрок, args: veh, entryId) |
| `ExitVehicle(a, b)` | 0x1098B8D0 | `[0x18E74FDC]` = bf3.exe **0x00AB4060** (thiscall игрок, 2 аргумента) |

## Наш сервер (сверено дизассемблером)
- **enterEntry = 0x00E37950** (thiscall: ecx = ServerPlayer; [esp+4] = управляемая сущность, [esp+8] = номер места; ret 8).
  Найдено через сообщение, которое шлёт функция: ctor VU 0x01F87480 (хэши 0x3E46CDF8/0xBE8B9F3B) ↔ наш **0x02308AB0**,
  единственный вызов — 0x00E37BE8 внутри 0x00E37950. Места: `[ent+0xFC..+0x100]` (у клиента +0xC8/+0xCC — у серверного
  класса сдвиг +0x34), assert «ServerEntryComponent::getEntry invoked with invalid index», занят ли — `[место+0x4C]` (игрок),
  прежний седок с isAI ≠ 0 выводится `exitEntry(1, 1)`.
- **exitEntry = 0x00E365F0** (thiscall игрок, 2 аргумента-флага; пролог 1:1 с VU 0x00AB4060: `[esi+0x3E8]`, `xor al,al`, если
  управляемой нет).
- Солдат: `[солдат+0x1590]` — компонент места, `[+0x1594]` — номер (0x00E3FE74, падение 173).
- Сущности техники: vtable ServerVehicleEntity 0x025BF8E8, тип 0x02C17090 (VextEntityManager.cpp, список для пробника).

## Что ещё нужно моду (fun-bots, по частоте)
`controlledControllable.transform` (62), `.physicsEntityBase` (23: `velocity`, `internalHealth`), `:Is('ServerVehicleEntity')`,
`:GetPlayerInEntry(i)`, `.entryCount`, `VehicleEntityData(e.data).controllableType`, `.partCount`, `.typeInfo.name`,
`.instanceId`, `.uniqueId`; итератор `EntityManager:GetIterator('ServerVehicleEntity')`; события `Vehicle:SpawnDone`,
`Vehicle:Unspawn`, `Vehicle:Enter`, `Vehicle:Exit`, `Vehicle:Damage`; NetEvent `Client:RequestEnterVehicle` (человек зовёт бота).
Смещения transform/скорости/здоровья у серверного класса — искать чтением живой памяти (`re\peek\`), клиентские не годятся.
