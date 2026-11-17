using System.Text;

namespace EVBots.Core;

/// <summary>
/// Число ботов мода fun-bots: присваивание InitNumberOfBots в Config.lua.
/// Других механизмов (CLI, env, API) у мода нет. Файл меняется побайтово: только цифры,
/// всё остальное (BOM, CRLF, кириллица в комментариях) сохраняется как было.
/// </summary>
public static class FunBotsConfig
{
    private static readonly byte[] Key = Encoding.ASCII.GetBytes("InitNumberOfBots");

    /// <summary>Текущее значение или null, если файла или присваивания нет.</summary>
    public static int? ReadInitBots(string path)
    {
        if (!File.Exists(path))
        {
            return null;
        }

        var bytes = File.ReadAllBytes(path);
        if (!TryLocate(bytes, out var start, out var end))
        {
            return null;
        }

        return int.TryParse(Encoding.ASCII.GetString(bytes, start, end - start), out var value) ? value : null;
    }

    /// <summary>
    /// Записывает значение. Если оно уже такое — ничего не делает и возвращает "".
    /// Иначе сохраняет копию файла в backupDir и возвращает путь к копии.
    /// </summary>
    public static string Apply(string path, int value, string backupDir)
    {
        var bytes = File.ReadAllBytes(path);
        if (!TryLocate(bytes, out var start, out var end))
        {
            throw new InvalidOperationException("В Config.lua не найдено присваивание InitNumberOfBots = <число>.");
        }

        var current = Encoding.ASCII.GetString(bytes, start, end - start);
        if (current == value.ToString())
        {
            return "";
        }

        Directory.CreateDirectory(backupDir);
        var backup = Path.Combine(backupDir, $"Config.lua.{DateTime.Now:yyyyMMdd-HHmmss-fff}.bak");
        File.Copy(path, backup, overwrite: false);

        var digits = Encoding.ASCII.GetBytes(value.ToString());
        var result = new byte[bytes.Length - (end - start) + digits.Length];
        Buffer.BlockCopy(bytes, 0, result, 0, start);
        Buffer.BlockCopy(digits, 0, result, start, digits.Length);
        Buffer.BlockCopy(bytes, end, result, start + digits.Length, bytes.Length - end);
        File.WriteAllBytes(path, result);
        return backup;
    }

    /// <summary>Находит границы цифр в «InitNumberOfBots = 8».</summary>
    private static bool TryLocate(byte[] bytes, out int start, out int end)
    {
        start = -1;
        end = -1;
        var k = IndexOf(bytes, Key);
        if (k < 0)
        {
            return false;
        }

        var j = SkipBlanks(bytes, k + Key.Length);
        if (j >= bytes.Length || bytes[j] != (byte)'=')
        {
            return false;
        }

        j = SkipBlanks(bytes, j + 1);
        var s = j;
        while (j < bytes.Length && bytes[j] >= (byte)'0' && bytes[j] <= (byte)'9')
        {
            j++;
        }

        if (j == s)
        {
            return false;
        }

        start = s;
        end = j;
        return true;
    }

    private static int SkipBlanks(byte[] bytes, int index)
    {
        while (index < bytes.Length && (bytes[index] == (byte)' ' || bytes[index] == (byte)'\t'))
        {
            index++;
        }

        return index;
    }

    private static int IndexOf(byte[] haystack, byte[] needle)
    {
        for (var i = 0; i <= haystack.Length - needle.Length; i++)
        {
            var m = 0;
            while (m < needle.Length && haystack[i + m] == needle[m])
            {
                m++;
            }

            if (m == needle.Length)
            {
                return i;
            }
        }

        return -1;
    }
}
