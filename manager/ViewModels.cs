using System.Collections.Generic;
using System.ComponentModel;
using System.Linq;
using System.Runtime.CompilerServices;
using System.Windows;
using GMLManager.Core;

namespace GMLManager
{
    public abstract class Notify : INotifyPropertyChanged
    {
        public event PropertyChangedEventHandler PropertyChanged;
        protected void Raise([CallerMemberName] string n = null) => PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(n));
        public void RaiseAll() => PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(null));
    }

    // One installed plugin, patcher or content folder (Mods tab).
    public class PluginRow : Notify
    {
        public PluginInfo Info;
        public CatalogMod Catalog;       // when the catalog knows its GUID
        public Release Latest;           // the catalog mod's newest release
        public bool Managed;             // installed by GML Manager (manifest)
        public bool Duplicate;           // another copy of the same GUID is installed
        public List<string> Problems = new List<string>();

        public string Name => Info.Name;
        public string Version => Info.ContentOnly ? "content" : Info.Version;
        public string Guid => Info.ContentOnly ? "content folder: " + RelDir : Info.Guid;
        public string RelDir { get; set; }
        public bool Enabled => !Info.Disabled;
        public string Kind => Info.ContentOnly ? "Content" : Info.IsPatcher && !Info.IsPlugin ? "Patcher" : null;
        public Visibility KindVisible => Kind != null ? Visibility.Visible : Visibility.Collapsed;
        public bool HasUpdate => Latest != null && !Info.ContentOnly && Versions.Compare(Latest.Version, Info.Version) > 0;
        public string UpdateText => HasUpdate ? $"Update to {Latest.Version}" : null;
        public Visibility UpdateVisible => HasUpdate ? Visibility.Visible : Visibility.Collapsed;
        public Visibility DisabledVisible => Info.Disabled ? Visibility.Visible : Visibility.Collapsed;
        public Visibility LocalVisible => !Managed && Catalog == null && !Info.ContentOnly ? Visibility.Visible : Visibility.Collapsed;
        public string ProblemText => string.Join("\n", Problems);
        public Visibility ProblemVisible => Problems.Count > 0 ? Visibility.Visible : Visibility.Collapsed;
        public Visibility SettingsVisible { get; set; }
        public double Opacity => Info.Disabled ? 0.55 : 1.0;
        public string Tooltip => $"{Info.Path}" + (Managed ? "\nInstalled by GML Manager: updates automatically." : Catalog != null ? "\nInstalled by hand: updates only when you click Update." : "");
    }

    // One catalog entry (Browse tab).
    public class CatalogRow : Notify
    {
        public CatalogMod Mod;
        public Release Latest;
        public string Error;
        public PluginInfo Installed;

        public string Name => Mod.Name;
        public string Author => string.IsNullOrEmpty(Mod.Author) ? "" : "by " + Mod.Author;
        public string Description => Mod.Description;
        public string Credits => Mod.Credits;
        public Visibility CreditsVisible => string.IsNullOrEmpty(Mod.Credits) ? Visibility.Collapsed : Visibility.Visible;
        public string Tags => string.Join("  ·  ", Mod.Tags);
        public string LatestText => Latest != null ? $"v{Latest.Version}" + (Latest.Published.Year > 2000 ? $"  ·  {Latest.Published:d MMM yyyy}" : "") : Error ?? "checking…";
        public bool CanInstall => Latest != null && (Installed == null || Versions.Compare(Latest.Version, Installed.Version) > 0);
        public string ActionText => Installed == null ? "Install" : $"Update to {Latest?.Version}";
        public bool IsInstalledCurrent => Installed != null && !CanInstall;
        public Visibility ButtonVisible => IsInstalledCurrent ? Visibility.Collapsed : Visibility.Visible;
        public Visibility InstalledVisible => IsInstalledCurrent ? Visibility.Visible : Visibility.Collapsed;
        public string InstalledText => Installed == null ? "" : $"installed: {Installed.Version}" + (Installed.Disabled ? " (off)" : "");
    }
}
