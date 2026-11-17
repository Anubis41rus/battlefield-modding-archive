using System.ComponentModel;
using System.Buffers.Binary;
using System.IO;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using EVBots.Installation;
using Microsoft.Win32.SafeHandles;

namespace EVBots.Launcher;

// The stock exe imports XINPUT9_1_0 and DINPUT8 before EVBots starts. BF-Portable
// may contain unrelated proxies; a mods-directory switch cannot isolate those.
internal static class RoyaleServerLayout
{
    internal sealed record Layout(string ServerDir, string InstanceDir);

    // Supported, statically inspected x86 server build. Never copy arbitrary DLLs.
    // The first two DLLs are PE imports; tbbmalloc/build-info are engine libraries.
    private static readonly (string Name, string Hash)[] Files =
    [
        ("bf3_server_Final.exe", "F39F7623063CFDE9C449A0BDC9A57448D2D137D6DEF00B17EA04800EDA955470"),
        ("D3DCompiler_43.dll", "2F23182EC6F4889397AC4BF03D62536136C5BDBA825C7D2C4EF08C827F3A8A1C"),
        ("d3dx11_43.dll", "492E960CB3CCFC8C25FC83F7C464BA77C86A20411347A1A9B3E5D3E8C9180A8D"),
        ("tbbmalloc.dll", "09300E05E6F1D34949EA78019B82D3FA3F3C87912B351AD6D77E5234F46D18DE"),
        ("Engine.BuildInfo_Win32_Final_dll.dll", "2D52D1DE3618DA8A6D07EE2734F74405017E061900402D2EC7F0AB6C7B9455B4"),
        ("Info_Win32_Final.BuildSettings", "37CC6BCE9100A376F5202DBA275A30FFEC1DF42D3AA5111BB9E104CF5E81F3DC"),
        // BF-Portable LAN/SSL redirect layer, independently traced through DllMain,
        // BF3 branch and every LoadLibrary call. No BotMod or plugin autoload.
        ("dinput8.dll", "2AFB8F19AA1225B40EDF3CDF1FA5C7BCFF84C83F3E9AE0376BA23C2F7F7A5C85"),
        // Native bootstrap 543C70 reads Scripts/ + Win32 + Game + .cfg. This is
        // where the stock distribution enables -server/-dedicated/-super layout.toc.
        ("Scripts/Win32Game.cfg", "3DADFCA05986C4266CEB323AC25F9C5FDB0979A101579D179E26B5242B1932BB")
    ];

    // dataDirectory is the actual Data directory, not the parent of Data.
    // A prepared asset overlay can be supplied here; the caller owns its readiness.
    // This helper neither starts processes nor changes source files or configs.
    internal static Layout Prepare(string sourceServerDir, string sourceInstanceDir,
        string targetServerDir, Action<string> log, string? dataDirectory = null, string serverName = "EVBots")
    {
        var source = Full(sourceServerDir);
        var instance = Full(sourceInstanceDir);
        var target = Full(targetServerDir);
        var sourceData = Path.Combine(source, "Data");
        var data = Full(dataDirectory ?? sourceData);
        // BF-Portable installations sometimes already point Data at their
        // dedicated data directory on another drive. Use that real source.
        if (Same(data, sourceData) && Directory.Exists(data) &&
            (File.GetAttributes(data) & FileAttributes.ReparsePoint) != 0)
        {
            var resolved = new DirectoryInfo(data).ResolveLinkTarget(returnFinalTarget: true);
            if (resolved is not DirectoryInfo)
                throw new IOException("Royale: не удалось найти серверные Data за ссылкой: " + data);
            data = Full(resolved.FullName);
        }
        ValidatePaths(source, instance, target, data);
        foreach (var file in Files) CheckFile(Path.Combine(source, file.Name), file.Hash);
        var provider = Path.Combine(source, "ProviderID.dat");
        CheckProvider(provider, privateCopy: false);
        CheckData(data);
        // Validate the complete existing destination before creating anything.
        CheckDestination(target, data, complete: false);

        Directory.CreateDirectory(target);
        foreach (var file in Files)
        {
            var destination = Path.Combine(target, file.Name);
            if (File.Exists(destination)) continue; // already hash-checked above
            var origin = Path.Combine(source, file.Name);
            Directory.CreateDirectory(Path.GetDirectoryName(destination)!);
            // Only immutable, hash-checked runtime files are linked. Configuration
            // is always a separate copy. Cross-volume / unsupported links copy once.
            if (Path.GetExtension(file.Name).Equals(".cfg", StringComparison.OrdinalIgnoreCase)
                || !CreateHardLinkW(destination, origin, IntPtr.Zero))
                File.Copy(origin, destination, overwrite: false);
            CheckFile(destination, file.Hash);
        }
        var result = new Layout(target, Path.Combine(target, "Instance"));
        var ownProvider = Path.Combine(target, "ProviderID.dat");
        if (!File.Exists(ownProvider)) File.Copy(provider, ownProvider, overwrite: false);
        RoyaleInstanceConfig.Write(result.InstanceDir, serverName);
        var link = Path.Combine(target, "Data");
        if (!Exists(link)) CreateJunction(link, data);
        Check(result, data);
        log("Отдельная папка сервера VU Battle Royale готова: " + target);
        return result;
    }

    internal static void Check(Layout layout, string dataDirectory)
    {
        var target = Full(layout.ServerDir);
        if (!Same(Full(layout.InstanceDir), Path.Combine(target, "Instance")))
            throw new IOException("Royale: Instance должен находиться внутри отдельной папки сервера.");
        CheckDestination(target, Full(dataDirectory), complete: true);
    }

    private static void ValidatePaths(string source, string instance, string target, string data)
    {
        NoReparseAncestors(source);
        NoReparseAncestors(instance);
        NoReparseAncestors(target);
        NoReparseAncestors(data);
        if (!Directory.Exists(source) || !Directory.Exists(instance))
            throw new DirectoryNotFoundException("Royale: не найдена исходная папка сервера или Instance.");
        foreach (var original in new[] { source, instance, data })
            if (Within(target, original) || Within(original, target))
                throw new IOException("Royale: отдельная папка сервера не должна пересекаться с исходными папками.");
    }

    private static void CheckDestination(string target, string data, bool complete)
    {
        NoReparseAncestors(target);
        NoReparseAncestors(data);
        CheckData(data);
        if (!Directory.Exists(target))
        {
            if (Exists(target) || complete) throw new IOException("Royale: отсутствует отдельная папка сервера: " + target);
            return;
        }
        foreach (var item in Directory.EnumerateFileSystemEntries(target))
        {
            var name = Path.GetFileName(item);
            if (Same(name, "Data")) { CheckJunction(item, data); continue; }
            var attributes = File.GetAttributes(item);
            if ((attributes & FileAttributes.ReparsePoint) != 0)
                throw new IOException("Royale: посторонняя ссылка в папке сервера: " + item);
            if ((attributes & FileAttributes.Directory) != 0) continue;
            if (name.StartsWith("BotMod", StringComparison.OrdinalIgnoreCase)
                || (Path.GetExtension(name).ToLowerInvariant() is ".dll" or ".exe" or ".asi"
                    && !Files.Any(file => Same(file.Name, name))))
                throw new IOException("Royale: посторонний модуль в отдельной папке сервера: " + name);
        }
        foreach (var file in Files)
        {
            var path = Path.Combine(target, file.Name);
            if (Exists(path) || complete) CheckFile(path, file.Hash);
        }
        var provider = Path.Combine(target, "ProviderID.dat");
        if (Exists(provider) || complete) CheckProvider(provider, privateCopy: true);
        var scripts = Path.Combine(target, "Scripts");
        NoReparseAncestors(scripts);
        if (Exists(scripts))
        {
            if (!Directory.Exists(scripts)) throw new IOException("Royale: Scripts должна быть отдельной папкой.");
            foreach (var item in Directory.EnumerateFileSystemEntries(scripts))
                if (!Same(Path.GetFileName(item), "Win32Game.cfg"))
                    throw new IOException("Royale: посторонний файл в Scripts: " + item);
        }
        var instance = Path.Combine(target, "Instance");
        var admin = Path.Combine(instance, "Admin");
        foreach (var dir in new[] { instance, admin })
        {
            NoReparseAncestors(dir);
            if ((Exists(dir) || complete) && !Directory.Exists(dir))
                throw new IOException("Royale: требуется собственная папка: " + dir);
        }
        // Do not accept redirected individual config files either. The caller writes
        // these three files after Prepare; refuse hardlinks to another config too.
        foreach (var config in new[] { "ServerOptions.cfg", "serverid.dat", "Admin\\Startup.txt", "Admin\\MapList.txt", "Admin\\ModList.txt" })
        {
            var path = Path.Combine(instance, config);
            if (Exists(path)) CheckPrivateConfig(path);
            else if (complete && config == "ServerOptions.cfg")
                throw new IOException("Royale: отсутствует ServerOptions.cfg в отдельном Instance.");
        }
        if (complete) CheckJunction(Path.Combine(target, "Data"), data);
    }

    private static void CheckPrivateConfig(string path)
    {
        CheckRegularFile(path);
        using var handle = File.OpenHandle(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
        if (!GetFileInformationByHandle(handle, out var info))
            throw new Win32Exception(Marshal.GetLastWin32Error(), "Royale: не удалось проверить файл настроек.");
        if (info.NumberOfLinks != 1)
            throw new IOException("Royale: файл настроек связан с другой папкой: " + path);
    }

    private static void CheckProvider(string path, bool privateCopy)
    {
        NoReparseAncestors(path);
        if (privateCopy) CheckPrivateConfig(path); else CheckRegularFile(path);
        // Stock ProviderInfo 011ECF90 reads 20 bytes, then 011ED048..011ED05C
        // sums five DWORDs modulo 2^32 and compares C18E65EF. This is identity
        // from the user's server install, not a fixed globally bundled value.
        if (new FileInfo(path).Length != 20) throw new IOException("Royale: ProviderID.dat должен содержать 20 байт: " + path);
        var bytes = File.ReadAllBytes(path);
        uint sum = 0;
        for (var at = 0; at < bytes.Length; at += 4)
            sum = unchecked(sum + BinaryPrimitives.ReadUInt32LittleEndian(bytes.AsSpan(at, 4)));
        if (sum != 0xC18E65EFu) throw new IOException("Royale: неверная контрольная сумма ProviderID.dat: " + path);
    }

    private static void CheckData(string path)
    {
        if (!Directory.Exists(path) || !File.Exists(Path.Combine(path, "layout.toc"))
            || !File.Exists(Path.Combine(path, "initfs_Linux")) || !Directory.Exists(Path.Combine(path, "Linux")))
            throw new IOException("Royale: неполная папка серверных Data: " + path);
    }

    private static void CheckFile(string path, string expected)
    {
        NoReparseAncestors(path);
        CheckRegularFile(path);
        using var stream = File.OpenRead(path);
        var hash = Convert.ToHexString(SHA256.HashData(stream));
        if (!string.Equals(hash, expected, StringComparison.OrdinalIgnoreCase))
            throw new IOException("Royale: неподдерживаемый или изменённый файл " + path + ". SHA256: " + hash);
    }

    private static void CheckRegularFile(string path)
    {
        if (!File.Exists(path) || (File.GetAttributes(path) & (FileAttributes.ReparsePoint | FileAttributes.Directory)) != 0)
            throw new IOException("Royale: отсутствует обычный файл: " + path);
    }

    private static void CheckJunction(string path, string expected)
    {
        var dir = new DirectoryInfo(path);
        if (!dir.Exists || (dir.Attributes & FileAttributes.ReparsePoint) == 0)
            throw new IOException("Royale: Data должна быть ссылкой на проверенную папку ресурсов: " + path);
        var resolved = dir.ResolveLinkTarget(returnFinalTarget: true);
        if (resolved is null || !Same(Full(resolved.FullName), expected))
            throw new IOException("Royale: ссылка Data ведёт в другую папку: " + path);
    }

    private static bool Exists(string path) => Path.Exists(path);
    private static string Full(string path)
    {
        if (string.IsNullOrWhiteSpace(path)) throw new ArgumentException("Royale: пустой путь.");
        var full = Path.TrimEndingDirectorySeparator(Path.GetFullPath(path));
        if (full.StartsWith(@"\\", StringComparison.Ordinal) || Same(full, Path.GetPathRoot(full) ?? ""))
            throw new IOException("Royale: нужна локальная папка, не корень диска: " + full);
        return full;
    }
    private static bool Same(string a, string b) => string.Equals(a, b, StringComparison.OrdinalIgnoreCase);
    private static bool Within(string path, string root) => Same(path, root)
        || path.StartsWith(root + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase);
    private static void NoReparseAncestors(string path)
    {
        for (var current = path; !string.IsNullOrEmpty(current); current = Path.GetDirectoryName(current))
            if (Exists(current) && (File.GetAttributes(current) & FileAttributes.ReparsePoint) != 0)
                throw new IOException("Royale: путь проходит через постороннюю ссылку: " + current);
    }

    // NTFS junctions work without Developer Mode or symlink privilege. No shell.
    private static void CreateJunction(string link, string target)
    {
        var substitute = Encoding.Unicode.GetBytes(@"\??\" + target);
        var print = Encoding.Unicode.GetBytes(target);
        var buffer = new byte[16 + substitute.Length + 2 + print.Length + 2];
        BitConverter.GetBytes(0xA0000003u).CopyTo(buffer, 0); // IO_REPARSE_TAG_MOUNT_POINT
        BitConverter.GetBytes(checked((ushort)(buffer.Length - 8))).CopyTo(buffer, 4);
        BitConverter.GetBytes(checked((ushort)substitute.Length)).CopyTo(buffer, 10);
        BitConverter.GetBytes(checked((ushort)(substitute.Length + 2))).CopyTo(buffer, 12);
        BitConverter.GetBytes(checked((ushort)print.Length)).CopyTo(buffer, 14);
        substitute.CopyTo(buffer, 16);
        print.CopyTo(buffer, 18 + substitute.Length);
        Directory.CreateDirectory(link);
        using var handle = CreateFileW(link, 0x40000000, 0, IntPtr.Zero, 3, 0x02200000, IntPtr.Zero);
        if (handle.IsInvalid || !DeviceIoControl(handle, 0x000900A4, buffer, buffer.Length, IntPtr.Zero, 0, out _, IntPtr.Zero))
        {
            var error = Marshal.GetLastWin32Error();
            handle.Dispose();
            // Only our just-created, empty directory; never recursive or a target.
            Directory.Delete(link, recursive: false);
            throw new Win32Exception(error, "Royale: не удалось создать ссылку Data.");
        }
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct FileInformation
    {
        public uint Attributes;
        public System.Runtime.InteropServices.ComTypes.FILETIME Creation, Access, Write;
        public uint VolumeSerial, SizeHigh, SizeLow, NumberOfLinks, IndexHigh, IndexLow;
    }
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool CreateHardLinkW(string name, string existing, IntPtr security);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern SafeFileHandle CreateFileW(string name, uint access, uint share, IntPtr security, uint creation, uint flags, IntPtr template);
    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool DeviceIoControl(SafeFileHandle file, uint code, byte[] input, int inputSize, IntPtr output, int outputSize, out uint returned, IntPtr overlapped);
    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetFileInformationByHandle(SafeFileHandle file, out FileInformation info);
}
