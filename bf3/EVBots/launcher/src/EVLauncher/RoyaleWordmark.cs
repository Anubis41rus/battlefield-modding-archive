using System.Globalization;
using System.Windows;
using System.Windows.Media;

namespace EVBots.Launcher;

/// <summary>Векторная эмблема: прозрачный фон, металлические грани и оранжевый акцент.</summary>
public sealed class RoyaleWordmark : FrameworkElement
{
    protected override Size MeasureOverride(Size availableSize) => new(440, 230);

    protected override void OnRender(DrawingContext dc)
    {
        base.OnRender(dc);
        dc.PushTransform(new ScaleTransform(ActualWidth / 440, ActualHeight / 230));
        var metal = new LinearGradientBrush();
        metal.StartPoint = new Point(0, 0); metal.EndPoint = new Point(0, 1);
        metal.GradientStops.Add(new GradientStop(Color.FromRgb(252, 254, 255), 0));
        metal.GradientStops.Add(new GradientStop(Color.FromRgb(181, 205, 221), 0.48));
        metal.GradientStops.Add(new GradientStop(Color.FromRgb(85, 112, 132), 0.50));
        metal.GradientStops.Add(new GradientStop(Color.FromRgb(233, 246, 252), 0.57));
        metal.GradientStops.Add(new GradientStop(Color.FromRgb(149, 169, 185), 1));
        var orange = new SolidColorBrush(Color.FromRgb(255, 135, 54));
        var caption = Glyphs("B A T T L E F I E L D   3", 13, FontWeights.Medium);
        Center(dc, caption, new SolidColorBrush(Color.FromRgb(190, 210, 225)), null, 10);
        var title = Glyphs("BF3", 142, FontWeights.Bold);
        for (var i = 5; i > 0; i--)
        {
            dc.PushTransform(new TranslateTransform(i * 0.7, i));
            Center(dc, title, new SolidColorBrush(Color.FromRgb(12, 24, 34)), new Pen(orange, 0.25), 39);
            dc.Pop();
        }
        Center(dc, title, metal, new Pen(new SolidColorBrush(Color.FromRgb(216, 236, 249)), 0.7), 39);
        var subtitle = Glyphs("BATTLE ROYALE", 29, FontWeights.Bold);
        Center(dc, subtitle, Brushes.White, null, 160);
        // Узкое свечение и острые штрихи вместо прямоугольной подложки.
        for (var i = 7; i > 0; i--)
            dc.DrawLine(new Pen(new SolidColorBrush(Color.FromArgb((byte)(18 - i), 255, 117, 36)), i * 2), new Point(62, 204), new Point(378, 204));
        dc.DrawLine(new Pen(orange, 2), new Point(62, 204), new Point(378, 204));
        dc.DrawLine(new Pen(orange, 3), new Point(45, 210), new Point(53, 198));
        dc.DrawLine(new Pen(orange, 3), new Point(387, 210), new Point(395, 198));
        dc.Pop();
    }

    private Geometry Glyphs(string text, double size, FontWeight weight)
    {
        var formatted = new FormattedText(text, CultureInfo.InvariantCulture, FlowDirection.LeftToRight,
            new Typeface(new FontFamily("Bahnschrift"), FontStyles.Normal, weight, FontStretches.Condensed),
            size, Brushes.White, VisualTreeHelper.GetDpi(this).PixelsPerDip);
        return formatted.BuildGeometry(new Point());
    }

    private static void Center(DrawingContext dc, Geometry shape, Brush fill, Pen? edge, double y)
    {
        var bounds = shape.Bounds;
        dc.PushTransform(new TranslateTransform((440 - bounds.Width) / 2 - bounds.Left, y - bounds.Top));
        dc.DrawGeometry(fill, edge, shape);
        dc.Pop();
    }
}
