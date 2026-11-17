# Royale — точка продолжения после паузы 12.11.2026

## Текущий WIP267n: callback receiver, fullbuild выполняется

EngineResourcesSHA C169394515EB1E119AB5C75BB1E1361B0434A68202270D8A9B046ECAFBE6FDE8;
backup6E78 проверен. ONLY private callback metadata probe843+bounded
postmarshal callblock919, original-first unchanged. No native calls/memory
writes/retains/releases/allocator/CS; caps2048/64buckets/64nodes/256records.
Exact-source187x86checks0fail, wholeunit/W4/WXcompile0, metadata mocks.
Native CFG00585A70 independently344instructions/ret585EED/no gaps;
record+04receiver/vtable+10 geometry confirmed. Fullbuild backgroundbaayouuvd,
не poll/read taskJSONL; ждать notification. Build log на D reports:
build-267n-unresolved-callback.log. Prepared run-267n-unresolved-callback.ps1
requires ExpectedDllSHA and sourceC169, parserPASS, protected NoTests/NoJoin.
Новый build hash/PE identity/deploy/runtime ещё НЕ получены; activationC290.
Следующее действие: verify completedbuild, bounded run доfirstcallback,
identify actualcallee from snapshot then bounded static analysis. Не принимать
это за cleanmap/ownership proof. Code compensation/retains/arenaflags unchanged.

## История checkpoint267m: 12.11, 11:38

Build/activation full x86 DLL SHA256
C290021D0841F2C450805C2D237D5026DCB8503165D333442E564E4F2EAA6937,
buildexit0 и PE014C/010B/DLL проверены. Reference helpers/private human RCON
теперь входят в DLL; collector НЕ подключён к postmarshal/retirement.
EngineResourcesSHA6E78F6BC3A1A106801BC81D7C04E7CDEB324CB1E131E1F3DBC419122E77AB0EC.
Новое изменение — только passive ProbeDomainSerialization: native metadata
reads, без CS acquire/visitor/retain/release/allocator/writes. Exact-source
x869228checks/0fail и wholeunit /W4 /WX compile0; metadata mocks, НЕ игра.

Run267m offset26465600/293lines, first12 earlydomainF8E86370 snapshots:
manager=globalF8E85E30, gateF8E85E40, owner=currentthread441C,
recursion1/LockCount-2. Это early postmarshal CS observation, НЕ failingtarget
или normal/forced pre-retirement/quiescence/cleanmap. Stopfinally/noForce
11:38:11 exit0, native fault/VEH0, main client/server/backend0.
Штатный deploy backup evbots-before-113802.dll SHA01B91 проверен;
start transcript «server not ready: exited» после deliberate diagnostic stop
ожидаем, не готовность карты. Reports на D:\Games-cache\royale\reports:
run-267m-domain-serialization-log.txt, build-267m-domain-serialization.log,
domain-serialization-manifest.json, ebx-unload-gate-native-proof.txt.

Plan research завершён: activation пока НЕ обоснована. Следующие native gaps:
resolved tagged→direct→clear credit balance; ordinary+forced boundary/reset
serialization и manager lifetime; survivor current-field/destructor/native
alias cleanup либо полный lease backing И Level parent pages. Sidecar EBX
generations/tickets потребуются для borrowed/owned/nested wrappers, raw
GC/releases/deferred partitions, collision transfers, customization queues,
registry pins и clone/value owners. Не ждать LuaGate под manager CS.
Не включены retain-only/compensation removal/global arena2/sticky reset.
Rebind0057FB50/0057F980 уже fully decoded/inconclusive: не повторять.
Узкий read-only агент00585A70 завершён,8calls/noedits/game/net/install.
Slot bookkeeping64h nodes/10h-byte records не установлен proxy-owner. Fixup
сам добавляет+1 к lookup target00585D68; old tag release skips. Перед этим
00585D5A вызывает receiver=record+04 virtual+10 с copiedString(node+20).
Concrete callbackcallee/registration неизвестны — следующий точный вопрос.
Main SHA/7PE byte-spans independently verified; complete accounting всё
ещё не доказано, activeagents0. Source/DLL после267m unchanged.
Карта, actual humans/two-human match/new round и final installer ещё нужны.

## История checkpoint до267m: возобновлено 12.11

Новейшая source-правка ещё НЕ в full/deployed DLL: ReadReferenceTarget
EngineResources.cpp1273 читает DWORD[(raw&~3)+10], а НЕ адресproxy+10;
unresolved→0 безresolver/retain. VextResourceManager.cpp2030 class getter
используетtarget. CollectReferenceSlots cpp1294 вызываетnative0047C860/
0045B570, context/thunk DWORD BYVALUE, thunkfastcall/ret4/true; сохраняет
каждоеoccurrence, partialresultприfaultочищается. Native signatures verified.
Collector НЕ подключён кpostmarshal/retire, callerownsserialization, backingнеpin.
DcThawInspect всё ещё отвергаетtaggedreferences: copy/transfer не включены
безaccounting/cleanup. Refcount/compensation/arena flags без изменений.
Exact-source x86 /W4 /WX:7458assertions PASS,6ABIfixturecalls/52callbacks;
bothproductiontranslationunits compile exit0. Traversal/ready mocked, НЕигра.
Backups*-reference-slots-pre.*, строгийdiffPASS; reports/reference-slots-*
иtest-reference-slots.cmd. EngineResSHA82D18091EE995EE1ABDCBFBB09FF9305C85BBD3CDA6F38E631AB9EEDE986A755;
ResourceSHAED1AE5CEF8B819AC06B69A88FB9DB1ABB3A5E7936E25CF6CAFE921AB8A6BC5C0;
HeaderSHA0B3C5AC760CA50875A016A2D2528E5C92312614AF5C24368C1FAA50EF31E842C.
Activation/buildвсёещё01B91, independentnativeprocesses0, никакихagentsrunning.
Forced follow-upзавершён:005875D0–005877B4 destructor,00588340–0058835D
deletingwrapper,virtual[02460E38+04]. Доcore005875DD нет изменения+40,
возвратcoreдоmemberteardown,3PEspansmainпроверены. ВнешняяживостьCS,
lockinversion/quiescenceНЕдоказаны. Next: полныйper-occurrenceownership и
CURRENTfieldcleanup+invalidation доbackingreset, ordinary Иforced; неretain-only.
Чистаякарта/actualhumanacceptance/матчдвухлюдей/finalinstaller остаютсяоткрыты.

Последний адресныйcheckpoint267k: fullDLL01B91A002E2971E40FB2195BE31BAE0E040CAA149089C5EA13380EDE94E214BF,
EngineResEE1A42BF3D49F77D23368A665B7269F9971983C5508A610B465764F1C078F8BF,
Resource7B76E313585E9AC6EF2833166EE77373C3F85082EBEE89A35016355D7444349E.
ProbeInstanceArena1263/.h91 +precopyriskdiagnostic2795–2798; exactbackupdiff
только3добавления, refcount/compensation/nativebehavior без изменений.
Main exact-source x86mock24/24/444checks /W4 /WX PASS.
Run267k offset26278272/381строк: targetF4D66130/12edges/stateB1000002,
ownerF4D6615C, обаpartitionF47296C0/domainF4940670/contextF494072C;
parent0287F330flags1next0/headF52EFAD0/checkpoint0/blocksize4096/mode0.
Exit0 толькоobservation; не20edgefaulttarget/cleanmap. Stop07:31:32finally/
noForceexit0, processes0, activation01B91matchesbuild.
Run267l SAME DLL завершён native-fault/exit1, offset26328789/934строки.
20edge targetF51A84E0/B1000002 иownerF51A8F0C — partitionF445E7C0,
domainF06CEA10/contextF06CEACC,parent0287F330flags1next0,headF629BC20,
checkpoint0,blocksize4096,mode0. CopiedB1000016, AV00431CDB/read6A35269C
имеет *dst=тотжеF51A84E0/B1000000 иFStringF51AA350/pagearena0287F330.
Адресная связь сfault теперь доказана; чистойкарты нет.07:34:09finally/noForce
stop exit0; main nativeprocesses0 и activation/build01B91 совпали.
Native agent завершён:005216B0→00430320 вызываетregisteredsubobject0287F33C
черезvtable+04, но bulk teardown/reset/shutdown не доказан. Retain-only,
arena2 и компенсации не менялись. Следующий конкретный вопрос:
pre-retirement current-field cleanup до штатного domain reset без продления
недоказанного parent/page lifetime; read-only source/native investigation.

Private RCON evbots.ReadHumanState(name) теперь вsourceVextRcon.cpp370,
RequiresLogin|Hidden, без мутаций/кэша/сетевого fallback. Ответ5слов OK,
playerPtr/bodyPtr/generation/hasSoldier; приbody ещё12nativefloat компонентов
left/up/forward/trans xyz. Main x86wholeproductionRCON/realfrozenLua:
95новых+57regression PASS, loadedDLLpath иfloat32roundtrip проверены.
Boundaryhuman mocked; НЕ gameproof. SourceSHA7181B75EE627A4567C1B626BC51655DA0D1929D3C82416CB5BFA7C30C8C4FC7F.
Backup есть, strictdiff толькоstandalonecommand. FullDLL НЕ пересобрана,
activation01B91 этукоманду ещё НЕ содержит.
Native retirement commoncore00584ED0 thiscall(domain,force)/ret4, normal
wrapper00587530 удерживает[domain+40]+10 ипередаётfalse. Точнаяграница
00584F3B послеearlyexit,доpartitioncleanup; force обходитdomain&4.
Ноdestructor-call005875DD(force1) безлокальногоlock; externalserialization,
реентрантность иquiescence неизвестны. Main6точныхbyte-spans проверены.
Wrapper-only hook неполный; полныйownership/retirement пока НЕ включён.

Работа возобновлена по команде «продолжи». Текущее состояние теперь в STATE.md
и верхней записи MODLOG.md; остальные разделы этого файла описывают checkpoint
паузы и НЕ описывают нынешние исходники. MakeWritable materialization уже
реализована; NativePlayerEvents/NativeSoldierCustomization включены в общую DLL.
Актуальный267e SHA338EAB29018FC1F6311DA6A1CA602E5DBD2613AB43080D81480FB082FFA29117:
полная сборка и core/Compat/server-init exit0, thaw/ArrValidate201/201 (native mocks).
Прогон03:19 прежней DLL показал EventConnection copy/refcount AV, emitter=nil и
AddRegistry вне native registration. NULL-write гипотеза опровергнута:
StructInfo меняет локальный stack-slot; value copy снова с NULL, добавлен EIP.
Исправлены alignment16 cleanup/class-pointer alignment4 и приём Vec3-массивов.
Server-only267e04:43:10: Vec3 ошибка исчезла, EventConnection AV повторился,
EIP00431CDB (heap block-size). Срез reports/run-267e-log.txt, offset25870112.
Сервер/backend остановлены04:44:01 через finally/stop; клиент не запускался.
267f собрана SHA2C9E23C7DB9983E44A890D488F384E987AA6DE55DE9ABC412544A5C4449FA3F6;
201/201 actual-source harness с mocked native. Прогон05:07:44 доказал:
F598F010 references20/before2/copy22; компенсация возвращает2, erase вызывает
dtor02366650 при0 и пытается free frozen FStringF5990E80. Сервер/backend
штатно остановлены05:08:28; срез reports/run-267f-log.txt/offset25944435.
VU дополнительно retains все class-refs ПОСЛЕ original partition marshal;
EVBots этот проход пока не имеет. Server equivalent/teardown исследуются.
267g собрана SHA35DD969FE19F15AFF06C7A167B0FE7BB08F53998B0568F1066CE269971CA4382.
Прогон05:20:54 доказал Emitter+70 readable1/raw0, не tagged decoding fault;
сервер/backend остановлены05:21:05 после первого diagnostic. Source/clone
различие ещё не проверено, Object-pointer сравнение добавлено в source.
ModdingInterface false-ready90с убран: actual-lambda14/14cases/201assertions
с mocks и focused x86 compile PASS; full267g этот source fix ещё не содержала.
EngineResources AddRegistry изменён после static client/server lifetime proof:
Load/InstanceLoad/Register синхронно, active-check+registry/element snapshot
pins под native CS; exit cdecl(DWORD), очередь освобождения только после
успешного final1→0, не GC/Level:Destroy/nonfinal/fault. Focused compile PASS;
Registry harness после исправления найденной гонки:39/39 runtime mocks,
8source checks PASS. Initial267h SHA7579697D469B3CA911CA8CA9BEE44B0233CF779FA23F468E31F90D3216717DB2
не развёрнута и не содержит race fix. Final267h собрана exit0:
SHA8BDC18E17570CEFE216DDCCED1A521736A45A2E860D703705280E9E444F529FA,
recheck подCS, failed-remove state preservation, genuine-ready waiter,
Object source/copy diagnostic и read-only postmarshal gate probe.
Main x86 mocks: failed Remove7/7scenarios/101assertions PASS, probe29/29 PASS.
Missing postmarshal mapping готов: server00588780/visitor0047C860; ABI ret8 с
(context,thunk) по значению, thunk thiscall ret4. Native clear refs условен:
partition+B0 (client+5C) flag1 и domain+3C flag8 отсутствует. Без проверки
runtime-флагов retain всех рёбер НЕ добавлен, MakeWritable компенсацию не меняли.
Bounded run267h завершён: deployed8BDC, AddRegistry native success05:53:47.298;
12postmarshal observations partition01/domain7B/clearRefs0. Исходный Object
совпадает с clone ObjectF4F314E0, emitterF4F31680+70raw0 уже в оригинале.
Erase05:53:47.965 повторил00431CDB/read6B31FCFC; чистой карты нет.
Сервер/backend остановлены05:53:51, клиент не запускался/processcheck пуст.
Срез reports/run-267h-log.txt, offset26071884.
Новый static unload-gate report готов: skip8 может выставляться после marshal;
ordinary domain&8==0 fullwalk до destructor balances allslots независимо B0.
При domain&8!=0 оба walk skipped; безусловный retain не доказан balanced.
Компенсации/retains по догадке не менялись, выбирается точный ownership fix.
Lazy Lua-wrapper static report готов/прочитан: literal raw0→nil, не proxy;
tagged unresolved wrapper хранит GUID snapshot, handlers keyed GUID pair.
GC/handle safety неизвестны. Не подменять пустой emitter фиктивным proxy.
Emitter cause теперь установлен: FX_WreckFire_01 dedicated XP3 main SHA1
0fd49977c063fe5eea96b74db186bb80c8ae168c/3584/imports0; полный COOP_009/010
c4b2a8b04780f5cca7c52c33616b529a5ae7d599/3712/imports4 уже с зависимостями.
Удаление только stripped дубликата из generated XP3_Shield/Alborz реализовано.
RoyaleAssets source906FF08CDB778C9C1BBFD7CDFAAF001CB0734E4F337697E61569E7424B7907B9,
cache server-assets-csharp-v3; schema3/10generated, COOP/CAS/layout==v2.
Agent289C#PASS+Python28525entries/38bundles; main review+2SB hash matches,
actual main repeat289/289PASS после offline local-source restore, source/v2 unchanged.
Native terminal skip8 всё равно release instances/table и reset domain+BC:
005851A9→0042E950(0,0), потенциально VirtualFree/allocator-free; lifetime не доказан.
Main dtor02366650→FString0046B630 не проверяет0800, premark не достаточно.
Native source ownership не менялся. Узкий VU string cleanup follow-up завершён:
client0044AEC0 frozen/0800 не проверяет, hook00418290→101977C0 только trampoline.
Server allocator-free0042B190 имеет no-op при конечном allocator+4&2;
не доказано, что реальные frozen FString при teardown принадлежат ему и backing жив.
Layout существующий Data junction не перенаправляет. MainWindow3 source теперь
assets-v3/server-v3; common defaults server-runtime-v3/server-assets-csharp-v3/Data.
Caller backups сверены SHA256; PS parser и extracted path-prefix8/8 PASS,
launcher publish --no-restore exit0/package-launcher, GUI не тестировался.
Штатный CLI helper подготовил server-runtime-v3/Instance с Data→assets-csharp-v3.
В DcCopyFault добавлен только read-only EDX allocator table/chain/flags probe,
server traversal+8/client no traversal подтверждены PE, refcounts/guards не менялись.
Full x86 build exit0/DLLE3724C240FDD1A0D66515C4CC8741BC7544D763150BE65D33543FBABA674289E.
Probe source8496D60B17CFF316838E2FD68B0AC757EBDA07BDBA9373368A967A35CD3BCCFA;
main reviewed/reran exact-source x86 mock tests53/53PASS. Новый
run-267i-server-only.ps1 ограничен110с/finally-stop/noForce/noJoin и не затираетh.
Bounded run267i завершён native-fault/exit1; report run-267i-log.txt,
offset26156933/607строк, deployed SHA совпалE372. Original FX GUID callback:
все три Emitter readable1/nonzero и TemplateData materialized, nil устранён.
AddRegistry native success06:32:18.264; erase06:32:18.928 AV00431CDB/read6A35269C.
FStringEDXF51AA350→arena0287F330 flags1 next0 readable1: no-free bit2 отсутствует.
Не ставить bit2 на globalarena и не убирать компенсацию по догадке.
stop.ps1 finally/noForce06:32:22 exit0; remaining bf3/zbf3/server/backend0.
Новый native proof: current native destructors чистят нынешние owning fields,
но skip8 и externalowners перед backingreset всё ещё не закрыты. Server0042E950
thiscall(context,S,bool)ret8 — rollback, не detach; context domain+BC размер0x14:
parent+00/head+04/checkpoint+08/blocksize+0C/VirtualAllocmode+10.
00437A10 checkpoint12байт выделяется в этой же backing-chain; constructor
0042E920 не retain-ит parent. Нужен positive lifetime contract фактического
allocator/forwarding target/page metadata, не только сохранённая chain.
Postmarshal retain/compensation не менялись, неполный ownership patch не включён.
Independent human readback добавлен в NativeSoldierCustomization.cpp/.h и
VextPlayerManager.cpp: NativeBridge.ReadHumanState(name) возвращает nativebody,
weakgeneration и полный nativegetter transform, post Owner check; ни Lua cache,
ни mutating calls. Missingbody не смешивается с error. Backups сохранены,
fullDLLbuild exit0/SHA64FB6A8F18BB7DA5294D031F36DEC34D6532121234BE24ACCA9BD4466C48D907,
не развёрнута, activationE372. Main reviewed complete harness и independently
reran exact-source x86/realLua5.4.2:104/104PASS/2464checks; wrongmatrix
negative control exit7/noPASS.12фрагментов/currentC++/headers и frozeninputs
SHA совпадают; fullbuild пересобрал production lua.dll, test использует
неизменённую snapshot-копию. Engine/frame/Blueprint ABI mocked, не realhuman proof.
Новый read-only ProbeDomainArena EngineResources843/871, sourceSHA
01566F951F542ED7D2C19B1F3685015B29DBA8330B1789DEC7CD9DB38AEEF8C7,
читает parent actualdomain+BC; backup сохранён, refcounts/writes не менялись.
FullDLL267j exit0/SHA85A40B57F12913CE76382042E5A1CABADF7B5B85E22F2514D7569E977DD4F1E2,
до нового запуска не развёрнута; main reviewed/independently reran exact-source
x86 mockprobe478/478PASS/53231checks/28sourceassertions. Original-first/gate12/
fresh/error/no-memorywrite проверены. run-267j-domain-arena.ps1 parserPASS,
110с/noJoin/noTests/finally/noForce запущен через existing explorer entry;
run267j завершён exit0/domain-arena-observed; offset26242439/281строк,
report run-267j-domain-arena-log.txt.07:19:50.841 первыйearlydomainF8E86370:
contextF8E8642C,parent02880980,flags1,next0,headF8E8A920,checkpoint0,
blocksize4096,mode0. Это неactualdomain faulttarget и неcleanmap; остановлено
доModifier/erase.07:19:54finally-stop/noForce exit0; independent nativeprocesses0,
activation/build85A40совпадают. Старый E372 штатно backedup приdeploy.
StaticLevelarena0287F330 constructor00430850→004C3700;00430430 регистрирует
rawsubobject0287F33C в02880F30/count02881130, не ownership/retain.
Vtable0244A608+0C004C65D0 только individualfree; полное teardown иpageownership
02888158 lifetime неизвестны. Новыйnativepass004304E0unregister swap-last,
004C5570nonvirtualteardown unregisterthis+0C и192DWORDpageownershipclear
приthis+18!=0; actual0287F330Level-unload/shutdown неустановлены.
Не включатьretain-only/unsafearena2patch. Следующийsnapshot доcopy:
eng::PartitionOfInstance(riskyref/root)→partition+8→actualdomaincontext;
безglobalwalk/nativegetterвSEH/fallbackкearlydomain.
Human manualtransport естьRCON:RegisterCommand queue→Engine:Update,
privateacceptancehandler отсутствует; standaloneTestLuaTrue/Consoleнеподходят.
Server-onlydiagnostic был; client/game/autodeploy не запускались;
independent bf3/zbf3/server/backend0. Карта/матч пока не работают.

Уточнение к pause-human-spawn: вывод oldcount==0 был ошибочен. После XADD
JNE использует ZF результата сложения, освобождение при новом count==0.
Реализация вызывает штатный native destructor, ручной weak release не нужен.

Пользователь попросил остановиться и сохранить работу перед выключением VPN.
Агенты заморозили production. После просьбы новые сборки/тесты/игровые запуски
не выполнялись. Сервер/backend остановлены штатным stop.ps1 в 01:29:44,
клиент за эту сессию не запускался. Последняя проверка процессов — всё остановлено.

## Требование пользователя

Оригинальный BF3BR/VU-BattleRoyale, отдельно от FunBots, **строго без ботов**.
Маленький установщик, выбор режимов в общем лаунчере, сохранение аккаунтов
и статистики друга. У друга BF3 + BF-Portable, без дополнительных сетевых программ.
BF4 отложен; упрощённый самодельный Royale отвергнут. Пользователь на месте:
никаких автоматических кликов/дислокации. Лимиты кончаются — не повторять
общий аудит/UI-переделки/уже пройденные проверки без новой причины.

## Точный текущий блокер

Последний запуск **01:28:32**, server-only, отдельный
`D:/Games-cache/royale/server-runtime-v2`, Data v2. Только3мода: VuCompass,
VEManager, VU-BattleRoyale. По модулям: XINPUT9_1_0 из Windows, BotMod отсутствует,
dinput8 — отдельно проверенный LAN redirect. Надпись Royale означает init,
**карта/матч ещё не работают**.

В01:28:38 оригинальный WeaponSwitchingModifier.lua:16 вызвал
`switchMap[1].toWeapon:add(WeaponSwitchingEnum.wsSlot9)`.
DCguardPASS: scope1, TLS есть, Havok-index0, arena-ready1. Затем **SEH в native
Array resize**, `DataContainer: native fault; mutations disabled`, `native array
resize failed`. Причина пока не доказана. Не снимать guards наугад.

Logoffset **24628955** в `C:/Games/EVBots-dev/stand/logs/EVBots.log`.
Состояние `stand/logs/run-state.json`: readyfalse, testsfalse.
Native log: `D:/Games-cache/royale/server-runtime-v2/Instance/Logs/RuntimeLog_DESKTOP-0RFO867_server.log`.
Первый следующий шаг — точная причина resize, затем исправление и проверка.

## Собранная DLL и WIP — не путать

Последняя полная x86 DLL:
**91D220BB1D3F750C6765365221806DE133F750F9AD20E5B040E3D2C989F4A57D**,
MD5 24deaa2e04a364a3299c68901f029e2d. Копии evbots-dll/build, stand/activation,
D:/Games-cache/royale. Core7:
**253DD24B7EC302239C79DE23506E3CF6AF1438D10AFF91F4B887BB6E3C2B675D**.
Build log `D:/Games-cache/royale/build267-royale-dc-custom.log`, manifest
`dc-custom-build-inputs.json`; после сборки0изменений входов.
Проверки DLL: Compat35/35, original server initPASS, core-only89/89.

Вошли исправленный ранний DC TLS/MemoryArena guard, partition.instances,
native human ApplyCustomization и предыдущие resource/terrain/entity/spatial
исправления. Прогон выше — первая реальная проверка новых частей.

**Source после сборки изменён:**

- Root `VextResourceManager.cpp`: ArrResizeFault SEHfilter пишет code/EIP/
  this/field/count/function/access/registers. НЕ компилировался/не запускался.
  Бэкап `backup/2026-11-12/royale/VextResourceManager-array-fault-pre.cpp`.
- Human transform: `NativeSoldierCustomization.cpp/.h`, `VextPlayerManager.cpp`.
  Код компилируется /W4/WX, LuaPASS, harness48/48, Spatial75/75. Полной DLL нет.
  Незавершённый крайний случай: human SetTransform при `playerPtr(p)==nil`
  проваливается в старое локальное worldTransform присваивание; нужен явный отказ
  для потерянной human binding и regression resolver=nil. Общая FIFO с
  ApplyCustomization, полный matrix/weak/body/owner готовы. Rootинтеграция новых
  calls не нужна (использует существующие Start/Poll/Shutdown).
- Human Spawn/Unspawn: production НЕ изменён, NativePlayerEvents НЕ создан,
  allowlist НЕ расширен. Есть доказательства/план, но реализация впереди.

Перед полной сборкой прочитать pause-отчёты, исправить обозначенный крайний
случай; не откатывать подготовленный WIP к старому DLL checkpoint.

## Что подготовлено

- Рабочие моды `D:/Games-cache/royale/working/Mods`. BR commit
  f5212d7d82adb4ee9b3a9d28e52d5186ecc01cc8, VEManager0.2.0-rc.1,
  публичный MIT Compass адаптирован (private BF3BRCompass недоступен).
  000_test_database исключать из пакета.
- RoyaleServerLayout.cs: отдельные server/Instance/Admin, pinned files,
  без xinput/BotMod. Нужные Scripts/Win32Game.cfg и LANdinput8 включены.
  ProviderID.dat20байт/checksum — отдельная копия; serverid.dat не переносится.
  65проверок PASS. Исходный сервер не переделывается.
- RoyaleAssets.cs v2 SHA
  5B0B37F0A4DD013834350A7E374219B43195A75A31F8CF5350C164C4B573E7E4:
  56C# +9независимых PASS. `D:/Games-cache/royale/server-assets-csharp-v2/Data`,
  6,980,386байт метаданных,16,936CAS entries. Большие архивы только ссылками.
  Удалены только9RES из native ignored list; EBX/chunks/поддерживаемые RES
  сохранены. Cooked physics runtime пока не подтверждён: прогон остановился
  раньше COOPmount. V1 ранее падал OccluderMesh/TurboStates.cpp1610/80000003;
  v1 не использовать, доказательства не удалять.
- MainWindow3 вызывает RoyaleAssets.Prepare(runtime/royale/assets-v2), затем
  Layout. Launcher publishPASS: `D:/Games-cache/royale/package-launcher`,
  log launcher-assets-v2-build.log. GUI функционально в игре не проверен.
- common.ps1 defaults server-runtime-v2/assets-csharp-v2; PS5.1 tests44PASS.
  Запуск только `stand/run/start-royale-server.cmd` через explorer.exe,
  это -NoTests -NoJoin. Launcher/stand ждут также LEVEL-MODE READY, ERR/VEH=отказ.
- Setup сохраняет профили/прогресс/runtime, не обходит junctions при update,
  при uninstall удаляет только ссылки;33проверки PASS. Старый кандидат
  `D:/Games-cache/royale/setup-candidate/EVBots-Setup.exe` около20МБ содержит
  **старые FFDF DLL/core6**, НЕ выдавать как рабочий Royale. Перепаковка впереди.
- Live fun-bots не менялся, последняя проверка460/460SHA совпала. Исходные
  BF-Portable Admin восстановлены3/3SHA до isolated запусков.

## Отчёты и следующий порядок

В `D:/Games-cache/royale/reports/`:

- `pause-array-resize-2026-11-12.md`: toWeapon TypeInfo02B2BD34,
  object=type-4, vtable024434FC, resize0048B120; форма вызова согласуется
  с native. SEHпричина неизвестна, нужны EIP/фактические аргументы.
- `pause-human-transform-2026-11-12.md`: WIP, SHA, команды проверок, nilbindingкрай.
- `pause-human-spawn-2026-11-12.md`: native Spawn/Unspawn/iterators и план.
  Уточнение после возобновления: weak release освобождает при новом count==0;
  прежний вывод про oldcount==0 в pause-отчёте ошибочен (см. раздел выше).
- `datacontainer-tls-2026-11-12.md`: уже исправленный ложный Havokguard.
- `coop-overlay-v2-2026-11-12.md`, `coop-overlay-v2-verification.txt`.
- `bot-isolation-2026-11-12.md`, `dinput8-lan-proxy-2026-11-12.md`.

`D:/Games-cache/royale/soldier-customization-report.txt` — готовый прежний checkpoint.
Native proofs/тесты рядом. Готовый `D:/Games-cache/re/` переиспользовать,
новые большие Ghidra projects/дампы не создавать. С 12.11.2026 это физическое
расположение реверса; `C:/Games/re/` сохранён как NTFS junction на D:.
Перенос проверен: 1343/1343 SHA-256, 142 каталога; исходная физическая копия
удалена только после сверки и шести проверок доступа через старый путь.
Освобождено C: 3,47 ГиБ (оригинал был сжат), после переноса C: 5,85 ГиБ / D: 53,98 ГиБ.
Подробности: STATE/MODLOG и `D:/Games-cache/re-transfer-2026-11-12-result.json`.

После Array resize: чистая загрузка XP5_003, завершить human spawn/unspawn/
transform, проверить клиент и матч минимум с2людьми (высадка, лут, зона,
выбывание, победа, новыйраунд), затем финальный компактный установщик,
сохранение прогресса и переключение. Матч не подтверждён; срок неизвестен.

Место при паузе C~9,66ГиБ/D~61,22ГиБ; кэши наD. Ранее auto-review отверг
удаления npmcache/Temp/других перечисленных данных; не повторять/не обходить.
