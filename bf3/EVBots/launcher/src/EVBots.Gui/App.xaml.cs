using System.IO;
using System.Windows;
using System.Windows.Threading;
using EVBots.Core;

namespace EVBots.Gui;

public partial class App : Application
{
    protected override void OnStartup(StartupEventArgs e)
    {
        // --selftest: открыть окно, пару секунд подождать и выйти с кодом 0 (проверка XAML и ресурсов)
        var selfTest = e.Args.Contains("--selftest");

        DispatcherUnhandledException += (_, ex) =>
        {
            LogCrash(ex.Exception);
            ex.Handled = true;
            if (selfTest)
            {
                Shutdown(1);
                return;
            }

            MessageBox.Show(
                "Ошибка лаунчера: " + ex.Exception.Message +
                "\n\nПодробности записаны в " + Path.Combine(Paths.LogsDir, "crash.log"),
                "EVBots", MessageBoxButton.OK, MessageBoxImage.Error);
        };

        base.OnStartup(e);

        var window = new MainWindow();
        MainWindow = window;
        window.Show();

        if (selfTest)
        {
            var timer = new DispatcherTimer { Interval = TimeSpan.FromSeconds(2) };
            timer.Tick += (_, _) =>
            {
                timer.Stop();
                window.Close();
                Shutdown(0);
            };
            timer.Start();
        }
    }

    private static void LogCrash(Exception ex)
    {
        try
        {
            Directory.CreateDirectory(Paths.LogsDir);
            File.AppendAllText(Path.Combine(Paths.LogsDir, "crash.log"),
                $"{DateTime.Now:yyyy-MM-dd HH:mm:ss} {ex}{Environment.NewLine}");
        }
        catch (Exception)
        {
            // не смогли записать — лог не главное
        }
    }
}
