# EVBots loader self-test on a harmless 32-bit process (SysWOW64\cmd.exe), never on the game.
# Run after build.cmd: powershell -File test\test.ps1   -> lines "TEST <name> PASS|FAIL", exit 1 on any FAIL.
# ASCII only (Windows PowerShell 5.1 reads BOM-less scripts as ANSI).
$ErrorActionPreference = 'Continue'
$root = Split-Path $PSScriptRoot -Parent
$tmp = Join-Path $env:TEMP ('evbots-loader-test-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Force $tmp | Out-Null
Copy-Item "$root\build\evbots-loader.exe", "$root\build\test\marker1.dll", "$root\build\test\marker2.dll" $tmp -Force
$L = "$tmp\evbots-loader.exe"
$G = 'C:\Windows\SysWOW64'
$env:EVBOTS_MARKER_FILE = "$tmp\marker.txt"
$env:BF3ACT_MODE = 'junk'; $env:BF3ACT_DLF_SRC = 'junk'; $env:BF3ACT_DNSREDIR = 'keep.me'
$fail = 0
function Check($name, $ok) {
    if ($ok) { Write-Output ('TEST ' + $name + ' PASS') } else { Write-Output ('TEST ' + $name + ' FAIL'); $script:fail++ }
}

$o = & $L --dedicated --game $G --exe cmd.exe --dll marker1.dll --dll marker2.dll --wait -- /c exit 7 | Out-String
$m = Get-Content "$tmp\marker.txt"
Check 'two-dlls-in-order' (($m.Count -eq 2) -and ($m[0] -like 'marker1.dll*') -and ($m[1] -like 'marker2.dll*marker1_loaded=1'))
Check 'dedicated-mode-env' ($m[0] -like '*BF3ACT_MODE=dedicated*')
Check 'wait-exit-code' ($o -like '*exited with code 7*')

& $L --client --no-activation --no-activation-guard --drm-strict --license-artifact "$tmp\marker1.dll" --log "$tmp\x.log" `
    --game $G --exe cmd.exe --wait -- /c "set > `"$tmp\env.txt`"" | Out-Null
$e = Get-Content "$tmp\env.txt"
Check 'client-env' (($e -contains 'EALaunchCode=1') -and ($e -contains 'ContentId=71067') -and ($e -contains 'BF3ACT_MODE=client') -and ($e -contains 'BF3ACT_DRM=strict'))
Check 'client-env-dlf' ($e -contains ('BF3ACT_DLF_SRC=' + "$tmp\marker1.dll"))
Check 'foreign-env-kept' ($e -contains 'BF3ACT_DNSREDIR=keep.me')

# --dll is required here: the dedicated default is EVBots.dll, which is not in the test folder.
& $L --dedicated --game $G --exe cmd.exe --dll marker1.dll --wait -- /c "echo a  b> `"$tmp\tail.txt`"" | Out-Null
Check 'tail-quoting' ((Get-Content "$tmp\tail.txt") -eq 'a  b')

& $L --dedicated --game $G --exe cmd.exe --dll nope.dll -- /c exit 0 | Out-Null; Check 'missing-dll-rc3' ($LASTEXITCODE -eq 3)
& $L --dedicated --game $G -- /c exit 0 | Out-Null;                             Check 'missing-exe-rc3' ($LASTEXITCODE -eq 3)
& $L --dedicated --game $G --exe cmd.exe -- /c exit 0 | Out-Null;               Check 'dedicated-default-evbots-dll-rc3' ($LASTEXITCODE -eq 3)
& $L --client --game $G --exe cmd.exe -- /c exit 0 | Out-Null;                  Check 'client-needs-activation-rc3' ($LASTEXITCODE -eq 3)
& $L --dedicated --drm-strict --game $G --exe cmd.exe -- /c exit 0 | Out-Null;  Check 'drm-on-dedicated-rc2' ($LASTEXITCODE -eq 2)
& $L --dedicated --game $G --exe cmd.exe /c | Out-Null;                         Check 'args-without-dashdash-rc2' ($LASTEXITCODE -eq 2)

& $L --guard --game $G --bf3-pid 999999 --log "$tmp\guard.log" --grace-ms 300 --max-ms 5000 | Out-Null
Check 'guard-exits-after-grace' ((Get-Content "$tmp\guard.log" -Encoding UTF8 | Out-String) -like '*grace 300*')

Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue
Write-Output ('TESTS-DONE fail=' + $fail)
if ($fail -gt 0) { exit 1 }
