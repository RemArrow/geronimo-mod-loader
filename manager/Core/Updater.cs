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
            if (!string.IsNullOrEmpty(m.Parts)) return await InstallParts(win64, m, r, progress).ConfigureAwait(false);
            var asset = r.FindAsset(m.Asset) ?? throw new GitHubException($"{m.Name} {r.Version} has no download matching \"{m.Asset}\".");
            var zip = await Fetch(asset, progress).ConfigureAwait(false);
            Guard(win64);
            var res = Installer.InstallZip(win64, zip, m.Name);
            Record(win64, m, r, res, asset.Name);
            TryDelete(zip);
            return res;
        }

        // A zip uploaded in parts (GitHub takes 2 GB per file): every part is downloaded (a cut-off
        // one carries on next time, finished ones are kept until the install succeeds), checked
        // against the catalog's sha256, and installed straight from the parts.
        static async Task<InstallResult> InstallParts(string win64, CatalogMod m, Release r, IProgress<double> progress)
        {
            var parts = r.FindAssets(m.Parts);
            if (parts.Count == 0) throw new GitHubException($"{m.Name} {r.Version} has no download matching \"{m.Parts}\".");
            long total = parts.Sum(a => a.Size);
            Guard(win64);
            var files = parts.Select(a => Path.Combine(Downloads, a.Name)).ToList();
            long have = parts.Select((a, i) => File.Exists(files[i]) ? Math.Min(new FileInfo(files[i]).Length, a.Size)
                                               : File.Exists(files[i] + ".part") ? Math.Min(new FileInfo(files[i] + ".part").Length, a.Size) : 0).Sum();
            Directory.CreateDirectory(Downloads);
            NeedSpace(Downloads, total - have, win64, total);

            long before = 0;
            for (int i = 0; i < parts.Count; i++)
            {
                long at = before, size = parts[i].Size;
                var p = progress == null ? null : new Progress<double>(v => progress.Report(total > 0 ? (at + v * size) / total : 0));
                await GitHub.Download(parts[i].Url, files[i], p, default, size).ConfigureAwait(false);
                before += size;
            }

            Guard(win64);
            InstallResult res;
            using (var joined = new JoinedStream(files))
            {
                if (!string.IsNullOrEmpty(m.Sha256))
                {
                    string hash;
                    using (var sha = System.Security.Cryptography.SHA256.Create())
                        hash = BitConverter.ToString(sha.ComputeHash(joined)).Replace("-", "");
                    if (!hash.Equals(m.Sha256.Trim(), StringComparison.OrdinalIgnoreCase))
                    {
                        joined.Dispose();
                        foreach (var f in files) TryDelete(f);
                        throw new InvalidDataException($"{m.Name}'s download is damaged (its checksum doesn't match). It was deleted: try again.");
                    }
                    joined.Position = 0;
                }
                res = Installer.InstallZip(win64, joined, m.Name);
            }
            Record(win64, m, r, res, parts[0].Name);
            foreach (var f in files) TryDelete(f);
            return res;
        }

        // Free space for the download (dlBytes, in Downloads) and the unpacked files (installBytes,
        // in the game folder): both on one drive when they share it.
        static void NeedSpace(string dlDir, long dlBytes, string win64, long installBytes)
        {
            var spare = 200L << 20;
            string dlRoot = Path.GetPathRoot(Path.GetFullPath(dlDir)), gameRoot = Path.GetPathRoot(Path.GetFullPath(win64));
            var need = new System.Collections.Generic.Dictionary<string, long>(StringComparer.OrdinalIgnoreCase);
            need[dlRoot] = dlBytes + spare;
            need[gameRoot] = (need.TryGetValue(gameRoot, out var x) ? x : spare) + installBytes;
            foreach (var kv in need)
            {
                long free;
                try { free = new DriveInfo(kv.Key).AvailableFreeSpace; } catch { continue; }
                if (free < kv.Value)
                    throw new IOException($"Not enough free space on {kv.Key.TrimEnd('\\')}: this needs {kv.Value / (double)(1L << 30):0.0} GB, " +
                                          $"{free / (double)(1L << 30):0.0} GB is free.");
            }
        }

        static void Record(string win64, CatalogMod m, Release r, InstallResult res, string what)
        {
            if (res.Plugins.Count == 0 && res.Files.Count == 0) throw new InvalidDataException(what + " is empty.");
            if (res.Plugins.Count == 0)
            {
                // content-only mod: record it under its guid all the same
                var m2 = Manifest.Load(win64);
                m2.Mods[m.Guid] = new ManifestEntry { Version = r.Version, Repo = m.Repo, Name = m.Name, Files = res.Files, Installed = DateTime.Now };
                m2.Save(win64);
            }
            else Installer.Commit(win64, res, m.Repo, r.Version, loader: false);
        }

        // A catalog mod with no DLL of its own (a map) that GML Manager installed and whose files are
        // all still there: what the Browse tab shows as installed for it.
        public static PluginInfo InstalledContent(string win64, Manifest manifest, CatalogMod m)
        {
            if (!manifest.Mods.TryGetValue(m.Guid, out var e) || e.Files.Count == 0) return null;
            if (!e.Files.All(f => File.Exists(Path.Combine(win64, f)))) return null;
            return new PluginInfo { Guid = m.Guid, Name = m.Name, Version = e.Version, ContentOnly = true, Path = win64, Dir = win64 };
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
