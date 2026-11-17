# EVBots stand: wait for TESTS-DONE in this run's log (EVBOTS_TESTS=1) and print the TEST lines.
# Never stops the server by itself (rule: not while a human is in the game). ASCII only (see common.ps1).
#   tests.ps1              wait up to 10 min
#   tests.ps1 -WaitMin 0   print what is there now
param([int]$WaitMin = 10)
. "$PSScriptRoot\common.ps1"
$st = Get-RunState
if (-not $st) { Write-Step 'no run state: start the stand with run\start.ps1'; exit 1 }
if ($st.PSObject.Properties.Name -contains 'tests' -and -not $st.tests) {
    Write-Step 'SKIP: self-tests were disabled for this run; no TESTS-DONE is expected'
    exit 0
}
$done = Wait-For { if ((Read-LogSince ([long]$st.offset)) -match 'TESTS-DONE') { return $true } } ($WaitMin * 60) 5000
$lines = @(Read-LogSince ([long]$st.offset))
$lines | Where-Object { $_ -match ' TEST |TESTS-DONE' } | ForEach-Object { $_ }
$pass = @($lines | Where-Object { $_ -match ' TEST \S+ PASS' }).Count
$fail = @($lines | Where-Object { $_ -match ' TEST \S+ FAIL' }).Count
$skip = @($lines | Where-Object { $_ -match ' TEST \S+ SKIP' }).Count
Write-Step ('PASS ' + $pass + ', FAIL ' + $fail + ', SKIP ' + $skip + $(if ($done) { '' } else { ' - TESTS-DONE not reached yet' }))
