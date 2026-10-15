// Finds Geronimo's Binaries\Win64 folder through Steam: the registry gives Steam's folder,
// libraryfolders.vdf lists every library, and appmanifest_2019620.acf marks the one with the game.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using Microsoft.Win32;

namespace GMLManager.Core
{
    public static class GameLocator
    {
        public const string AppId = "2019620";
        public const string ExeName = "Geronimo-Win64-Shipping.exe";

        public static bool IsGameDir(string dir) =>
            !string.IsNullOrEmpty(dir) && File.Exists(Path.Combine(dir, ExeName));

        // Accepts the game's root, its Geronimo folder, or Binaries\Win64 itself.
        public static string Normalize(string dir)
        {
            if (string.IsNullOrEmpty(dir)) return null;
            foreach (var c in new[] { dir, Path.Combine(dir, "Binaries", "Win64"), Path.Combine(dir, "Geronimo", "Binaries", "Win64") })
                if (IsGameDir(c)) return Path.GetFullPath(c);
            var parent = Path.GetDirectoryName(dir);
            return parent != null && IsGameDir(parent) ? parent : null;
        }

        public static string Find()
        {
            foreach (var lib in SteamLibraries())
            {
                try
                {
                    var acf = Path.Combine(lib, "steamapps", $"appmanifest_{AppId}.acf");
                    string installDir = "GERONIMO";
                    if (File.Exists(acf))
                    {
                        var m = Regex.Match(File.ReadAllText(acf), "\"installdir\"\\s+\"([^\"]+)\"");
                        if (m.Success) installDir = m.Groups[1].Value;
                    }
                    var win64 = Path.Combine(lib, "steamapps", "common", installDir, "Geronimo", "Binaries", "Win64");
                    if (IsGameDir(win64)) return win64;
                }
                catch { }
            }
            return null;
        }

        static IEnumerable<string> SteamLibraries()
        {
            var roots = new List<string>();
            void Add(string p) { if (!string.IsNullOrEmpty(p) && Directory.Exists(p)) roots.Add(Path.GetFullPath(p.Replace('/', '\\'))); }
            try { Add(Registry.GetValue(@"HKEY_CURRENT_USER\Software\Valve\Steam", "SteamPath", null) as string); } catch { }
            try { Add(Registry.GetValue(@"HKEY_LOCAL_MACHINE\SOFTWARE\WOW6432Node\Valve\Steam", "InstallPath", null) as string); } catch { }
            Add(@"C:\Program Files (x86)\Steam");
            var libs = new List<string>();
            foreach (var root in roots.Distinct(StringComparer.OrdinalIgnoreCase))
            {
                libs.Add(root);
                try
                {
                    var vdf = Path.Combine(root, "steamapps", "libraryfolders.vdf");
                    if (File.Exists(vdf))
                        foreach (Match m in Regex.Matches(File.ReadAllText(vdf), "\"path\"\\s+\"([^\"]+)\""))
                            libs.Add(m.Groups[1].Value.Replace(@"\\", @"\"));
                }
                catch { }
            }
            return libs.Distinct(StringComparer.OrdinalIgnoreCase);
        }

        public static bool IsGameRunning()
        {
            foreach (var n in new[] { "Geronimo-Win64-Shipping", "Geronimo" })
            {
                var ps = Process.GetProcessesByName(n);
                bool any = ps.Length > 0;
                foreach (var p in ps) p.Dispose();
                if (any) return true;
            }
            return false;
        }

        public static void Launch(string extraArgs)
        {
            if (string.IsNullOrWhiteSpace(extraArgs))
            {
                Process.Start(new ProcessStartInfo($"steam://rungameid/{AppId}") { UseShellExecute = true });
                return;
            }
            // Arguments only reach the game through steam.exe -applaunch (Steam asks the user once).
            string steam = null;
            try { steam = Registry.GetValue(@"HKEY_CURRENT_USER\Software\Valve\Steam", "SteamExe", null) as string; } catch { }
            if (string.IsNullOrEmpty(steam) || !File.Exists(steam))
            {
                Process.Start(new ProcessStartInfo($"steam://rungameid/{AppId}") { UseShellExecute = true });
                return;
            }
            Process.Start(new ProcessStartInfo(steam, $"-applaunch {AppId} {extraArgs}") { UseShellExecute = false });
        }
    }
}
