// GitHub releases: the loader, the manager itself and every catalog mod ship as release assets.
// Unauthenticated API calls are limited to 60 an hour, so answers are cached for 10 minutes
// (also on disk, across restarts).
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Net;
using System.Net.Http;
using System.Text.RegularExpressions;
using System.Threading;
using System.Threading.Tasks;

namespace GMLManager.Core
{
    public class ReleaseAsset
    {
        public string Name, Url;
        public long Size;
    }

    public class Release
    {
        public string Repo, Tag, Title, Notes, PageUrl;
        public DateTime Published;
        public bool Prerelease;
        public List<ReleaseAsset> Assets = new List<ReleaseAsset>();
        public string Version => Versions.Clean(Tag);

        public ReleaseAsset FindAsset(string pattern)
        {
            var rx = Pattern(pattern);
            return Assets.FirstOrDefault(a => rx.IsMatch(a.Name));
        }

        public List<ReleaseAsset> FindAssets(string pattern)
        {
            var rx = Pattern(pattern);
            return Assets.Where(a => rx.IsMatch(a.Name)).OrderBy(a => a.Name, StringComparer.OrdinalIgnoreCase).ToList();
        }

        // "Name-*.zip" style wildcard, or a regex when it starts with ^
        public static Regex Pattern(string p) =>
            p.StartsWith("^") ? new Regex(p, RegexOptions.IgnoreCase)
                              : new Regex("^" + Regex.Escape(p).Replace("\\*", ".*").Replace("\\?", ".") + "$", RegexOptions.IgnoreCase);
    }

    public class GitHubException : Exception
    {
        public GitHubException(string m) : base(m) { }
    }

    public static class GitHub
    {
        static readonly HttpClient Http;
        static readonly Dictionary<string, (DateTime at, string body)> Cache = new Dictionary<string, (DateTime, string)>();
        static readonly string CacheFile = Path.Combine(Settings.Dir, "github-cache.json");
        static readonly TimeSpan CacheAge = TimeSpan.FromMinutes(10);

        static GitHub()
        {
            ServicePointManager.SecurityProtocol |= SecurityProtocolType.Tls12;
            Http = new HttpClient { Timeout = TimeSpan.FromMinutes(10) };
            Http.DefaultRequestHeaders.UserAgent.ParseAdd($"GMLManager/{App.Version}");
            try
            {
                if (File.Exists(CacheFile))
                {
                    var o = Json.Parse(File.ReadAllText(CacheFile)) as Dictionary<string, object>;
                    foreach (var kv in o)
                        Cache[kv.Key] = (DateTime.Parse(kv.Value.S("at"), null, System.Globalization.DateTimeStyles.RoundtripKind), kv.Value.S("body"));
                }
            }
            catch { }
        }

        static void SaveCache()
        {
            try
            {
                var o = new Dictionary<string, object>();
                lock (Cache)
                    foreach (var kv in Cache)
                        o[kv.Key] = new Dictionary<string, object> { ["at"] = kv.Value.at.ToString("o"), ["body"] = kv.Value.body };
                Directory.CreateDirectory(Settings.Dir);
                File.WriteAllText(CacheFile, Json.Write(o));
            }
            catch { }
        }

        public static void ClearCache()
        {
            lock (Cache) Cache.Clear();
            try { File.Delete(CacheFile); } catch { }
        }

        public static async Task<string> GetText(string url, bool useCache = true)
        {
            lock (Cache)
                if (useCache && Cache.TryGetValue(url, out var c) && DateTime.UtcNow - c.at < CacheAge) return c.body;
            using (var req = new HttpRequestMessage(HttpMethod.Get, url))
            {
                if (url.Contains("api.github.com")) req.Headers.Accept.ParseAdd("application/vnd.github+json");
                HttpResponseMessage res;
                try { res = await Http.SendAsync(req).ConfigureAwait(false); }
                catch (Exception e) { throw new GitHubException("Couldn't reach GitHub: " + (e.InnerException?.Message ?? e.Message)); }
                using (res)
                {
                    var body = await res.Content.ReadAsStringAsync().ConfigureAwait(false);
                    if ((int)res.StatusCode == 403 || (int)res.StatusCode == 429)
                    {
                        lock (Cache)
                            if (Cache.TryGetValue(url, out var stale)) return stale.body;   // better old than nothing
                        throw new GitHubException("GitHub's hourly limit for update checks was reached. Try again later.");
                    }
                    if (res.StatusCode == HttpStatusCode.NotFound) throw new GitHubException("Not found: " + url);
                    if (!res.IsSuccessStatusCode) throw new GitHubException($"GitHub answered {(int)res.StatusCode} for {url}");
                    lock (Cache) Cache[url] = (DateTime.UtcNow, body);
                    SaveCache();
                    return body;
                }
            }
        }

        // The newest published release (drafts never; pre-releases only when asked).
        public static async Task<Release> Latest(string repo, bool prerelease = false)
        {
            var body = await GetText($"https://api.github.com/repos/{repo}/releases?per_page=15").ConfigureAwait(false);
            foreach (var r in (List<object>)Json.Parse(body))
            {
                if (r.B("draft") || (!prerelease && r.B("prerelease"))) continue;
                var rel = new Release
                {
                    Repo = repo, Tag = r.S("tag_name"), Title = r.S("name"), Notes = r.S("body"),
                    PageUrl = r.S("html_url"), Prerelease = r.B("prerelease")
                };
                DateTime.TryParse(r.S("published_at"), out rel.Published);
                foreach (var a in r.A("assets"))
                    rel.Assets.Add(new ReleaseAsset { Name = a.S("name"), Url = a.S("browser_download_url"), Size = (long)a.N("size") });
                return rel;
            }
            return null;
        }

        // A download that was cut off (dest + ".part" left behind) carries on where it stopped when
        // the server takes a Range request; else it starts over. size > 0: a finished dest of that
        // size is kept as it is (multi-GB map parts aren't fetched twice).
        public static async Task Download(string url, string dest, IProgress<double> progress, CancellationToken ct = default, long size = -1)
        {
            Directory.CreateDirectory(Path.GetDirectoryName(dest));
            if (size > 0 && File.Exists(dest) && new FileInfo(dest).Length == size) { progress?.Report(1); return; }
            var tmp = dest + ".part";
            long have = File.Exists(tmp) ? new FileInfo(tmp).Length : 0;
            if (size > 0 && have > size) { File.Delete(tmp); have = 0; }
            using (var req = new HttpRequestMessage(HttpMethod.Get, url))
            {
            if (have > 0) req.Headers.Range = new System.Net.Http.Headers.RangeHeaderValue(have, null);
            using (var res = await Http.SendAsync(req, HttpCompletionOption.ResponseHeadersRead, ct).ConfigureAwait(false))
            {
                if (!res.IsSuccessStatusCode) throw new GitHubException($"Download failed ({(int)res.StatusCode}): {url}");
                bool resume = have > 0 && res.StatusCode == HttpStatusCode.PartialContent;
                if (!resume) have = 0;
                long clen = res.Content.Headers.ContentLength ?? -1;
                long total = clen >= 0 ? clen + have : -1, done = have;
                using (var src = await res.Content.ReadAsStreamAsync().ConfigureAwait(false))
                using (var dst = new FileStream(tmp, resume ? FileMode.Append : FileMode.Create, FileAccess.Write))
                {
                    var buf = new byte[1 << 16];
                    int n;
                    while ((n = await src.ReadAsync(buf, 0, buf.Length, ct).ConfigureAwait(false)) > 0)
                    {
                        await dst.WriteAsync(buf, 0, n, ct).ConfigureAwait(false);
                        done += n;
                        if (total > 0) progress?.Report((double)done / total);
                    }
                }
                if (total > 0 && done != total) throw new GitHubException($"Download cut off at {done:N0} of {total:N0} bytes: try again, it carries on.");
            }
            }
            if (File.Exists(dest)) File.Delete(dest);
            File.Move(tmp, dest);
        }
    }
}
