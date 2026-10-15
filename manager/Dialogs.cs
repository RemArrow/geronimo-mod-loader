// Dialogs built in code: a mod's settings (its .cfg), and "New plugin project".
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using GMLManager.Core;

namespace GMLManager
{
    static class Ui
    {
        public static Brush B(string key) => (Brush)Application.Current.Resources[key];
        public static Style S(string key) => (Style)Application.Current.Resources[key];

        public static void Theme(Window w, string title, double width, double height)
        {
            w.Title = title;
            w.Width = width;
            w.Height = height;
            w.Background = B("Bg");
            w.Foreground = B("Fg");
            w.FontFamily = new FontFamily("Segoe UI");
            w.FontSize = 13;
            w.WindowStartupLocation = WindowStartupLocation.CenterOwner;
            w.ShowInTaskbar = false;
        }

        public static TextBlock Text(string s, string style = null, double size = 0)
        {
            var t = new TextBlock { Text = s, TextWrapping = TextWrapping.Wrap };
            if (style != null) t.Style = S(style);
            if (size > 0) t.FontSize = size;
            return t;
        }
    }

    // ---------------------------------------------------------------------------------------------
    // A plugin's settings: one row per "Key = value" in its .cfg, grouped by section.
    public class CfgWindow : Window
    {
        readonly CfgFile _cfg;
        readonly List<(CfgEntry e, Func<string> get, Action<string> set)> _editors = new List<(CfgEntry, Func<string>, Action<string>)>();

        public CfgWindow(string path, string title, bool gameRunning)
        {
            Ui.Theme(this, title + " settings", 640, 640);
            _cfg = CfgFile.Load(path);

            var root = new DockPanel { Margin = new Thickness(18) };
            var head = new StackPanel { Margin = new Thickness(0, 0, 0, 12) };
            head.Children.Add(Ui.Text(title, "H2", 17));
            head.Children.Add(Ui.Text(gameRunning
                ? "The game is running: most mods read their settings at launch, so changes apply from the next start."
                : "Changes apply the next time the game starts.", "Dim", 12));
            DockPanel.SetDock(head, Dock.Top);
            root.Children.Add(head);

            var buttons = new StackPanel { Orientation = Orientation.Horizontal, HorizontalAlignment = HorizontalAlignment.Right, Margin = new Thickness(0, 12, 0, 0) };
            var open = new Button { Content = "Open file" };
            open.Click += (s, e) => System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo(path) { UseShellExecute = true });
            var cancel = new Button { Content = "Cancel", IsCancel = true };
            var save = new Button { Content = "Save", Style = Ui.S("AccentButton"), IsDefault = true, Margin = new Thickness(0) };
            save.Click += (s, e) => Save();
            buttons.Children.Add(open);
            buttons.Children.Add(cancel);
            buttons.Children.Add(save);
            DockPanel.SetDock(buttons, Dock.Bottom);
            root.Children.Add(buttons);

            var list = new StackPanel();
            if (_cfg.Entries.Count == 0) list.Children.Add(Ui.Text("This mod has no settings.", "Dim"));
            foreach (var section in _cfg.Entries.GroupBy(x => x.Section))
            {
                list.Children.Add(new TextBlock { Text = section.Key, FontSize = 13, FontWeight = FontWeights.SemiBold, Foreground = Ui.B("Accent"), Margin = new Thickness(0, 10, 0, 6) });
                foreach (var e in section) list.Children.Add(Row(e));
            }
            root.Children.Add(new ScrollViewer { Content = list, VerticalScrollBarVisibility = ScrollBarVisibility.Auto, Padding = new Thickness(0, 0, 8, 0) });
            Content = root;
        }

        UIElement Row(CfgEntry e)
        {
            var card = new Border { Style = Ui.S("Card"), Padding = new Thickness(12, 10, 12, 10) };
            var g = new Grid();
            g.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            var left = new StackPanel { Margin = new Thickness(0, 0, 12, 0) };
            left.Children.Add(new TextBlock { Text = e.Key, FontWeight = FontWeights.SemiBold });
            if (!string.IsNullOrEmpty(e.Description)) left.Children.Add(Ui.Text(e.Description, "Dim", 12));
            if (e.Default != null) left.Children.Add(Ui.Text($"default: {e.Default}", "Dim", 11));
            g.Children.Add(left);

            FrameworkElement editor;
            var type = (e.Type ?? "").ToLowerInvariant();
            if (type == "boolean")
            {
                var cb = new CheckBox { IsChecked = e.Value.Equals("true", StringComparison.OrdinalIgnoreCase), VerticalAlignment = VerticalAlignment.Center };
                _editors.Add((e, () => cb.IsChecked == true ? "true" : "false", v => cb.IsChecked = v.Equals("true", StringComparison.OrdinalIgnoreCase)));
                editor = cb;
            }
            else if (e.AcceptableValues.Count > 0)
            {
                var combo = new ComboBox { MinWidth = 140, VerticalAlignment = VerticalAlignment.Center };
                foreach (var v in e.AcceptableValues) combo.Items.Add(v);
                combo.SelectedItem = e.AcceptableValues.FirstOrDefault(v => v.Equals(e.Value, StringComparison.OrdinalIgnoreCase));
                _editors.Add((e, () => combo.SelectedItem as string ?? e.Value, v => combo.SelectedItem = v));
                editor = combo;
            }
            else
            {
                bool numeric = type == "single" || type == "double" || type.StartsWith("int") || type.StartsWith("uint");
                var tb = new TextBox { Text = e.Value, Width = numeric ? 110 : 220, VerticalAlignment = VerticalAlignment.Center };
                _editors.Add((e, () => tb.Text.Trim(), v => tb.Text = v));
                editor = tb;
            }
            var right = new StackPanel { Orientation = Orientation.Horizontal, VerticalAlignment = VerticalAlignment.Center };
            right.Children.Add(editor);
            if (e.Default != null)
            {
                var reset = new Button { Content = "↺", ToolTip = "Back to " + e.Default, Padding = new Thickness(8, 3, 8, 3), Margin = new Thickness(8, 0, 0, 0) };
                var set = _editors.Last().set;
                reset.Click += (s, a) => set(e.Default);
                right.Children.Add(reset);
            }
            Grid.SetColumn(right, 1);
            g.Children.Add(right);
            card.Child = g;
            return card;
        }

        void Save()
        {
            foreach (var (e, get, _) in _editors)
            {
                var v = get();
                if (!Valid(e, v))
                {
                    MessageBox.Show(this, $"{e.Section} / {e.Key}: \"{v}\" isn't a valid {e.Type}.", Title, MessageBoxButton.OK, MessageBoxImage.Warning);
                    return;
                }
            }
            foreach (var (e, get, _) in _editors)
                if (get() != e.Value) _cfg.Set(e, get());
            try { _cfg.Save(); }
            catch (Exception ex) { MessageBox.Show(this, ex.Message, Title); return; }
            DialogResult = true;
        }

        static bool Valid(CfgEntry e, string v)
        {
            switch ((e.Type ?? "").ToLowerInvariant())
            {
                case "single":
                case "double": return double.TryParse(v, NumberStyles.Float, CultureInfo.InvariantCulture, out _);
                case "int32":
                case "int64":
                case "int16": return long.TryParse(v, NumberStyles.Integer, CultureInfo.InvariantCulture, out _);
                case "uint32":
                case "uint64": return ulong.TryParse(v, NumberStyles.Integer, CultureInfo.InvariantCulture, out _);
                case "boolean": return v == "true" || v == "false";
                default: return !v.Contains('\n');
            }
        }
    }

    // ---------------------------------------------------------------------------------------------
    public class NewPluginWindow : Window
    {
        public string CreatedDir;
        readonly TextBox _name = new TextBox(), _author = new TextBox(), _guid = new TextBox(), _dir = new TextBox();
        bool _guidEdited;

        public NewPluginWindow(string win64)
        {
            Ui.Theme(this, "New plugin project", 560, 520);
            ResizeMode = ResizeMode.NoResize;
            SizeToContent = SizeToContent.Height;
            var p = new StackPanel { Margin = new Thickness(20) };
            p.Children.Add(Ui.Text("New plugin project", "H2", 17));
            p.Children.Add(Ui.Text("Makes a folder with a starter plugin (C++), the GML SDK headers and a build.bat. " +
                                   "\"build.bat install\" builds it and copies it into this game. Needs Visual Studio 2022 or its Build Tools with C++.", "Dim", 12));

            void Field(string label, TextBox box, string hint)
            {
                p.Children.Add(new TextBlock { Text = label, Margin = new Thickness(0, 14, 0, 4) });
                p.Children.Add(box);
                if (hint != null) p.Children.Add(Ui.Text(hint, "Dim", 11));
            }
            Field("Name", _name, "Letters, digits and _ - the DLL and folder name, e.g. MyGunMod");
            Field("Author", _author, null);
            Field("GUID", _guid, "Unique, reverse-domain style: com.author.plugin");

            p.Children.Add(new TextBlock { Text = "Create in", Margin = new Thickness(0, 14, 0, 4) });
            var dirRow = new DockPanel();
            var browse = new Button { Content = "Browse…", Margin = new Thickness(6, 0, 0, 0) };
            browse.Click += (s, e) =>
            {
                using (var d = new System.Windows.Forms.FolderBrowserDialog { Description = "Where the plugin's folder goes", SelectedPath = _dir.Text })
                    if (d.ShowDialog() == System.Windows.Forms.DialogResult.OK) _dir.Text = d.SelectedPath;
            };
            DockPanel.SetDock(browse, Dock.Right);
            dirRow.Children.Add(browse);
            dirRow.Children.Add(_dir);
            p.Children.Add(dirRow);
            _dir.Text = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments), "GML Plugins");

            _author.Text = Environment.UserName;
            _name.TextChanged += (s, e) => AutoGuid();
            _author.TextChanged += (s, e) => AutoGuid();
            _guid.TextChanged += (s, e) => { if (_guid.IsKeyboardFocused) _guidEdited = true; };

            var buttons = new StackPanel { Orientation = Orientation.Horizontal, HorizontalAlignment = HorizontalAlignment.Right, Margin = new Thickness(0, 22, 0, 0) };
            var cancel = new Button { Content = "Cancel", IsCancel = true };
            var create = new Button { Content = "Create", Style = Ui.S("AccentButton"), IsDefault = true, Margin = new Thickness(0) };
            create.Click += (s, e) =>
            {
                var name = _name.Text.Trim();
                var guid = _guid.Text.Trim();
                if (!PluginTemplate.ValidName(name)) { MessageBox.Show(this, "The name must start with a letter and hold only letters, digits and _.", Title); return; }
                if (!PluginTemplate.ValidGuid(guid)) { MessageBox.Show(this, "The GUID must look like com.author.plugin (lower case, at least three parts).", Title); return; }
                try
                {
                    CreatedDir = PluginTemplate.Create(_dir.Text.Trim(), name, guid, _author.Text.Trim(), win64 ?? @"C:\Program Files (x86)\Steam\steamapps\common\GERONIMO\Geronimo\Binaries\Win64");
                    DialogResult = true;
                }
                catch (Exception ex) { MessageBox.Show(this, ex.Message, Title); }
            };
            buttons.Children.Add(cancel);
            buttons.Children.Add(create);
            p.Children.Add(buttons);
            Content = p;
            AutoGuid();
            Loaded += (s, e) => _name.Focus();
        }

        void AutoGuid()
        {
            if (_guidEdited) return;
            string Part(string s) => Regex.Replace((s ?? "").ToLowerInvariant(), "[^a-z0-9_]", "");
            var a = Part(_author.Text);
            _guid.Text = $"com.{(a.Length > 0 ? a : "you")}.{Part(_name.Text)}";
        }
    }
}
