# Короткий аудит human Ready → ServerPlayerEvent

17.11.2026. Production-исходники не изменялись, запись в процессы не выполнялась.

**Доказуемых ошибок ABI/guard в Create/DispatchSpawn не найдено.**
Dedicated `bf3_server_Final.exe` F39F7623063CFDE9C449A0BDC9A57448D2D137D6DEF00B17EA04800EDA955470:

- DD8660: thiscall, 9 аргументов `player,id,bool×6,team`, ret24.
  Флаги пишутся последовательно +C..11, team+14, player weak+18. Наш вызов совпадает.
- DE03C0: thiscall onEvent(event*), ret4. Spawn-id DC320DE.
  Движок сам требует `[entity+14]->[+180]==0` и byte `[entity+D18]!=0`.
  Дополнительные проверки адаптера совпадают с native, не блокируют допустимый Spawn.
- Native queue на DE0471..0487 удерживает собственную player weak-ссылку,
  DDE6C0 увеличивает refcount; уничтожение Lua event после dispatch не отменяет queue.
- DD8720 destructor принимает flag0 и ret4; память Event принадлежит нашему new/delete.
- `player-spawn-abi-audit.py`: **15 PASS** по PE-байтам, layout/guards/ret/vtable.

**Риск следующего шага:** onEvent только ставит Spawn в очередь.
Обработка DE05D0 после enqueue требует `E144C0(player)`:
`player[49F] != 0 || player[318] != 0`; затем `E44EA0(point)` проверяет
native-условие точки (сравнение вычисленного значения с point+8C).
Лишь после обоих условий DE0751 вызывает spawn через virtual+6C.
Если условия ложны, запись остаётся в очереди, а `native Spawn submitted`
сам по себе не доказывает появление soldier. Флаги принудительно не менять.

Read-only проверка PID320: context F2CE0050, manager F2CC0100,
вектор humans manager+80/+84 пуст. PID9380 уже отсутствует; свежий лог
05:44:03 сообщает, что KOTuk1 не найден, heartbeat подтверждает людей0.
Старый playerptr из лога не использован после disconnect.
`player-spawn-gate-read.py PID` читает эти флаги у только текущих игроков;
для живого подтверждения нужен следующий вход. Запусков/остановок не делал.

## Signed eventId

Дополнительный аудит: `NativePlayerEvents.cpp` ещё выдаёт `params.id` как
положительный uint32 в EventIndex и отвергает отрицательный numeric ID в
CreateEvent. Это несовместимо с signed int32 MathUtils:FNVHash для high-bit
значений. Текущий Spawn=0DC320DE положительный, поэтому текущий Ready этим
не заблокирован. Родителю переданы точные места и предложение принимать
диапазон [-2^31, 2^32-1], хранить uint32-биты, выдавать int32.
Исходник не менялся. В Native/VextVisualEnvironment и VextClientRaycast
hash/eventId callback-путей нет.
