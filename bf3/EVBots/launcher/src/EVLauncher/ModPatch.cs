using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Text;
using System.Text.RegularExpressions;

namespace EVBots.Launcher;

/// <summary>
/// 01.11: русский язык меню F12 мода fun-bots (просьба друга; правка мода — с разрешения пользователя 01.11).
/// Мод у каждого свой в BF-Portable (и хост, и гость грузят его со своего ПК). Ставит установщик EVBots-Setup, а лаунчер
/// при старте и по галке «Меню F12 на русском» (общий файл: EVLauncher и EVBotsSetup, поэтому без новшеств C# сверх net48):
///   ext\Shared\Languages\ru_RU.lua — тексты настроек, колеса Alt и сообщений (Language:add, как у других языков мода);
///   ext\Shared\Languages\Languages.lua — "ru_RU" в списке (выбор языка в настройках F12);
///   ext\Shared\Config.lua — Language = "ru_RU" (так язык включается в моде: Language.lua читает Config.Language и на
///     сервере, и на клиенте); снять галку — Language = nil;
///   ui.vuic — index.html с &lt;script&gt; перевода веб-панели (window.Language.ru_RU) и сам скрипт. Патч собран заранее
///     (launcher\tools\build-vuic-ru.js) и ставится, только если index.html в ui.vuic — тот же, что у нашей копии мода.
/// Бэкапов не делаем (пользователь 01.11: «не надо бэкап фанботсов», исходный мод у друзей есть в архиве).
/// </summary>
internal static class ModPatch
{
    public const string Lang = "ru_RU";

    /// <summary>Папка мода fun-bots в BF-Portable (или «Battlefield Portable») рядом с папкой root; нет — "".</summary>
    public static string FindModDir(string root)
    {
        foreach (var name in new[] { "BF-Portable", "Battlefield Portable" })
        {
            var d = Path.Combine(root, name, "Battlefield_3_Server", "Instance", "Admin", "Mods", "fun-bots");
            if (File.Exists(Path.Combine(d, "mod.json"))) return d;
        }
        return "";
    }

    public static List<string> ApplyRussian(string modDir, bool on)
    {
        var log = new List<string>();
        if (!File.Exists(Path.Combine(modDir, "mod.json")))
        {
            log.Add("мод fun-bots не найден — язык меню не ставлю");
            return log;
        }
        var langDir = Path.Combine(modDir, "ext", "Shared", "Languages");
        if (on)
        {
            var lua = Path.Combine(langDir, Lang + ".lua");
            var want = Resource("ru_RU.lua");
            if (!File.Exists(lua) || !File.ReadAllBytes(lua).SequenceEqual(want))
            {
                File.WriteAllBytes(lua, want);
                log.Add("ru_RU.lua записан");
            }
            log.Add(AddToLanguages(Path.Combine(langDir, "Languages.lua")));
            log.Add(PatchVuic(Path.Combine(modDir, "ui.vuic")));
        }
        log.Add(SetConfigLanguage(Path.Combine(modDir, "ext", "Shared", "Config.lua"), on ? "\"" + Lang + "\"" : "nil"));
        log.RemoveAll(s => s.Length == 0);
        return log;
    }

    private static byte[] Resource(string name)
    {
        var asm = Assembly.GetExecutingAssembly();
        using (var s = asm.GetManifestResourceStream("ModPatch." + name))
        {
            if (s == null) throw new InvalidOperationException("нет ресурса " + name);
            using (var ms = new MemoryStream())
            {
                s.CopyTo(ms);
                return ms.ToArray();
            }
        }
    }

    /// <summary>"ru_RU" строкой перед закрывающей «}» таблицы Languages; остальное — как было.</summary>
    private static string AddToLanguages(string path)
    {
        if (!File.Exists(path)) return "Languages.lua нет — список языков не дополнен";
        var text = File.ReadAllText(path);
        if (text.Contains("\"" + Lang + "\"")) return "";
        var close = text.IndexOf('}');
        if (close < 0) return "Languages.lua: нет «}» — список языков не дополнен";
        var nl = text.Contains("\r\n") ? "\r\n" : "\n";
        File.WriteAllText(path, text.Substring(0, close) + "\t\"" + Lang + "\"," + nl + text.Substring(close), new UTF8Encoding(false));
        return "Languages.lua: ru_RU добавлен";
    }

    private static readonly Regex LanguageLine = new Regex(@"(?m)^(\s*Language\s*=\s*)([^,\r\n]*)(,)");

    private static string SetConfigLanguage(string path, string value)
    {
        if (!File.Exists(path)) return "Config.lua мода нет — язык не задан";
        var bytes = File.ReadAllBytes(path);
        var bom = bytes.Length >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF;
        var utf8 = new UTF8Encoding(false);
        var text = utf8.GetString(bytes, bom ? 3 : 0, bytes.Length - (bom ? 3 : 0));
        var m = LanguageLine.Match(text);
        if (!m.Success) return "Config.lua: нет строки Language = … — язык не задан";
        if (m.Groups[2].Value.Trim() == value) return "";
        var g = m.Groups[2];
        var result = utf8.GetBytes(text.Substring(0, g.Index) + value + text.Substring(g.Index + g.Length));
        using (var f = File.Create(path))
        {
            if (bom) f.Write(new byte[] { 0xEF, 0xBB, 0xBF }, 0, 3);
            f.Write(result, 0, result.Length);
        }
        return "Config.lua: Language = " + value;
    }

    private struct Entry
    {
        public uint Size, Stored, Aux, Key, Offset;
    }

    private struct Add
    {
        public uint Key, Aux, Size;
        public byte[] Data;
    }

    /// <summary>
    /// Патч .evrp: "EVRP" | 1 | aux исходного index.html | n | n × {key, aux, size, stored, zstd}. Первая запись заменяет
    /// index.html, остальные добавляются в конец таблицы. Записи ui.vuic: {size, stored, aux, key, offset}.
    /// </summary>
    private static string PatchVuic(string path)
    {
        if (!File.Exists(path)) return "ui.vuic нет — веб-панель останется на английском";
        var d = File.ReadAllBytes(path);
        var p = Resource("ui-ru.evrp");
        if (d.Length < 12 || Encoding.ASCII.GetString(d, 0, 4) != "CIUV" || U32(d, 4) != 2)
            return "ui.vuic: не CIUV v2 — веб-панель останется на английском";
        if (Encoding.ASCII.GetString(p, 0, 4) != "EVRP" || U32(p, 4) != 1) return "ui-ru.evrp испорчен";
        var origAux = U32(p, 8);
        var count = (int)U32(p, 12);
        var add = new List<Add>();
        for (int i = 0, at = 16; i < count; i++)
        {
            var stored = (int)U32(p, at + 12);
            var data = new byte[stored];
            Buffer.BlockCopy(p, at + 16, data, 0, stored);
            add.Add(new Add { Key = U32(p, at), Aux = U32(p, at + 4), Size = U32(p, at + 8), Data = data });
            at += 16 + stored;
        }
        var n = (int)U32(d, 8);
        var entries = new List<Entry>();
        for (var i = 0; i < n; i++)
        {
            var o = 12 + i * 20;
            entries.Add(new Entry { Size = U32(d, o), Stored = U32(d, o + 4), Aux = U32(d, o + 8), Key = U32(d, o + 12), Offset = U32(d, o + 16) });
        }
        var index = entries.FindIndex(e => e.Key == add[0].Key);
        if (index < 0) return "ui.vuic: нет index.html — веб-панель останется на английском";
        if (entries[index].Aux == add[0].Aux) return "";  // уже с русским
        if (entries[index].Aux != origAux)
            return "ui.vuic: другая версия меню мода — веб-панель останется на английском (настройки — на русском)";

        // Новый файл: таблица записей, данные — как были (index.html — новый), добавленные — в конце
        var extra = add.Skip(1).Where(a => !entries.Any(e => e.Key == a.Key)).ToList();
        var total = n + extra.Count;
        var table = new List<Entry>();
        using (var ms = new MemoryStream())
        {
            ms.Write(new byte[12 + total * 20], 0, 12 + total * 20);
            for (var i = 0; i < n; i++)
            {
                var e = entries[i];
                var off = (uint)ms.Position;
                if (i == index)
                {
                    ms.Write(add[0].Data, 0, add[0].Data.Length);
                    table.Add(new Entry { Size = add[0].Size, Stored = (uint)add[0].Data.Length, Aux = add[0].Aux, Key = e.Key, Offset = off });
                }
                else
                {
                    ms.Write(d, (int)e.Offset, (int)e.Stored);
                    table.Add(new Entry { Size = e.Size, Stored = e.Stored, Aux = e.Aux, Key = e.Key, Offset = off });
                }
            }
            foreach (var a in extra)
            {
                table.Add(new Entry { Size = a.Size, Stored = (uint)a.Data.Length, Aux = a.Aux, Key = a.Key, Offset = (uint)ms.Position });
                ms.Write(a.Data, 0, a.Data.Length);
            }
            var buf = ms.ToArray();
            Encoding.ASCII.GetBytes("CIUV").CopyTo(buf, 0);
            BitConverter.GetBytes(2u).CopyTo(buf, 4);
            BitConverter.GetBytes((uint)table.Count).CopyTo(buf, 8);
            for (var i = 0; i < table.Count; i++)
            {
                var o = 12 + i * 20;
                BitConverter.GetBytes(table[i].Size).CopyTo(buf, o);
                BitConverter.GetBytes(table[i].Stored).CopyTo(buf, o + 4);
                BitConverter.GetBytes(table[i].Aux).CopyTo(buf, o + 8);
                BitConverter.GetBytes(table[i].Key).CopyTo(buf, o + 12);
                BitConverter.GetBytes(table[i].Offset).CopyTo(buf, o + 16);
            }
            var tmp = path + ".evbots-tmp";
            File.WriteAllBytes(tmp, buf);
            File.Copy(tmp, path, true);
            File.Delete(tmp);
        }
        return "ui.vuic: русский перевод веб-панели добавлен";
    }

    private static uint U32(byte[] b, int at) => BitConverter.ToUInt32(b, at);
}
