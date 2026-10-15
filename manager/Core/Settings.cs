// The manager's own settings: %LOCALAPPDATA%\GMLManager\settings.json
using System;
using System.Collections.Generic;
using System.IO;

namespace GMLManager.Core
{
    public class Settings
    {
        // GMLMANAGER_HOME moves settings, cache and downloads elsewhere (tests, portable use)
        public static readonly string Dir = !string.IsNullOrEmpty(Environment.GetEnvironmentVariable("GMLMANAGER_HOME"))
            ? Environment.GetEnvironmentVariable("GMLMANAGER_HOME")
            : Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "GMLManager");
        static string FilePath => Path.Combine(Dir, "settings.json");

        public string GameDir;                 // ...\Geronimo\Binaries\Win64
        public bool AutoUpdate = true;         // update the loader and manager-installed mods at start
        public bool UpdateSelf = true;         // update GML Manager itself
        public bool Prereleases = false;
        public string CatalogUrl = "";         // "" = the official catalog
        public string LaunchArgs = "";         // passed to the game through Steam
        public bool CloseOnPlay = false;

        public static Settings Load()
        {
            var s = new Settings();
            try
            {
                if (!File.Exists(FilePath)) return s;
                var o = Json.Parse(File.ReadAllText(FilePath));
                s.GameDir = o.S("gameDir");
                s.AutoUpdate = o.B("autoUpdate", true);
                s.UpdateSelf = o.B("updateSelf", true);
                s.Prereleases = o.B("prereleases");
                s.CatalogUrl = o.S("catalogUrl") ?? "";
                s.LaunchArgs = o.S("launchArgs") ?? "";
                s.CloseOnPlay = o.B("closeOnPlay");
            }
            catch { }
            return s;
        }

        public void Save()
        {
            Directory.CreateDirectory(Dir);
            File.WriteAllText(FilePath, Json.Write(new Dictionary<string, object>
            {
                ["gameDir"] = GameDir, ["autoUpdate"] = AutoUpdate, ["updateSelf"] = UpdateSelf,
                ["prereleases"] = Prereleases, ["catalogUrl"] = CatalogUrl, ["launchArgs"] = LaunchArgs,
                ["closeOnPlay"] = CloseOnPlay
            }));
        }
    }
}
