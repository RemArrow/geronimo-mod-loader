using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Threading;
using GMLManager.Core;
using Microsoft.Win32;

namespace GMLManager
{
    public partial class MainWindow : Window
    {
        readonly Settings _s = Settings.Load();
        readonly ObservableCollection<PluginRow> _plugins = new ObservableCollection<PluginRow>();
        readonly ObservableCollection<CatalogRow> _catalog = new ObservableCollection<CatalogRow>();
        List<CatalogMod> _mods = Catalog.BuiltIn();
        readonly Dictionary<string, Release> _latest = new Dictionary<string, Release>(StringComparer.OrdinalIgnoreCase);
        readonly Dictionary<string, string> _latestError = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        Release _loaderRelease;
        string _onlineError;
        LoaderState _loader = new LoaderState();
        bool _busy, _gameRunning, _loadingUi;
        readonly DispatcherTimer _timer = new DispatcherTimer { Interval = TimeSpan.FromSeconds(1) };
        Action _bannerAction;

        string Win64 => _s.GameDir;
        bool HasGame => GameLocator.IsGameDir(Win64);

        public MainWindow()
        {
            InitializeComponent();
            PluginList.ItemsSource = _plugins;
            CatalogList.ItemsSource = _catalog;
            Loaded += async (s, e) => await Startup();
            _timer.Tick += (s, e) => Tick();
        }

        // ================================================================== startup

        bool _started;

        async Task Startup()
        {
            if (_started) return;
            _started = true;
            Status($"GML Manager {App.Version} started");
            _loadingUi = true;
            if (!HasGame)
            {
                var found = GameLocator.Find();
                if (found != null) { _s.GameDir = found; _s.Save(); }
            }
            AutoUpdate.IsChecked = _s.AutoUpdate;
            UpdateSelf.IsChecked = _s.UpdateSelf;
            Prereleases.IsChecked = _s.Prereleases;
            CloseOnPlay.IsChecked = _s.CloseOnPlay;
            CatalogUrlBox.Text = _s.CatalogUrl;
            var args = _s.LaunchArgs ?? "";
            FlatMode.IsChecked = Regex.IsMatch(args, @"(^|\s)-nohmd(\s|$)");
            LaunchArgs.Text = Regex.Replace(args, @"(^|\s)-nohmd(?=\s|$)", "").Trim();
            AboutText.Text = $"GML Manager {App.Version}";
            _loadingUi = false;

            _gameRunning = GameLocator.IsGameRunning();
            RefreshLocal();
            _timer.Start();
            if (Environment.GetCommandLineArgs().Contains("--updated")) Status($"GML Manager was updated to {App.Version}.");
            await CheckOnline(startup: true);
        }

        // ================================================================== local state -> UI

        void RefreshLocal()
        {
            _loader = HasGame ? Installer.Loader(Win64) : new LoaderState();
            GameText.Text = HasGame ? Win64 : "Geronimo not found";
            GameDirBox.Text = Win64 ?? "";

            // loader header
            if (!HasGame) { LoaderText.Text = ""; LoaderSub.Text = ""; LoaderButton.Visibility = Visibility.Collapsed; }
            else if (!_loader.Installed)
            {
                LoaderText.Text = "Loader not installed";
                LoaderSub.Text = _loaderRelease != null ? $"GML {_loaderRelease.Version} available" : "";
                LoaderButton.Content = "Install GML";
                LoaderButton.Visibility = Visibility.Visible;
            }
            else
            {
                LoaderText.Text = "GML " + (_loader.Version ?? "(unknown version)");
                bool newer = LoaderHasUpdate();
                LoaderSub.Text = _loader.DevBuild ? "your own build" + (newer ? $" · {_loaderRelease.Version} released" : "")
                               : newer ? $"{_loaderRelease.Version} available"
                               : _loaderRelease != null ? "up to date" : "";
                LoaderButton.Content = $"Update to {_loaderRelease?.Version}";
                LoaderButton.Visibility = newer ? Visibility.Visible : Visibility.Collapsed;
            }
            _loadingUi = true;
            ModsSwitch.IsChecked = _loader.Installed && _loader.Enabled;
            ModsSwitch.IsEnabled = _loader.Installed;
            _loadingUi = false;

            // plugins
            var infos = HasGame ? PluginInfo.Scan(Win64) : new List<PluginInfo>();
            var manifest = HasGame ? Manifest.Load(Win64) : new Manifest();
            var byGuid = infos.Where(p => !p.ContentOnly).GroupBy(p => p.Guid).ToDictionary(g => g.Key, g => g.ToList());
            var rows = new List<PluginRow>();
            foreach (var p in infos)
            {
                var row = new PluginRow { Info = p, RelDir = p.Dir.Length > Win64.Length ? p.Dir.Substring(Win64.Length + 1) : p.Dir };
                row.Catalog = _mods.FirstOrDefault(m => string.Equals(m.Guid, p.Guid, StringComparison.OrdinalIgnoreCase));
                if (row.Catalog != null) _latest.TryGetValue(row.Catalog.Guid, out row.Latest);
                row.Managed = manifest.Mods.ContainsKey(p.Guid);
                row.SettingsVisible = !p.ContentOnly && File.Exists(Installer.ConfigPath(Win64, p.Guid)) ? Visibility.Visible : Visibility.Collapsed;
                if (!p.Disabled && !p.ContentOnly)
                {
                    foreach (var d in p.Dependencies.Where(d => !d.Soft))
                    {
                        var dep = byGuid.TryGetValue(d.Guid, out var l) ? l.Where(x => !x.Disabled).OrderByDescending(x => x.Version, Comparer<string>.Create(Versions.Compare)).FirstOrDefault() : null;
                        var known = _mods.FirstOrDefault(m => m.Guid == d.Guid);
                        var what = known?.Name ?? d.Guid;
                        if (dep == null) row.Problems.Add($"Needs {what}{(string.IsNullOrEmpty(d.MinVersion) ? "" : " " + d.MinVersion + " or newer")}, which isn't installed or is off.");
                        else if (!string.IsNullOrEmpty(d.MinVersion) && Versions.Compare(dep.Version, d.MinVersion) < 0)
                            row.Problems.Add($"Needs {what} {d.MinVersion} or newer (installed: {dep.Version}).");
                    }
                    foreach (var g in p.Incompatibilities.Where(g => byGuid.ContainsKey(g) && byGuid[g].Any(x => !x.Disabled)))
                        row.Problems.Add($"Doesn't work together with {byGuid[g][0].Name}: GML skips it while that is on.");
                    if (byGuid.TryGetValue(p.Guid, out var same) && same.Count(x => !x.Disabled) > 1)
                        row.Problems.Add("Installed twice: GML loads only the newest copy. Remove the other one: " +
                                         string.Join(", ", same.Where(x => x.Path != p.Path).Select(x => x.Path.Substring(Win64.Length + 1))));
                    if (row.Catalog?.MinGml != null && _loader.Version != null && Versions.Compare(_loader.Version, row.Catalog.MinGml) < 0)
                        row.Problems.Add($"Needs GML {row.Catalog.MinGml} or newer: update the loader.");
                    if (p.ApiVersion > 2 && _loader.Version != null && Versions.Compare(_loader.Version, "3.0.0") < 0)
                        row.Problems.Add($"Built for a newer GML (API v{p.ApiVersion}): update the loader.");
                }
                rows.Add(row);
            }
            _plugins.Clear();
            foreach (var r in rows.OrderBy(r => r.Info.IsPatcher && !r.Info.IsPlugin).ThenBy(r => r.Name, StringComparer.OrdinalIgnoreCase)) _plugins.Add(r);
            NoPlugins.Visibility = HasGame && _plugins.Count == 0 ? Visibility.Visible : Visibility.Collapsed;

            // catalog
            _catalog.Clear();
            foreach (var m in _mods)
            {
                var row = new CatalogRow { Mod = m };
                _latest.TryGetValue(m.Guid, out row.Latest);
                _latestError.TryGetValue(m.Guid, out row.Error);
                if (row.Latest == null && row.Error == null && _onlineError != null) row.Error = "couldn't check";
                row.Installed = infos.Where(p => string.Equals(p.Guid, m.Guid, StringComparison.OrdinalIgnoreCase))
                                     .OrderByDescending(p => p.Version, Comparer<string>.Create(Versions.Compare)).FirstOrDefault();
                _catalog.Add(row);
            }
            CatalogSource.Text = $"{_mods.Count} mod(s) · catalog: {Catalog.Source}";

            RefreshGmlCfg();
            RefreshBanner();
            RefreshPlay();
        }

        bool LoaderHasUpdate() => _loader.Installed && _loaderRelease != null && _loader.Version != null &&
                                  Versions.Compare(_loaderRelease.Version, _loader.Version) > 0;

        bool SelfHasUpdate() => _loaderRelease?.FindAsset(Catalog.ManagerAsset) != null && Versions.Compare(_loaderRelease.Version, App.Version) > 0;

        void RefreshBanner()
        {
            string text = null, button = null;
            _bannerAction = null;
            if (!HasGame)
            {
                text = "Couldn't find Geronimo. Show GML Manager where it's installed.";
                button = "Browse…"; _bannerAction = BrowseGame;
            }
            else if (!_loader.Installed)
            {
                text = "The mod loader (GML) isn't installed in this game yet. Mods need it.";
                button = "Install GML"; _bannerAction = () => _ = InstallLoader();
            }
            else if (!_loader.Enabled)
            {
                text = "Mods are switched off: the game starts without GML and every mod.";
                button = "Switch on"; _bannerAction = () => SetModsOn(true);
            }
            else if (!_s.UpdateSelf && SelfHasUpdate())
            {
                text = $"GML Manager {_loaderRelease.Version} is out (you have {App.Version}).";
                button = "Update"; _bannerAction = () => _ = UpdateManager();
            }
            Banner.Visibility = text != null ? Visibility.Visible : Visibility.Collapsed;
            BannerText.Text = text ?? "";
            BannerButton.Content = button;
        }

        void RefreshPlay()
        {
            PlayButton.IsEnabled = HasGame && !_gameRunning;
            PlayText.Text = _gameRunning ? "RUNNING" : "▶  PLAY";
        }

        // ================================================================== online

        async Task CheckOnline(bool startup)
        {
            Status("Checking for updates…");
            _onlineError = null;
            _latestError.Clear();
            try
            {
                _mods = await Catalog.Load(_s.CatalogUrl);
                try { _loaderRelease = await Updater.LatestLoader(_s.Prereleases); }
                catch (GitHubException e) { _onlineError = e.Message; }
                var tasks = _mods.Select(async m =>
                {
                    try { var r = await GitHub.Latest(m.Repo, _s.Prereleases); lock (_latest) { if (r != null) _latest[m.Guid] = r; else _latestError[m.Guid] = "no releases yet"; } }
                    catch (GitHubException e) { lock (_latest) _latestError[m.Guid] = e.Message.StartsWith("Not found") ? "repo not found" : "couldn't check"; if (_onlineError == null) _onlineError = e.Message; }
                }).ToList();
                await Task.WhenAll(tasks);
            }
            catch (Exception e) { _onlineError = e.Message; }
            RefreshLocal();
            int updates = _plugins.Count(p => p.HasUpdate) + (LoaderHasUpdate() ? 1 : 0);
            Status(_onlineError != null ? "Couldn't check everything: " + _onlineError
                   : updates > 0 ? $"{updates} update(s) available." : "Everything is up to date.");
            if (startup) await AutoUpdateAll();
        }

        async Task AutoUpdateAll()
        {
            if (_s.UpdateSelf && SelfHasUpdate())
            {
                if (await UpdateManager()) return;
            }
            if (!_s.AutoUpdate || !HasGame || _gameRunning) return;
            var done = new List<string>();
            if (LoaderHasUpdate() && !_loader.DevBuild)
            {
                var v = _loaderRelease.Version;
                if (await Run($"Updating GML to {v}…", p => Updater.InstallLoader(Win64, _loaderRelease, p))) done.Add("GML " + v);
            }
            var manifest = Manifest.Load(Win64);
            foreach (var row in _plugins.Where(r => r.HasUpdate && r.Managed && r.Catalog != null).ToList())
            {
                var v = row.Latest.Version;
                if (await Run($"Updating {row.Name} to {v}…", p => Updater.InstallMod(Win64, row.Catalog, row.Latest, p))) done.Add($"{row.Name} {v}");
            }
            if (done.Count > 0) Status("Updated: " + string.Join(", ", done));
        }

        async Task<bool> UpdateManager()
        {
            bool restart = false;
            await Run($"Updating GML Manager to {_loaderRelease.Version}…", async p => restart = await Updater.UpdateSelf(_loaderRelease, p));
            if (restart) Application.Current.Shutdown();
            return restart;
        }

        // ================================================================== busy work

        async Task<bool> Run(string what, Func<IProgress<double>, Task> work)
        {
            if (_busy) { Status("Busy - wait for the current step to finish."); return false; }
            _busy = true;
            Status(what);
            Progress.Value = 0;
            Progress.IsIndeterminate = true;
            Progress.Visibility = Visibility.Visible;
            var progress = new Progress<double>(v => { Progress.IsIndeterminate = false; Progress.Value = v; });
            bool ok = false;
            try
            {
                await work(progress);
                ok = true;
                Status(what.TrimEnd('…', '.') + " - done.");
            }
            catch (Exception e)
            {
                Status("Failed: " + e.Message);
                MessageBox.Show(this, e.Message, what.TrimEnd('…'), MessageBoxButton.OK, MessageBoxImage.Warning);
            }
            finally
            {
                Progress.Visibility = Visibility.Collapsed;
                _busy = false;
                RefreshLocal();
            }
            return ok;
        }

        void Status(string s)
        {
            StatusText.Text = s;
            try
            {
                Directory.CreateDirectory(Settings.Dir);
                File.AppendAllText(Path.Combine(Settings.Dir, "manager.log"), $"{DateTime.Now:yyyy-MM-dd HH:mm:ss} {s}\r\n");
            }
            catch { }
        }

        bool CheckGameClosed()
        {
            if (!_gameRunning) return true;
            MessageBox.Show(this, "Close Geronimo first: its mod files are in use while it runs.", "GML Manager", MessageBoxButton.OK, MessageBoxImage.Information);
            return false;
        }

        // every second: game running? and the Developer tab's log
        void Tick()
        {
            bool running = GameLocator.IsGameRunning();
            if (running != _gameRunning)
            {
                _gameRunning = running;
                RefreshPlay();
                if (!running) RefreshLocal();   // the game may have written configs and logs
            }
            if (Tabs.SelectedIndex == 2) RefreshLog(false);
        }

        // ================================================================== header

        async Task InstallLoader()
        {
            if (!HasGame || !CheckGameClosed()) return;
            if (_loaderRelease == null) await CheckOnline(false);
            if (_loaderRelease == null) { MessageBox.Show(this, "Couldn't reach GitHub to download GML. " + _onlineError, "Install GML"); return; }
            await Run($"Installing GML {_loaderRelease.Version}…", p => Updater.InstallLoader(Win64, _loaderRelease, p));
        }

        async void OnLoaderButton(object sender, RoutedEventArgs e)
        {
            if (_loader.Installed && _loader.DevBuild &&
                MessageBox.Show(this, $"The installed loader is your own build (GML.pdb is next to it). Replace it with the released GML {_loaderRelease?.Version}?",
                                "Update GML", MessageBoxButton.YesNo, MessageBoxImage.Question) != MessageBoxResult.Yes) return;
            await InstallLoader();
        }

        void OnModsSwitch(object sender, RoutedEventArgs e)
        {
            if (_loadingUi) return;
            SetModsOn(ModsSwitch.IsChecked == true);
        }

        void SetModsOn(bool on)
        {
            if (!HasGame || !_loader.Installed) return;
            try
            {
                Installer.SetLoaderEnabled(Win64, on);
                Status(on ? "Mods on: GML loads with the game." : "Mods off: the game starts without GML (takes effect at the next launch).");
            }
            catch (Exception ex) { MessageBox.Show(this, ex.Message, "GML Manager"); }
            RefreshLocal();
        }

        async void OnPlay(object sender, RoutedEventArgs e)
        {
            if (!HasGame) return;
            if (_busy) { Status("Wait for the update to finish."); return; }
            if (_loader.Installed && _loader.Enabled)
            {
                var broken = _plugins.Where(p => p.Enabled && p.Problems.Count > 0).Select(p => p.Name).ToList();
                if (broken.Count > 0) Status("Note: " + string.Join(", ", broken) + " may not load - see the Mods tab.");
            }
            try
            {
                GameLocator.Launch(_s.LaunchArgs);
                _gameRunning = true;
                RefreshPlay();
                if (!(StatusText.Text ?? "").StartsWith("Note:")) Status("Starting Geronimo through Steam…");
                if (_s.CloseOnPlay) { await Task.Delay(1500); Close(); }
            }
            catch (Exception ex) { MessageBox.Show(this, "Couldn't start the game through Steam: " + ex.Message, "Play"); }
        }

        void OnBannerButton(object sender, RoutedEventArgs e) => _bannerAction?.Invoke();

        // ================================================================== Mods tab

        static PluginRow RowOf(object sender) => (sender as FrameworkElement)?.Tag as PluginRow;

        void OnPluginSwitch(object sender, RoutedEventArgs e)
        {
            var row = RowOf(sender);
            if (row == null) return;
            try
            {
                Installer.SetEnabled(Win64, row.Info, ((CheckBox)sender).IsChecked == true);
                Status($"{row.Name} {(row.Info.Disabled ? "on" : "off")} from the next launch.");
            }
            catch (Exception ex) { MessageBox.Show(this, ex.Message, row.Name); }
            RefreshLocal();
        }

        async void OnPluginUpdate(object sender, RoutedEventArgs e)
        {
            var row = RowOf(sender);
            if (row?.Catalog == null || row.Latest == null || !CheckGameClosed()) return;
            if (!row.Managed && File.Exists(Path.ChangeExtension(row.Info.Path, ".pdb")) &&
                MessageBox.Show(this, $"{row.Name} {row.Info.Version} looks like your own build (a .pdb is next to it). Replace it with the released {row.Latest.Version}?",
                                row.Name, MessageBoxButton.YesNo, MessageBoxImage.Question) != MessageBoxResult.Yes) return;
            await Run($"Updating {row.Name} to {row.Latest.Version}…", p => Updater.InstallMod(Win64, row.Catalog, row.Latest, p));
        }

        void OnPluginSettings(object sender, RoutedEventArgs e)
        {
            var row = RowOf(sender);
            if (row == null) return;
            var path = Installer.ConfigPath(Win64, row.Info.Guid);
            if (!File.Exists(path)) return;
            new CfgWindow(path, row.Name, _gameRunning) { Owner = this }.ShowDialog();
        }

        void OnPluginFolder(object sender, RoutedEventArgs e)
        {
            var row = RowOf(sender);
            if (row == null) return;
            if (File.Exists(row.Info.Path)) Process.Start("explorer.exe", $"/select,\"{row.Info.Path}\"");
            else Open(row.Info.Dir);
        }

        void OnPluginRemove(object sender, RoutedEventArgs e)
        {
            var row = RowOf(sender);
            if (row == null || !CheckGameClosed()) return;
            var folder = Installer.OwnFolder(Win64, row.Info);
            var what = folder != null ? $"its folder ({folder.Substring(Win64.Length + 1)})" : Path.GetFileName(row.Info.Path);
            if (MessageBox.Show(this, $"Remove {row.Name}?\n\nThis moves {what} to the Recycle Bin. Its settings file stays, so reinstalling keeps them.",
                                "Remove mod", MessageBoxButton.YesNo, MessageBoxImage.Question) != MessageBoxResult.Yes) return;
            try { Installer.Remove(Win64, row.Info); Status($"{row.Name} removed (in the Recycle Bin)."); }
            catch (Exception ex) { MessageBox.Show(this, ex.Message, "Remove mod"); }
            RefreshLocal();
        }

        async void OnCheckUpdates(object sender, RoutedEventArgs e)
        {
            if (_busy) return;
            GitHub.ClearCache();
            await CheckOnline(false);
        }

        void OnOpenPlugins(object sender, RoutedEventArgs e) => OpenGameFolder(@"GML\plugins");

        void OnInstallFile(object sender, RoutedEventArgs e)
        {
            var dlg = new OpenFileDialog { Filter = "Mods (*.zip, *.dll)|*.zip;*.dll", Multiselect = true, Title = "Install mods" };
            if (dlg.ShowDialog(this) == true) _ = InstallFiles(dlg.FileNames);
        }

        void OnDragOver(object sender, DragEventArgs e)
        {
            e.Effects = e.Data.GetDataPresent(DataFormats.FileDrop) ? DragDropEffects.Copy : DragDropEffects.None;
            e.Handled = true;
        }

        void OnDrop(object sender, DragEventArgs e)
        {
            if (e.Data.GetData(DataFormats.FileDrop) is string[] files) _ = InstallFiles(files);
        }

        async Task InstallFiles(string[] files)
        {
            if (!HasGame) { MessageBox.Show(this, "Set the game folder first (Settings tab).", "Install"); return; }
            if (!CheckGameClosed()) return;
            var ok = files.Where(f => f.EndsWith(".zip", StringComparison.OrdinalIgnoreCase) || f.EndsWith(".dll", StringComparison.OrdinalIgnoreCase)).ToList();
            if (ok.Count == 0) { MessageBox.Show(this, "Drop a mod's .zip or a plugin .dll.", "Install"); return; }
            var names = new List<string>();
            await Run($"Installing {string.Join(", ", ok.Select(Path.GetFileName))}…", p => Task.Run(() =>
            {
                foreach (var f in ok)
                {
                    var res = f.EndsWith(".dll", StringComparison.OrdinalIgnoreCase) ? Installer.InstallDll(Win64, f) : Installer.InstallZip(Win64, f);
                    if (res.HasLoader)
                    {
                        var v = Installer.Loader(Win64).Version;
                        Installer.Commit(Win64, res, null, v, loader: true);
                        names.Add("GML " + v);
                    }
                    if (res.Plugins.Count > 0)
                    {
                        Installer.Commit(Win64, res, null, null, loader: false);
                        names.AddRange(res.Plugins.Select(x => $"{x.Name} {x.Version}"));
                    }
                    else if (!res.HasLoader)
                        names.Add(Path.GetFileNameWithoutExtension(f) + " (no plugin DLL inside: content only)");
                }
            }));
            if (names.Count > 0) Status("Installed: " + string.Join(", ", names));
        }

        // ================================================================== Browse tab

        async void OnCatalogInstall(object sender, RoutedEventArgs e)
        {
            var row = (sender as FrameworkElement)?.Tag as CatalogRow;
            if (row?.Latest == null || !HasGame || !CheckGameClosed()) return;
            if (!_loader.Installed)
            {
                if (MessageBox.Show(this, $"{row.Name} needs the mod loader (GML), which isn't installed yet. Install it too?",
                                    row.Name, MessageBoxButton.YesNo, MessageBoxImage.Question) != MessageBoxResult.Yes) return;
                await InstallLoader();
                if (!_loader.Installed) return;
            }
            else if (row.Mod.MinGml != null && _loader.Version != null && Versions.Compare(_loader.Version, row.Mod.MinGml) < 0)
            {
                if (MessageBox.Show(this, $"{row.Name} needs GML {row.Mod.MinGml} or newer (you have {_loader.Version}). Update the loader first?",
                                    row.Name, MessageBoxButton.YesNo, MessageBoxImage.Question) == MessageBoxResult.Yes)
                    await InstallLoader();
            }
            await Run($"Installing {row.Name} {row.Latest.Version}…", p => Updater.InstallMod(Win64, row.Mod, row.Latest, p));
        }

        void OnCatalogPage(object sender, RoutedEventArgs e)
        {
            if ((sender as FrameworkElement)?.Tag is CatalogRow row) Url(row.Mod.Page);
        }

        // ================================================================== Developer tab

        long _logStamp;
        string _logText = "";

        void OnTabChanged(object sender, SelectionChangedEventArgs e)
        {
            if (e.Source == Tabs && Tabs.SelectedIndex == 2) RefreshLog(true);
        }

        void RefreshLog(bool force)
        {
            if (!HasGame) return;
            var path = Path.Combine(Win64, "GML", "LogOutput.log");
            if (!File.Exists(path)) { LogBox.Text = "No log yet: GML writes GML\\LogOutput.log when the game starts."; return; }
            var fi = new FileInfo(path);
            long stamp = fi.LastWriteTimeUtc.Ticks ^ fi.Length;
            if (!force && stamp == _logStamp) return;
            _logStamp = stamp;
            try
            {
                using (var r = new StreamReader(new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete)))
                    _logText = r.ReadToEnd();
            }
            catch { return; }
            ApplyLogFilter();
        }

        void ApplyLogFilter()
        {
            IEnumerable<string> lines = _logText.Split('\n').Select(l => l.TrimEnd('\r'));
            if (LogProblems.IsChecked == true) lines = lines.Where(l => Regex.IsMatch(l, @"^\[(Warning|Error|Fatal)|exception|FAIL", RegexOptions.IgnoreCase));
            var f = LogFilter.Text;
            if (!string.IsNullOrEmpty(f)) lines = lines.Where(l => l.IndexOf(f, StringComparison.OrdinalIgnoreCase) >= 0);
            var list = lines.ToList();
            if (list.Count > 5000) list = list.Skip(list.Count - 5000).ToList();
            LogBox.Text = string.Join("\n", list);
            if (LogFollow.IsChecked == true) LogBox.ScrollToEnd();
        }

        void OnLogFilter(object sender, RoutedEventArgs e) => ApplyLogFilter();

        void OnOpenLog(object sender, RoutedEventArgs e)
        {
            var path = Path.Combine(Win64 ?? "", "GML", "LogOutput.log");
            if (File.Exists(path)) Process.Start(new ProcessStartInfo(path) { UseShellExecute = true });
        }

        string GmlCfgPath => Path.Combine(Win64 ?? "", "GML", "config", "GML.cfg");

        void RefreshGmlCfg()
        {
            bool has = HasGame && File.Exists(GmlCfgPath);
            GmlCfgNote.Visibility = has ? Visibility.Collapsed : Visibility.Visible;
            GmlSwitches.IsEnabled = has;
            if (!has) return;
            _loadingUi = true;
            try
            {
                var c = CfgFile.Load(GmlCfgPath);
                foreach (var cb in new[] { CfgConsole, CfgTimestamps, CfgTrace, CfgPaks, CfgMods })
                {
                    var k = ((string)cb.Tag).Split('|');
                    var en = c.Get(k[0], k[1]);
                    cb.IsEnabled = en != null;
                    cb.IsChecked = en != null && en.Value.Equals("true", StringComparison.OrdinalIgnoreCase);
                }
                var lvl = c.Get("Logging.Disk", "LogLevel")?.Value;
                CfgLevel.SelectedItem = CfgLevel.Items.Cast<ComboBoxItem>().FirstOrDefault(i => string.Equals((string)i.Content, lvl, StringComparison.OrdinalIgnoreCase));
            }
            catch { }
            finally { _loadingUi = false; }
        }

        void OnGmlCfg(object sender, RoutedEventArgs e)
        {
            var cb = (CheckBox)sender;
            var k = ((string)cb.Tag).Split('|');
            if (!CfgFile.TrySet(GmlCfgPath, k[0], k[1], cb.IsChecked == true ? "true" : "false"))
                MessageBox.Show(this, "GML.cfg doesn't have that setting yet: launch the game once with this GML.", "GML settings");
            else Status($"GML.cfg: [{k[0]}] {k[1]} = {(cb.IsChecked == true ? "true" : "false")} (from the next launch)");
            RefreshGmlCfg();
        }

        void OnGmlLevel(object sender, SelectionChangedEventArgs e)
        {
            if (_loadingUi || !(CfgLevel.SelectedItem is ComboBoxItem i)) return;
            if (CfgFile.TrySet(GmlCfgPath, "Logging.Disk", "LogLevel", (string)i.Content)) Status($"GML.cfg: LogLevel = {i.Content}");
        }

        void OnGmlAllSettings(object sender, RoutedEventArgs e)
        {
            if (File.Exists(GmlCfgPath)) new CfgWindow(GmlCfgPath, "Geronimo Mod Loader", _gameRunning) { Owner = this }.ShowDialog();
            RefreshGmlCfg();
        }

        void SaveLaunchArgs()
        {
            if (_loadingUi) return;
            var extra = (LaunchArgs.Text ?? "").Trim();
            _s.LaunchArgs = ((FlatMode.IsChecked == true ? "-nohmd " : "") + extra).Trim();
            _s.Save();
        }

        void OnFlatMode(object sender, RoutedEventArgs e) => SaveLaunchArgs();
        void OnLaunchArgs(object sender, RoutedEventArgs e) => SaveLaunchArgs();

        void OnNewPlugin(object sender, RoutedEventArgs e)
        {
            var dlg = new NewPluginWindow(Win64) { Owner = this };
            if (dlg.ShowDialog() == true && dlg.CreatedDir != null)
            {
                Status("Plugin project created: " + dlg.CreatedDir);
                Process.Start("explorer.exe", $"\"{dlg.CreatedDir}\"");
            }
        }

        void OnDocs(object sender, RoutedEventArgs e) => Url("https://github.com/RemArrow/geronimo-mod-loader#writing-a-plugin");

        void OnOpenFolder(object sender, RoutedEventArgs e) => OpenGameFolder((string)((FrameworkElement)sender).Tag);

        // ================================================================== Settings tab

        void OnSettingChanged(object sender, RoutedEventArgs e)
        {
            if (_loadingUi) return;
            _s.AutoUpdate = AutoUpdate.IsChecked == true;
            _s.UpdateSelf = UpdateSelf.IsChecked == true;
            bool pre = _s.Prereleases;
            _s.Prereleases = Prereleases.IsChecked == true;
            _s.CloseOnPlay = CloseOnPlay.IsChecked == true;
            _s.Save();
            if (pre != _s.Prereleases) OnCheckUpdates(null, null);
            RefreshBanner();
        }

        void OnCatalogUrl(object sender, RoutedEventArgs e)
        {
            var v = (CatalogUrlBox.Text ?? "").Trim();
            if (v == (_s.CatalogUrl ?? "")) return;
            _s.CatalogUrl = v;
            _s.Save();
            OnCheckUpdates(null, null);
        }

        void OnBrowseGame(object sender, RoutedEventArgs e) => BrowseGame();

        void BrowseGame()
        {
            var dlg = new OpenFileDialog
            {
                Title = "Find Geronimo: pick Geronimo.exe or Geronimo-Win64-Shipping.exe",
                Filter = "Geronimo|Geronimo.exe;Geronimo-Win64-Shipping.exe|Programs|*.exe"
            };
            if (dlg.ShowDialog(this) != true) return;
            var dir = GameLocator.Normalize(Path.GetDirectoryName(dlg.FileName));
            if (dir == null) { MessageBox.Show(this, "That isn't Geronimo's folder: Geronimo-Win64-Shipping.exe isn't there.", "Game folder"); return; }
            _s.GameDir = dir;
            _s.Save();
            RefreshLocal();
        }

        void OnFindGame(object sender, RoutedEventArgs e)
        {
            var found = GameLocator.Find();
            if (found == null) { MessageBox.Show(this, "Steam doesn't list Geronimo in any library. Use Browse.", "Game folder"); return; }
            _s.GameDir = found;
            _s.Save();
            RefreshLocal();
            Status("Found Geronimo: " + found);
        }

        async void OnReinstallLoader(object sender, RoutedEventArgs e)
        {
            if (_loader.DevBuild && MessageBox.Show(this, "The installed loader is your own build. Replace it with the latest release?",
                                                    "Reinstall GML", MessageBoxButton.YesNo) != MessageBoxResult.Yes) return;
            await InstallLoader();
        }

        void OnUninstallLoader(object sender, RoutedEventArgs e)
        {
            if (!HasGame || !_loader.Installed || !CheckGameClosed()) return;
            if (MessageBox.Show(this, "Uninstall the mod loader?\n\nThe game will start without any mods. Your mods and their settings stay in GML\\plugins and GML\\config, so reinstalling GML brings them back.",
                                "Uninstall GML", MessageBoxButton.YesNo, MessageBoxImage.Question) != MessageBoxResult.Yes) return;
            try { Installer.UninstallLoader(Win64); Status("GML uninstalled."); }
            catch (Exception ex) { MessageBox.Show(this, ex.Message, "Uninstall GML"); }
            RefreshLocal();
        }

        void OnClearCache(object sender, RoutedEventArgs e)
        {
            GitHub.ClearCache();
            Status("Update cache cleared.");
        }

        // ================================================================== helpers

        void OpenGameFolder(string rel)
        {
            if (!HasGame) return;
            var p = Path.Combine(Win64, rel ?? "");
            Directory.CreateDirectory(p);
            Open(p);
        }

        static void Open(string dir) { if (Directory.Exists(dir)) Process.Start("explorer.exe", $"\"{dir}\""); }

        static void Url(string url) { try { Process.Start(new ProcessStartInfo(url) { UseShellExecute = true }); } catch { } }
    }
}
