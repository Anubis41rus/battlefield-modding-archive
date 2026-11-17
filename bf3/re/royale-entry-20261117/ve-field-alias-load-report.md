# VU aliases и полная проверка полей LoadPresets, 17.11.2026

## Живые ошибки и общий resolver

Клиент4620, DLL F818D265: 05:54:55 `FogComponentData: нет поля endValue`,
затем 05:56:26 Quit → `ActionNode: нет поля inValue`.
В VU Lua API зарезервированные имена получают суффикс Value, тогда как
native reflection сохраняет исходное имя. `ResolveNativeField` добавлен
в `VextResourceManager.cpp` для обоих Live_Index и Live_NewIndex:

| Lua API | Native reflection |
|---|---|
| endValue | End |
| inValue | In |
| trueValue | True |
| falseValue | False |
| repeatValue | Repeat |

Сначала выполняется прямой поиск: настоящее поле InValue и подобные не
перекрываются alias. Существующая LinearTransform.left→right сохранена.
Произвольное удаление суффикса Value не применяется; outValue не придуман.
Имена в TypeInformation.fields остаются native, как в VU.

Доказательства: read-only обход живого TypeInfo списка клиента4620
(head022EA470,6531 типов), документация `VU-Docs/content/vext/ref/fb`.
В20 используемых VE ComponentData262 объявленных поля; единственное
расхождение имён — Fog.endValue. Дополнительный поиск всех documented
reserved aliases дал70 классов/376 полей, включая ActionNode,
InstanceOutputNode, BinaryLogicNode, AwardData. UINodeConnection:
все5 имён совпадают, дополнительный alias не нужен.
Объединение схем:89 классов,618 полей/свойств.

VEXT FogComponentData регистрацию endValue подтверждает12B3DF12..DF3C,
getter110B1DD0/setter12B3EA10; setter12B3EAA6 пишет native+9C,
что совпадает с живым offset поля End. Runtime FogData — отдельная
структура, её endValue по+3C ранее независимо доказан.

`test-field-alias.cmd`: настоящий extracted resolver,618 API-свойств,
приоритет native InValue, LinearTransform.left и отказ неизвестного
outValue — **621 PASS,0 FAIL**. Production VextResourceManager.cpp
скомпилирован x86 `/W4 /WX` успешно.

## Остальная цепочка VEManager

Изменена только working-копия VEManager; архив и fun-bots неизменны.
Backup до новой пачки:
`C:/Games/backup/2026-11-17/VEManager-client-before-default-field-name.lua`.

1. GetDefaultValue теперь применяет ту же нормализацию End→endValue,
   которую LoadPresets уже применял при записи. Без этого default читал
   runtime Fog['end'] и возвращал nil для числового поля.
2. В9 полях3 пресетов явно заданы TextureAsset пути. Старый parser
   безусловно падал `Found unexpected DataContainer`. Теперь он ищет
   настоящий загруженный ресурс через ResourceManager по точному пути.
   Если ресурс не загружен, используется существующая ветка
   GetDefaultValue с реальным runtime-значением и явной диагностикой.
3. Три CloudLayer2Texture-ветки больше не вызывают TextureAsset(nil)
   при отсутствии g_Stars. Если runtime-текстура существует, она
   сохраняется; если ни одного ресурса нет, записывается допустимый nil.
4. В OutOfCircleNightPreset.lua и XP5_003_Night_Three.lua исправлена
   опечатка `evels/Testrange_Lighting/Sky/SKY_03_Panorama_D` → `Levels/...`.
   Точное существование подтверждено локальным
   `Venice-EBX/Levels/Testrange_Lighting/Sky/SKY_03_Panorama_D.txt`.
   Обе исходные working-версии сохранены в backup/2026-11-17
   с суффиксом `before-texture-path.lua`.

FilmGrain.Texture=nil само по себе допустимо: VU property TextureAsset|nil.
Его диагностическая строка не являлась причиной аварии.

## Проверка всей загрузки

`test-ve-full-presets.cmd`, x86 `/W4 /WX`: настоящий LoadPresets из
working-мода, все20 классов/262 объявленных DC-поля из live reflection,
14 локальных JSON-входов, **2871 типизированная запись,14 endValue PASS**.

Native GetStates/чтение всех runtime-компонентов выполняют настоящие
NativeVisualEnvironment.cpp и VextVisualEnvironment.cpp над собственными
буферами fixture. Имена и offsets DC разрешает настоящий extracted
ResolveNativeField, backing schema взята из живого клиента. Проверены:

- явно заданные TextureAsset найдены;
- явные ресурсы и runtime-текстуры отсутствуют, nullable nil допустим;
- явные ресурсы отсутствуют, runtime-текстуры существуют: настоящий
  runtime getter отдаёт retained asset, он используется как fallback;
- g_Stars отсутствует во всех трёх случаях, TextureAsset(nil) запрещён;
- Fog.End fallback читает проверяемое native runtime значение2700.

Граница fixture: native DC конструкторы/копирование ссылок и EntityManager
заменены строгими doubles; это проверяет все имена/типы/ветви Lua и
настоящие runtime getters, но не заменяет live создание entity.
Наследуемые поля начальной настройки component не входят в262declared.
Старый working-файл в том же fixture воспроизводит TextureAsset(nil).
Полный результат — `full-presets-test-output.txt`,
`full-presets-no-textures.txt`, `full-presets-runtime-fallback.txt`.

ShowroomModifier/фон лобби не изменены по последнему указанию пользователя.
Процессы не перезапускались и память живой игры не менялась.
