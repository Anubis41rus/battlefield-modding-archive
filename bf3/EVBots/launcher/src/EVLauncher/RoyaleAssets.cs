using System.Buffers.Binary;
using System.ComponentModel;
using System.Globalization;
using System.IO;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using Microsoft.Win32.SafeHandles;

namespace EVBots.Launcher;

// Local BF3 assets are never packaged or modified. DBO/CAS format references:
// Frostbite-Scripts (MIT), see GameAssetCache.NOTICE.txt. Native loader contract
// and independent parser checks: build-coop-overlay.py / verify-coop-overlay.py.
// Win32 cooked payload compatibility still needs the isolated runtime checkpoint.
internal static class RoyaleAssets
{
    private const int MaxIndex = 64 * 1024 * 1024;
    private const long MaxCopies = 256L * 1024 * 1024;
    private static readonly UTF8Encoding Utf8 = new(false, true);
    private static readonly object Gate = new();
    private const string ManifestName = "overlay-manifest.json";
    private const string WreckFire = "fx/vehicles/damagestates/fire/fx_wreckfire_01";
    private const string StrippedFireSha = "0FD49977C063FE5EEA96B74DB186BB80C8AE168C";
    private const string FullFireSha = "C4B2A8B04780F5CCA7C52C33616B529A5AE7D599";
    // Stock dedicated 00D664A0..00D66552 calls 0054FB10 for these ignored
    // resource names. Hash = lower-case DJB xor, seed 5381. All nine occur in
    // Win32 COOP; none occurs in the stock server's 512 bundles. Do not infer
    // client-only status from a missing loader or drop unknown RES types.
    private static readonly HashSet<uint> ClientOnlyResources =
    [
        0x2D47A5FF, // SwfMovie
        0x30B4A553, // OccluderMesh
        0x36F3F2C0, // IShaderDatabase
        0x49B156D4, // MeshSet
        0x59CEEB57, // EnlightenShaderDatabase
        0x5C4954A6, // DxTexture
        0x70C5CB3E, // EnlightenDatabase
        0x7AEFC446, // StaticEnlightenDatabase
        0xE156AF73  // EnlightenProbeSet
    ];
    private static readonly HashSet<uint> StockResources =
        [0x22FE8AC8, 0x319D8CD0, 0x4864737B, 0x51A3C853, 0x6BB6D7D2, 0x91043F65, 0xD070EED1];
    private sealed record Input(string Path, long Length, long Modified, string Sha256);
    private sealed record Link(string Path, string Target, bool Directory, long Length, long Modified, bool Cas);
    private sealed record WrittenLink(string Path, string Target, string Kind, long Length, long Modified, string? Sha256);
    private sealed record Manifest(int Schema, string Client, string Server, int CasEntries,
        List<Input> Inputs, List<WrittenLink> Links, Dictionary<string, string> Generated);
    private sealed record CasEntry(string Path, uint Offset, uint Size);
    private sealed class Plan
    {
        internal string Client = "", Server = "";
        internal int CasEntries;
        internal readonly List<Input> Inputs = [];
        internal readonly List<Link> Links = [];
        internal readonly Dictionary<string, byte[]> Generated = new(StringComparer.OrdinalIgnoreCase);
    }

    // Returns the actual Data directory, ready for RoyaleServerLayout's Data link.
    // Synchronous by design: call on a worker thread and marshal the log callback.
    internal static string Prepare(string clientGameDir, string sourceServerDir, string targetRoot, Action<string> log)
    {
        lock (Gate)
        {
            var client = LocalPath(clientGameDir);
            var server = LocalPath(sourceServerDir);
            var target = LocalPath(targetRoot);
            foreach (var path in new[] { client, server, target }) NoReparseAncestors(path);
            Require(!Overlaps(client, server) && !Overlaps(target, client) && !Overlaps(target, server),
                "Папка кэша Royale должна быть отделена от BF3 и BF-Portable.");
            Require(Directory.Exists(client) && Directory.Exists(server), "Не найдена папка BF3 или BF-Portable.");
            // Build/validate every metadata transformation before any output write.
            var plan = BuildPlan(client, server);
            ValidateOutputNames(plan);
            if (Path.Exists(target))
            {
                ValidateReady(target, plan);
                return Path.Combine(target, "Data");
            }
            var parent = Path.GetDirectoryName(target)!;
            Directory.CreateDirectory(parent);
            var lockPath = target + ".prepare.lock";
            NoReparseAncestors(lockPath);
            using var cacheLock = new FileStream(lockPath, FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None);
            if (Path.Exists(target))
            {
                ValidateReady(target, plan);
                return Path.Combine(target, "Data");
            }
            var stage = target + ".preparing-" + Guid.NewGuid().ToString("N");
            Directory.CreateDirectory(stage);
            try
            {
                log("Подготавливаю отдельные ресурсы Royale из установленной BF3…");
                long copied = 0;
                var links = new List<WrittenLink>();
                foreach (var link in plan.Links)
                {
                    var destination = Child(stage, link.Path);
                    Directory.CreateDirectory(Path.GetDirectoryName(destination)!);
                    var kind = "junction";
                    string? hash = null;
                    if (link.Directory) CreateJunction(destination, link.Target);
                    else if (CreateHardLinkW(destination, link.Target, IntPtr.Zero)) kind = "hardlink";
                    else if (CreateFileSymlink(destination, link.Target)) kind = "symlink";
                    else
                    {
                        // Never copy a CAS archive, even a small one. Other source
                        // metadata can be copied, with both per-file and total caps.
                        Require(!link.Cas && link.Length <= MaxIndex && copied + link.Length <= MaxCopies,
                            "Windows не разрешает ссылки между томами. Помести BF3, BF-Portable и кэш Royale на один том NTFS либо разреши символьные ссылки Windows. Большие игровые архивы не копируются.");
                        File.Copy(link.Target, destination, false);
                        copied += link.Length;
                        hash = HashFile(destination);
                        Require(hash == HashFile(link.Target), "Копия ресурса Royale не совпала с источником.");
                        kind = "copy";
                    }
                    var written = new WrittenLink(link.Path, link.Target, kind, link.Length, link.Modified, hash);
                    ValidateLink(stage, link, written);
                    links.Add(written);
                }
                foreach (var (relative, bytes) in plan.Generated)
                {
                    var path = Child(stage, relative);
                    Directory.CreateDirectory(Path.GetDirectoryName(path)!);
                    using var file = new FileStream(path, FileMode.CreateNew, FileAccess.Write, FileShare.None);
                    file.Write(bytes);
                }
                var manifest = new Manifest(3, client, server, plan.CasEntries, plan.Inputs, links,
                    plan.Generated.ToDictionary(x => x.Key, x => Hash(x.Value), StringComparer.OrdinalIgnoreCase));
                File.WriteAllText(Path.Combine(stage, ManifestName), JsonSerializer.Serialize(manifest), Utf8);
                ValidateReady(stage, plan);
                Directory.Move(stage, target); // publish only the fully checked directory
                log($"Кэш ресурсов Royale подготовлен: 2 набора COOP, {plan.CasEntries} записей CAS; stripped FX исключён из двух XP3 main, полный FX с emitter-зависимостями сохранён в COOP. CAS-архивы используются по ссылкам.");
                return Path.Combine(target, "Data");
            }
            catch
            {
                // Only the unique directory created by this invocation. Never follow
                // links during cleanup, and never remove a pre-existing cache.
                RemoveStage(stage, stage);
                throw;
            }
        }
    }

    private static Plan BuildPlan(string client, string server)
    {
        var plan = new Plan { Client = client, Server = server };
        byte[] Read(string path)
        {
            NoReparseAncestors(path);
            var info = new FileInfo(path);
            Require(info.Exists && info.Length <= MaxIndex, "Отсутствует или слишком велик индекс BF3: " + path);
            var data = File.ReadAllBytes(path);
            plan.Inputs.Add(new(path, info.Length, info.LastWriteTimeUtc.Ticks, Hash(data)));
            return Decode(data);
        }
        Node ReadTree(string path, out byte[] raw)
        {
            raw = Read(path);
            var tree = ParseWhole(raw);
            Require(tree.Encode().AsSpan().SequenceEqual(raw), "DBO roundtrip не совпал: " + path);
            return tree;
        }
        byte[] ReadSuperbundle(string path)
        {
            NoReparseAncestors(path);
            using var file = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read);
            Require(file.Length <= MaxCopies, "Слишком велик non-CAS superbundle BF3: " + path);
            var info = new FileInfo(path);
            var bytes = new byte[checked((int)file.Length)];
            file.ReadExactly(bytes);
            Require(file.Length == bytes.Length, "Размер non-CAS superbundle изменился: " + path);
            plan.Inputs.Add(new(path, bytes.Length, info.LastWriteTimeUtc.Ticks, Hash(bytes)));
            return bytes;
        }
        var dataRoot = Path.Combine(server, "Data");
        var layout = ReadTree(Path.Combine(dataRoot, "layout.toc"), out _);
        var fs = layout.List("fs");
        Require(fs.Count == 1 && fs[0].Text() == "initfs_Linux" && layout.Field("head").Integer() == 45002,
            "Неподдерживаемый layout выделенного сервера BF3.");
        var layoutBundles = layout.List("superBundles");
        var oldNames = layoutBundles.Select(x => x.Field("name").Text()).ToArray();
        Require(oldNames.Length == 39 && oldNames.Distinct(StringComparer.OrdinalIgnoreCase).Count() == oldNames.Length
            && oldNames.All(x => x.StartsWith("Linux/", StringComparison.Ordinal)), "Неподдерживаемый список серверных superBundles.");
        var catalog = new Dictionary<string, CasEntry>(StringComparer.Ordinal);
        byte[]? header = null;
        foreach (var root in new[] { Path.Combine(client, "Data"), Path.Combine(client, "Update", "Patch", "Data") })
        {
            var cat = Read(Path.Combine(root, "cas.cat"));
            Require(cat.Length >= 16 && (cat.Length - 16) % 32 == 0 && cat.AsSpan(0, 16).SequenceEqual("NyanNyanNyanNyan"u8), "Некорректный CAS-каталог BF3.");
            header ??= cat.AsSpan(0, 16).ToArray();
            Require(cat.AsSpan(0, 16).SequenceEqual(header), "Разные форматы CAS-каталогов BF3.");
            for (var at = 16; at < cat.Length; at += 32)
            {
                var sha = Convert.ToHexString(cat.AsSpan(at, 20));
                var offset = U32(cat, at + 20); var size = U32(cat, at + 24); var number = U32(cat, at + 28);
                Require(number is > 0 and < 100, "Неверный номер CAS-архива BF3.");
                catalog[sha] = new(Path.Combine(root, $"cas_{number:00}.cas"), offset, size); // Patch wins
            }
        }
        var needed = new HashSet<string>(StringComparer.Ordinal);
        var totalBundles = 0;
        foreach (var name in new[] { "COOP_009", "COOP_010" })
        {
            var source = Path.Combine(client, "Update", "Patch", "Data", "Win32", "Levels", name, name + ".toc");
            var toc = ReadTree(source, out _);
            var sb = ReadTree(Path.ChangeExtension(source, ".sb"), out var sbRaw);
            Require(toc.Field("cas").Boolean() && toc.Field("name").Text() == $"Win32/Levels/{name}/{name}", "Неподдерживаемый COOP индекс: " + name);
            var entries = toc.List("bundles");
            Require(entries.Count == (name == "COOP_009" ? 20 : 19), "Неподдерживаемое число COOP bundles.");
            var bundles = sb.List("bundles");
            Require(bundles.Count == entries.Count, "Число COOP bundles не совпало с TOC.");
            var byName = new Dictionary<string, Node>(StringComparer.OrdinalIgnoreCase);
            foreach (var bundle in bundles)
                Require(byName.TryAdd(bundle.Field("path").Text(), bundle), "Повторяется имя COOP bundle.");
            var tracked = bundles.ToHashSet();
            var original = sb.EncodeTracked(tracked, out var originalSpans);
            Require(original.AsSpan().SequenceEqual(sbRaw), "Исходные границы COOP не прошли roundtrip.");
            var matched = new HashSet<Node>();
            foreach (var entry in entries)
            {
                var (offset, size) = Span(entry, sbRaw.Length);
                Require(byName.TryGetValue(entry.Field("id").Text(), out var bundle) && matched.Add(bundle!)
                    && originalSpans[bundle!] == (offset, size), "Исходный TOC не указывает на отдельный COOP bundle.");
                Require(entry.Field("checksum").Raw.AsSpan().SequenceEqual(SHA1.HashData(sbRaw.AsSpan(offset, size))), "Повреждён checksum исходного COOP bundle.");
            }
            ValidateCoopFire(bundles, $"Win32/Levels/{name}/{name}");
            foreach (var bundle in bundles) FilterClientResources(bundle);
            ValidateCoopFire(bundles, $"Win32/Levels/{name}/{name}");
            Rename(sb); Rename(toc);
            var rewritten = sb.EncodeTracked(tracked, out var rewrittenSpans);
            foreach (var entry in entries)
            {
                // TOC ids are aliased now; the lookup was built from the source.
                var sourceId = "Win32/" + entry.Field("id").Text()[6..];
                var (offset, size) = rewrittenSpans[byName[sourceId]];
                entry.Field("offset").Integer((ulong)offset);
                entry.Field("size").Integer((ulong)size);
                var at = offset; var count = 0;
                var bundle = Parse(rewritten, ref at, 0, ref count);
                Require(at == offset + size && string.Equals(bundle.Field("path").Text(), entry.Field("id").Text(), StringComparison.OrdinalIgnoreCase), "Границы или имя COOP bundle не совпали.");
                Require(entry.Field("id").Text().StartsWith("Linux/", StringComparison.Ordinal), "COOP bundle не получил Linux alias.");
                var checksum = entry.Field("checksum");
                Require(checksum.Kind == 16, "Неверный тип checksum COOP.");
                checksum.Raw = SHA1.HashData(rewritten.AsSpan(offset, size));
                totalBundles++;
            }
            // DBX are editor sidecars, intentionally missing from shipped CAS.
            foreach (var bundle in bundles)
                foreach (var group in new[] { "ebx", "res", "chunks" })
                    foreach (var item in bundle.OptionalList(group)) CollectSha(item, needed);
            var relative = $"Data/Linux/Levels/{name}/{name}";
            plan.Generated.Add(relative + ".sb", rewritten);
            plan.Generated.Add(relative + ".toc", toc.Encode());
            var alias = $"Linux/Levels/{name}/{name}";
            Require(!oldNames.Contains(alias, StringComparer.OrdinalIgnoreCase), "COOP уже присутствует в исходном сервере.");
            layoutBundles.Add(Node.Object([Node.String("name", alias)]));
        }
        Require(totalBundles == 39 && needed.Count > 0 && needed.All(catalog.ContainsKey), "Не все COOP ресурсы найдены в CAS установленной игры.");
        var archives = needed.Select(x => catalog[x].Path).Distinct(StringComparer.OrdinalIgnoreCase).OrderBy(x => x, StringComparer.OrdinalIgnoreCase).ToArray();
        Require(archives.Length < 100, "Слишком много COOP CAS-архивов.");
        var numbers = archives.Select((path, index) => (path, number: index + 1)).ToDictionary(x => x.path, x => x.number, StringComparer.OrdinalIgnoreCase);
        var archiveLengths = new Dictionary<string, long>(StringComparer.OrdinalIgnoreCase);
        foreach (var archive in archives)
        {
            NoReparseAncestors(archive);
            var info = new FileInfo(archive);
            Require(info.Exists, "Не найден COOP CAS-архив: " + archive);
            archiveLengths.Add(archive, info.Length);
        }
        using (var stream = new MemoryStream())
        using (var writer = new BinaryWriter(stream, Utf8, leaveOpen: true))
        {
            writer.Write(header!);
            foreach (var sha in needed.Order(StringComparer.Ordinal))
            {
                var entry = catalog[sha];
                Require((ulong)entry.Offset + entry.Size <= (ulong)archiveLengths[entry.Path], "COOP диапазон выходит за CAS-архив: " + entry.Path);
                writer.Write(Convert.FromHexString(sha)); writer.Write(entry.Offset); writer.Write(entry.Size); writer.Write((uint)numbers[entry.Path]);
            }
            plan.Generated.Add("Data/cas.cat", stream.ToArray());
        }
        plan.Generated.Add("Data/layout.toc", layout.Encode());
        plan.CasEntries = needed.Count;
        foreach (var name in new[] { "XP3_Shield", "XP3_Alborz" })
        {
            var source = Path.Combine(dataRoot, "Linux", "Levels", name, name + ".toc");
            var toc = ReadTree(source, out _);
            var rewritten = RewriteXp3(toc, ReadSuperbundle(Path.ChangeExtension(source, ".sb")), name);
            var relative = $"Data/Linux/Levels/{name}/{name}";
            plan.Generated.Add(relative + ".sb", rewritten);
            plan.Generated.Add(relative + ".toc", toc.Encode());
        }
        foreach (var path in Directory.EnumerateFileSystemEntries(dataRoot).Order(StringComparer.OrdinalIgnoreCase))
        {
            var name = Path.GetFileName(path);
            if (name is "layout.toc" or "Linux") continue;
            Require(!name.Equals("cas.cat", StringComparison.OrdinalIgnoreCase) && !name.StartsWith("cas_", StringComparison.OrdinalIgnoreCase), "Исходный сервер уже содержит CAS-каталог.");
            AddLink(plan, path, "Data/" + name);
        }
        var linux = Path.Combine(dataRoot, "Linux");
        foreach (var path in Directory.EnumerateFileSystemEntries(linux).Order(StringComparer.OrdinalIgnoreCase))
            if (!Path.GetFileName(path).Equals("Levels", StringComparison.OrdinalIgnoreCase)) AddLink(plan, path, "Data/Linux/" + Path.GetFileName(path));
        foreach (var path in Directory.EnumerateFileSystemEntries(Path.Combine(linux, "Levels")).Order(StringComparer.OrdinalIgnoreCase))
        {
            var name = Path.GetFileName(path);
            Require(!name.Equals("COOP_009", StringComparison.OrdinalIgnoreCase) && !name.Equals("COOP_010", StringComparison.OrdinalIgnoreCase), "COOP уже присутствует в исходном сервере.");
            if (name.Equals("XP3_Shield", StringComparison.OrdinalIgnoreCase) || name.Equals("XP3_Alborz", StringComparison.OrdinalIgnoreCase))
            {
                foreach (var sibling in Directory.EnumerateFileSystemEntries(path).Order(StringComparer.OrdinalIgnoreCase))
                {
                    var file = Path.GetFileName(sibling);
                    if (file.Equals(name + ".toc", StringComparison.OrdinalIgnoreCase) || file.Equals(name + ".sb", StringComparison.OrdinalIgnoreCase)) continue;
                    AddLink(plan, sibling, "Data/Linux/Levels/" + name + "/" + file);
                }
            }
            else AddLink(plan, path, "Data/Linux/Levels/" + name);
        }
        foreach (var path in archives) AddLink(plan, path, $"Data/cas_{numbers[path]:00}.cas", cas: true);
        Require(plan.Links.Any(x => x.Path == "Data/initfs_Linux"), "Не найден initfs_Linux.");
        return plan;
    }

    private static void ValidateCoopFire(List<Node> bundles, string mainPath)
    {
        var mains = bundles.Where(x => Same(x.Field("path").Text(), mainPath)).ToArray();
        Require(mains.Length == 1, "Отсутствует или повторяется COOP main.");
        (string Name, string Sha)[] expected =
        [
            (WreckFire, FullFireSha),
            ("fx/vehicles/damagestates/wreck/tank/emitters/em_vehicledestruction_wreck_tank_01_fire_m_01", "B6A36C0765BBD4C2B39BB5AAC32521391A40A00B"),
            ("fx/vehicles/damagestates/wreck/tank/emitters/em_vehicledestruction_wreck_tank_01_fire_m_directional_01", "71CB832CBF6A3FBD39E12648D159D88945D60100"),
            ("fx/vehicles/damagestates/wreck/tank/emitters/em_vehicledestruction_wreck_tank_01_smoke_m_01", "E7B59393D450F2201D5776272F6B8EC3F9A294BC"),
            ("sound/gamesounds/fires/fire_burn_medium", "47F8E5C95485C69C0CF072A42FDA858FC06AF001")
        ];
        var ebx = mains[0].List("ebx");
        foreach (var (name, sha) in expected)
        {
            var matches = ebx.Where(x => Same(x.Field("name").Text(), name) || Convert.ToHexString(x.Field("sha1").Raw) == sha).ToArray();
            Require(matches.Length == 1 && Same(matches[0].Field("name").Text(), name)
                && matches[0].Field("sha1").Kind == 16 && Convert.ToHexString(matches[0].Field("sha1").Raw) == sha,
                "Не найден полный FX или его зависимость в COOP main: " + name);
        }
    }

    private sealed record NonCasEntry(int ShaOffset, int RowOffset, int PayloadOffset, int Size, string? Name);
    private sealed record NonCasBundle(int MetaEnd, int EbxCount, List<NonCasEntry> Entries);
    private static uint Be32(byte[] data, int at) => BinaryPrimitives.ReadUInt32BigEndian(data.AsSpan(at, 4));
    private static void Be32(byte[] data, int at, uint value) => BinaryPrimitives.WriteUInt32BigEndian(data.AsSpan(at, 4), value);
    private static Node? OptionalField(Node node, string name)
    {
        var found = node.Children!.Where(x => Utf8.GetString(x.Name) == name).ToArray();
        Require(found.Length <= 1, "Повторяется поле DBO: " + name);
        return found.Length == 0 ? null : found[0];
    }
    private static void NonCasFlags(Node node)
    {
        foreach (var flag in new[] { "delta", "base", "cas" })
            if (OptionalField(node, flag) is { } value)
                Require(!value.Boolean(), "Неподдерживаемый non-CAS флаг: " + flag);
    }
    private static NonCasBundle ReadNonCasBundle(byte[] data, int offset, int size)
    {
        Require(offset >= 0 && size >= 36 && offset <= data.Length - size && offset % 16 == 0, "Неверные границы non-CAS bundle.");
        var end = offset + size; var start = offset + 4; var metaSize = Be32(data, offset);
        Require(metaSize >= 32 && metaSize <= MaxIndex && metaSize <= size - 4 && Be32(data, start) == 0x970D1C13, "Неверный non-CAS заголовок.");
        var metaEnd = start + (int)metaSize;
        var count = Be32(data, start + 4); var ebx = Be32(data, start + 8);
        var res = Be32(data, start + 12); var chunks = Be32(data, start + 16);
        Require(count <= 1_000_000 && count == (ulong)ebx + res + chunks, "Не совпали счётчики non-CAS.");
        var tablesEnd = start + 32L + count * 20L + (ebx + (long)res) * 12 + res * 20L + chunks * 28L;
        var strings = Be32(data, start + 20); var chunkOffset = Be32(data, start + 24); var chunkSize = Be32(data, start + 28);
        Require(tablesEnd <= metaEnd && strings <= metaSize && start + (long)strings >= tablesEnd, "Таблицы non-CAS выходят за метаданные.");
        if (chunks == 0) Require(chunkOffset == 0 && chunkSize == 0, "Неизвестные chunkMeta без chunks.");
        else
        {
            Require(chunkSize > 0 && start + (long)chunkOffset >= tablesEnd && (ulong)chunkOffset + chunkSize <= strings, "Неверные границы chunkMeta.");
            var bytes = data.AsSpan(start + (int)chunkOffset, (int)chunkSize).ToArray();
            var tree = ParseWhole(bytes);
            Require(tree.Kind == 1 && tree.Children!.Count == chunks && tree.Encode().AsSpan().SequenceEqual(bytes), "Неверные записи chunkMeta.");
        }
        var entries = new List<NonCasEntry>((int)count);
        var shaStart = start + 32; var rows = shaStart + (int)count * 20;
        var chunkRows = rows + (int)(ebx + res) * 12 + (int)res * 20;
        var payload = metaEnd;
        for (var i = 0; i < count; i++)
        {
            int row; uint length; string? name = null;
            if (i < ebx + res)
            {
                row = rows + i * 12;
                var nameOffset = Be32(data, row);
                Require((ulong)strings + nameOffset < metaSize, "Имя non-CAS выходит за таблицу строк.");
                var nameStart = start + (int)strings + (int)nameOffset;
                var nameEnd = Array.IndexOf(data, (byte)0, nameStart, metaEnd - nameStart);
                Require(nameEnd > nameStart && nameEnd - nameStart < 4096, "Неверное имя non-CAS.");
                name = Utf8.GetString(data, nameStart, nameEnd - nameStart);
                length = Be32(data, row + 4);
            }
            else
            {
                row = chunkRows + (i - (int)ebx - (int)res) * 28;
                var rangeStart = Be32(data, row + 16); var rangeEnd = Be32(data, row + 20);
                Require(rangeEnd >= rangeStart, "Неверный диапазон non-CAS chunk.");
                length = rangeEnd - rangeStart;
            }
            var aligned = ((long)payload + 15) & ~15L;
            Require(aligned <= end && length <= end - aligned, "Payload non-CAS выходит за bundle.");
            entries.Add(new(shaStart + i * 20, row, (int)aligned, (int)length, name));
            payload = (int)aligned + (int)length;
        }
        return new(metaEnd, (int)ebx, entries);
    }

    private static byte[] RewriteXp3(Node toc, byte[] source, string name)
    {
        NonCasFlags(toc);
        Require(Same(toc.Field("name").Text(), $"Linux/Levels/{name}/{name}"), "Неверное имя XP3 TOC.");
        var entries = toc.List("bundles");
        Require(entries.Count == 19, "Неподдерживаемое число XP3 bundles.");
        var spans = entries.Select(x => (Entry: x, Span: Span(x, source.Length))).OrderBy(x => x.Span.Offset).ToArray();
        var ids = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        NonCasEntry? removed = null; NonCasBundle? main = null; Node? mainToc = null;
        var mainOffset = 0; var previousEnd = 0;
        foreach (var (entry, (offset, size)) in spans)
        {
            NonCasFlags(entry);
            var id = entry.Field("id").Text();
            Require(ids.Add(id) && id.StartsWith($"Linux/levels/{name}/", StringComparison.OrdinalIgnoreCase) && offset >= previousEnd, "Повторяются или пересекаются XP3 bundles.");
            var bundle = ReadNonCasBundle(source, offset, size);
            var checksum = entry.Field("checksum");
            // Non-CAS TOC checksums cover the ordered SHA1 table, not the bundle
            // bytes (including empty bundles: SHA1 of zero bytes).
            Require(checksum.Kind == 16 && checksum.Raw.AsSpan().SequenceEqual(SHA1.HashData(source.AsSpan(offset + 36, bundle.Entries.Count * 20))), "Повреждён checksum XP3 bundle.");
            var hits = bundle.Entries.Where(x => Same(x.Name ?? "", WreckFire) || Convert.ToHexString(source.AsSpan(x.ShaOffset, 20)) == StrippedFireSha).ToArray();
            if (Same(id, $"Linux/levels/{name}/{name}"))
            {
                Require(main is null && hits.Length == 1, "Отсутствует или повторяется stripped FX в XP3 main.");
                var hit = hits[0]; var index = bundle.Entries.IndexOf(hit);
                Require(index < bundle.EbxCount && Same(hit.Name ?? "", WreckFire)
                    && Convert.ToHexString(source.AsSpan(hit.ShaOffset, 20)) == StrippedFireSha
                    && hit.Size == 3584 && Be32(source, hit.RowOffset + 8) == 3584
                    && Convert.ToHexString(SHA1.HashData(source.AsSpan(hit.PayloadOffset, hit.Size))) == StrippedFireSha
                    && U32(source, hit.PayloadOffset) == 0x0FB2D1CE && U32(source, hit.PayloadOffset + 12) == 0,
                    "Неподдерживаемый stripped FX payload XP3.");
                var precedingEnd = index == 0 ? bundle.MetaEnd : bundle.Entries[index - 1].PayloadOffset + bundle.Entries[index - 1].Size;
                // The verified raw entry and metadata deletion are both multiples of
                // ABS16; deleting no padding keeps every other byte and alignment.
                Require(precedingEnd == hit.PayloadOffset, "Неизвестное выравнивание stripped FX XP3.");
                removed = hit; main = bundle; mainToc = entry; mainOffset = offset;
            }
            else Require(hits.Length == 0, "Stripped FX найден вне XP3 main.");
            previousEnd = offset + size;
        }
        Require(removed is not null && main is not null && mainToc is not null, "Не найден XP3 main.");
        var cuts = new[] { (Offset: removed!.ShaOffset, Size: 20), (Offset: removed.RowOffset, Size: 12), (Offset: removed.PayloadOffset, Size: removed.Size) };
        int Map(int offset, int size)
        {
            Require(!cuts.Any(x => offset < x.Offset + x.Size && (long)offset + size > x.Offset), "TOC chunk пересекает удаляемый FX или его метаданные.");
            return offset - cuts.Where(x => x.Offset + x.Size <= offset).Sum(x => x.Size);
        }
        var chunks = toc.OptionalList("chunks").Select(x => (Entry: x, Span: Span(x, source.Length))).ToArray();
        foreach (var (entry, (offset, size)) in chunks)
        {
            NonCasFlags(entry);
            Require(size > 0 && entry.Field("id").Kind == 15, "Неверный TOC chunk XP3.");
            if (OptionalField(entry, "checksum") is { } checksum)
                Require(checksum.Kind == 16 && checksum.Raw.AsSpan().SequenceEqual(SHA1.HashData(source.AsSpan(offset, size))), "Повреждён checksum TOC chunk XP3.");
            Map(offset, size);
        }
        using var output = new MemoryStream(source.Length - cuts.Sum(x => x.Size));
        var cursor = 0;
        foreach (var cut in cuts)
        {
            output.Write(source.AsSpan(cursor, cut.Offset - cursor));
            cursor = cut.Offset + cut.Size;
        }
        output.Write(source.AsSpan(cursor));
        var rewritten = output.ToArray(); var header = mainOffset + 4;
        Be32(rewritten, mainOffset, Be32(source, mainOffset) - 32);
        Be32(rewritten, header + 4, Be32(source, header + 4) - 1);
        Be32(rewritten, header + 8, Be32(source, header + 8) - 1);
        Be32(rewritten, header + 20, Be32(source, header + 20) - 32);
        var chunkMeta = Be32(source, header + 24);
        if (chunkMeta != 0) Be32(rewritten, header + 24, chunkMeta - 32);
        foreach (var (entry, (offset, size)) in spans)
        {
            var nextOffset = ReferenceEquals(entry, mainToc) ? offset : Map(offset, size);
            var nextSize = ReferenceEquals(entry, mainToc) ? size - cuts.Sum(x => x.Size) : size;
            var checkedBundle = ReadNonCasBundle(rewritten, nextOffset, nextSize);
            Require(checkedBundle.Entries.All(x => !Same(x.Name ?? "", WreckFire)
                && Convert.ToHexString(rewritten.AsSpan(x.ShaOffset, 20)) != StrippedFireSha), "Stripped FX остался в XP3.");
            entry.Field("offset").Integer((ulong)nextOffset);
            entry.Field("size").Integer((ulong)nextSize);
            entry.Field("checksum").Raw = SHA1.HashData(rewritten.AsSpan(nextOffset + 36, checkedBundle.Entries.Count * 20));
        }
        foreach (var (entry, (offset, size)) in chunks) entry.Field("offset").Integer((ulong)Map(offset, size));
        return rewritten;
    }

    private static void FilterClientResources(Node bundle)
    {
        ulong TotalSize()
        {
            ulong total = 0;
            foreach (var group in new[] { "ebx", "dbx", "res", "chunks" })
                foreach (var item in bundle.OptionalList(group)) total = checked(total + item.Field("size").Integer());
            return total;
        }
        Require(bundle.Field("totalSize").Integer() == TotalSize(), "Неподдерживаемый totalSize исходного COOP bundle.");
        var resources = bundle.OptionalList("res").ToArray();
        foreach (var item in resources)
        {
            var type = item.Field("resType").Integer();
            Require(type <= uint.MaxValue && (StockResources.Contains((uint)type) || ClientOnlyResources.Contains((uint)type)),
                "Неизвестный тип ресурса COOP: " + type.ToString("X8", CultureInfo.InvariantCulture));
        }
        if (resources.Length != 0)
            bundle.List("res").RemoveAll(item => ClientOnlyResources.Contains((uint)item.Field("resType").Integer()));
        bundle.Field("totalSize").Integer(TotalSize());
    }

    private static void AddLink(Plan plan, string source, string relative, bool cas = false)
    {
        NoReparseAncestors(source);
        var info = new FileInfo(source);
        var directory = Directory.Exists(source);
        Require(directory || info.Exists, "Не найден исходный ресурс: " + source);
        plan.Links.Add(new(relative, source, directory, directory ? 0 : info.Length, directory ? 0 : info.LastWriteTimeUtc.Ticks, cas));
    }
    private static void ValidateOutputNames(Plan plan)
    {
        var all = new HashSet<string>(StringComparer.OrdinalIgnoreCase) { ManifestName };
        foreach (var name in plan.Generated.Keys.Concat(plan.Links.Select(x => x.Path)))
        {
            Require(name.Split('/').All(x => x.Length > 0 && x is not "." and not ".." && x.IndexOfAny(Path.GetInvalidFileNameChars()) < 0), "Недопустимый путь ресурса Royale.");
            Require(all.Add(name), "Совпали пути ресурсов Royale: " + name);
        }
        foreach (var name in all)
        {
            var at = name.LastIndexOf('/');
            while (at >= 0)
            {
                Require(!all.Contains(name[..at]), "Выходной файл перекрывает каталог ресурсов Royale.");
                at = name.LastIndexOf('/', at - 1);
            }
        }
    }

    private static void ValidateReady(string root, Plan plan)
    {
        NoReparseAncestors(root);
        var path = Path.Combine(root, ManifestName);
        NoReparseAncestors(path);
        Require(File.Exists(path) && new FileInfo(path).Length <= 1024 * 1024, "Папка ресурсов Royale уже существует, но не является завершённым кэшем: " + root);
        Manifest? manifest;
        try { manifest = JsonSerializer.Deserialize<Manifest>(File.ReadAllText(path, Utf8)); }
        catch (JsonException e) { throw new InvalidDataException("Повреждён manifest ресурсов Royale.", e); }
        Require(manifest is { Schema: 3 } && Same(manifest.Client, plan.Client) && Same(manifest.Server, plan.Server)
            && manifest.CasEntries == plan.CasEntries && manifest.Inputs is not null && manifest.Links is not null && manifest.Generated is not null,
            "Кэш Royale создан для другой установки или версии. Нужна новая папка кэша.");
        var m = manifest!;
        var inputs = m.Inputs ?? throw new InvalidDataException("Отсутствует список источников Royale.");
        var links = m.Links ?? throw new InvalidDataException("Отсутствует список ссылок Royale.");
        var generated = m.Generated ?? throw new InvalidDataException("Отсутствует список метаданных Royale.");
        Require(inputs.SequenceEqual(plan.Inputs) && links.Count == plan.Links.Count && generated.Count == plan.Generated.Count,
            "Источники ресурсов Royale изменились. Нужна новая папка кэша.");
        var expectedPaths = new HashSet<string>(StringComparer.OrdinalIgnoreCase) { ManifestName };
        foreach (var (relative, bytes) in plan.Generated)
        {
            var file = Child(root, relative); NoReparseAncestors(file);
            Require(generated.TryGetValue(relative, out var hash) && hash == Hash(bytes) && File.Exists(file) && HashFile(file) == hash,
                "Повреждены подготовленные метаданные Royale: " + relative);
            expectedPaths.Add(relative);
        }
        for (var i = 0; i < plan.Links.Count; i++)
        {
            ValidateLink(root, plan.Links[i], links[i]);
            expectedPaths.Add(plan.Links[i].Path);
        }
        // Complete metadata/source snapshot must still be current at publication.
        foreach (var input in plan.Inputs)
        {
            NoReparseAncestors(input.Path);
            var info = new FileInfo(input.Path);
            Require(info.Exists && info.Length == input.Length && info.LastWriteTimeUtc.Ticks == input.Modified && HashFile(input.Path) == input.Sha256,
                "Исходные метаданные BF3 изменились во время подготовки.");
        }
        CheckTree(root, root, expectedPaths);
    }
    private static void ValidateLink(string root, Link source, WrittenLink written)
    {
        Require(written.Path == source.Path && Same(written.Target, source.Target) && written.Length == source.Length && written.Modified == source.Modified,
            "Путь ресурса Royale не соответствует manifest.");
        NoReparseAncestors(source.Target);
        var path = Child(root, source.Path); NoReparseAncestors(Path.GetDirectoryName(path)!);
        if (source.Directory)
        {
            Require(written.Kind == "junction" && Directory.Exists(source.Target), "Неверный тип ссылки каталога Royale.");
            CheckLinkTarget(new DirectoryInfo(path), source.Target);
            return;
        }
        var info = new FileInfo(source.Target);
        Require(info.Exists && info.Length == source.Length && info.LastWriteTimeUtc.Ticks == source.Modified, "Исходный ресурс BF3 изменился: " + source.Target);
        Require(File.Exists(path), "Отсутствует ресурс Royale: " + source.Path);
        switch (written.Kind)
        {
            case "symlink": CheckLinkTarget(new FileInfo(path), source.Target); break;
            case "hardlink":
                Require((File.GetAttributes(path) & FileAttributes.ReparsePoint) == 0 && FileIdentity(path) == FileIdentity(source.Target), "Hardlink ресурса Royale изменён.");
                break;
            case "copy":
                Require(!source.Cas && source.Length <= MaxIndex && (File.GetAttributes(path) & FileAttributes.ReparsePoint) == 0
                    && written.Sha256 is not null && HashFile(path) == written.Sha256 && HashFile(source.Target) == written.Sha256,
                    "Копия ресурса Royale повреждена.");
                break;
            default: throw new InvalidDataException("Неизвестный тип ссылки ресурса Royale.");
        }
    }
    private static void CheckLinkTarget(FileSystemInfo item, string expected)
    {
        Require(item.Exists && (item.Attributes & FileAttributes.ReparsePoint) != 0, "Не найдена ссылка ресурса Royale: " + item.FullName);
        var resolved = item.ResolveLinkTarget(true);
        Require(resolved is not null && Same(LocalPath(resolved.FullName), expected), "Ссылка ресурса Royale ведёт в другую папку.");
    }
    private static void CheckTree(string path, string root, HashSet<string> expected)
    {
        foreach (var item in Directory.EnumerateFileSystemEntries(path))
        {
            var relative = Path.GetRelativePath(root, item).Replace('\\', '/');
            if (expected.Contains(relative)) continue; // a checked directory link is not traversed
            var attributes = File.GetAttributes(item);
            Require((attributes & FileAttributes.Directory) != 0 && (attributes & FileAttributes.ReparsePoint) == 0
                && expected.Any(x => x.StartsWith(relative + "/", StringComparison.OrdinalIgnoreCase)), "Посторонний файл в кэше Royale: " + relative);
            CheckTree(item, root, expected);
        }
    }

    // Lossless DBO: preserve original header/name/primitive bytes and exact order.
    private sealed class Node(byte header, byte[] name, byte[] raw, List<Node>? children = null)
    {
        internal byte Header = header;
        internal byte[] Name = name, Raw = raw;
        internal List<Node>? Children = children;
        internal int Kind => Header & 31;
        internal Node Field(string name)
        {
            Require(Kind == 2 && Children is not null, "Ожидался объект DBO.");
            var found = Children!.Where(x => Utf8.GetString(x.Name) == name).ToArray();
            Require(found.Length == 1, "Отсутствует или повторяется поле DBO: " + name);
            return found[0];
        }
        internal List<Node> List(string name) { var node = Field(name); Require(node.Kind == 1 && node.Children is not null, "Ожидался список DBO."); return node.Children!; }
        internal IEnumerable<Node> OptionalList(string name) => Children!.Any(x => Utf8.GetString(x.Name) == name) ? List(name) : [];
        internal ulong Integer() { Require(Kind is 8 or 9, "Ожидалось целое число DBO."); return Kind == 8 ? BinaryPrimitives.ReadUInt32LittleEndian(Raw) : BinaryPrimitives.ReadUInt64LittleEndian(Raw); }
        internal void Integer(ulong value)
        {
            Require(Kind is 8 or 9 && (Kind == 9 || value <= uint.MaxValue), "Переполнение целого числа DBO.");
            if (Kind == 8) BinaryPrimitives.WriteUInt32LittleEndian(Raw, (uint)value);
            else BinaryPrimitives.WriteUInt64LittleEndian(Raw, value);
        }
        internal bool Boolean() { Require(Kind == 6 && Raw.Length == 1 && Raw[0] <= 1, "Ожидалось логическое значение DBO."); return Raw[0] != 0; }
        internal string Text()
        {
            Require(Kind == 7, "Ожидалась строка DBO.");
            var at = 0; var length = Var(Raw, ref at);
            Require(length > 0 && length == (ulong)(Raw.Length - at) && Raw[^1] == 0, "Повреждена строка DBO.");
            return Utf8.GetString(Raw, at, Raw.Length - at - 1);
        }
        internal void Text(string text) { using var s = new MemoryStream(); var b = Utf8.GetBytes(text); WriteVar(s, (ulong)b.Length + 1); s.Write(b); s.WriteByte(0); Raw = s.ToArray(); }
        internal byte[] Encode()
        {
            using var s = new MemoryStream(); s.WriteByte(Header);
            if ((Header & 128) == 0) { s.Write(Name); s.WriteByte(0); }
            if (Kind is 1 or 2)
            {
                using var body = new MemoryStream();
                foreach (var child in Children!) body.Write(child.Encode());
                body.WriteByte(0); Require(body.Length <= MaxIndex, "Слишком большой объект DBO.");
                WriteVar(s, (ulong)body.Length); body.Position = 0; body.CopyTo(s);
            }
            else s.Write(Raw);
            return s.ToArray();
        }
        internal byte[] EncodeTracked(HashSet<Node> tracked, out Dictionary<Node, (int Offset, int Size)> spans)
        {
            var lengths = new Dictionary<Node, int>();
            var result = new Dictionary<Node, (int Offset, int Size)>();
            int Measure(Node node)
            {
                var length = 1 + ((node.Header & 128) == 0 ? node.Name.Length + 1 : 0);
                if (node.Kind is 1 or 2)
                {
                    var body = 1;
                    foreach (var child in node.Children!) body = checked(body + Measure(child));
                    Require(body <= MaxIndex, "Слишком большой объект DBO.");
                    length += body;
                    do { length++; body >>= 7; } while (body != 0);
                }
                else length += node.Raw.Length;
                lengths.Add(node, length); return length;
            }
            using var stream = new MemoryStream(Measure(this));
            void Write(Node node)
            {
                var start = checked((int)stream.Position);
                stream.WriteByte(node.Header);
                if ((node.Header & 128) == 0) { stream.Write(node.Name); stream.WriteByte(0); }
                if (node.Kind is 1 or 2)
                {
                    WriteVar(stream, (ulong)(1 + node.Children!.Sum(child => lengths[child])));
                    foreach (var child in node.Children!) Write(child);
                    stream.WriteByte(0);
                }
                else stream.Write(node.Raw);
                Require(stream.Position - start == lengths[node], "Граница сериализации DBO не совпала.");
                if (tracked.Contains(node)) result.Add(node, (start, lengths[node]));
            }
            Write(this); spans = result; return stream.ToArray();
        }
        internal static Node Object(List<Node> fields) => new(130, [], [], fields);
        internal static Node String(string name, string value) { var n = new Node(7, Utf8.GetBytes(name), []); n.Text(value); return n; }
    }
    private static Node ParseWhole(byte[] data)
    {
        var at = 0; var count = 0; var node = Parse(data, ref at, 0, ref count);
        Require(at == data.Length, "Лишние данные после объекта DBO."); return node;
    }
    private static Node Parse(byte[] data, ref int at, int depth, ref int count)
    {
        Require(depth < 32 && at < data.Length && ++count <= 1_000_000, "Неверная глубина или граница DBO.");
        var header = data[at++]; byte[] name = [];
        if ((header & 128) == 0)
        {
            var end = Array.IndexOf(data, (byte)0, at);
            Require(end >= at && end - at < 4096, "Неверное имя DBO.");
            name = data.AsSpan(at, end - at).ToArray(); at = end + 1;
        }
        var kind = header & 31; var start = at;
        if (kind is 1 or 2)
        {
            var size = Var(data, ref at);
            Require(size > 0 && size <= MaxIndex && size <= (ulong)(data.Length - at), "Неверная длина объекта DBO.");
            var end = at + (int)size; var children = new List<Node>();
            while (at < end - 1) children.Add(Parse(data, ref at, depth + 1, ref count));
            Require(at == end - 1 && data[at] == 0, "Неверное завершение DBO."); at++;
            return new(header, name, [], children);
        }
        int length = kind switch { 4 => 0, 5 => 12, 6 => 1, 8 or 11 => 4, 9 or 12 or 13 => 8, 14 => 6, 15 or 18 => 16, 16 or 20 => 20, 17 => 64, _ => -1 };
        if (kind is 7 or 19) { var size = Var(data, ref at); Require(size <= MaxIndex, "Слишком длинные данные DBO."); length = (int)size; }
        else if (kind is 10 or 21) { Var(data, ref at); length = 0; }
        Require(length >= 0 && length <= data.Length - at, "Неподдерживаемый или обрезанный DBO leaf.");
        at += length; return new(header, name, data.AsSpan(start, at - start).ToArray());
    }
    private static ulong Var(byte[] data, ref int at)
    {
        ulong value = 0;
        for (var shift = 0; shift < 64; shift += 7)
        {
            Require(at < data.Length, "Обрезан varint DBO."); var b = data[at++];
            Require(shift != 63 || b <= 1, "Переполнение varint DBO.");
            value |= (ulong)(b & 127) << shift; if (b < 128) return value;
        }
        throw new InvalidDataException("Неверный varint DBO.");
    }
    private static void WriteVar(Stream stream, ulong value) { while (value >= 128) { stream.WriteByte((byte)((value & 127) | 128)); value >>= 7; } stream.WriteByte((byte)value); }
    private static byte[] Decode(byte[] data)
    {
        if (data.Length < 4 || data[0] != 0 || data[1] != 0xd1 || data[2] != 0xce || data[3] > 1) return data;
        Require(data.Length >= 556, "Обрезан XOR-заголовок BF3.");
        var result = new byte[data.Length - 556];
        for (var i = 0; i < result.Length; i++) result[i] = (byte)(data[556 + i] ^ data[296 + i % 257] ^ 0x7b);
        return result;
    }
    private static void Rename(Node node)
    {
        if (node.Kind is 1 or 2) foreach (var child in node.Children!) Rename(child);
        else if (node.Kind == 7)
        {
            var text = node.Text();
            if (text.StartsWith("Win32/", StringComparison.OrdinalIgnoreCase)) node.Text("Linux/" + text[6..]);
        }
    }
    private static void CollectSha(Node node, HashSet<string> needed)
    {
        if (node.Kind is 1 or 2) foreach (var child in node.Children!) CollectSha(child, needed);
        else if (node.Name.AsSpan().SequenceEqual("sha1"u8))
        { Require(node.Kind == 16 && node.Raw.Length == 20, "Неверный SHA1 COOP."); needed.Add(Convert.ToHexString(node.Raw)); }
    }
    private static (int Offset, int Size) Span(Node entry, int length)
    {
        var offset = entry.Field("offset").Integer(); var size = entry.Field("size").Integer();
        Require(offset <= (ulong)length && size <= (ulong)length - offset, "COOP bundle выходит за границы файла.");
        return ((int)offset, (int)size);
    }
    private static uint U32(byte[] b, int at) => BinaryPrimitives.ReadUInt32LittleEndian(b.AsSpan(at, 4));
    private static string Hash(byte[] data) => Convert.ToHexString(SHA256.HashData(data));
    private static string HashFile(string path) { using var stream = File.OpenRead(path); return Convert.ToHexString(SHA256.HashData(stream)); }
    private static void Require(bool condition, string message) { if (!condition) throw new InvalidDataException(message); }
    private static bool Same(string a, string b) => string.Equals(a, b, StringComparison.OrdinalIgnoreCase);
    private static bool Within(string a, string b) => Same(a, b) || a.StartsWith(b + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase);
    private static bool Overlaps(string a, string b) => Within(a, b) || Within(b, a);
    private static string LocalPath(string path)
    {
        Require(!string.IsNullOrWhiteSpace(path), "Пустой путь ресурсов Royale.");
        var full = Path.TrimEndingDirectorySeparator(Path.GetFullPath(path));
        Require(!full.StartsWith(@"\\", StringComparison.Ordinal) && !Same(full, Path.GetPathRoot(full) ?? ""), "Нужна локальная папка ресурсов Royale, не корень диска.");
        return full;
    }
    private static string Child(string root, string relative)
    {
        var path = Path.GetFullPath(Path.Combine(root, relative.Replace('/', Path.DirectorySeparatorChar)));
        Require(Within(path, root) && !Same(path, root), "Путь ресурса вышел за папку кэша."); return path;
    }
    private static void NoReparseAncestors(string path)
    {
        for (var current = path; !string.IsNullOrEmpty(current); current = Path.GetDirectoryName(current))
            Require(!Path.Exists(current) || (File.GetAttributes(current) & FileAttributes.ReparsePoint) == 0, "Исходный или целевой путь проходит через ссылку: " + current);
    }
    private static void RemoveStage(string path, string stage)
    {
        Require(Within(Path.GetFullPath(path), stage), "Отказ удаления за пределами временного кэша.");
        if (!Path.Exists(path)) return;
        var attributes = File.GetAttributes(path);
        if ((attributes & FileAttributes.Directory) == 0) { File.Delete(path); return; }
        if ((attributes & FileAttributes.ReparsePoint) == 0)
            foreach (var child in Directory.EnumerateFileSystemEntries(path)) RemoveStage(child, stage);
        Directory.Delete(path, false);
    }
    private static (uint Volume, uint High, uint Low) FileIdentity(string path)
    {
        using var handle = File.OpenHandle(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
        if (!GetFileInformationByHandle(handle, out var info)) throw new Win32Exception(Marshal.GetLastWin32Error());
        return (info.VolumeSerial, info.IndexHigh, info.IndexLow);
    }
    private static bool CreateFileSymlink(string path, string target) => CreateSymbolicLinkW(path, target, 2)
        || (Marshal.GetLastWin32Error() == 87 && CreateSymbolicLinkW(path, target, 0));
    private static void CreateJunction(string link, string target)
    {
        var substitute = Encoding.Unicode.GetBytes(@"\??\" + target); var print = Encoding.Unicode.GetBytes(target);
        var buffer = new byte[16 + substitute.Length + 2 + print.Length + 2];
        BitConverter.GetBytes(0xA0000003u).CopyTo(buffer, 0);
        BitConverter.GetBytes(checked((ushort)(buffer.Length - 8))).CopyTo(buffer, 4);
        BitConverter.GetBytes(checked((ushort)substitute.Length)).CopyTo(buffer, 10);
        BitConverter.GetBytes(checked((ushort)(substitute.Length + 2))).CopyTo(buffer, 12);
        BitConverter.GetBytes(checked((ushort)print.Length)).CopyTo(buffer, 14);
        substitute.CopyTo(buffer, 16); print.CopyTo(buffer, 18 + substitute.Length);
        Directory.CreateDirectory(link);
        using var handle = CreateFileW(link, 0x40000000, 0, IntPtr.Zero, 3, 0x02200000, IntPtr.Zero);
        if (handle.IsInvalid || !DeviceIoControl(handle, 0x000900A4, buffer, buffer.Length, IntPtr.Zero, 0, out _, IntPtr.Zero))
            throw new Win32Exception(Marshal.GetLastWin32Error(), "Не удалось создать junction ресурсов Royale.");
    }
    [StructLayout(LayoutKind.Sequential)]
    private struct FileInformation
    {
        public uint Attributes;
        public System.Runtime.InteropServices.ComTypes.FILETIME Creation, Access, Write;
        public uint VolumeSerial, SizeHigh, SizeLow, NumberOfLinks, IndexHigh, IndexLow;
    }
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)] private static extern bool CreateHardLinkW(string name, string existing, IntPtr security);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.I1)] private static extern bool CreateSymbolicLinkW(string name, string target, uint flags);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern SafeFileHandle CreateFileW(string name, uint access, uint share, IntPtr security, uint creation, uint flags, IntPtr template);
    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)] private static extern bool DeviceIoControl(SafeFileHandle file, uint code, byte[] input, int size, IntPtr output, int outputSize, out uint returned, IntPtr overlapped);
    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)] private static extern bool GetFileInformationByHandle(SafeFileHandle file, out FileInformation info);
}
