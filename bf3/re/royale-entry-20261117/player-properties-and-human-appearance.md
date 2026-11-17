# Живые Player.teamId/squadId и внешность человека, 17.11.2026

Изменены только `VextPlayerManager.cpp` и небольшой read-only helper в `SpmProbe.cpp`. Сохранены существующие Player:Created/Fade/spawn правки родителя. Backup исходного состояния: `C:/Games/backup/2026-11-17/royale-spawn/VextPlayerManager-team-properties-pre.cpp`, `SpmProbe-team-properties-pre.cpp`. Мод не редактировался. Сети, запусков/остановок игр и записи в процессы дочерний агент не выполнял.

## Команда и отряд

Прежние поля `teamId`/`squadId` находились в Lua table; любые присваивания обходили `__newindex` и не меняли человека в движке. Теперь публичные raw keys отсутствуют. Геттер читает native+31C и squad component+[F4], setter вызывает native manager и проверяет результат. `_teamId/_squadId` — только fallback до привязки и снимок; pending присваивания бота применяются при nativePtr. Снимок наблюдения team-change отделён от live getter, прежний poll не делает повторный setter по уже прочитанному native состоянию.

VU server binding: регистрация1098DCC9/1098DDAA; getter1098C9E0→player+31C, getter1098C9B0→player+784 (retail component690+F4). Setter109904E0 вызывает manager vtable+C (core10524E30), setter10990240 — vtable+8 (10524A90, force=true). Клиентские107CD4F0/107CD4C0 имеют другой squad offset46C и здесь не используются.

Dedicated manager: addToTeam118EF80 cdecl(player,team,keepOrAutoSquad,checkTeamCapacity). Используются flags0,0: явная команда без автоподбора, снятие старого squad. Native проверяет fullint team против TeamSize count, вызывает E207A0, затем118D590(oldTeam). VU custom manager снимает старый squad перед своим SetTeam; stock последовательность сохранена, полный перенос всех VU сообщений и балансировки не заявляется. addToSquad118D8D0 cdecl(player,squad,allowPrivate,flag) вызывается с1,0, диапазон0..32 доказан сравнением118DBA7. Native getter11714C0 читает component+F4. Верхний team limit128 разрешён только при установленном TeamCapacity, затем проверяется фактический end-begin текущей таблицы.

Перед вызовами: batch signatures по файлу и runtime, реальный server frame, engine TLS, текущий player list, точное имя, диапазоны, level-transition gate. Native context сохраняется/восстанавливается, включая SEH. Native refusal не выдаётся за успех. После SEH повторные мутации отключены. SpmProbe helper принимает изменённый entrypoint только при владении соответствующим собственным InlineHook: Target/Detour, E9 destination, NOP padding, исходные stolen bytes в trampoline и остаток тела должны совпасть. Чужой detour не принимается. Вызов идёт через текущий entrypoint; TRACE hooks не обходятся.

## SelectUnlockAssets для человека

Подтверждённый пробел: BRPlayer:SetAppearance находил настоящие kit/appearance и вызывал Player:SelectUnlockAssets, но мост отправлял выбор в очереди ботов по имени/nativePtr. У человека реальный адрес находился в `_humanPtr`; handler выбора не вызывался.

Теперь human branch передаёт исходные native DataContainer userdata напрямую отдельному bridge. Проверяется настоящий текущий human player, frame/TLS, тип kit CharacterCustomizationAsset, тип элементов UnlockAssetBase, owned reference либо текущий partition/GUID каждого borrowed asset. Никакого поиска/подмены по строковому имени. Все unlock-permission проверки проходят до изменения, чтобы отказ одного asset не очистил прежнюю внешность. Пределы:64 visual,37 misc (native misc limit). Отключение `EVBOTS_SELECT_ENGINE=0` возвращает ошибку, fallback raw writes отсутствует.

VU1098EB70 создаёт NetworkPlayerSelectedUnlockAssetsMessage размер48; kit+38, visual array+3C, misc array+40.1098F68E синхронно вызывает native handler,1098F69A/69E немедленно уничтожает/освобождает сообщение. Dedicated E331B0 использует эти три поля, копирует visual array в player+4B8/+4BC, устанавливает kit через8C2660 с native retain/release, ставит replication dirty40 (E3324B..53), обрабатывает misc/оружие/customization. В нашем прямом вызове метода сообщение и fb::Array headers находятся на стеке; header события не используется, сообщение не передаётся async dispatcher. После возврата проверяются selectedKit+90 и точное совпадение всего visual vector с запросом. Silent refusal выдаёт Lua error; local metadata обновляется только после native успеха. Никаких искусственных client-showcase событий не посылается.

## Проверки и границы результата

- `extract_player_team_fixture.py`: точные PE bytes + Capstone instruction boundaries для118EF80,118D8D0,11714C0,E331B0,E145A0 PASS.
- `build_player_team_properties.cmd`: оба production cpp собраны MSVC x86 /O2 /MT /W4; Lua syntax `chunks1 bad0`.
- Extracted production guards/helper выполняются в отдельном x86 fixture:43 PASS. Team20: known trace ownership, foreign detour/trampoline refusal, signatures, live reads, team127/squad32, args, repeated no-op, frame/TLS/list/identity, transition/storage limits, native refusal, SEH restore/no retry. Human23: arrays/count prefix, повторный выбор, native kit/visual readback, denied unlock до mutation, signatures/human/env/ranges/transition, owned/borrowed asset liveness/type, silent readback failure, SEH restoration/no retry.
- В fixture engine absolute addresses перенесены на+20000000 в собственной памяти процесса, чтобы не конфликтовать с Windows low heaps. Только native-call boundaries/engine lookup заменены тестовыми stubs. Это проверка фактического адаптера, а не исполнение настоящего игрового handler и не доказательство сетевой репликации.
- Actual Lua functions: repeated assignments не создают raw public keys; pending применяется при привязке; native getter видит внешние изменения; invalid enum/off-frame/refusal не кэшируются как успех. Human selection идёт напрямую, повторяется и не изменяет metadata при отказе; bot fallback не вызывается.

Исходник передан родителю готовым к общей сборке. Живой client showcase/Ready и replication требуют следующего штатного родительского прогона.

## Исправление по живому F818D265

В05:54:58 parent run PID13220 выявил signature118D8D0 FAIL; остальные четыре подписи PASS. Read-only RPM подтвердил штатный hook: entry118D8D0 `E9 4B F5 48 6D 90 90 90 01 00 00 80 38 00`; trampoline04D80000 `A1 D8 D7 B5 02 81 EC B8 01 00 00 E9 CB D8 40 FC`. Причина в нашем helper: он ошибочно ожидал NOP до stolen11, хотя ev-core EvHook.cpp атомарно пишет ровно8 байт, сохраняя live bytes8..10. Предыдущий fixture повторял эту неверную модель; его успешность не доказывала совместимость с реальным ev-core.

После backup `SpmProbe-hook8-pre.cpp` проверка исправлена: NOP5..7, оригинальный live tail начиная+8, весь stolen prefix в trampoline и обязательный E9 back→entry+stolen. Владение InlineHook и точный detour сохраняются; чужие переходы не принимаются. Fixture моделирует фактический8-byte patch и дополнен отказами при повреждении живого непереписанного tail и trampoline backjump. Итог45 native PASS (Team22+Human23), оба Lua fixtures PASS, production compile. Cached runtime отказ не обходился; требуется штатная новая сборка/перезапуск родителем.
