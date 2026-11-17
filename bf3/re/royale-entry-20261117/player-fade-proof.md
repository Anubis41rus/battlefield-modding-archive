# Player:Fade — проверка 17.11.2026

Исходники и бинарники локального LAN-стенда, без изменений exe.

- Royale Server/__init__.lua:361 вызывает Player:Fade(1.0,false) при PlayerConnected именно для показа showroom. В EVBots метод был log-only.
- VU регистрация Fade: строка177F519C, ссылка1098E5DA, функция1098B910. Получает соединение через слот18E750F8 (A426A0), отправляет NetworkScreenFadeMessage через18E75068 (A31930). duration+38, toBlack+3C.
- Dedicated FindConnection D6E1C0 перебирает server+2FF8..2FFC, сравнивает player с результатом E3B0E0 (connection+1924), ret4. Это тот же алгоритм, что клиент A426A0, с отличающимся смещением вектора.
- Dedicated Send E4DE80 подтверждён через парные цепочки broadcast: клиент A42C00→A42B90→A31930; сервер D6E9E0→D6E970→E4DE80. У dedicated дополнительная проверка типа сообщения, поэтому точная сигнатура клиентской функции не подходит. Используется штатный dedicated ctor22FF0D0 и dtor1A0D980, уже проверенные FadeAll.
- В EVBots все пять входов проверяются по16байт до первого вызова; серверный поток, проверка длительности, EVBOTS_NATIVE_FADE=0. При отсутствии игрока/соединения false; перехода на broadcast нет.

Предыдущий запуск DLL74F99D67: клиент PID13064. Native DebugRenderSettings.enable=0 после исправления writable-настроек. READY остаётся до нажатия, но пользователь видит чёрный фон. ReadProcessMemory подтвердил ReferenceObjectData DD24FCC0 BlueprintTransform=(470.018,173.260,-978.173) с правильными осями; CharacterBackdrop E7EC4270 и MenuBlackCover DCDCFED0 Enabled=0. ShowSoldier UIGraphEntity EEBD8660 существует, граф содержит5узлов/4связи с корректными указателями портов и actionKey. Это не доказывает успешное выполнение UI action или видимость солдата.

DLL3D737A8524924ED1B15674E99435013EA15B2F721E7FA0EA3750733880D46D6B: сборка успешна, Lua chunk1 PASS, core96 PASS. Визуальный эффект требует следующего прогона.

## Второй разрыв цепочки — Player:Connected

В прогоне03:46 подключение прошло, но ScreenFade не вызван. Клиентская реализация вообще не преобразовывала ClientPlayerConnectMessage в Player:Connected. Royale отправляет серверу PlayerEvents.PlayerConnected именно из этого обработчика; без него серверный Fade недостижим.

Проверено по VU10C0F710: категория24FE118F, Connect69E0643A требует byte[msg+2C]!=0; ClientPlayer[msg+28], id[player+3EC]. VU подавляет повторный Connected для того же id. Deleted608997B9 отправляет Player:Deleted и убирает id из реестра. В EVBots добавлено чтение сообщения под SEH, копируется только id; события доставляются после нативного кадра через FIFO, с подавлением повторов до Deleted и новым Lua-объектом после удаления. Тест проверяет отложенность, порядок, дубли и повторное подключение.

DLL320482ADEF6505F5BD85C5A70D7182956BA1F6A2CA61808CC009D063BE658B1D: core97 PASS, Lua1 PASS. Следующий запуск проверяет всю цепочку.

Подтверждено03:55:47 Connected native id0; первое событие мода VuBattleRoyale:PlayerConnected.03:56:47 импорт человекаF2DDAAF0, пять сигнатур PASS, два targeted FadeIn1с. Скриншот50480c17 пользователя: READY, боец и карта видны, ярко-синее небо остаётся. Ранний чёрный HUD был промежуточным состоянием загрузки; порядок событий не исправлялся повторно. Тесты стенда отключены профилем Royale (SKIP), core97 не является проверкой полного матча.
