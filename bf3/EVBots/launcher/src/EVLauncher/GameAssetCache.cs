using System.Buffers.Binary;
using System.Globalization;
using System.IO;
using System.IO.Compression;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Windows.Media;
using System.Windows.Media.Imaging;

namespace EVBots.Launcher;

// Только чтение установленной BF3; игровые текстуры не входят в дистрибутив.
// Форматы DBO/CAS/DxTexture сверены с Frostbite-Scripts (MIT), commit
// a07ee6d38ecc62598dfc9fdf990be067d9f21929. См. GameAssetCache.NOTICE.txt.
internal static class GameAssetCache
{
    private const int Schema = 1;
    private const int MaxPayload = 64 * 1024 * 1024;
    private static readonly SemaphoreSlim Gate = new(1, 1);
    private static readonly UTF8Encoding Utf8 = new(false, true);
    private sealed record Input(string Path, long Length, long Modified);
    private sealed record Image(string Name, string Sha256);
    private sealed record Manifest(int Schema, string Game, List<Input> Inputs, List<Image> Images);
    private sealed record CasEntry(string Path, uint Offset, uint Size);
    private sealed record Node(string Name, object? Value)
    {
        public object? Get(string name) => Value is Dictionary<string, object?> fields && fields.TryGetValue(name, out var v) ? v : null;
        public IEnumerable<Node> Items(string name) => Get(name) as List<Node> ?? [];
    }

    internal static async Task<string> EnsureAsync(string gameDir, string cacheRoot, Action<string> log)
    {
        await Gate.WaitAsync();
        try { return await Task.Run(() => Ensure(gameDir, cacheRoot, log)); }
        finally { Gate.Release(); }
    }

    private static string Ensure(string gameDir, string cacheRoot, Action<string> log)
    {
        gameDir = Path.GetFullPath(gameDir);
        cacheRoot = Path.GetFullPath(cacheRoot);
        Directory.CreateDirectory(cacheRoot);
        // Между экземплярами лаунчера незаконченный кэш также не считается готовым.
        using var cacheLock = new FileStream(Path.Combine(cacheRoot, ".extract.lock"), FileMode.OpenOrCreate,
            FileAccess.ReadWrite, FileShare.None);
        var manifestPath = Path.Combine(cacheRoot, "manifest.json");
        if (CacheValid(manifestPath, gameDir, cacheRoot)) return cacheRoot;
        log("Подготавливаю иконки Royale из установленной BF3…");
        var inputs = new Dictionary<string, Input>(StringComparer.OrdinalIgnoreCase);
        var catalogue = new Dictionary<string, CasEntry>(StringComparer.Ordinal);
        var resources = new Dictionary<string, Node>(StringComparer.Ordinal);
        var chunks = new Dictionary<string, Node>(StringComparer.Ordinal);
        void Track(string path)
        {
            var info = new FileInfo(path);
            inputs[path] = new(path, info.Length, info.LastWriteTimeUtc.Ticks);
        }
        foreach (var root in new[] { Path.Combine(gameDir, "Data"), Path.Combine(gameDir, "Update", "Patch", "Data") })
        {
            var cat = Path.Combine(root, "cas.cat");
            Track(cat);
            using (var reader = Reader(DecodeFile(cat)))
            {
                reader.BaseStream.Position = 16;
                while (reader.BaseStream.Position < reader.BaseStream.Length)
                {
                    var sha = Hex(Bytes(reader, 20));
                    var offset = reader.ReadUInt32();
                    var size = reader.ReadUInt32();
                    var number = reader.ReadUInt32();
                    catalogue[sha] = new(Path.Combine(root, $"cas_{number:00}.cas"), offset, size);
                }
            }
            var tocPath = Path.Combine(root, "Win32", "UI.toc");
            var sbPath = Path.ChangeExtension(tocPath, ".sb");
            Track(tocPath); Track(sbPath);
            using var tocReader = Reader(DecodeFile(tocPath));
            var toc = ReadNode(tocReader);
            if (toc.Get("cas") is not true) throw new InvalidDataException("BF3 UI: требуется PC CAS.");
            using (var sb = new BinaryReader(File.OpenRead(sbPath), Utf8))
            {
                foreach (var entry in toc.Items("bundles"))
                {
                    var offset = Convert.ToInt64(entry.Get("offset"), CultureInfo.InvariantCulture);
                    if (offset < 0 || offset >= sb.BaseStream.Length) throw new InvalidDataException("BF3 UI: неверный offset.");
                    sb.BaseStream.Position = offset;
                    var bundle = ReadNode(sb);
                    foreach (var item in bundle.Items("res"))
                    {
                        var name = (item.Get("name") as string ?? "").ToLowerInvariant();
                        if (Wanted(name)) resources[name] = item;
                    }
                    foreach (var item in bundle.Items("chunks")) chunks[Hex(RequireBytes(item.Get("id")))] = item;
                }
            }
            foreach (var path in Directory.EnumerateFiles(Path.Combine(root, "Win32"), "*Chunks*.toc"))
            {
                Track(path);
                using var reader = Reader(DecodeFile(path));
                foreach (var item in ReadNode(reader).Items("chunks")) chunks[Hex(RequireBytes(item.Get("id")))] = item;
            }
        }
        byte[] Payload(Node node, bool compressed)
        {
            if (!catalogue.TryGetValue(Hex(RequireBytes(node.Get("sha1"))), out var entry))
                throw new InvalidDataException("BF3 UI: текстура отсутствует в CAS-каталоге.");
            if (entry.Size > MaxPayload) throw new InvalidDataException("BF3 UI: слишком большая текстура.");
            Track(entry.Path);
            using var reader = new BinaryReader(File.OpenRead(entry.Path));
            if ((ulong)entry.Offset + entry.Size > (ulong)reader.BaseStream.Length) throw new InvalidDataException("BF3 UI: обрезанный CAS.");
            reader.BaseStream.Position = entry.Offset;
            var data = Bytes(reader, checked((int)entry.Size));
            return compressed ? Inflate(data) : data;
        }
        if (resources.Count == 0) throw new InvalidDataException("В установленной BF3 не найдены иконки Royale.");
        var images = new List<Image>();
        foreach (var (name, resource) in resources.OrderBy(x => x.Key, StringComparer.Ordinal))
        {
            var header = Payload(resource, true);
            if (header.Length < 128) throw new InvalidDataException($"BF3 UI: обрезанный заголовок {name}.");
            var version = U32(header, 0); var type = U32(header, 4); var format = U32(header, 8);
            var width = U16(header, 16); var height = U16(header, 18);
            // Все 481 проверенные иконки BF3 имеют этот формат. Другие форматы не подменяем.
            if (version != 10 || type != 0 || format != 2 || header[27] != 0 || width == 0 || height == 0 || width > 4096 || height > 4096)
                throw new InvalidDataException($"BF3 UI: неподдерживаемая текстура {name} ({version}/{type}/{format}).");
            var guid = header.AsSpan(28, 16).ToArray();
            if (!chunks.TryGetValue(Hex(guid), out var chunk)) throw new InvalidDataException($"BF3 UI: не найден chunk {name}.");
            var pixels = DecodeBc3(Payload(chunk, (guid[15] & 1) != 0), width, height);
            var image = BitmapSource.Create(width, height, 96, 96, PixelFormats.Bgra32, null, pixels, width * 4);
            image.Freeze();
            var encoder = new PngBitmapEncoder();
            encoder.Frames.Add(BitmapFrame.Create(image));
            var target = ImagePath(cacheRoot, name);
            Directory.CreateDirectory(Path.GetDirectoryName(target)!);
            using (var output = File.Create(target + ".tmp")) encoder.Save(output);
            File.Move(target + ".tmp", target, true);
            images.Add(new(name, FileHash(target)));
        }
        var manifest = new Manifest(Schema, gameDir, inputs.Values.ToList(), images);
        File.WriteAllText(manifestPath + ".tmp", JsonSerializer.Serialize(manifest), Utf8);
        File.Move(manifestPath + ".tmp", manifestPath, true);
        log($"Иконки Royale готовы: {images.Count}.");
        return cacheRoot;
    }

    private static bool CacheValid(string manifestPath, string game, string root)
    {
        try
        {
            if (!File.Exists(manifestPath)) return false;
            var m = JsonSerializer.Deserialize<Manifest>(File.ReadAllText(manifestPath, Utf8));
            if (m is null || m.Schema != Schema || !string.Equals(m.Game, game, StringComparison.OrdinalIgnoreCase) ||
                m.Inputs is not { Count: > 0 } || m.Images is not { Count: > 0 }) return false;
            foreach (var i in m.Inputs)
            {
                var f = new FileInfo(i.Path);
                if (!f.Exists || f.Length != i.Length || f.LastWriteTimeUtc.Ticks != i.Modified) return false;
            }
            return m.Images.All(i => File.Exists(ImagePath(root, i.Name)) && FileHash(ImagePath(root, i.Name)) == i.Sha256);
        }
        catch (Exception e) when (e is IOException or JsonException or ArgumentException or InvalidDataException or UnauthorizedAccessException)
        { return false; }
    }

    private static bool Wanted(string name) => name.StartsWith("ui/art/persistence/weapons/", StringComparison.Ordinal) ||
        name.StartsWith("ui/art/persistence/weaponaccessory/", StringComparison.Ordinal) ||
        name.StartsWith("ui/art/persistence/kititem/", StringComparison.Ordinal) ||
        name.StartsWith("ui/art/persistence/specializations/", StringComparison.Ordinal) ||
        name is "ui/art/persistence/ranks/rank001" or "ui/art/persistence/ranks/rank002" or "ui/art/persistence/ranks/rank003";
    private static string ImagePath(string root, string name)
    {
        if (!Wanted(name) || name.Any(c => !(c is >= 'a' and <= 'z' or >= '0' and <= '9' or '/' or '_' or '-' or ' ')) ||
            name.Split('/').Any(x => x.Length == 0 || x[0] == ' ' || x[^1] == ' '))
            throw new InvalidDataException($"BF3 UI: недопустимое имя иконки: {name}.");
        return Path.Combine(root, name.Replace('/', Path.DirectorySeparatorChar) + ".png");
    }
    private static string FileHash(string path) { using var s = File.OpenRead(path); return Hex(SHA256.HashData(s)); }
    private static string Hex(byte[] b) => Convert.ToHexString(b);
    private static byte[] RequireBytes(object? value) => value as byte[] ?? throw new InvalidDataException("BF3 UI: ожидался идентификатор.");
    private static ushort U16(byte[] b, int at) => BinaryPrimitives.ReadUInt16LittleEndian(b.AsSpan(at, 2));
    private static uint U32(byte[] b, int at) => BinaryPrimitives.ReadUInt32LittleEndian(b.AsSpan(at, 4));
    private static BinaryReader Reader(byte[] b) => new(new MemoryStream(b, false), Utf8);
    private static byte[] Bytes(BinaryReader r, int count)
    {
        if (count < 0 || count > MaxPayload) throw new InvalidDataException("BF3 UI: неверная длина.");
        var value = r.ReadBytes(count);
        if (value.Length != count) throw new EndOfStreamException("BF3 UI: обрезанные данные.");
        return value;
    }
    private static byte[] DecodeFile(string path)
    {
        var size = new FileInfo(path).Length;
        if (size > MaxPayload) throw new InvalidDataException("BF3 UI: слишком большой индекс.");
        var b = File.ReadAllBytes(path);
        if (b.Length >= 4 && b[0] == 0 && b[1] == 0xd1 && b[2] == 0xce && b[3] <= 1)
        {
            if (b.Length < 556) throw new InvalidDataException("BF3 UI: обрезанный XOR-заголовок.");
            var decoded = new byte[b.Length - 556];
            for (var i = 0; i < decoded.Length; i++) decoded[i] = (byte)(b[556 + i] ^ b[296 + i % 257] ^ 0x7b);
            return decoded;
        }
        return b;
    }
    private static ulong VarUInt(BinaryReader r)
    {
        ulong value = 0;
        for (var shift = 0; shift < 64; shift += 7)
        {
            var b = r.ReadByte();
            if (shift == 63 && b > 1) throw new InvalidDataException("BF3 UI: переполнение varint.");
            value |= (ulong)(b & 127) << shift;
            if (b < 128) return value;
        }
        throw new InvalidDataException("BF3 UI: неверный varint.");
    }
    private static string Name(BinaryReader r)
    {
        using var b = new MemoryStream();
        while (true)
        {
            var next = r.ReadByte();
            if (next == 0) return Utf8.GetString(b.ToArray());
            if (b.Length >= 4096) throw new InvalidDataException("BF3 UI: слишком длинное имя.");
            b.WriteByte(next);
        }
    }
    private static Node ReadNode(BinaryReader r, int depth = 0)
    {
        if (depth > 32) throw new InvalidDataException("BF3 UI: слишком глубокий объект.");
        var header = r.ReadByte();
        var type = header & 31;
        var name = (header & 128) != 0 ? "" : Name(r);
        if (type is 1 or 2)
        {
            var size = checked((long)VarUInt(r));
            var end = checked(r.BaseStream.Position + size);
            if (size < 1 || size > MaxPayload || end > r.BaseStream.Length) throw new InvalidDataException("BF3 UI: неверная длина объекта.");
            var list = new List<Node>();
            var fields = new Dictionary<string, object?>(StringComparer.Ordinal);
            while (r.BaseStream.Position < end - 1)
            {
                var child = ReadNode(r, depth + 1);
                if (type == 1) list.Add(child); else fields.Add(child.Name, child.Value);
            }
            if (r.BaseStream.Position != end - 1 || r.ReadByte() != 0) throw new InvalidDataException("BF3 UI: неверное завершение объекта.");
            return new(name, type == 1 ? list : fields);
        }
        object? value;
        switch (type)
        {
            case 4: value = null; break;
            case 5: value = Bytes(r, 12); break;
            case 6: value = r.ReadBoolean(); break;
            case 7:
                var size = checked((int)VarUInt(r));
                value = Utf8.GetString(Bytes(r, size - 1));
                if (r.ReadByte() != 0) throw new InvalidDataException("BF3 UI: неверная строка.");
                break;
            case 8: value = r.ReadUInt32(); break;
            case 9: value = r.ReadUInt64(); break;
            case 10: case 21: value = VarUInt(r); break;
            case 11: value = r.ReadSingle(); break;
            case 12: value = r.ReadDouble(); break;
            case 13: value = Bytes(r, 8); break;
            case 14: value = Bytes(r, 6); break;
            case 15: case 18: value = Bytes(r, 16); break;
            case 16: case 20: value = Bytes(r, 20); break;
            case 17: value = Bytes(r, 64); break;
            case 19: value = Bytes(r, checked((int)VarUInt(r))); break;
            default: throw new InvalidDataException($"BF3 UI: неподдерживаемый DBO type {type}.");
        }
        return new(name, value);
    }

    private static byte[] Inflate(byte[] data)
    {
        using var output = new MemoryStream();
        var at = 0;
        while (at < data.Length)
        {
            if (data.Length - at < 8) throw new InvalidDataException("BF3 UI: обрезанный блок CAS.");
            var plain = checked((int)BinaryPrimitives.ReadUInt32BigEndian(data.AsSpan(at, 4)));
            var packed = checked((int)BinaryPrimitives.ReadUInt32BigEndian(data.AsSpan(at + 4, 4)));
            at += 8;
            if (plain > 16 * 1024 * 1024 || packed > data.Length - at || output.Length + plain > MaxPayload)
                throw new InvalidDataException("BF3 UI: неверная длина блока CAS.");
            byte[] decoded;
            try
            {
                using var z = new ZLibStream(new MemoryStream(data, at, packed, false), CompressionMode.Decompress);
                decoded = new byte[plain];
                z.ReadExactly(decoded);
                if (z.ReadByte() != -1) throw new InvalidDataException("BF3 UI: неверный размер распакованного блока.");
            }
            catch (InvalidDataException) when (plain == packed) { decoded = data.AsSpan(at, packed).ToArray(); }
            output.Write(decoded);
            at += packed;
        }
        return output.ToArray();
    }

    // DXT5 / BC3: первая mip-ступень, прямой BGRA без premultiply.
    private static byte[] DecodeBc3(byte[] data, int width, int height)
    {
        var blocksX = (width + 3) / 4; var blocksY = (height + 3) / 4;
        if (data.Length < blocksX * blocksY * 16) throw new InvalidDataException("BF3 UI: обрезанные DXT5-пиксели.");
        var pixels = new byte[width * height * 4];
        var alphas = new byte[8]; var colors = new byte[4, 3];
        for (var by = 0; by < blocksY; by++) for (var bx = 0; bx < blocksX; bx++)
        {
            var at = (by * blocksX + bx) * 16;
            alphas[0] = data[at]; alphas[1] = data[at + 1];
            if (alphas[0] > alphas[1])
                for (var i = 1; i <= 6; i++) alphas[i + 1] = (byte)(((7 - i) * alphas[0] + i * alphas[1]) / 7);
            else
            {
                for (var i = 1; i <= 4; i++) alphas[i + 1] = (byte)(((5 - i) * alphas[0] + i * alphas[1]) / 5);
                alphas[6] = 0; alphas[7] = 255;
            }
            ulong alphaBits = 0;
            for (var i = 0; i < 6; i++) alphaBits |= (ulong)data[at + 2 + i] << (i * 8);
            for (var i = 0; i < 2; i++)
            {
                var c = U16(data, at + 8 + i * 2);
                var b = c & 31; var g = (c >> 5) & 63; var r = (c >> 11) & 31;
                colors[i, 0] = (byte)((b << 3) | (b >> 2));
                colors[i, 1] = (byte)((g << 2) | (g >> 4));
                colors[i, 2] = (byte)((r << 3) | (r >> 2));
            }
            for (var channel = 0; channel < 3; channel++)
            {
                colors[2, channel] = (byte)((2 * colors[0, channel] + colors[1, channel]) / 3);
                colors[3, channel] = (byte)((colors[0, channel] + 2 * colors[1, channel]) / 3);
            }
            var colorBits = U32(data, at + 12);
            for (var y = 0; y < 4; y++) for (var x = 0; x < 4; x++)
            {
                var n = y * 4 + x; var px = bx * 4 + x; var py = by * 4 + y;
                if (px >= width || py >= height) continue;
                var dest = (py * width + px) * 4; var color = (int)((colorBits >> (n * 2)) & 3);
                for (var channel = 0; channel < 3; channel++) pixels[dest + channel] = colors[color, channel];
                pixels[dest + 3] = alphas[(alphaBits >> (n * 3)) & 7];
            }
        }
        return pixels;
    }
}
