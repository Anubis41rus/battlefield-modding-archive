# EVBots — полная проверка ПК (только чтение, ничего не меняет). Запуск: «Проверка EVBots.cmd».
# Итог — архив EVBots-check-<ПК>-<время>.zip на рабочем столе: его прислать.
# Внутри: report.txt (система, реестр, сеть, фаервол, настройки сервера), manifest.csv (все файлы игры, сервера,
# стенда и лаунчера: путь, размер, MD5), logs\ (хвосты логов). Сравнение с эталоном — tools\compare-check.py.
# Пароль из friend-settings.json в отчёт не попадает.
$ErrorActionPreference = 'Continue'
# В одном файле «Проверка EVBots.cmd» папку передаёт cmd-часть ($Root); отдельный .ps1 — берёт свою
if (-not $Root) { $Root = $PSScriptRoot }
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$work = Join-Path $env:TEMP "evbots-check-$stamp"
New-Item -ItemType Directory -Force -Path (Join-Path $work 'logs') | Out-Null
$rep = New-Object System.Collections.Generic.List[string]
function W([string]$t) { $rep.Add($t) }
function Safe([scriptblock]$b) { try { & $b } catch { W ("   ошибка: " + $_.Exception.Message) } }
function Ok([string]$p) { try { return [bool]$p -and (Test-Path -LiteralPath $p) } catch { return $false } }
$HashLimit = 64MB

Write-Host 'Проверка EVBots: собираю данные, это займёт 1-3 минуты...'
W ("Дата: " + (Get-Date -Format 'dd.MM.yyyy HH:mm:ss') + "   ПК: " + $env:COMPUTERNAME)
W ("Windows: " + [Environment]::OSVersion.VersionString + ", 64-бит: " + [Environment]::Is64BitOperatingSystem)
W ("Папка скрипта: " + $Root)
Safe { Get-PSDrive -PSProvider FileSystem | Where-Object { $_.Used -ne $null } | ForEach-Object {
    W ("Диск " + $_.Name + ": свободно " + [math]::Round($_.Free / 1GB, 1) + " ГБ") } }
W ""

# ---------- где что лежит ----------
$games = New-Object System.Collections.Generic.List[string]
$servers = New-Object System.Collections.Generic.List[string]
$stands = New-Object System.Collections.Generic.List[string]
$launchers = New-Object System.Collections.Generic.List[string]
$standNames = @('BF3NativeBots', 'Battlefield EVBots', 'Battlefield 3 EVBots LAN')
$p = $Root
for ($i = 0; $i -lt 4 -and $p; $i++) {
    Get-ChildItem -LiteralPath $p -Recurse -Depth 3 -File -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -in 'bf3.exe', 'bf3_server_Final.exe', 'EVLauncher.exe' } | ForEach-Object {
            if ($_.Name -eq 'bf3.exe') { $games.Add($_.DirectoryName) }
            elseif ($_.Name -eq 'bf3_server_Final.exe') { $servers.Add($_.DirectoryName) }
            else { $launchers.Add($_.DirectoryName) }
        }
    Get-ChildItem -LiteralPath $p -Recurse -Depth 2 -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -in $standNames } | ForEach-Object { $stands.Add($_.FullName) }
    # Нашли игру — выше не поднимаемся (иначе дойдём до корня диска и будем долго перебирать Windows)
    if ($games.Count -gt 0) { break }
    $p = Split-Path $p -Parent
}

# ---------- реестр ----------
foreach ($view in 'Registry32', 'Registry64') {
    W "== Реестр HKLM ($view): SOFTWARE\EA Games\Battlefield 3"
    Safe {
        $k = [Microsoft.Win32.RegistryKey]::OpenBaseKey('LocalMachine', $view).OpenSubKey('SOFTWARE\EA Games\Battlefield 3')
        if ($null -eq $k) { W "   ключа нет" }
        else {
            foreach ($n in $k.GetValueNames()) {
                $v = [string]$k.GetValue($n)
                $codes = ($v.ToCharArray() | Select-Object -First 80 | ForEach-Object { [int]$_ }) -join ' '
                W ("   $n [" + $k.GetValueKind($n) + "] длина " + $v.Length + " = «" + $v + "»")
                if ($n -in 'Locale', 'Install Dir') { W "      коды: $codes" }
                if ($n -eq 'Install Dir' -and (Ok (Join-Path $v.TrimEnd([char]0) 'bf3.exe'))) { $games.Add($v.TrimEnd('\')) }
            }
        }
    }
}
W ""

$games = @($games | ForEach-Object { $_.TrimEnd('\') } | Select-Object -Unique)
$servers = @($servers | Select-Object -Unique)
$stands = @($stands | Select-Object -Unique)
$launchers = @($launchers | Select-Object -Unique)
W "== Найдено"
W ("   игра (bf3.exe): " + $(if ($games) { $games -join ' | ' } else { 'НЕТ' }))
W ("   сервер (bf3_server_Final.exe): " + $(if ($servers) { $servers -join ' | ' } else { 'НЕТ' }))
W ("   стенд EVBots: " + $(if ($stands) { $stands -join ' | ' } else { 'НЕТ' }))
W ("   лаунчер (EVLauncher.exe): " + $(if ($launchers) { $launchers -join ' | ' } else { 'НЕТ' }))
W ""

# ---------- сеть, фаервол, .NET, процессы ----------
W "== Сеть (IPv4)"
Safe { [System.Net.NetworkInformation.NetworkInterface]::GetAllNetworkInterfaces() | Where-Object { $_.OperationalStatus -eq 'Up' } | ForEach-Object {
    $ni = $_; $ipp = $ni.GetIPProperties()
    $ips = ($ipp.UnicastAddresses | Where-Object { $_.Address.AddressFamily -eq 'InterNetwork' } | ForEach-Object { $_.Address.ToString() }) -join ', '
    $gw = ($ipp.GatewayAddresses | ForEach-Object { $_.Address.ToString() }) -join ', '
    if ($ips) { W ("   " + $ni.Name + " (" + $ni.Description + "): " + $ips + "  шлюз: " + $gw) } } }
W "== Фаервол (правила EVBots)"
foreach ($r in 'EVBots-TCP', 'EVBots-UDP', 'EVBots BF3 client', 'EVBots BF3 server') {
    $o = (netsh advfirewall firewall show rule name="$r" verbose 2>&1) -join "`n"
    if ($o -match 'No rules|Нет правил' -or $o -notmatch '-----') { W "   $r : НЕТ" }
    else {
        W "   $r : есть"
        ($o -split "`n") | Where-Object { $_ -match '^(Enabled|Включено|Direction|Направление|Action|Действие|Program|Программа|LocalPort|Локальный порт|Protocol|Протокол|Profiles|Профили)' } |
            ForEach-Object { W ("      " + $_.Trim()) }
    }
}
W "== URL-ACL (порт 8080)"
Safe { (netsh http show urlacl) | Select-String '8080' | ForEach-Object { W ("   " + $_.Line.Trim()) } }
W "== .NET"
Safe { $d = Get-Command dotnet -ErrorAction SilentlyContinue
    if ($d) { (& dotnet --list-runtimes 2>&1) | ForEach-Object { W "   $_" } } else { W "   dotnet не найден в PATH" } }
W "== Процессы сейчас"
Safe { Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.Name -match '^(bf3|zbf3|zbf3Sp|bf3_server_Final|EVBots\.Backend|EVLauncher|evbots-loader|activation-loader|PortableBF|httpd|mysqld|Origin)$' } |
    ForEach-Object { $path = ''; try { $path = $_.Path } catch { }; W ("   " + $_.Name + " pid " + $_.Id + "  " + $path) } }
W ""

# ---------- файлы: manifest.csv ----------
$man = New-Object System.Collections.Generic.List[string]
$man.Add('area;root;rel;size;md5')
$skipDirs = @('logs', 'Logs', 'TelemetryTransactionLog', 'svlogs', 'backup', 'profiles', 'Screenshots')
function Manifest([string]$area, [string]$root) {
    Write-Host "  список файлов: $area  $root"
    $md5 = [System.Security.Cryptography.MD5]::Create()
    Get-ChildItem -LiteralPath $root -Recurse -File -Force -ErrorAction SilentlyContinue | ForEach-Object {
        $rel = $_.FullName.Substring($root.Length).TrimStart('\')
        $parts = $rel.Split('\')
        if ($parts.Count -gt 1 -and ($parts[0..($parts.Count - 2)] | Where-Object { $_ -in $skipDirs })) { return }
        $h = ''
        if ($_.Length -le $HashLimit) {
            try { $fs = [IO.File]::OpenRead($_.FullName); $h = ([BitConverter]::ToString($md5.ComputeHash($fs))).Replace('-', ''); $fs.Close() }
            catch { $h = 'НЕ_ЧИТАЕТСЯ' }
        }
        $man.Add("$area;$root;$rel;" + $_.Length + ";$h")
    }
}
$n = 0; foreach ($g in $games) { $n++; Manifest "GAME$n" $g }
$n = 0; foreach ($s in $servers) { $n++; Manifest "SERVER$n" $s }
$n = 0; foreach ($s in $stands) { $n++; Manifest "STAND$n" $s }
$n = 0; foreach ($l in $launchers) { $n++; Manifest "LAUNCHER$n" $l }
[IO.File]::WriteAllLines((Join-Path $work 'manifest.csv'), $man, (New-Object System.Text.UTF8Encoding($true)))
W ("== manifest.csv: строк " + ($man.Count - 1))
W ""

# ---------- языковые файлы (кратко) ----------
foreach ($g in $games) {
    W "== Язык: $g"
    foreach ($loc in 'Data\Win32\Loc', 'Data\Win32\Loc\packs', 'Update\Patch\Data\Win32\Loc', 'Update\Patch\Data\Win32\Loc\packs') {
        $full = Join-Path $g $loc
        if (-not (Ok $full)) { W "   $loc — папки нет"; continue }
        $f = Get-ChildItem -LiteralPath $full -File | ForEach-Object { $_.Name + " (" + [math]::Round($_.Length / 1MB, 1) + " МБ)" }
        W ("   $loc : " + $(if ($f) { $f -join ', ' } else { 'пусто' }))
    }
}
W ""

# ---------- настройки сервера и мода (текстом) ----------
function Dump([string]$title, [string]$file, [int]$max = 400) {
    W "== $title : $file"
    if (-not (Ok $file)) { W "   файла нет"; return }
    Get-Content -LiteralPath $file -TotalCount $max -Encoding UTF8 -ErrorAction SilentlyContinue | ForEach-Object { W ("   | " + $_) }
}
foreach ($s in $servers) {
    Dump 'ServerOptions.cfg' (Join-Path $s 'Instance\ServerOptions.cfg')
    foreach ($f in 'Startup.txt', 'MapList.txt', 'ModList.txt') { Dump $f (Join-Path $s "Instance\Admin\$f") }
    $mods = Join-Path $s 'Instance\Admin\Mods'
    W "== Моды: $mods"
    if (Ok $mods) { Get-ChildItem -LiteralPath $mods -Directory | ForEach-Object { W ("   " + $_.Name) } } else { W "   папки нет" }
    Dump 'fun-bots mod.json' (Join-Path $mods 'fun-bots\mod.json')
    $cfg = Join-Path $mods 'fun-bots\ext\Shared\Config.lua'
    W "== fun-bots Config.lua (строки с Bots/Enable): $cfg"
    if (Ok $cfg) { Select-String -LiteralPath $cfg -Pattern 'Bots|Enable|Spawn' | Select-Object -First 80 | ForEach-Object { W ("   " + $_.LineNumber + ": " + $_.Line.Trim()) } }
    else { W "   файла нет" }
}
foreach ($l in $launchers) {
    $fs = Join-Path $l 'friend-settings.json'
    W "== Настройки лаунчера (пароль скрыт): $fs"
    if (Ok $fs) { Get-Content -LiteralPath $fs | ForEach-Object { W ("   | " + ($_ -replace '("Password"\s*:\s*")[^"]*', '$1***')) } } else { W "   файла нет" }
}

# ---------- логи (хвосты) ----------
function Tail([string]$file, [string]$name, [int]$lines) {
    if (-not (Ok $file)) { return }
    try {
        # 19.10: хвост байтами, без перекодировки (Get-Content в PS 5.1 читал UTF-8 лог как ANSI — кракозябры)
        $fi = Get-Item -LiteralPath $file
        $max = [long]$lines * 400
        $fs = [IO.File]::Open($file, 'Open', 'Read', 'ReadWrite')
        try {
            $from = [Math]::Max([long]0, $fs.Length - $max)
            [void]$fs.Seek($from, 'Begin')
            $buf = New-Object byte[] ($fs.Length - $from)
            $read = 0; while ($read -lt $buf.Length) { $k = $fs.Read($buf, $read, $buf.Length - $read); if ($k -le 0) { break }; $read += $k }
        } finally { $fs.Close() }
        [IO.File]::WriteAllBytes((Join-Path $work "logs\$name"), $buf)
        W ("   logs\" + $name + "  ← " + $file + "  (" + $fi.LastWriteTime.ToString('dd.MM.yyyy HH:mm') + ", " + [math]::Round($fi.Length / 1KB) + " КБ)")
    }
    catch { W "   лог не прочитан: $file" }
}
W "== Логи в архиве (последние строки; время — у исходного файла):"
$n = 0
foreach ($s in $stands) {
    $n++
    Get-ChildItem -LiteralPath (Join-Path $s 'logs') -File -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 8 |
        ForEach-Object { Tail $_.FullName ("stand$n-" + $_.Name) 4000 }
}
$n = 0
foreach ($s in $servers) {
    $n++
    Get-ChildItem -LiteralPath (Join-Path $s 'Instance\Logs') -File -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 2 |
        ForEach-Object { Tail $_.FullName ("server$n-" + $_.Name) 4000 }
}
$n = 0
foreach ($l in $launchers) {
    $n++
    Get-ChildItem -LiteralPath (Join-Path $l 'logs') -File -ErrorAction SilentlyContinue | ForEach-Object { Tail $_.FullName ("launcher$n-" + $_.Name) 1000 }
}

[IO.File]::WriteAllLines((Join-Path $work 'report.txt'), $rep, (New-Object System.Text.UTF8Encoding($true)))
$zip = Join-Path ([Environment]::GetFolderPath('Desktop')) ("EVBots-check-" + $env:COMPUTERNAME + "-$stamp.zip")
Compress-Archive -Path (Join-Path $work '*') -DestinationPath $zip -Force
Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
Write-Host "Готово: $zip"
if (-not $env:EVBOTS_CHECK_QUIET) {
    Add-Type -AssemblyName System.Windows.Forms
    [System.Windows.Forms.MessageBox]::Show("Готово. Пришли этот файл:`n$zip", 'Проверка EVBots') | Out-Null
    Start-Process explorer.exe "/select,`"$zip`""
}
