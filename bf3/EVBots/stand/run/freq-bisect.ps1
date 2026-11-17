# 26.10 (238-4): поиск, какая группа правок частоты клиента вызывает «неверную лицензию». Для каждого набора групп
# (EVBOTS_FREQ_CLIENT_PARTS: h — перехваты, c — код, d — константы; '-' — без правок) — вход клиента к работающему
# серверу, ждём: подмена DNS в журнале activation (перехват #1) = OK, сторож убил activation.exe = LICENSE; клиент закрываем.
# Клиент закрывается — только когда пользователь разрешил (26.10 «можно закрывать bf3»). Итог — run\freq-bisect.txt.
param([string[]]$Sets = @('h', 'c', 'd'), [int]$WaitSec = 70)
. "$PSScriptRoot\common.ps1"
$out = $PSScriptRoot + '\freq-bisect.txt'
$logs = $Kit + '\activation\logs'
foreach ($set in $Sets) {
    $c = Get-Proc 'bf3'
    if ($c) { Stop-Process -Id $c.Id -Force; [void](Wait-For { -not (Get-Proc 'bf3') } 20 500) }
    Start-Sleep -Seconds 3
    $t0 = Get-Date
    if ($set -eq '-') { $env:EVBOTS_FREQ_CLIENT_PARTS = 'x' } else { $env:EVBOTS_FREQ_CLIENT_PARTS = $set }
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File ($PSScriptRoot + '\join.ps1') | Out-Null
    $res = Wait-For {
        $g = Get-ChildItem $logs -Filter 'guard-*.log' | Where-Object { $_.LastWriteTime -ge $t0 } | Sort-Object LastWriteTime | Select-Object -Last 1
        $a = Get-ChildItem $logs -Filter 'activation-*.log' | Where-Object { $_.LastWriteTime -ge $t0 } | Sort-Object LastWriteTime | Select-Object -Last 1
        if ($g -and ((Get-Content $g.FullName -Raw -Encoding UTF8) -match 'activation\.exe')) { return 'LICENSE' }
        if ($a -and ((Get-Content $a.FullName -Raw -Encoding UTF8) -match '#1 gethostbyname')) { return 'OK' }
        if (-not (Get-Proc 'bf3') -and ((Get-Date) - $t0).TotalSeconds -gt 20) { return 'EXIT' }
        return $null
    } $WaitSec 1000
    if (-not $res) { $res = 'TIMEOUT' }
    $fl = @(Get-Content ($Kit + '\activation\logs\ev-core.log') -Tail 200 -Encoding UTF8 | Where-Object { $_ -match '\[freq\]' }) | Select-Object -Last 3
    Add-Content -Path $out -Encoding UTF8 -Value ((Get-Date).ToString('HH:mm:ss') + ' parts=' + $set + ' -> ' + $res + ' | ' + ($fl -join ' || '))
}
$c = Get-Proc 'bf3'
if ($c) { Stop-Process -Id $c.Id -Force }
Add-Content -Path $out -Encoding UTF8 -Value ((Get-Date).ToString('HH:mm:ss') + ' BISECT-DONE')
