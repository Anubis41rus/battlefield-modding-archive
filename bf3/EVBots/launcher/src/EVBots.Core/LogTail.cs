using System.Text;

namespace EVBots.Core;

/// <summary>
/// Хвост текстового лога, который пишет другой процесс. Файл открывается на чтение
/// с разрешением на запись и удаление у других, поэтому игру и сервер не блокируем.
/// </summary>
public sealed class LogTail
{
    private const int TailBytes = 256 * 1024;
    private const int ChunkBytes = 512 * 1024;

    private long _pos;
    private string _partial = "";

    /// <summary>Путь к файлу, который сейчас читаем (null = ничего).</summary>
    public string? Path { get; private set; }

    /// <summary>Подключает файл и возвращает последние строки (не больше lastLines).</summary>
    public List<string> Attach(string? path, int lastLines)
    {
        Path = path;
        _pos = 0;
        _partial = "";
        if (path is null || !File.Exists(path))
        {
            return new List<string>();
        }

        using var fs = Open(path);
        var len = fs.Length;
        var start = Math.Max(0, len - TailBytes);
        fs.Seek(start, SeekOrigin.Begin);
        var buf = new byte[len - start];
        var read = ReadFully(fs, buf);
        _pos = len;

        var lines = Encoding.UTF8.GetString(buf, 0, read).Split('\n').Select(l => l.TrimEnd('\r')).ToList();
        if (start > 0 && lines.Count > 0)
        {
            lines.RemoveAt(0); // первая строка могла обрезаться
        }

        if (lines.Count > 0 && lines[^1].Length == 0)
        {
            lines.RemoveAt(lines.Count - 1);
        }

        return lines.Skip(Math.Max(0, lines.Count - lastLines)).ToList();
    }

    /// <summary>Строки, появившиеся с прошлого вызова. Неполная последняя строка ждёт перевода строки.</summary>
    public List<string> ReadNew()
    {
        var result = new List<string>();
        if (Path is null || !File.Exists(Path))
        {
            return result;
        }

        using var fs = Open(Path);
        if (fs.Length < _pos)
        {
            // файл пересоздан или усечён
            _pos = 0;
            _partial = "";
        }

        if (fs.Length == _pos)
        {
            return result;
        }

        fs.Seek(_pos, SeekOrigin.Begin);
        var want = (int)Math.Min(fs.Length - _pos, ChunkBytes);
        var buf = new byte[want];
        var read = ReadFully(fs, buf);
        _pos += read;

        var parts = (_partial + Encoding.UTF8.GetString(buf, 0, read)).Split('\n');
        _partial = parts[^1];
        for (var i = 0; i < parts.Length - 1; i++)
        {
            result.Add(parts[i].TrimEnd('\r'));
        }

        return result;
    }

    private static FileStream Open(string path) =>
        new(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);

    private static int ReadFully(Stream s, byte[] buf)
    {
        var total = 0;
        while (total < buf.Length)
        {
            var n = s.Read(buf, total, buf.Length - total);
            if (n <= 0)
            {
                break;
            }

            total += n;
        }

        return total;
    }
}
