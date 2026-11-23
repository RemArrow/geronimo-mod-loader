// The GML Forge: community mods uploaded to the FDG website (fireteamdevgru.org/forge) and
// checked by an FDG admin before they're listed. The Forge tab lists them and installs a .zip or
// plugin .dll the same way a dropped file is installed. The download's SHA-256 must match what
// the site lists, and a Forge upload may never replace the loader itself.
//
// GMLMANAGER_FORGE overrides the site (tests, a local copy of the website).
using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Net;
using System.Net.Http;
using System.Security.Cryptography;
using System.Threading.Tasks;

namespace GMLManager.Core
{
    public class ForgeMod
    {
        public int Id;
        public string Title, Summary, Description, Version, Category, FileName, Sha256, Uploader;
        public long FileSize;
        public int Downloads;
        public bool HasImage, Featured;
        public DateTime Created;

        public string Ext => Path.GetExtension(FileName ?? "").ToLowerInvariant();
        // .7z/.rar need an unpacker GML Manager doesn't have: those open on the website instead
        public bool CanInstall => Ext == ".zip" || Ext == ".dll";
        public string PageUrl => $"{Forge.Base}/forge/{Id}";
        public string DownloadUrl => $"{Forge.Base}/api/forge/{Id}/download";
        public string ImageUrl => HasImage ? $"{Forge.Base}/api/forge/{Id}/image" : null;
        // what the manifest records for files installed from this upload
        public string Source => "forge:" + Id;
    }

    public static class Forge
    {
        public static readonly string Base = (Environment.GetEnvironmentVariable("GMLMANAGER_FORGE") ?? "https://fireteamdevgru.org").TrimEnd('/');
        public static string SiteUrl => Base + "/forge";

        static readonly HttpClient Http;

        static Forge()
        {
            ServicePointManager.SecurityProtocol |= SecurityProtocolType.Tls12;
            Http = new HttpClient { Timeout = TimeSpan.FromSeconds(30) };
            Http.DefaultRequestHeaders.UserAgent.ParseAdd($"GMLManager/{App.Version}");
        }

        static string Downloads => Path.Combine(Settings.Dir, "downloads", "forge");

        public static async Task<List<ForgeMod>> Load()
        {
            string body;
            try
            {
                using (var res = await Http.GetAsync(Base + "/api/forge").ConfigureAwait(false))
                {
                    body = await res.Content.ReadAsStringAsync().ConfigureAwait(false);
                    if (!res.IsSuccessStatusCode) throw new IOException($"The Forge answered {(int)res.StatusCode}.");
                }
            }
            catch (HttpRequestException e) { throw new IOException("Couldn't reach the GML Forge: " + (e.InnerException?.Message ?? e.Message)); }
            catch (TaskCanceledException) { throw new IOException("The GML Forge didn't answer in time."); }

            var o = Json.Parse(body);
            return o.A("mods").Select(m =>
            {
                var mod = new ForgeMod
                {
                    Id = (int)m.N("id"), Title = m.S("title"), Summary = m.S("summary"), Description = m.S("description"),
                    Version = m.S("version"), Category = m.S("category"), FileName = m.S("fileName"), Sha256 = m.S("sha256"),
                    FileSize = (long)m.N("fileSize"), Downloads = (int)m.N("downloads"), HasImage = m.B("hasImage"), Featured = m.B("featured"),
                    Uploader = (m as Dictionary<string, object>)?.TryGetValue("uploader", out var u) == true ? u.S("displayName") : null,
                };
                DateTime.TryParse((m.S("createdAt") ?? "").Replace(' ', 'T') + "Z", null, System.Globalization.DateTimeStyles.AdjustToUniversal, out mod.Created);
                return mod;
            }).Where(m => m.Id > 0 && !string.IsNullOrEmpty(m.Title) && !string.IsNullOrEmpty(m.FileName)).ToList();
        }

        // Downloads, checks and installs one upload. Returns the names of what was installed.
        public static async Task<List<string>> Install(string win64, ForgeMod mod, IProgress<double> progress)
        {
            if (!mod.CanInstall) throw new InvalidOperationException($"{mod.Title} is a {mod.Ext} file: GML Manager installs .zip and .dll. Get it from the website.");
            var dir = Path.Combine(Downloads, mod.Id.ToString());
            var file = Path.Combine(dir, Installer.Safe(Path.GetFileName(mod.FileName)));
            try
            {
                await GitHub.Download(mod.DownloadUrl, file, progress).ConfigureAwait(false);

                if (!string.IsNullOrEmpty(mod.Sha256))
                {
                    string hash;
                    using (var s = File.OpenRead(file))
                    using (var sha = SHA256.Create())
                        hash = BitConverter.ToString(sha.ComputeHash(s)).Replace("-", "");
                    if (!hash.Equals(mod.Sha256, StringComparison.OrdinalIgnoreCase))
                        throw new InvalidDataException($"{mod.Title}'s download doesn't match the file the Forge lists (checksum mismatch). Nothing was installed.");
                }

                Updater.Guard(win64);
                InstallResult res;
                if (mod.Ext == ".dll") res = Installer.InstallDll(win64, file);
                else
                {
                    CheckZip(file);
                    res = Installer.InstallZip(win64, file, mod.Title);
                }
                if (res.Files.Count == 0) throw new InvalidDataException(mod.Title + " is empty.");

                var version = string.IsNullOrWhiteSpace(mod.Version) ? null : Versions.Clean(mod.Version);
                if (res.Plugins.Count > 0) Installer.Commit(win64, res, mod.Source, version, loader: false);
                else
                {
                    // content only (textures, paks, a map): recorded under the upload's own key
                    var m = Manifest.Load(win64);
                    m.Mods[mod.Source] = new ManifestEntry { Version = version ?? "forge", Repo = mod.Source, Name = mod.Title, Files = res.Files, Installed = DateTime.Now };
                    m.Save(win64);
                }
                return res.Plugins.Count > 0 ? res.Plugins.Select(p => $"{p.Name} {p.Version}").ToList() : new List<string> { mod.Title + " (content)" };
            }
            finally
            {
                try { Directory.Delete(dir, true); } catch { }
            }
        }

        // A community zip must never bring its own loader (version.dll, GML\core): that would
        // replace GML for every mod. Checked before anything is unpacked.
        static void CheckZip(string zip)
        {
            using (var a = ZipFile.OpenRead(zip))
            {
                foreach (var e in a.Entries)
                {
                    var n = e.FullName.Replace('\\', '/').TrimStart('/');
                    var parts = n.Split('/');
                    var name = parts[parts.Length - 1];
                    bool loaderFile = parts.Any(p => p.Equals("core", StringComparison.OrdinalIgnoreCase)) && n.IndexOf("GML/core/", StringComparison.OrdinalIgnoreCase) >= 0;
                    bool doorstop = name.Equals("version.dll", StringComparison.OrdinalIgnoreCase) || name.Equals("doorstop_config.ini", StringComparison.OrdinalIgnoreCase);
                    if (loaderFile || doorstop)
                        throw new InvalidDataException("This upload contains loader files (" + n + "). Forge mods can't replace GML, so it wasn't installed.");
                }
            }
        }

        // Is this upload installed? Its manifest entries (by Source) with their files still there.
        public static bool IsInstalled(string win64, Manifest manifest, ForgeMod mod)
        {
            var entries = manifest.Mods.Values.Where(e => e.Repo == mod.Source).ToList();
            return entries.Count > 0 && entries.Any(e => e.Files.Count > 0 && e.Files.All(f => File.Exists(Path.Combine(win64, f))));
        }
    }
}
