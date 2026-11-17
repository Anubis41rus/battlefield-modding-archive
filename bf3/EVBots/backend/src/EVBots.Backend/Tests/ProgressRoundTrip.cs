// Проверки общего прогресса без игры, сети и живых профилей.
using EVBots.Backend.Blaze;
using EVBots.Backend.Data;

namespace EVBots.Backend.Tests;

internal static class ProgressRoundTrip
{
    public static int Run(string root)
    {
        Directory.CreateDirectory(root);
        var cfg = new ServeConfig { ProfilesDir = Path.Combine(root, "profiles") };
        Directory.CreateDirectory(cfg.ProfilesDir);
        var log = new Log(false);
        var store = new ProfileStore(cfg, log);
        store.Load();
        var player = store.Create("ProgressTest")!;
        var id = player.Id;
        var modes = new Dictionary<string, string>
        {
            ["c___k_g"] = "increment", ["sc_general"] = "increment",
            ["record"] = "max", ["setting"] = "set",
        };
        int pass = 0, fail = 0;
        void Check(bool ok, string text)
        {
            if (ok) pass++; else fail++;
            Console.WriteLine($"{(ok ? "PASS" : "FAIL")} {text}");
        }
        Dictionary<string, double> Match(double kills, double score) => new()
        {
            ["c___k_g"] = kills, ["sc_general"] = score,
        };
        void Report(double kills, double score, bool end = false) =>
            GameReporting.ApplyPlayer(id, Match(kills, score), modes, store, end);
        bool Is(double kills, double score)
        {
            var p = store.GetProgress(id).Stats;
            return p.GetValueOrDefault("c___k_g") == kills && p.GetValueOrDefault("sc_general") == score;
        }

        store.UpdateProgress(id, p => { p.Stats["c___k_g"] = 10; p.Stats["sc_general"] = 1000; });
        Report(5, 500);
        Check(Is(15, 1500), "промежуточный отчёт сохраняет накопленный прогресс");
        Parallel.For(0, 16, _ => Report(5, 500));
        Check(Is(15, 1500), "параллельные повторы одного отчёта не удваивают начисление");
        Report(5, 500, end: true);
        Check(Is(15, 1500), "пара 0x64/0x65 начисляется один раз");
        Report(3, 300);
        Check(Is(18, 1800), "новая карта прибавляет прогресс к предыдущей");
        Report(1, 100);
        Check(Is(19, 1900), "повторный вход со счётом с нуля сохраняет старый прогресс");
        var alias = Match(1, 100);
        alias["sc_general_a"] = 1900;
        alias["c___k_g"] = double.NaN;
        alias["unknown"] = 42;
        var merged = GameReporting.ApplyPlayer(id, alias, modes, store, endReport: false);
        Check(Is(19, 1900) && merged.Unknown == 2 && merged.Absolute == 1,
            "абсолютный показатель, NaN и неизвестное имя не портят итог");

        var before = store.GetProgress(id).Stats["sc_general"];
        var pending = Path.Combine(cfg.ProfilesDir, id.ToString(), "progress.json.tmp");
        bool failed = false;
        using (var blocked = new FileStream(pending, FileMode.Create, FileAccess.ReadWrite, FileShare.None))
        {
            try { Report(2, 200); }
            catch (IOException) { failed = true; }
            Check(failed && store.GetProgress(id).Stats["sc_general"] == before,
                "ошибка записи оставляет предыдущий progress.json целым");
        }
        Report(2, 200);
        Check(Is(20, 2000), "повтор после ошибки записи не теряет прирост из-за Ledger");

        var concurrent = store.Create("ConcurrentProgressTest")!.Id;
        Parallel.For(0, 100, _ => store.UpdateProgress(concurrent, p =>
            p.Stats["c___k_g"] = p.Stats.GetValueOrDefault("c___k_g") + 1));
        Check(store.GetProgress(concurrent).Stats["c___k_g"] == 100,
            "100 одновременных изменений не затирают друг друга");

        // Синхронизация читает файл под тем же локом и не может записать снимок до отчёта поверх отчёта.
        using var entered = new ManualResetEventSlim();
        using var release = new ManualResetEventSlim();
        var report = Task.Run(() => store.UpdateProgress(concurrent, p =>
        {
            entered.Set();
            if (!release.Wait(TimeSpan.FromSeconds(5))) throw new TimeoutException("проверка синхронизации");
            p.Stats["sc_general"] = 500;
        }));
        if (!entered.Wait(TimeSpan.FromSeconds(5))) throw new TimeoutException("проверка отчёта");
        var sync = Task.Run(() => store.UpdateProgress(concurrent, p =>
            p.Stats["sc_general"] = Math.Max(p.Stats.GetValueOrDefault("sc_general"), 100)));
        release.Set();
        Task.WaitAll(report, sync);
        Check(store.GetProgress(concurrent).Stats["sc_general"] == 500,
            "старая копия лаунчера не затирает свежий отчёт матча");

        var reloaded = new ProfileStore(cfg, log);
        reloaded.Load();
        Check(reloaded.GetProgress(id).Stats["sc_general"] == 2000 &&
              reloaded.GetProgress(concurrent).Stats["c___k_g"] == 100,
            "повторное открытие хранилища сохраняет звание и накопленные показатели");
        Console.WriteLine($"PROGRESS-DONE PASS={pass} FAIL={fail}; {root}");
        return fail == 0 ? 0 : 1;
    }
}
