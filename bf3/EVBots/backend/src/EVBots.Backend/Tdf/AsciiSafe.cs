// AsciiSafe.cs — байтовые строки для TDF.
//
// В TDF нет Unicode: строка это «длина + байты + 0x00», и клиент BF3
// ожидает ASCII. Если записать UTF-8 кириллицы, в игре будет «кракозябры»,
// а длина в байтах разъедется с символами.
//
// Поэтому: кириллицу транслитерируем в латиницу, прочее непечатаемое
// заменяем на «?». В HTTP-морде имя остаётся полным — там UTF-8 корректен.

using System.Text;

namespace EVBots.Backend.Tdf;

internal static class AsciiSafe
{
    private const string Cyrillic =
        "абвгдеёжзийклмнопрстуфхцчшщъыьэюя";

    private const string Translit =
        "abvgdeezhziiklmnoprstufxchtshshchshyyehyu";

    /// <summary>Строка -> байты, пригодные для TDF.</summary>
    public static byte[] GetBytes(string value)
    {
        var sb = new StringBuilder(value.Length + 4);

        foreach (var ch in value)
        {
            if (ch < 0x80)
            {
                sb.Append(ch);
                continue;
            }

            var idx = Cyrillic.IndexOf(char.ToLowerInvariant(ch));
            if (idx >= 0)
            {
                // регистр сохраняем: заглавная кириллица -> заглавная латиница
                sb.Append(char.IsUpper(ch)
                    ? char.ToUpperInvariant(Translit[idx])
                    : Translit[idx]);
                continue;
            }

            sb.Append('?');
        }

        var ascii = sb.ToString();

        // выкидываем управляющие символы — они ломают парсер клиента
        ascii = new string(ascii.Where(c => c >= 0x20 && c != 0x7F).ToArray());

        return Encoding.ASCII.GetBytes(ascii);
    }

    /// <summary>Байты -> строка (для логов и отладки).</summary>
    public static string GetString(byte[] data, int offset, int count) =>
        Encoding.ASCII.GetString(data, offset, count);

    /// <summary>Транслитерация строки целиком — для показа в логах.</summary>
    public static string Transliterate(string value) =>
        Encoding.ASCII.GetString(GetBytes(value));
}