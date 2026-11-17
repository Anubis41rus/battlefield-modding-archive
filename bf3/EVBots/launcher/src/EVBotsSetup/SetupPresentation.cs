using System;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.IO;
using System.Windows.Forms;

namespace EVBots.Setup;

// Только представление: не распаковывает файлы, не меняет настройки и не запускает процессы.
internal sealed class SetupPresentation
{
    private static readonly Color Background = Color.FromArgb(20, 22, 26);
    private static readonly Color Card = Color.FromArgb(29, 33, 38);
    private static readonly Color Text = Color.FromArgb(232, 237, 243);
    private static readonly Color Muted = Color.FromArgb(158, 169, 182);
    private static readonly Color Accent = Color.FromArgb(245, 153, 74);
    private static readonly Color Good = Color.FromArgb(123, 216, 143);
    private static readonly Color Amber = Color.FromArgb(241, 190, 103);
    private readonly Label _game;
    private readonly Label _server;
    private readonly Label _destination;
    private readonly Label _state;
    private readonly ToolTip _paths = new() { AutoPopDelay = 15000, InitialDelay = 300 };

    internal SetupPresentation(Form form, Label state, CheckBox shortcut, CheckBox launch, CheckBox firewall,
        ProgressBar progress, ListBox log, Button install, Button cancel)
    {
        _state = state;
        form.SuspendLayout();
        form.Text = "Battle Royale LAN · Установка";
        form.BackColor = Background;
        form.ForeColor = Text;
        form.Font = new Font("Segoe UI", 9.5f);
        form.AutoScaleDimensions = new SizeF(96, 96);
        form.AutoScaleMode = AutoScaleMode.Dpi;
        form.FormBorderStyle = FormBorderStyle.Sizable;
        form.MinimumSize = new Size(660, 500);
        form.ClientSize = new Size(820, 740);
        form.MaximizeBox = false;

        var layout = new TableLayoutPanel { Dock = DockStyle.Fill, Size = form.ClientSize, ColumnCount = 1, RowCount = 3, Margin = Padding.Empty };
        layout.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 244));
        layout.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
        layout.RowStyles.Add(new RowStyle(SizeType.Absolute, 92));
        var header = new BrandingHeader { Dock = DockStyle.Fill, Size = new Size(820, 244), Margin = Padding.Empty };
        layout.Controls.Add(header, 0, 0);

        // Середина прокручивается на небольшом экране и при масштабе Windows 125–150%.
        // Кнопки и состояние установки остаются видимыми в нижней панели.
        var scroll = new Panel { Dock = DockStyle.Fill, Size = new Size(820, 490), AutoScroll = true, Padding = new Padding(24, 0, 24, 8), Margin = Padding.Empty };
        var body = new TableLayoutPanel { Dock = DockStyle.Top, Size = new Size(772, 458), AutoSize = true, ColumnCount = 1, RowCount = 4, Margin = Padding.Empty };
        body.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        foreach (var height in new[] { 116, 144, 100, 98 }) body.RowStyles.Add(new RowStyle(SizeType.Absolute, height));
        scroll.Controls.Add(body);
        layout.Controls.Add(scroll, 0, 1);

        var modes = new TableLayoutPanel { Dock = DockStyle.Fill, Size = new Size(772, 104), ColumnCount = 2, RowCount = 1, Margin = new Padding(0, 0, 0, 12) };
        modes.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 50));
        modes.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 50));
        modes.Controls.Add(ModeCard("BATTLE ROYALE", "LAN", "Создай локальный сервер\nили подключись к друзьям.", Accent, new Padding(0, 0, 6, 0)), 0, 0);
        modes.Controls.Add(ModeCard("ЛОКАЛЬНАЯ ИГРА", "ТЕСТ", "Общий сервер в вашей сети.\nПолный матч ещё проверяется.", Amber, new Padding(6, 0, 0, 0)), 1, 0);
        body.Controls.Add(modes, 0, 0);

        var folders = Surface(new Padding(0, 0, 0, 12));
        folders.Controls.Add(LabelAt("ПАПКИ РЯДОМ С УСТАНОВЩИКОМ", 8.5f, FontStyle.Bold, Muted, 18, 12, 680, 20));
        _game = PathLine(38); _server = PathLine(64); _destination = PathLine(90);
        folders.Controls.AddRange(new Control[] { _game, _server, _destination });
        body.Controls.Add(folders, 0, 1);

        var options = Surface(new Padding(0, 0, 0, 12));
        shortcut.Text = "Создать ярлык EV Launcher на рабочем столе";
        launch.Text = "Открыть EV Launcher после установки";
        firewall.Text = "Настроить брандмауэр Windows для игры по сети (запрос UAC)";
        var checks = new[] { shortcut, launch, firewall };
        for (var i = 0; i < checks.Length; i++)
        {
            var check = checks[i];
            check.AutoSize = false; check.SetBounds(18, 9 + i * 23, 730, 23);
            check.Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right;
            check.ForeColor = Text; check.BackColor = Card; check.FlatStyle = FlatStyle.Flat;
            check.TabIndex = i; options.Controls.Add(check);
        }
        body.Controls.Add(options, 0, 2);

        var details = Surface(Padding.Empty);
        details.Size = new Size(772, 98);
        details.Controls.Add(LabelAt("ЖУРНАЛ УСТАНОВКИ", 8.5f, FontStyle.Bold, Muted, 18, 10, 400, 19));
        log.Location = new Point(18, 34); log.Size = new Size(730, 48);
        log.Anchor = AnchorStyles.Top | AnchorStyles.Bottom | AnchorStyles.Left | AnchorStyles.Right;
        log.BackColor = Card; log.ForeColor = Muted; log.BorderStyle = BorderStyle.None;
        log.Font = new Font("Consolas", 8.5f); log.IntegralHeight = false;
        log.TabStop = true; log.AccessibleName = "Журнал установки";
        details.Controls.Add(log);
        body.Controls.Add(details, 0, 3);

        var footer = new Panel { Dock = DockStyle.Fill, Size = new Size(820, 92), Padding = new Padding(24, 12, 24, 12), Margin = Padding.Empty };
        progress.SetBounds(24, 8, 772, 5); progress.Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right;
        progress.Style = ProgressBarStyle.Continuous;
        state.AutoSize = false; state.SetBounds(24, 28, 460, 48);
        state.Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right;
        state.ForeColor = Muted; state.TextAlign = ContentAlignment.MiddleLeft;
        state.AccessibleName = "Состояние установки";
        StyleButton(install, true); StyleButton(cancel, false);
        install.SetBounds(500, 29, 174, 42); cancel.SetBounds(686, 29, 110, 42);
        install.Anchor = cancel.Anchor = AnchorStyles.Top | AnchorStyles.Right;
        install.Text = "Установить"; cancel.Text = "Отмена";
        footer.Controls.AddRange(new Control[] { progress, state, install, cancel });
        layout.Controls.Add(footer, 0, 2);
        form.Controls.Add(layout);
        form.AcceptButton = install; form.CancelButton = cancel;
        form.Disposed += (_, _) => _paths.Dispose();
        form.Load += (_, _) =>
        {
            var area = Screen.FromControl(form).WorkingArea;
            form.Size = new Size(Math.Min(form.Width, area.Width - 24), Math.Min(form.Height, area.Height - 24));
        };
        form.ResumeLayout(true);
    }

    internal void UpdateDetectedPaths(string root, string game, string server)
    {
        _game.Text = game.Length > 0 ? "НАЙДЕНА   Battlefield 3  ·  " + Path.GetDirectoryName(game) : "НЕ НАЙДЕНА   Battlefield 3  ·  нужна папка с bf3.exe";
        _game.ForeColor = game.Length > 0 ? Good : Amber;
        _server.Text = server.Length > 0 ? "НАЙДЕН   BF-Portable  ·  можно создать свой сервер" : "НЕТ BF-Portable  ·  доступен только вход на сервер друга";
        _server.ForeColor = server.Length > 0 ? Good : Muted;
        _destination.Text = "УСТАНОВКА   " + Path.Combine(root, "EVBots");
        _paths.SetToolTip(_game, game.Length > 0 ? game : "Положите установщик рядом с папкой Battlefield 3 и откройте его снова.");
        _paths.SetToolTip(_server, server.Length > 0 ? server : "Для своего сервера рядом нужна папка BF-Portable или Battlefield Portable.");
        _paths.SetToolTip(_destination, Path.Combine(root, "EVBots"));
        _state.Text = game.Length > 0 ? "Всё готово к установке\nлокального Battle Royale." : "Переместите установщик рядом с Battlefield 3\nи откройте его снова.";
        _state.ForeColor = game.Length > 0 ? Muted : Amber;
    }

    private static Panel ModeCard(string name, string badge, string description, Color color, Padding margin)
    {
        var card = Surface(margin);
        card.Size = new Size(380, 104);
        card.Controls.Add(LabelAt(name, 14, FontStyle.Bold, Text, 18, 13, 300, 27));
        var tag = LabelAt(badge, 8, FontStyle.Bold, color, 290, 20, 62, 18);
        tag.TextAlign = ContentAlignment.TopRight; tag.Anchor = AnchorStyles.Top | AnchorStyles.Right;
        card.Controls.Add(tag);
        card.Controls.Add(LabelAt(description, 9.25f, FontStyle.Regular, Muted, 18, 48, 330, 44));
        return card;
    }

    private static Label PathLine(int top)
    {
        var line = LabelAt("", 9.25f, FontStyle.Regular, Muted, 18, top, 730, 23);
        line.AutoEllipsis = true;
        return line;
    }

    private static Label LabelAt(string value, float size, FontStyle style, Color color, int x, int y, int width, int height) =>
        new() { Text = value, Font = new Font("Segoe UI", size, style), ForeColor = color, BackColor = Color.Transparent,
            Location = new Point(x, y), Size = new Size(width, height), Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right };

    private static Panel Surface(Padding margin) => new SurfacePanel { Dock = DockStyle.Fill, Size = new Size(772, 132), Margin = margin, BackColor = Card };

    private static void StyleButton(Button button, bool primary)
    {
        button.FlatStyle = FlatStyle.Flat; button.FlatAppearance.BorderSize = primary ? 0 : 1;
        button.FlatAppearance.BorderColor = Color.FromArgb(65, 76, 88);
        button.FlatAppearance.MouseOverBackColor = primary ? Color.FromArgb(255, 194, 127) : Color.FromArgb(42, 49, 57);
        button.BackColor = primary ? Accent : Card;
        button.ForeColor = primary ? Color.FromArgb(12, 25, 39) : Text;
        button.Font = new Font("Segoe UI", 9.5f, primary ? FontStyle.Bold : FontStyle.Regular);
        button.Cursor = Cursors.Hand; button.UseVisualStyleBackColor = false;
        button.EnabledChanged += (_, _) =>
        {
            button.BackColor = !button.Enabled ? Color.FromArgb(39, 45, 53) : primary ? Accent : Card;
            button.ForeColor = !button.Enabled ? Muted : primary ? Color.FromArgb(12, 25, 39) : Text;
            button.Cursor = button.Enabled ? Cursors.Hand : Cursors.Default;
        };
    }

    private sealed class BrandingHeader : Panel
    {
        private readonly Image _background = LoadArtwork("launcher-background.png");
        private readonly Image _icon = LoadArtwork("battle-royale-icon.png");
        private readonly Image _wordmark = LoadArtwork("battle-royale-wordmark.png");

        internal BrandingHeader() { DoubleBuffered = true; ResizeRedraw = true; }

        private static Image LoadArtwork(string name)
        {
            using var stream = typeof(SetupPresentation).Assembly.GetManifestResourceStream(name)
                ?? throw new InvalidOperationException("В сборке отсутствует изображение: " + name);
            using var image = Image.FromStream(stream);
            return new Bitmap(image);
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            base.OnPaint(e);
            var g = e.Graphics;
            g.InterpolationMode = InterpolationMode.HighQualityBicubic;
            var scale = Math.Max((float)Width / _background.Width, (float)Height / _background.Height);
            var width = _background.Width * scale;
            var height = _background.Height * scale;
            g.DrawImage(_background, (Width - width) / 2, (Height - height) / 2, width, height);
            var unit = DeviceDpi / 96f;
            using var shade = new SolidBrush(Color.FromArgb(95, 7, 14, 22));
            g.FillRectangle(shade, ClientRectangle);
            g.DrawImage(_icon, 24 * unit, 28 * unit, 72 * unit, 72 * unit);
            g.TextRenderingHint = System.Drawing.Text.TextRenderingHint.AntiAliasGridFit;
            using var title = new Font("Bahnschrift", 19, FontStyle.Bold);
            using var label = new Font("Segoe UI", 10, FontStyle.Bold);
            using var ink = new SolidBrush(Color.White);
            g.DrawString("EV LAUNCHER", title, ink, new PointF(24 * unit, 118 * unit));
            g.DrawString("УСТАНОВКА  /  BF3 LAN", label, ink, new PointF(24 * unit, Height - 38 * unit));
            var logoWidth = Math.Min(390 * unit, Width - 245 * unit);
            var logoHeight = logoWidth * _wordmark.Height / _wordmark.Width;
            g.DrawImage(_wordmark, Width - logoWidth - 12 * unit, (Height - logoHeight) / 2, logoWidth, logoHeight);
        }

        protected override void Dispose(bool disposing)
        {
            if (disposing) { _background.Dispose(); _icon.Dispose(); _wordmark.Dispose(); }
            base.Dispose(disposing);
        }
    }

    private sealed class SurfacePanel : Panel
    {
        internal SurfacePanel() { DoubleBuffered = true; ResizeRedraw = true; }
        protected override void OnPaint(PaintEventArgs e)
        {
            base.OnPaint(e);
            e.Graphics.SmoothingMode = SmoothingMode.AntiAlias;
            using var border = new Pen(Color.FromArgb(43, 50, 59));
            e.Graphics.DrawRectangle(border, 0, 0, Math.Max(0, Width - 1), Math.Max(0, Height - 1));
        }
    }
}

internal sealed class SetupProgressBar : ProgressBar
{
    internal SetupProgressBar() => SetStyle(ControlStyles.UserPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.AllPaintingInWmPaint, true);

    protected override void OnPaint(PaintEventArgs e)
    {
        e.Graphics.Clear(Color.FromArgb(43, 50, 59));
        var range = Maximum - Minimum;
        var width = range > 0 ? (int)((long)ClientSize.Width * (Value - Minimum) / range) : 0;
        if (width <= 0) return;
        using var fill = new SolidBrush(Color.FromArgb(245, 153, 74));
        e.Graphics.FillRectangle(fill, 0, 0, width, ClientSize.Height);
    }

    protected override void WndProc(ref Message message)
    {
        base.WndProc(ref message);
        if (message.Msg == 0x402 || message.Msg == 0x403 || message.Msg == 0x406) Invalidate();
    }
}
