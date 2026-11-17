using System.Runtime.InteropServices;
using System.Windows.Media;

namespace EVBots.Gui;

/// <summary>
/// Эффект Acrylic (Windows 10 1803+) через user32!SetWindowCompositionAttribute.
/// Если вызов не удался (старая Windows, ошибка API) — возвращаем false, окно переходит
/// на полупрозрачные градиенты без размытия.
/// </summary>
internal static class Acrylic
{
    private const int WcaAccentPolicy = 19;
    private const int AccentDisabled = 0;
    private const int AccentEnableAcrylicBlurBehind = 4;
    private const int MinBuildAcrylic = 17134;

    [StructLayout(LayoutKind.Sequential)]
    private struct AccentPolicy
    {
        public int AccentState;
        public int AccentFlags;
        public uint GradientColor;
        public int AnimationId;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct WindowCompositionAttributeData
    {
        public int Attribute;
        public IntPtr Data;
        public int SizeOfData;
    }

    [DllImport("user32.dll")]
    private static extern int SetWindowCompositionAttribute(IntPtr hwnd, ref WindowCompositionAttributeData data);

    /// <summary>Включает размытие с цветовой подложкой (tint с альфой).</summary>
    public static bool TryEnable(IntPtr hwnd, Color tint)
    {
        if (hwnd == IntPtr.Zero || Environment.OSVersion.Version.Build < MinBuildAcrylic)
        {
            return false;
        }

        // GradientColor в формате AABBGGRR
        var abgr = ((uint)tint.A << 24) | ((uint)tint.B << 16) | ((uint)tint.G << 8) | tint.R;
        return Apply(hwnd, AccentEnableAcrylicBlurBehind, abgr);
    }

    public static void Disable(IntPtr hwnd)
    {
        if (hwnd != IntPtr.Zero)
        {
            Apply(hwnd, AccentDisabled, 0);
        }
    }

    private static bool Apply(IntPtr hwnd, int state, uint color)
    {
        var accent = new AccentPolicy
        {
            AccentState = state,
            AccentFlags = 2,
            GradientColor = color,
            AnimationId = 0,
        };

        var size = Marshal.SizeOf<AccentPolicy>();
        var ptr = Marshal.AllocHGlobal(size);
        try
        {
            Marshal.StructureToPtr(accent, ptr, false);
            var data = new WindowCompositionAttributeData
            {
                Attribute = WcaAccentPolicy,
                Data = ptr,
                SizeOfData = size,
            };
            return SetWindowCompositionAttribute(hwnd, ref data) != 0;
        }
        catch (Exception)
        {
            // нет точки входа или API недоступен
            return false;
        }
        finally
        {
            Marshal.FreeHGlobal(ptr);
        }
    }
}
