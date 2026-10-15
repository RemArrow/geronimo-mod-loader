// Installs the loader and mods into Binaries\Win64, and remembers what it wrote.
//
// GML\manager.json records each install's version and files, so an update removes files the new
// version no longer ships, and auto-update only ever touches what the manager installed itself:
// a plugin you built and copied in by hand is never overwritten behind your back.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Text.RegularExpressions;
using Microsoft.VisualBasic.FileIO;

namespace GMLManager.Core
{
    public class ManifestEntry
    {
        public string Version, Repo, Name;
        public List<string> Files = new List<string>();   // relative to Win64, '\' separators
        public DateTime Installed;
    }

    public class Manifest
    {
        public ManifestEntry Loader;
        public Dictionary<string, ManifestEntry> Mods = new Dictionary<string, ManifestEntry>(StringComparer.OrdinalIgnoreCase);

        static string PathFor(string win64) => Path.Combine(win64, "GML", "manager.json");

        public static Manifest Load(string win64)
        {
            var m = new Manifest();
            try
            {
                var p = PathFor(win64);
                if (!File.Exists(p)) return m;
                var o = Json.Parse(File.ReadAllText(p));
                m.Loader = Entry(o.O("loader"));
                var mods = o.O("mods");
                if (mods != null) foreach (var kv in mods) m.Mods[kv.Key] = Entry(kv.Value);
            }
            catch { }
            return m;
        }

        static ManifestEntry Entry(object o)
        {
            if (o == null) return null;
            var e = new ManifestEntry { Version = o.S("version"), Repo = o.S("repo"), Name = o.S("name") };
            e.Files = o.A("files").Select(f => f?.ToString()).Where(f => f != null).ToList();
            DateTime.TryParse(o.S("installed"), out e.Installed);
            return e;
        }

        static object Obj(ManifestEntry e) => e == null ? null : new Dictionary<string, object>
        {
            ["version"] = e.Version, ["repo"] = e.Repo, ["name"] = e.Name,
            ["installed"] = e.Installed.ToString("o"), ["files"] = e.Files
        };

        public void Save(string win64)
        {
            var mods = new SortedDictionary<string, object>(StringComparer.OrdinalIgnoreCase);
            foreach (var kv in Mods) mods[kv.Key] = Obj(kv.Value);
            Directory.CreateDirectory(Path.Combine(win64, "GML"));
            File.WriteAllText(PathFor(win64), Json.Write(new Dictionary<string, object> { ["loader"] = Obj(Loader), ["mods"] = mods }));
        }
    }

    public class LoaderState
    {
        public bool Installed, Enabled, DevBuild, Managed;
        public string Version;
    }

    public class InstallResult
    {
        public List<string> Files = new List<string>();
        public List<PluginInfo> Plugins = new List<PluginInfo>();   // plugins found among the files
        public bool HasLoader;
    }

    public static class Installer
    {
        // ------------------------------------------------------------------ the loader

        public static LoaderState Loader(string win64)
        {
            var s = new LoaderState();
            string core = Path.Combine(win64, "GML", "core", "GML.dll");
            s.Installed = File.Exists(core) && File.Exists(Path.Combine(win64, "version.dll"));
            if (!s.Installed) return s;
            s.DevBuild = File.Exists(Path.Combine(win64, "GML", "core", "GML.pdb"));
            s.Managed = Manifest.Load(win64).Loader != null;
            // 2.4+ carries a version resource. Older ones: the log's first line, if the log was written
            // by this GML.dll, else GML_INSTALL.txt, if it came with this GML.dll (a build.bat
            // install leaves an old one behind).
            try
            {
                var fv = FileVersionInfo.GetVersionInfo(core);
                if (!string.IsNullOrEmpty(fv.ProductVersion) && fv.ProductName == "Geronimo Mod Loader") s.Version = fv.ProductVersion;
            }
            catch { }
            var coreTime = File.GetLastWriteTimeUtc(core);
            foreach (var f in new[] { @"GML\LogOutput.log", "GML_INSTALL.txt" })
            {
                if (s.Version != null) break;
                try
                {
                    var p = Path.Combine(win64, f);
                    if (!File.Exists(p)) continue;
                    var t = File.GetLastWriteTimeUtc(p);
                    if (f.EndsWith(".log") ? t < coreTime : Math.Abs((t - coreTime).TotalMinutes) > 10) continue;
                    using (var r = new StreamReader(new FileStream(p, FileMode.Open, FileAccess.Read, FileShare.ReadWrite)))
                        for (int i = 0; i < 3; i++)
                        {
                            var m = Regex.Match(r.ReadLine() ?? "", @"Geronimo Mod Loader (\d+\.\d+\.\d+)");
                            if (m.Success) { s.Version = m.Groups[1].Value; break; }
                        }
                }
                catch { }
            }
            var ini = Path.Combine(win64, "doorstop_config.ini");
            s.Enabled = true;
            if (File.Exists(ini))
                foreach (var line in File.ReadAllLines(ini))
                {
                    var m = Regex.Match(line, @"^\s*enabled\s*=\s*(\w+)", RegexOptions.IgnoreCase);
                    if (m.Success) s.Enabled = m.Groups[1].Value.Equals("true", StringComparison.OrdinalIgnoreCase);
                }
            return s;
        }

        public static void SetLoaderEnabled(string win64, bool on)
        {
            var ini = Path.Combine(win64, "doorstop_config.ini");
            var lines = File.Exists(ini) ? File.ReadAllLines(ini).ToList() : new List<string> { "[General]" };
            int i = lines.FindIndex(l => Regex.IsMatch(l, @"^\s*enabled\s*=", RegexOptions.IgnoreCase));
            var v = "enabled = " + (on ? "true" : "false");
            if (i >= 0) lines[i] = v; else lines.Insert(Math.Min(1, lines.Count), v);
            File.WriteAllText(ini, string.Join("\r\n", lines) + "\r\n");
        }

        // Removes the loader and what it deployed (paks, mirrored Unreal mods). GML\plugins and
        // GML\config stay, so reinstalling brings every mod back as it was.
        public static void UninstallLoader(string win64)
        {
            foreach (var f in new[] { "version.dll", "doorstop_config.ini", "GML_INSTALL.txt" })
                TryDelete(Path.Combine(win64, f));
            TryDeleteDir(Path.Combine(win64, "GML", "core"));
            var gameRoot = GameRoot(win64);
            if (gameRoot != null)
            {
                TryDeleteDir(Path.Combine(gameRoot, "Content", "Paks", "~GML"));
                var mods = Path.Combine(gameRoot, "Mods");
                var list = Path.Combine(mods, ".gml-managed");
                if (File.Exists(list))
                {
                    foreach (var n in File.ReadAllLines(list).Select(l => l.Trim()).Where(l => l.Length > 0 && l.IndexOfAny(new[] { '\\', '/', ':' }) < 0 && l != ".." && l != "."))
                        TryDeleteDir(Path.Combine(mods, n));
                    TryDelete(list);
                }
            }
            var m = Manifest.Load(win64);
            m.Loader = null;
            m.Save(win64);
        }

        // <game>\Geronimo (Win64 is <game>\Geronimo\Binaries\Win64)
        public static string GameRoot(string win64) => Directory.GetParent(win64)?.Parent?.FullName;

        // ------------------------------------------------------------------ installing

        // A zip laid out like Win64 (GML\plugins\X\..., or the loader's version.dll + GML\core).
        // Also accepted: one wrapping folder around that, or plain plugin files (a folder with the
        // DLL, or loose files), which go to GML\plugins\<folder or zip name>\.
        public static InstallResult InstallZip(string win64, string zip, string displayName = null)
        {
            var res = new InstallResult();
            using (var a = ZipFile.OpenRead(zip))
            {
                var entries = a.Entries.Where(e => !string.IsNullOrEmpty(e.Name))
                                       .Select(e => (e, n: e.FullName.Replace('\\', '/').TrimStart('/')))
                                       .Where(x => !x.n.StartsWith("__MACOSX/")).ToList();
                if (entries.Count == 0) throw new InvalidDataException("The zip is empty.");
                var bad = entries.FirstOrDefault(x => x.n.Split('/').Any(seg => seg == ".." || seg.Contains(':')));
                if (bad.n != null) throw new InvalidDataException("The zip has a file with an unsafe path: " + bad.n);

                bool IsRootLayout(IEnumerable<string> names) =>
                    names.Any(n => n.StartsWith("GML/", StringComparison.OrdinalIgnoreCase) || n.Equals("version.dll", StringComparison.OrdinalIgnoreCase));

                string strip = "", prefix = "";
                if (!IsRootLayout(entries.Select(x => x.n)))
                {
                    var tops = entries.Select(x => x.n.Contains('/') ? x.n.Substring(0, x.n.IndexOf('/')) : "").Distinct(StringComparer.OrdinalIgnoreCase).ToList();
                    if (tops.Count == 1 && tops[0] != "" && IsRootLayout(entries.Select(x => x.n.Substring(tops[0].Length + 1))))
                        strip = tops[0] + "/";                                   // MyMod-1.0/GML/plugins/...
                    else if (tops.Count == 1 && tops[0] != "")
                        prefix = "GML/plugins/";                                 // MyMod/MyMod.dll
                    else
                        prefix = "GML/plugins/" + Safe(displayName ?? Path.GetFileNameWithoutExtension(zip)) + "/";
                }

                string root = Path.GetFullPath(win64).TrimEnd('\\') + "\\";
                var staged = new List<(ZipArchiveEntry e, string rel)>();
                foreach (var (e, n) in entries)
                {
                    var rel = (prefix + n.Substring(strip.Length)).Replace('/', '\\');
                    var full = Path.GetFullPath(Path.Combine(root, rel));
                    if (!full.StartsWith(root, StringComparison.OrdinalIgnoreCase))
                        throw new InvalidDataException("The zip tries to write outside the game folder: " + n);
                    staged.Add((e, full.Substring(root.Length)));
                }

                foreach (var (e, rel) in staged)
                {
                    var full = Path.Combine(root, rel);
                    // the user's own doorstop settings (enabled = false) survive a loader update
                    if (rel.Equals("doorstop_config.ini", StringComparison.OrdinalIgnoreCase) && File.Exists(full)) { res.Files.Add(rel); continue; }
                    // never overwrite a plugin's settings or a mod's disable switch
                    if (rel.StartsWith(@"GML\config\", StringComparison.OrdinalIgnoreCase) && File.Exists(full)) continue;
                    Directory.CreateDirectory(Path.GetDirectoryName(full));
                    var tmp = full + ".gmlnew";
                    e.ExtractToFile(tmp, true);
                    if (File.Exists(full)) File.Delete(full);
                    File.Move(tmp, full);
                    res.Files.Add(rel);
                    if (rel.Equals("version.dll", StringComparison.OrdinalIgnoreCase) || rel.Equals(@"GML\core\GML.dll", StringComparison.OrdinalIgnoreCase))
                        res.HasLoader = true;
                    if (rel.EndsWith(".dll", StringComparison.OrdinalIgnoreCase) && (rel.StartsWith(@"GML\plugins\", StringComparison.OrdinalIgnoreCase) || rel.StartsWith(@"GML\patchers\", StringComparison.OrdinalIgnoreCase)))
                    {
                        PluginInfo p = null;
                        try { p = PluginInfo.Read(full); } catch { }
                        if (p != null && (p.IsPlugin || p.IsPatcher)) res.Plugins.Add(p);
                    }
                }
            }
            return res;
        }

        // A bare plugin DLL goes to GML\plugins\<Name>\<Name>.dll.
        public static InstallResult InstallDll(string win64, string dll)
        {
            var info = PluginInfo.Read(dll);
            if (info == null || !(info.IsPlugin || info.IsPatcher))
                throw new InvalidDataException(Path.GetFileName(dll) + " isn't a GML plugin (it has no GML_Awake or GML_Patch export).");
            string folder = info.IsPatcher && !info.IsPlugin ? Path.Combine("GML", "patchers") : Path.Combine("GML", "plugins", Safe(Path.GetFileNameWithoutExtension(dll)));
            var rel = Path.Combine(folder, Path.GetFileName(dll));
            var full = Path.Combine(win64, rel);
            Directory.CreateDirectory(Path.GetDirectoryName(full));
            if (!string.Equals(Path.GetFullPath(dll), Path.GetFullPath(full), StringComparison.OrdinalIgnoreCase))
                File.Copy(dll, full, true);
            var res = new InstallResult();
            res.Files.Add(rel);
            res.Plugins.Add(PluginInfo.Read(full));
            return res;
        }

        // After an install: record it, drop files the previous version had and this one doesn't,
        // and move other copies of the same plugin (another folder, an older install) to the
        // Recycle Bin so GML doesn't find two.
        public static void Commit(string win64, InstallResult res, string repo, string version, bool loader)
        {
            var m = Manifest.Load(win64);
            var now = new HashSet<string>(res.Files, StringComparer.OrdinalIgnoreCase);

            void DropOld(ManifestEntry old)
            {
                if (old == null) return;
                foreach (var f in old.Files.Where(f => !now.Contains(f)))
                {
                    var full = Path.GetFullPath(Path.Combine(win64, f));
                    if (full.StartsWith(Path.GetFullPath(win64).TrimEnd('\\') + "\\", StringComparison.OrdinalIgnoreCase))
                        TryDelete(full);
                }
            }

            if (loader || res.HasLoader)
            {
                var loaderFiles = res.Files.Where(f => !f.StartsWith(@"GML\plugins\", StringComparison.OrdinalIgnoreCase) && !f.StartsWith(@"GML\patchers\", StringComparison.OrdinalIgnoreCase)).ToList();
                DropOld(m.Loader);
                m.Loader = new ManifestEntry { Version = version, Repo = repo, Name = "Geronimo Mod Loader", Files = loaderFiles, Installed = DateTime.Now };
            }
            if (!loader)
            {
                var all = PluginInfo.Scan(win64);
                foreach (var p in res.Plugins)
                {
                    m.Mods.TryGetValue(p.Guid, out var old);
                    DropOld(old);
                    var mine = OwnFolder(win64, p);
                    foreach (var other in all.Where(o => o.Guid == p.Guid && !string.Equals(o.Path, p.Path, StringComparison.OrdinalIgnoreCase)))
                    {
                        var theirs = OwnFolder(win64, other);
                        if (theirs != null && mine != null && string.Equals(theirs, mine, StringComparison.OrdinalIgnoreCase))
                            FileSystem.DeleteFile(other.Path, UIOption.OnlyErrorDialogs, RecycleOption.SendToRecycleBin);  // same folder: just that DLL
                        else
                            Remove(win64, other, keepManifest: true);
                    }
                    var files = res.Files.Where(f => PluginInfo.IsUnder(Path.Combine(win64, f), p.Dir) || string.Equals(Path.Combine(win64, f), p.Path, StringComparison.OrdinalIgnoreCase)).ToList();
                    if (Path.GetFullPath(p.Dir).TrimEnd('\\').Equals(Path.GetFullPath(Path.Combine(win64, "GML", "plugins")).TrimEnd('\\'), StringComparison.OrdinalIgnoreCase) ||
                        Path.GetFullPath(p.Dir).TrimEnd('\\').Equals(Path.GetFullPath(Path.Combine(win64, "GML", "patchers")).TrimEnd('\\'), StringComparison.OrdinalIgnoreCase))
                        files = new List<string> { p.Path.Substring(Path.GetFullPath(win64).TrimEnd('\\').Length + 1) };
                    m.Mods[p.Guid] = new ManifestEntry { Version = version ?? p.Version, Repo = repo, Name = p.Name, Files = files, Installed = DateTime.Now };
                }
            }
            m.Save(win64);
        }

        // ------------------------------------------------------------------ one plugin

        // Its own folder under plugins\ (or patchers\), or null when the DLL sits loose there.
        public static string OwnFolder(string win64, PluginInfo p)
        {
            foreach (var top in new[] { Path.Combine(win64, "GML", "plugins"), Path.Combine(win64, "GML", "patchers") })
            {
                var t = Path.GetFullPath(top).TrimEnd('\\');
                var d = Path.GetFullPath(p.Dir).TrimEnd('\\');
                if (d.Equals(t, StringComparison.OrdinalIgnoreCase)) return null;
                if (PluginInfo.IsUnder(d, t))
                {
                    // the first folder below plugins\ is the mod's
                    var rest = d.Substring(t.Length + 1);
                    return Path.Combine(t, rest.Split('\\')[0]);
                }
            }
            return null;
        }

        public static void SetEnabled(string win64, PluginInfo p, bool on)
        {
            var dir = OwnFolder(win64, p);
            if (dir == null) throw new InvalidOperationException(
                $"{Path.GetFileName(p.Path)} sits directly in {Path.GetFileName(p.Dir)}\\, so it can't be switched off on its own. Move it into a folder of its own first.");
            var marker = Path.Combine(dir, "disabled.txt");
            if (on)
            {
                TryDelete(marker);
                // a disabled.txt deeper down (between plugins\X and the DLL) disables it too
                for (var d = p.Dir; d != null && PluginInfo.IsUnder(d, dir); d = Path.GetDirectoryName(d))
                    TryDelete(Path.Combine(d, "disabled.txt"));
            }
            else File.WriteAllText(marker, "Disabled by GML Manager. Delete this file (or switch the mod on there) to load it again.\r\n");
        }

        // To the Recycle Bin: the plugin's folder, or the DLL alone when it sits loose in plugins\.
        public static void Remove(string win64, PluginInfo p, bool keepManifest = false)
        {
            var dir = OwnFolder(win64, p);
            if (dir != null && Directory.Exists(dir))
                FileSystem.DeleteDirectory(dir, UIOption.OnlyErrorDialogs, RecycleOption.SendToRecycleBin);
            else if (File.Exists(p.Path))
                FileSystem.DeleteFile(p.Path, UIOption.OnlyErrorDialogs, RecycleOption.SendToRecycleBin);
            if (keepManifest) return;
            var m = Manifest.Load(win64);
            if (m.Mods.Remove(p.Guid)) m.Save(win64);
        }

        public static string ConfigPath(string win64, string guid) => Path.Combine(win64, "GML", "config", guid + ".cfg");

        // ------------------------------------------------------------------ helpers

        public static string Safe(string name)
        {
            foreach (var c in Path.GetInvalidFileNameChars()) name = name.Replace(c, '_');
            return Regex.Replace(name, @"[-_. ]*\d+(\.\d+)+$", "").Trim().TrimEnd('.');   // "MyMod-1.2.0" -> "MyMod"
        }

        static void TryDelete(string p) { try { if (File.Exists(p)) File.Delete(p); } catch { } }

        static void TryDeleteDir(string p) { try { if (Directory.Exists(p)) Directory.Delete(p, true); } catch { } }
    }
}
