// Tdf.cs — модель данных формата TDF (Blaze).
//
// Формат: каждое поле = 3 байта тег(метка) + 1 байт тип + значение.
// Метка кодируется по 6 бит на символ, символ = (буква & 0x1F) | 0x40.
//   bits |= (0x20 | (c & 0x1F)) << (3 - i) * 6
//   затем 24-битное число записывается старшим байтом первым.
// При чтении символы вне A-Z отбрасываются — метка короче 4 символов.
//
// Числа — переменной длины: если первый байт < 0x40, значение напрямую (0..63);
// иначе 6 младших бит и продолжение по 7 бит. (Не protobuf!)
// Строки — varint(длина+1), ASCII-байты, 0x00.
// Структура — поля подряд до 0x00 (не включая).
//
// Формат восстановлен из New-Blaze-Emulator (Blaze/TdfEncoder.cs,
// TdfDecoder.cs) и подтверждён round-trip тестом на настоящем payload'е
// getStatGroup из BF3NativeBots\progress\bf3-stats-catalog.json.

namespace EVBots.Backend.Tdf;

public enum TdfType : byte
{
    Integer = 0,
    String = 1,
    Binary = 2,
    Struct = 3,
    List = 4,
    Map = 5,
    Union = 6,
    VariableIntegerList = 7,
    ObjectType = 8,
    ObjectId = 9,
    Float = 0x0A,
    TimeValue = 0x0B,
}

public sealed class TdfFloat(string label, float value) : TdfNode
{
    public float Value { get; } = value;
    public override TdfType Type => TdfType.Float;
    public override string Label { get; init; } = label;
    public override string ToString() => $"{Label}={Value}";
}

/// <summary>Одно поле TDF. Порядок аргументов везде: (метка, значение).</summary>
public abstract class TdfNode
{
    public virtual string Label { get; init; } = "";
    public abstract TdfType Type { get; }

    // ── фабрики ────────────────────────────────────────────────────────────

    public static TdfInt Int(string label, ulong value) => new(label, value);

    public static TdfInt Int(string label, long value) => new(label, (ulong)value);

    public static TdfInt Int(string label, int value) => new(label, (ulong)value);

    public static TdfStr Str(string label, string value) => new(label, value);

    public static TdfBin Bin(string label, byte[] value) => new(label, value);

    public static TdfFloat Float(string label, float value) => new(label, value);

    public static TdfFloat Float(string label, double value) =>
        new(label, (float)value);

    public static TdfStruct Struct(string label, params TdfNode[] items) =>
        new(label, items);

    public static TdfList ListOf(string label, TdfType itemType,
        params object[] items) => new(label, itemType, items);

    public static TdfMap MapOf(string label, TdfType keyType, TdfType valueType,
        List<(object Key, object Value)> pairs) => new(label, keyType, valueType, pairs);

    public static TdfMap Map(string label, TdfType keyType, TdfType valueType,
        List<(object Key, object Value)> pairs) => new(label, keyType, valueType, pairs);

    public static TdfVarInts VarInts(string label, params ulong[] values) =>
        new(label, values);

    public static TdfObjectType ObjectType(string label, ulong a, ulong b) =>
        new(label, a, b);

    public static TdfObjectId ObjectId(string label, ulong a, ulong b, ulong c) =>
        new(label, a, b, c);

    /// <summary>Значение как число — для удобства в обработчиках.</summary>
    public virtual ulong AsUInt() =>
        throw new InvalidOperationException($"{Label}: {Type} не является числом");

    /// <summary>Значение как строка — для обработчиков.</summary>
    public virtual string AsString() =>
        throw new InvalidOperationException($"{Label}: {Type} не является строкой");
}

public sealed class TdfInt(string label, ulong value) : TdfNode
{
    public ulong Value { get; } = value;
    public override TdfType Type => TdfType.Integer;
    public override string Label { get; init; } = label;
    public override ulong AsUInt() => Value;
    public override string ToString() => $"{Label}={Value}";
}

public sealed class TdfStr(string label, string value) : TdfNode
{
    public string Value { get; } = value;
    public override TdfType Type => TdfType.String;
    public override string Label { get; init; } = label;
    public override string AsString() => Value;
    public override string ToString() => $"{Label}=\"{Value}\"";
}

public sealed class TdfBin(string label, byte[] value) : TdfNode
{
    public byte[] Value { get; } = value;
    public override TdfType Type => TdfType.Binary;
    public override string Label { get; init; } = label;
    public override string ToString() => $"{Label}=<{Value.Length} байт>";
}

public sealed class TdfStruct : TdfNode
{
    public IReadOnlyList<TdfNode> Items { get; }
    public override TdfType Type => TdfType.Struct;
    public override string Label { get; init; }

    public TdfStruct(string label, IReadOnlyList<TdfNode> items)
    {
        Label = label;
        Items = items;
    }

    public TdfStruct(string label, params TdfNode[] items)
    {
        Label = label;
        Items = items;
    }

    /// <summary>Поле структуры по метке — как у TdfFields.</summary>
    public TdfNode? this[string label]
    {
        get
        {
            foreach (var n in Items)
            {
                if (string.Equals(n.Label, label, StringComparison.Ordinal))
                {
                    return n;
                }
            }

            return null;
        }
    }

    /// <summary>Список полей как TdfFields (удобно для чтения).</summary>
    public TdfFields AsFields()
    {
        var f = new TdfFields();
        foreach (var n in Items)
        {
            f.Add(n);
        }

        return f;
    }

    public override string ToString() => $"{Label}{{ {Items.Count} полей }}";
}

public sealed class TdfList : TdfNode
{
    public TdfType ItemType { get; }
    public IReadOnlyList<object> Items { get; }
    public override TdfType Type => TdfType.List;
    public override string Label { get; init; }

    /// <summary>
    /// HNET-особый случай: список из двух структур пишется как
    /// <c>02 &lt;struct&gt; &lt;struct&gt; 00</c> вместо обычного списка.
    /// </summary>
    public bool IsHnetStub { get; init; }

    public TdfList(string label, TdfType itemType, IReadOnlyList<object> items,
        bool hnetStub = false)
    {
        Label = label;
        ItemType = itemType;
        Items = items;
        IsHnetStub = hnetStub;
    }

    public override string ToString() => $"{Label}[{ItemType}x{Items.Count}]";
}

public sealed class TdfMap(string label, TdfType keyType, TdfType valueType,
    List<(object Key, object Value)> pairs) : TdfNode
{
    public TdfType KeyType { get; } = keyType;
    public TdfType ValueType { get; } = valueType;
    public List<(object Key, object Value)> Pairs { get; } = pairs;
    public override TdfType Type => TdfType.Map;
    public override string Label { get; init; } = label;
}

public sealed class TdfUnion(string label, byte activeMember, TdfNode? data)
    : TdfNode
{
    public byte ActiveMember { get; } = activeMember;
    public TdfNode? Data { get; } = data;
    public override TdfType Type => TdfType.Union;
    public override string Label { get; init; } = label;
}

public sealed class TdfVarInts(string label, ulong[] values) : TdfNode
{
    public ulong[] Values { get; } = values;
    public override TdfType Type => TdfType.VariableIntegerList;
    public override string Label { get; init; } = label;
}

public sealed class TdfObjectType(string label, ulong a, ulong b) : TdfNode
{
    public ulong A { get; } = a;
    public ulong B { get; } = b;
    public override TdfType Type => TdfType.ObjectType;
    public override string Label { get; init; } = label;
}

public sealed class TdfObjectId(string label, ulong a, ulong b, ulong c) : TdfNode
{
    public ulong A { get; } = a;
    public ulong B { get; } = b;
    public ulong C { get; } = c;
    public override TdfType Type => TdfType.ObjectId;
    public override string Label { get; init; } = label;
}

/// <summary>Набор полей: метка -> значение, с сохранением порядка.</summary>
public sealed class TdfFields
{
    private readonly Dictionary<string, TdfNode> _byLabel = new(StringComparer.Ordinal);

    public List<TdfNode> Nodes { get; } = new();

    public void Add(TdfNode node)
    {
        Nodes.Add(node);
        // дубликаты меток: для поиска побеждает последний
        _byLabel[node.Label] = node;
    }

    public TdfNode? this[string label] =>
        _byLabel.TryGetValue(label, out var n) ? n : null;

    public bool Has(string label) => _byLabel.ContainsKey(label);

    public ulong UInt(string label, ulong fallback = 0) =>
        this[label] is TdfInt i ? i.Value : fallback;

    public string Str(string label, string fallback = "") =>
        this[label] is TdfStr s ? s.Value : fallback;

    public byte[]? Bin(string label) =>
        this[label] is TdfBin b ? b.Value : null;

    public TdfStruct? Struct(string label) => this[label] as TdfStruct;

    public TdfList? List(string label) => this[label] as TdfList;

    public TdfMap? Map(string label) => this[label] as TdfMap;

    /// <summary>Все поля в исходном порядке.</summary>
    public IEnumerable<TdfNode> All => Nodes;

    public int Count => Nodes.Count;

    public override string ToString() =>
        $"TdfFields[{Count}]: " +
        string.Join(", ", Nodes.Take(8).Select(n => n.Label));
}