// TdfCodec.cs — сериализация и десериализация TDF.
//
// Детали формата (проверены round-trip на настоящем payload'е getStatGroup,
// 135 690 байт, byte-for-byte совпадение):
//
//  Метка:  3 байта. Для каждого символа метки (макс. 4):
//            bits |= (0x20 | (c & 0x1F)) << (3 - i) * 6
//          затем 24-битное число записывается big-endian (старший байт первым).
//          При чтении не-буквы отбрасываются — метка короче 4 символов.
//
//  Varint: первый байт < 0x40 — значение напрямую (0..63).
//          иначе 6 младших бит + продолжение по 7 бит.
//          (Не 7 бит с первого байта, как в protobuf — здесь 6!)
//
//  Строка: varint(длина+1), затем ASCII-байты, затем 0x00.
//
//  Структура: поля подряд до 0x00 (не включая).
//
//  Список:  байт типа элемента, байт количества, затем элементы.
//  Карта:   байт типа ключа, байт типа значения, байт количества, пары.

using System.Text;

namespace EVBots.Backend.Tdf;

public static class TdfCodec
{
    // ── запись ──────────────────────────────────────────────────────────────

    public static byte[] Encode(IEnumerable<TdfNode> nodes)
    {
        var buf = new List<byte>(512);
        foreach (var n in nodes)
        {
            WriteNode(buf, n);
        }

        return buf.ToArray();
    }

    public static byte[] Encode(TdfFields fields) => Encode(fields.Nodes);

    private static void WriteNode(List<byte> buf, TdfNode node)
    {
        WriteLabel(buf, node.Label);
        buf.Add((byte)node.Type);

        switch (node)
        {
            case TdfInt i:
                WriteVarint(buf, i.Value);
                break;

            case TdfStr s:
                WriteString(buf, s.Value);
                break;

            case TdfBin b:
                WriteVarint(buf, (ulong)b.Value.Length);
                buf.AddRange(b.Value);
                break;

            case TdfStruct st:
                foreach (var item in st.Items)
                {
                    WriteNode(buf, item);
                }

                buf.Add(0x00); // конец структуры
                break;

            case TdfList l:
                buf.Add((byte)l.ItemType);
                // счётчик — varint (как TdfWriter рабочего: WriteVarInt(s, list.Items.Count)).
                // Байтовое (byte)Count ломало всё при count >= 128: клиент/сервер
                // читали varint, съедали первый байт данных и получали мусор
                // (notify 0x32 на 1432 стата: count 0x98 съедал 0x05 → 344 значения).
                WriteVarint(buf, (ulong)l.Items.Count);
                if (l.IsHnetStub)
                {
                    buf.Add(0x02);
                }

                foreach (var item in l.Items)
                {
                    WriteListItem(buf, l.ItemType, item);
                }

                break;

            case TdfMap m:
                buf.Add((byte)m.KeyType);
                buf.Add((byte)m.ValueType);
                WriteVarint(buf, (ulong)m.Pairs.Count); // varint, как в эталоне
                foreach (var (k, v) in m.Pairs)
                {
                    WriteListItem(buf, m.KeyType, k);
                    WriteListItem(buf, m.ValueType, v);
                }

                break;

            case TdfUnion u:
                // union: байт активного члена, затем его значение.
                // Unset (0x7F) означает «данных нет».
                buf.Add(u.ActiveMember);
                if (u.ActiveMember != 0x7F && u.Data is not null)
                {
                    WriteNode(buf, u.Data);
                }

                break;

            case TdfVarInts v:
                WriteVarint(buf, (ulong)v.Values.Length); // varint, как ReadVariable рабочего
                foreach (var x in v.Values)
                {
                    WriteVarint(buf, x);
                }

                break;

            case TdfObjectType ot:
                WriteVarint(buf, ot.A);
                WriteVarint(buf, ot.B);
                break;

            case TdfObjectId oi:
                WriteVarint(buf, oi.A);
                WriteVarint(buf, oi.B);
                WriteVarint(buf, oi.C);
                break;

            case TdfFloat fl:
                // IEEE 754, 4 байта, little-endian — соглашение DICE для TDF
                WriteFloat(buf, fl.Value);
                break;

            default:
                throw new NotSupportedException(
                    $"Запись TDF типа {node.GetType().Name} не реализована");
        }
    }

    private static void WriteFloat(List<byte> buf, float value)
    {
        var bits = BitConverter.SingleToUInt32Bits(value);
        buf.Add((byte)(bits & 0xFF));
        buf.Add((byte)((bits >> 8) & 0xFF));
        buf.Add((byte)((bits >> 16) & 0xFF));
        buf.Add((byte)((bits >> 24) & 0xFF));
    }

    private static float ReadFloat(byte[] d, ref int p)
    {
        // явно uint на каждом сдвиге: иначе знаковое расширение байта
        uint bits = (uint)d[p] |
                    ((uint)d[p + 1] << 8) |
                    ((uint)d[p + 2] << 16) |
                    ((uint)d[p + 3] << 24);
        p += 4;
        return BitConverter.UInt32BitsToSingle(bits);
    }

    private static void WriteBlob(List<byte> buf, byte[] data)
    {
        WriteVarint(buf, (ulong)data.Length);
        buf.AddRange(data);
    }

    private static void WriteListItem(List<byte> buf, TdfType itemType, object item)
    {
        switch (itemType)
        {
            case TdfType.Integer:
                WriteVarint(buf, Convert.ToUInt64(item));
                break;

            case TdfType.String:
                WriteString(buf, (string)item);
                break;

            case TdfType.Float:
                WriteFloat(buf, Convert.ToSingle(item));
                break;

            case TdfType.Binary:
                WriteBlob(buf, (byte[])item);
                break;

            case TdfType.Struct:
                foreach (var n in (IEnumerable<TdfNode>)item)
                {
                    WriteNode(buf, n);
                }

                buf.Add(0x00);
                break;

            case TdfType.ObjectType:
            {
                var ot = (TdfObjectType)item;
                WriteVarint(buf, ot.A);
                WriteVarint(buf, ot.B);
                break;
            }

            case TdfType.ObjectId:
            {
                var oi = (TdfObjectId)item;
                WriteVarint(buf, oi.A);
                WriteVarint(buf, oi.B);
                WriteVarint(buf, oi.C);
                break;
            }

            default:
                throw new NotSupportedException(
                    $"Элемент списка типа {itemType} не реализован");
        }
    }

    private static void WriteLabel(List<byte> buf, string label)
    {
        uint bits = 0;
        var n = Math.Min(label.Length, 4);
        for (var i = 0; i < n; i++)
        {
            bits |= (uint)(0x20 | (label[i] & 0x1F)) << ((3 - i) * 6);
        }

        // 24 бита старшим байтом вперёд
        buf.Add((byte)((bits >> 16) & 0xFF));
        buf.Add((byte)((bits >> 8) & 0xFF));
        buf.Add((byte)(bits & 0xFF));
    }

    private static void WriteVarint(List<byte> buf, ulong value)
    {
        if (value < 0x40)
        {
            buf.Add((byte)value);
            return;
        }

        buf.Add((byte)((value & 0x3F) | 0x80));

        // остаток идёт по 7 бит на байт, старший бит = «ещё есть».
        // 64-битному значению нужно до 10 байт.
        var rest = value >> 6;
        while (rest >= 0x80)
        {
            buf.Add((byte)((rest & 0x7F) | 0x80));
            rest >>= 7;
        }

        buf.Add((byte)rest);
    }

    private static void WriteString(List<byte> buf, string value)
    {
        // TDF-строки — байтовые. Кириллицу транслитерируем, иначе клиент
        // покажет «кракозябры»: в TDF нет UTF-8, только ASCII/Latin-1.
        var bytes = AsciiSafe.GetBytes(value);
        WriteVarint(buf, (ulong)bytes.Length + 1);
        buf.AddRange(bytes);
        buf.Add(0x00);
    }

    // ── чтение ──────────────────────────────────────────────────────────────

    public static TdfFields Decode(byte[] data)
    {
        var fields = new TdfFields();
        var p = 0;
        while (p < data.Length)
        {
            var node = ReadNode(data, ref p);
            if (node is not null)
            {
                fields.Add(node);
            }
        }

        return fields;
    }

    private static TdfNode? ReadNode(byte[] d, ref int p)
    {
        if (p + 4 > d.Length)
        {
            // Хвост короче заголовка узла (метка 3 байта + тип 1). Раньше p не
            // сдвигался, и цикл Decode крутился вечно (13.10: GameReporting 0x1C/0x64,
            // 723 байта, поток чтения сессии сервера зависал — сервер уходил в OFFLINE).
            p = d.Length;
            return null;
        }

        var label = ReadLabel(d, ref p);
        var type = (TdfType)d[p];
        p++;

        switch (type)
        {
            case TdfType.Integer:
                return TdfNode.Int(label, ReadVarint(d, ref p));

            case TdfType.String:
                return TdfNode.Str(label, ReadString(d, ref p));

            case TdfType.Binary:
            {
                var len = (int)ReadVarint(d, ref p);
                var bytes = new byte[len];
                Array.Copy(d, p, bytes, 0, len);
                p += len;
                return TdfNode.Bin(label, bytes);
            }

            case TdfType.Struct:
                return new TdfStruct(label, ReadStructItems(d, ref p));

            case TdfType.List:
                return ReadList(d, ref p, label);

            case TdfType.Map:
                return ReadMap(d, ref p, label);

            case TdfType.Union:
            {
                var active = d[p];
                p++;
                if (active == 0x7F)
                {
                    return new TdfUnion(label, active, null);
                }

                var inner = ReadNode(d, ref p);
                return new TdfUnion(label, active, inner);
            }

            case TdfType.VariableIntegerList:
            {
                var cnt = (int)ReadVarint(d, ref p);
                var vals = new ulong[cnt];
                for (var i = 0; i < cnt; i++)
                {
                    vals[i] = ReadVarint(d, ref p);
                }

                return TdfNode.VarInts(label, vals);
            }

            case TdfType.ObjectType:
            {
                var a = ReadVarint(d, ref p);
                var b = ReadVarint(d, ref p);
                return TdfNode.ObjectType(label, a, b);
            }

            case TdfType.ObjectId:
            {
                var a = ReadVarint(d, ref p);
                var b = ReadVarint(d, ref p);
                var c = ReadVarint(d, ref p);
                return TdfNode.ObjectId(label, a, b, c);
            }

            case TdfType.Float:
                return TdfNode.Float(label, ReadFloat(d, ref p));

            default:
                throw new InvalidDataException(
                    $"Неизвестный тип TDF 0x{(int)type:X2} на смещении {p - 1} " +
                    $"(метка '{label}')");
        }
    }

    private static string ReadLabel(byte[] d, ref int p)
    {
        var bits = ((uint)d[p] << 16) | ((uint)d[p + 1] << 8) | d[p + 2];
        p += 3;

        var sb = new StringBuilder(4);
        for (var shift = 18; shift >= 0; shift -= 6)
        {
            var c = (char)((((bits >> shift) & 0x3F) & 0x1F) | 0x40);
            if (c >= 'A' && c <= 'Z')
            {
                sb.Append(c);
            }
        }

        return sb.ToString();
    }

    private static ulong ReadVarint(byte[] d, ref int p)
    {
        var b = d[p++];
        if (b < 0x80)
        {
            return b;
        }

        var value = (ulong)(b & 0x3F);
        var shift = 6;

        // 64-битному значению нужно до 9 продолжений (сдвиги 6..62)
        while (p < d.Length)
        {
            var next = d[p++];
            if (shift < 64)
            {
                value |= (ulong)(next & 0x7F) << shift;
            }

            if (next < 0x80)
            {
                break;
            }

            shift += 7;
        }

        return value;
    }

    private static string ReadString(byte[] d, ref int p)
    {
        var len = (int)ReadVarint(d, ref p) - 1;
        if (len <= 0)
        {
            p++; // конечный байт
            return "";
        }

        var s = AsciiSafe.GetString(d, p, len);
        p += len;
        p++; // конечный байт
        return s;
    }

    private static List<TdfNode> ReadStructItems(byte[] d, ref int p)
    {
        // пустая структура: сразу 0x00
        if (p < d.Length && d[p] == 0x00)
        {
            p++;
            return new List<TdfNode>();
        }

        // «странная» пустая структура из двух нулей — GameReporting
        if (p + 1 < d.Length && d[p] == 0x00 && d[p + 1] == 0x00)
        {
            p += 2;
            return new List<TdfNode>();
        }

        var items = new List<TdfNode>();
        while (p < d.Length && d[p] != 0x00)
        {
            var node = ReadNode(d, ref p);
            if (node is not null)
            {
                items.Add(node);
            }
        }

        if (p < d.Length)
        {
            p++; // конец структуры
        }

        return items;
    }

    private static TdfList ReadList(byte[] d, ref int p, string label)
    {
        var itemType = (TdfType)d[p];
        p++;
        // счётчик — varint (TdfReader рабочего: ReadCount → ReadVarInt)
        var count = (int)ReadVarint(d, ref p);

        var items = new List<object>(count);

        // HNET-список структур: префикс 0x02, затем ДВА full-узла и завершающий байт.
        // Срабатывает на 0x02 независимо от count (в live CreateGame count=0x01, в
        // GameSetupNotification — 0x02). Ровно как в референсе TdfDecoder.cs.
        if (itemType == TdfType.Struct && p < d.Length && d[p] == 0x02)
        {
            p++;
            for (var i = 0; i < 2; i++)
            {
                var node = ReadNode(d, ref p);
                items.Add(node is TdfStruct st ? st.Items : new List<TdfNode>());
            }

            if (p < d.Length)
            {
                p++;
            }

            return new TdfList(label, itemType, items, hnetStub: true);
        }

        for (var i = 0; i < count; i++)
        {
            items.Add(ReadListItem(d, ref p, itemType));
        }

        return new TdfList(label, itemType, items);
    }

    private static object ReadListItem(byte[] d, ref int p, TdfType itemType) =>
        itemType switch
        {
            TdfType.Integer => ReadVarint(d, ref p),
            TdfType.String => ReadString(d, ref p),
            TdfType.Struct => ReadStructItems(d, ref p),
            TdfType.Float => ReadFloat(d, ref p),
            TdfType.ObjectType => TdfNode.ObjectType("", ReadVarint(d, ref p),
                ReadVarint(d, ref p)),
            TdfType.ObjectId => TdfNode.ObjectId("", ReadVarint(d, ref p),
                ReadVarint(d, ref p), ReadVarint(d, ref p)),
            _ => throw new InvalidDataException($"Элемент списка {itemType} не реализован")
        };

    private static TdfMap ReadMap(byte[] d, ref int p, string label)
    {
        var keyType = (TdfType)d[p];
        var valueType = (TdfType)d[p + 1];
        p += 2;
        var count = (int)ReadVarint(d, ref p); // varint, как в эталоне

        var pairs = new List<(object Key, object Value)>(count);
        for (var i = 0; i < count; i++)
        {
            var key = ReadListItem(d, ref p, keyType);
            var value = ReadListItem(d, ref p, valueType);
            pairs.Add((key, value));
        }

        return new TdfMap(label, keyType, valueType, pairs);
    }
}