# launcher-ui.ps1 — нажимает кнопки установленного EV Launcher (C:\Games\EVBots) через UI Automation.
# 31.10.2026: прежний ui.ps1 жил в scratchpad и потерялся (NEXT-SESSION-2026-10-28-b). Кнопки — по x:Name из MainWindow.xaml.
#   launcher-ui.ps1 -Click LoginBtn,HostBtn         — нажать по порядку (пропуская невидимые/недоступные)
#   launcher-ui.ps1 -List                           — показать кнопки окна
# Лаунчер запускать через explorer.exe (правило 14), скрипт его не стартует.
param([string[]]$Click = @(), [switch]$List, [int]$WaitSec = 30)
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
$root = [System.Windows.Automation.AutomationElement]::RootElement
$cond = New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::NameProperty, 'EV Launcher')
$win = $null
for ($i = 0; $i -lt $WaitSec -and $win -eq $null; $i++) {
    $win = $root.FindFirst([System.Windows.Automation.TreeScope]::Children, $cond)
    if ($win -eq $null) {
        # заголовок может отличаться — ищем по процессу
        $p = Get-Process -Name 'EV Launcher' -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($p -and $p.MainWindowHandle -ne 0) { $win = [System.Windows.Automation.AutomationElement]::FromHandle($p.MainWindowHandle) }
    }
    if ($win -eq $null) { Start-Sleep -Seconds 1 }
}
if ($win -eq $null) { Write-Output 'LAUNCHER-UI: окно лаунчера не найдено'; exit 2 }
$btnCond = New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::ControlTypeProperty, [System.Windows.Automation.ControlType]::Button)
if ($List) {
    foreach ($b in $win.FindAll([System.Windows.Automation.TreeScope]::Descendants, $btnCond)) {
        Write-Output ("LAUNCHER-UI: кнопка {0} «{1}» enabled={2} offscreen={3}" -f $b.Current.AutomationId, $b.Current.Name, $b.Current.IsEnabled, $b.Current.IsOffscreen)
    }
}
foreach ($id in $Click) {
    $idCond = New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::AutomationIdProperty, $id)
    $b = $null
    for ($i = 0; $i -lt $WaitSec -and $b -eq $null; $i++) {
        $b = $win.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $idCond)
        if ($b -ne $null -and (-not $b.Current.IsEnabled -or $b.Current.IsOffscreen)) { $b = $null }
        if ($b -eq $null) { Start-Sleep -Seconds 1 }
    }
    if ($b -eq $null) { Write-Output "LAUNCHER-UI: $id — нет или недоступна за $WaitSec с"; continue }
    $b.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
    Write-Output ("LAUNCHER-UI: нажата {0} «{1}»" -f $id, $b.Current.Name)
    Start-Sleep -Seconds 2
}
