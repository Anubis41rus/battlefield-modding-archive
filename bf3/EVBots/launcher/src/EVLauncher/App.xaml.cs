using System.IO;
using System.Windows;
using System.Windows.Threading;

namespace EVBots.Launcher;

public partial class App : Application
{
    protected override void OnStartup(StartupEventArgs e)
    {
        // The stand uses the same preparation code as the GUI. No window or
        // game process is created by this explicit maintenance operation.
        if (e.Args.Length > 0 && e.Args[0] == "--prepare-royale-server")
        {
            if (e.Args.Length != 6) { Shutdown(2); return; }
            try
            {
                if (HostRunner.AnyGameRunning()) throw new InvalidOperationException("Закрой игру и сервер перед подготовкой Royale.");
                var layout = RoyaleServerLayout.Prepare(e.Args[1], e.Args[2], e.Args[3], _ => { }, e.Args[4]);
                File.WriteAllText(e.Args[5], System.Text.Json.JsonSerializer.Serialize(layout));
                Shutdown(0);
            }
            catch (Exception ex)
            {
                File.WriteAllText(e.Args[5], System.Text.Json.JsonSerializer.Serialize(new { error = ex.Message }));
                Shutdown(1);
            }
            return;
        }
        var selfTest = e.Args.Contains("--selftest");

        DispatcherUnhandledException += (_, ex) =>
        {
            ex.Handled = true;
            try
            {
                var dir = Path.Combine(AppContext.BaseDirectory, "logs");
                Directory.CreateDirectory(dir);
                File.AppendAllText(Path.Combine(dir, "crash.log"),
                    $"{DateTime.Now:yyyy-MM-dd HH:mm:ss} {ex.Exception}{Environment.NewLine}");
            }
            catch (Exception) { }
            if (selfTest)
            {
                Shutdown(1);
                return;
            }
            MessageBox.Show("Ошибка лаунчера: " + ex.Exception.Message, "EVBots — другу",
                MessageBoxButton.OK, MessageBoxImage.Error);
        };

        base.OnStartup(e);

        var window = new MainWindow(e.Args.Contains("--preview") || selfTest);
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
}
