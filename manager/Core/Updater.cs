// Checking for and applying updates: the loader, catalog mods, and GML Manager itself.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Threading.Tasks;

namespace GMLManager.Core
{
    public static class Updater
    {
        static string Downloads => Path.Combine(Settings.Dir, "downloads");

        public static async Task<string> Fetch(ReleaseAsset a, IProgress<double> progress)
        {
            var dest = Path.Combine(Downloads, a.Name);
            await GitHub.Download(a.Url, dest, progress).ConfigureAwait(false);
            return dest;
        }

        public static Task<Release> LatestLoader(bool pre) => GitHub.Latest(Catalog.LoaderRepo, pre);

        public static async Task InstallLoader(string win64, Release r, IProgress<double> progress)
        {
            var asset = r.FindAsset(Catalog.LoaderAsset) ?? throw new GitHubException($"GML {r.Version} has no loader zip attached.");
            var zip = await Fetch(asset, progress).ConfigureAwait(false);
            Guard(win64);
            var res = Installer.InstallZip(win64, zip);
            if (!res.HasLoader) throw new InvalidDataException(asset.Name + " doesn't contain the loader.");
            Installer.Commit(win64, res, Catalog.LoaderRepo, r.Version, loader: true);
            TryDelete(zip);
        }

        public static async Task<InstallResult> InstallMod(string win64, CatalogMod m, Release r, IProgress<double> progress)
        {
            var asset = r.FindAsset(m.Asset) ?? throw new GitHubException($"{m.Name} {r.Version} has no download matching \"{m.Asset}\".");
            var zip = await Fetch(asset, progress).ConfigureAwait(false);
            Guard(win64);
            var res = Installer.InstallZip(win64, zip, m.Name);
            if (res.Plugins.Count == 0 && res.Files.Count == 0) throw new InvalidDataException(asset.Name + " is empty.");
            if (res.Plugins.Count == 0)
            {
                // content-only mod: record it under its guid all the same
                var m2 = Manifest.Load(win64);
                m2.Mods[m.Guid] = new ManifestEntry { Version = r.Version, Repo = m.Repo, Name = m.Name, Files = res.Files, Installed = DateTime.Now };
                m2.Save(win64);
            }
            else Installer.Commit(win64, res, m.Repo, r.Version, loader: false);
            TryDelete(zip);
            return res;
        }

        public static void Guard(string win64)
        {
            if (GameLocator.IsGameRunning()) throw new InvalidOperationException("Close Geronimo first: its files are in use while it runs.");
            if (!GameLocator.IsGameDir(win64)) throw new InvalidOperationException("The game folder isn't set (Settings tab).");
        }

        static void TryDelete(string p) { try { File.Delete(p); } catch { } }

        // ------------------------------------------------------------------ GML Manager itself

        public static string ExePath => Assembly.GetExecutingAssembly().Location;

        // Removes the copy an update left behind (it can't delete itself while running).
        public static void CleanUpOldExe()
        {
            var old = ExePath + ".old";
            for (int i = 0; i < 10 && File.Exists(old); i++)
            {
                try { File.Delete(old); }
                catch { System.Threading.Thread.Sleep(300); }
            }
        }

        // Downloads the new exe, swaps it in (a running exe can be renamed, not overwritten) and
        // starts it. Returns true when the caller should exit.
        public static async Task<bool> UpdateSelf(Release r, IProgress<double> progress)
        {
            var asset = r?.FindAsset(Catalog.ManagerAsset);
            if (asset == null || Versions.Compare(r.Version, App.Version) <= 0) return false;
            var exe = ExePath;
            var fresh = exe + ".new";
            await GitHub.Download(asset.Url, fresh, progress).ConfigureAwait(false);
            var info = FileVersionInfo.GetVersionInfo(fresh);
            if (info.ProductName != "GML Manager") { TryDelete(fresh); throw new InvalidDataException("The downloaded GMLManager.exe doesn't look right; not installed."); }
            TryDelete(exe + ".old");
            File.Move(exe, exe + ".old");
            File.Move(fresh, exe);
            Process.Start(new ProcessStartInfo(exe, "--updated") { UseShellExecute = true });
            return true;
        }
    }
}
