using System.Net;
using System.Net.Sockets;
using System.Text.RegularExpressions;

namespace EVBots.Core;

/// <summary>Проверки ввода. Правила логина и пароля — как в AccountsStore.cs бэкенда.</summary>
public static class Validation
{
    private static readonly Regex LoginRx = new(@"^[A-Za-z0-9_.\-]{3,32}$");
    private static readonly Regex IdRx = new(@"^[A-Za-z0-9_]{2,40}$");

    public static string? Login(string? login)
    {
        if (string.IsNullOrEmpty(login))
        {
            return "Укажите логин.";
        }

        return LoginRx.IsMatch(login)
            ? null
            : "Логин: от 3 до 32 символов, только латиница, цифры, «_», «-», «.».";
    }

    public static string? Password(string? password) =>
        string.IsNullOrEmpty(password) || password.Length < 4
            ? "Пароль: не меньше 4 символов."
            : null;

    public static string? ServerName(string? name)
    {
        var n = name?.Trim() ?? "";
        if (n.Length < 1 || n.Length > 40)
        {
            return "Имя сервера: от 1 до 40 символов.";
        }

        // символы " % ^ & | < > ломают командную строку cmd.exe, через которую стартуют бэкенд и сервер
        return n.Any(c => "\"%^&|<>".Contains(c) || char.IsControl(c))
            ? "Имя сервера: без кавычек, % ^ & | < > и управляющих символов."
            : null;
    }

    public static string? MaxPlayers(int value) =>
        value is >= 1 and <= 64 ? null : "Макс. игроков: от 1 до 64.";

    public static string? Bots(int value) =>
        value is >= 0 and <= 128 ? null : "Боты: от 0 до 128.";

    public static string? MapOrMode(string? id, string what) =>
        id is not null && IdRx.IsMatch(id) ? null : $"{what}: латиница, цифры и «_», от 2 до 40 символов.";

    public static bool TryIPv4(string? text, out IPAddress? address)
    {
        address = null;
        if (!IPAddress.TryParse(text ?? "", out var parsed) || parsed.AddressFamily != AddressFamily.InterNetwork)
        {
            return false;
        }

        address = parsed;
        return true;
    }
}
