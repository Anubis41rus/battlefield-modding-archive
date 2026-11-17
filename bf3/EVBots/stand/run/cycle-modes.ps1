# Автономный прогон всех режимов (19.10, сборка 201): все режимы списка карт за ОДИН старт, без человека и без автоклика.
# Флаги сервера: EVBOTS_TESTS_NOHUMAN=1 (тесты стартуют, как только у ботов есть тела), EVBOTS_TESTS_CYCLE=1 (после
# TESTS-DONE мост шлёт RCON mapList.runNextRound, на следующем уровне тесты идут заново; в конце — TESTS-ALL-DONE).
# Конец — TESTS-ALL-DONE, падение (процесс сервера исчез) или -MaxMin; печатает сводку по уровням, строки FAIL и частые ERR,
# потом закрывает свой клиент и гасит сервер и бэкенд (-Keep — оставить всё запущенным).
#   cycle-modes.ps1                        режимы по умолчанию (ниже), MapList.txt в конце восстанавливается
#   cycle-modes.ps1 -Maps "MP_007 ConquestLarge0 1;XP5_002 AirSuperiority0 1"
#   cycle-modes.ps1 -Maps ''               оставить текущий MapList.txt
# Не стартует, пока запущен клиент (может играть человек). Файл — UTF-8 с BOM (иначе PowerShell 5.1 читает кириллицу как ANSI).
param(
    # Выход из AirSuperiority дважды ронял сервер (15:41, 16:25) — он идёт последним, следом XP5_002 CQL (проверка этого перехода).
    [string]$Maps = 'MP_007 ConquestLarge0 1;MP_007 TeamDeathMatch0 1;MP_007 RushLarge0 1;MP_007 SquadDeathMatch0 1;XP5_002 AirSuperiority0 1;XP5_002 ConquestLarge0 1',
    [int]$MaxMin = 35,
    [string]$ExtraEnv = '',
    [switch]$Keep
)
. "$PSScriptRoot\common.ps1"
$transcript = $RunDir + '\cycle-modes-' + (Get-Date).ToString('HHmmss') + '.log'
Start-Transcript -Path $transcript -Force | Out-Null
$mapList = 'C:\Games\BF-Portable\Battlefield_3_Server\Instance\Admin\MapList.txt'
$mapBackup = $RunDir + '\MapList-before-cycle-modes.txt'
$started = $false
try {
    $c = Get-Proc 'bf3'
    if ($c) { Write-Step ('STOP: client bf3 is running (PID ' + $c.Id + ') - a human may be playing. Close it first.'); return }
    if ($Maps -ne '') {
        Copy-Item $mapList $mapBackup -Force
        $entries = @($Maps -split ';' | ForEach-Object { $_.Trim() } | Where-Object { $_ -ne '' })
        Set-Content -Path $mapList -Value $entries -Encoding ASCII
        Write-Step ('MapList: ' + ($entries -join ' | '))
    }
    $envAll = 'EVBOTS_TESTS_NOHUMAN=1,EVBOTS_TESTS_CYCLE=1'
    if ($ExtraEnv -ne '') { $envAll = $envAll + ',' + $ExtraEnv }
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File ($PSScriptRoot + '\start.ps1') -Force -ExtraEnv $envAll
    if ($LASTEXITCODE -ne 0) { Write-Step ('start.ps1 failed, code ' + $LASTEXITCODE); return }
    $started = $true
    $st = Get-RunState
    $why = 'timeout ' + $MaxMin + ' min'
    $until = (Get-Date).AddMinutes($MaxMin)
    $seenLevels = 0
    # 20.10 (прогон 210): свёрнутый клиент не грузит уровень — 6 мин без входа, ботов нет (мод ждёт игрока). Разворачиваем окно.
    Add-Type -Namespace EvCycle -Name Win -MemberDefinition '[DllImport("user32.dll")] public static extern bool IsIconic(System.IntPtr h); [DllImport("user32.dll")] public static extern bool ShowWindow(System.IntPtr h, int c);' -ErrorAction SilentlyContinue
    while ((Get-Date) -lt $until) {
        Start-Sleep -Seconds 10
        $cl = Get-Proc 'bf3'
        if ($cl -and $cl.MainWindowHandle -ne [IntPtr]::Zero -and [EvCycle.Win]::IsIconic($cl.MainWindowHandle)) {
            [void][EvCycle.Win]::ShowWindow($cl.MainWindowHandle, 9)
            Write-Step 'client window was minimized - restored (a minimized client does not load the level)'
        }
        $lines = @(Read-LogSince ([long]$st.offset))
        $lv = @($lines | Where-Object { $_ -match 'TESTS-LEVEL ' })
        if ($lv.Count -gt $seenLevels) { $lv[$seenLevels..($lv.Count - 1)] | ForEach-Object { Write-Step $_ }; $seenLevels = $lv.Count }
        if ($lines -match 'TESTS-ALL-DONE') { $why = 'TESTS-ALL-DONE'; break }
        # Строка VEH сама по себе — не падение: мост ловит исключения в своих вызовах (спавн, прогон 16:13), сервер живёт дальше.
        if (-not (Get-Proc 'bf3_server_Final')) { $why = 'CRASH: server process gone'; break }
    }
    Write-Step ('END: ' + $why)
    $lines = @(Read-LogSince ([long]$st.offset))
    Write-Step '--- levels'
    $lines | Where-Object { $_ -match 'TESTS-LEVEL |TESTS-DONE|mapList.runNextRound' } | ForEach-Object { $_ }
    Write-Step '--- FAIL'
    @($lines | Where-Object { $_ -match ' TEST \S+ FAIL' }) | Select-Object -First 60 | ForEach-Object { $_ }
    Write-Step '--- ERR (most frequent, timestamps and numbers folded)'
    # Метки времени и шестнадцатеричные адреса сворачиваются, чтобы одинаковые ошибки сложились в одну строку.
    $lines | Where-Object { $_ -match ' ERR ' } | ForEach-Object { ($_ -replace '^\[[0-9:.]+\]\s*', '') -replace '[0-9A-F]{6,8}', 'N' } |
        Group-Object | Sort-Object Count -Descending | Select-Object -First 12 | ForEach-Object { ('{0,6}  {1}' -f $_.Count, $_.Name) }
    Write-Step '--- VEH'
    @($lines | Where-Object { $_ -match 'VEH #|EIP=' }) | Select-Object -First 6 | ForEach-Object { $_ }
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File ($PSScriptRoot + '\status.ps1') | Select-Object -Last 12
} finally {
    if ($started -and -not $Keep) {
        # Клиент запустил этот же скрипт (start.ps1), человек в нём не играет — закрываем, затем гасим сервер и бэкенд.
        $c2 = Get-Proc 'bf3'
        if ($c2) { Stop-Process -Id $c2.Id -Force; [void](Wait-For { -not (Get-Proc 'bf3') } 15 500) }
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File ($PSScriptRoot + '\stop.ps1') -Force
    }
    if ($Maps -ne '' -and (Test-Path $mapBackup)) { Copy-Item $mapBackup $mapList -Force; Write-Step 'MapList.txt restored' }
    Write-Step 'CYCLE-MODES-DONE'
    Stop-Transcript | Out-Null
}
