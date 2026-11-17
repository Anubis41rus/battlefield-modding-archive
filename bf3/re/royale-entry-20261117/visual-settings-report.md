# Две визуальные команды VU — 17.11.2026

Реализованы `vu.ColorCorrectionEnabled` и `vu.SunFlareEnabled`. Изменения не затрагивают фон Showroom и не записывают одноимённые предполагаемые поля ServerSettings.

## Доказательство нативного поведения

Референс: локальный `vu-core.dll.ev-orig`, SHA256 `44F4DD2A55362AD63E7B5B4F060C8411A569544C3A0981BEA3591737F0B9F163`. Клиент: `zbf3.exe`, SHA256 `8201C5E14667D511F402C424D5C87A3A5ED2D9FDF81C67C914A145A9D3BC42B9`.

- ColorCorrection: handler `1018AA30` → singleton `101F3690` → setter `101F3840`. Менеджер читает native global `023B6844`, исключает имя `EffectEntity`, берёт runtime component по state+2C и пишет enable+3D / colorGradingEnable+3C. Затем dirty manager+18C0. Инициализация новых именованных состояний при выключенном флаге повторяет обе записи (`101F3580..101F35F3`). Наш код использует уже проверенный NativeVisualEnvironment для существующих состояний и его init hook для новых.
- SunFlare: handler `1018B9F0` пишет VU global `10874308`. Hook `10113FE0` пропускает native `01799C20` при выключении (ret4); hook `10114000` вместо native `0178BFD0` возвращает eax=0/retC. Оба адреса и 16 байт прологов сверены с zbf3. Установка нашей пары сначала проверяет обе сигнатуры; отказ второго hook откатывает первый. Trampoline доступен обработчику уже при публикации detour.
- Серверные handlers `1018AA70` / `1018BA80` используют собственный реестр VU: setter `10189450`, value+11A4 / flags+11B1..11B2. Это реплицируемая настройка расширения, не поле native ServerSettings.

## Интеграция

Новые файлы: `NativeVisualSettings.cpp/.h`, `NativeVisualSettingsBridge.cpp/.h`. В `NativeVisualEnvironment.cpp` добавлен только вызов OnStateCreated после native init и регистрации состояния. Бэкап существовавшего исходника: `C:/Games/backup/2026-11-17/NativeVisualEnvironment-before-visual-settings.cpp`.

Вызвать `evbots::RegisterNativeVisualSettings(L)` после регистрации Events, NetEvents, RCON и инициализации NativeBlueprint на обеих сторонах. Дополнительные вызовы frame tick не требуются: bridge подписывается на Engine:Update и Client:UpdateInput. Нативные клиентские hooks устанавливаются на первом применении snapshot в разрешённом клиентском кадре. До этого сохраняется штатное enabled=true. На сервере hooks рендера не устанавливаются.

Сервер хранит два bool, revision и идентификатор процесса/сеанса. Мутатор передан только приватному серверному chunk и дополнительно требует настоящий серверный frame либо LoadResources. RCON требует RequiresLogin; принимаются только строки true/false. Клиентский запрос несёт только version=1, валидируется точная арность. Его обработчик отправляет текущий snapshot и не вызывает setter. Клиент проверяет типы, версию, revision, session; старые revision одного сеанса отбрасываются. Новый session сбрасывает отметки применённых значений.

Клиент запрашивает snapshot при регистрации, Level:Loaded и каждые пять секунд для восстановления после reconnect или слишком раннего hello. Применяет только изменённые компоненты; ошибки готовности оставляют pending для следующего кадра. Новые VE states наследуют выключенный ColorCorrection. Состояние bridge сохраняется в Lua registry при повторной регистрации; старые Event/NetEvent handles и RCON handlers снимаются. Регистрация временно устанавливает пустого engine owner и восстанавливает прежнего владельца после lua_pcall, включая ошибку.

## Проверки

- NativeVisualSettings fixture: **20 PASS / 0 FAIL**, MSVC x86 `/W4 /WX`. Обе реальные runtime flags, EffectEntity, dirty, новый state, запрет вне кадра, сигнатуры, аргументы и return обоих SunFlare hook.
- Реальный embedded Lua transport: **29 PASS / 0 FAIL**. Запрос/ответ, команды/query, malformed payload, неизменяемость сервера через запрос, stale revision, новый session, reload без дубликатов, Level:Loaded, reconnect request, retry и маска только изменённой настройки.
- Production NativeVisualSettingsBridge.cpp отдельно компилируется x86 `/W4 /WX`.
- Сетевой транспорт тестируется его настоящим Lua chunk с doubles NetEvents/Events/RCON; TCP и настоящий кадр рендера этим fixture не проверяются. Проверка применения в живом клиенте остаётся частью общего прогона родителя.

## Дополнительная граница TextureAsset

В working VEManager перед приведением результата SearchForDataContainer требуется ненулевой `ResourceManager:FindPartitionForInstance`. Наш binding возвращает partition только для настоящего LiveDataContainer; offline EBX proxy возвращает nil и идёт по существующему runtime fallback. Штатный stand уже включает live search и по умолчанию не возвращает offline proxy, но проверка защищает также режим EVBOTS_OFFLINE_SEARCH.

Actual LoadPresets fixture расширен offline userdata proxy: найденный proxy не присваивается native полю. Оба режима (с настоящим runtime fallback и без текстур) проходят: **14 входов, 2871 типизированная запись, 14 endValue**, без TextureAsset(nil). Ранее проверенные режимы foundNative / missingNative остаются действительными. Исходные архивы и fun-bots не изменялись.
