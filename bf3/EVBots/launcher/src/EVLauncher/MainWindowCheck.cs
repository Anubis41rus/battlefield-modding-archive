// Кнопки первого экрана: «Проверить файлы» и «Указать папку EVBots».
using System.IO;
using System.Windows;

namespace EVBots.Launcher;

public partial class MainWindow
{
    private async void Check_Click(object sender, RoutedEventArgs e) => await RunCheckAsync();

    /// <summary>Пишет в лог, что найдено и чего не хватает. На диске ничего не меняет.</summary>
    private async Task RunCheckAsync()
    {
        LogPanel.IsExpanded = true;
        CheckBtn.IsEnabled = false;
        try
        {
            SetStatus("Проверяю файлы...");
            Log("--- Проверка файлов ---");
            var profile = EffectiveProfile ?? SelectedProfile;
            var server = _authIp == LocalIp || (_authIp.Length == 0 && _p.ServerDir.Length > 0);
            var lines = await Task.Run(() => FileCheck.Build(_p, profile, server));
            foreach (var l in lines) Log($"[{l.Mark}] {l.Text}");
            var missing = lines.Count(l => l.Mark == "НЕТ");
            Log(missing == 0
                ? "Итог: всё на месте."
                : $"Итог: не хватает {missing}, смотри строки [НЕТ] выше.");
            SetStatus(missing == 0
                ? profile.IsRoyale ? "Файлы есть. Работа полного матча VU Battle Royale ещё не подтверждена." : "Проверка: всё на месте."
                : $"Проверка: не хватает {missing}, смотри лог.");
        }
        finally
        {
            CheckBtn.IsEnabled = true;
        }
    }

    /// <summary>Друг выбирает папку EVBots сам, если лаунчер её не нашёл.</summary>
    private async void EVBotsDir_Click(object sender, RoutedEventArgs e)
    {
        var dlg = new Microsoft.Win32.OpenFolderDialog
        {
            Title = "Папка EVBots: та, где лежат activation и backend",
        };
        if (dlg.ShowDialog(this) != true) return;
        _p.StandDir = dlg.FolderName;
        _s.StandDir = dlg.FolderName;
        _s.Save();
        Log("Папка EVBots выбрана: " + _p.StandDir);
        await RunCheckAsync();
    }
}
