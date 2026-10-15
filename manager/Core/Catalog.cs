// The list of mods offered in the Browse tab: catalog.json in the loader's repo. A copy is built
// into the exe for when GitHub can't be reached. Settings.CatalogUrl (a URL or a local file)
// overrides it, so mod authors can test their entry before opening a pull request.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Threading.Tasks;

namespace GMLManager.Core
{
    public class CatalogMod
    {
        public string Guid, Name, Author, Description, Repo, Asset, Credits, Homepage, MinGml;
        public List<string> Tags = new List<string>();
        public string Page => !string.IsNullOrEmpty(Homepage) ? Homepage : $"https://github.com/{Repo}";
    }

    public static class Catalog
    {
        public const string LoaderRepo = "RemArrow/geronimo-mod-loader";
        public const string LoaderAsset = @"^GML-\d[\d.]*\.zip$";
        public const string SdkAsset = @"^GML-SDK-\d[\d.]*\.zip$";
        public const string ManagerAsset = "GMLManager.exe";
        public const string DefaultUrl = "https://raw.githubusercontent.com/RemArrow/geronimo-mod-loader/main/catalog.json";

        public static string Source { get; private set; } = "built-in";

        public static async Task<List<CatalogMod>> Load(string url)
        {
            url = string.IsNullOrWhiteSpace(url) ? DefaultUrl : url.Trim();
            try
            {
                string text = File.Exists(url) ? File.ReadAllText(url) : await GitHub.GetText(url).ConfigureAwait(false);
                var mods = Parse(text);
                Source = url;
                return mods;
            }
            catch
            {
                Source = "built-in (couldn't load " + url + ")";
                return BuiltIn();
            }
        }

        public static List<CatalogMod> BuiltIn()
        {
            using (var s = Assembly.GetExecutingAssembly().GetManifestResourceStream("catalog.json"))
            using (var r = new StreamReader(s))
                return Parse(r.ReadToEnd());
        }

        static List<CatalogMod> Parse(string text)
        {
            var o = Json.Parse(text);
            return o.A("mods").Select(m => new CatalogMod
            {
                Guid = m.S("guid"), Name = m.S("name"), Author = m.S("author"), Description = m.S("description"),
                Repo = m.S("repo"), Asset = m.S("asset") ?? "*.zip", Credits = m.S("credits"), Homepage = m.S("homepage"),
                MinGml = m.S("minGml"), Tags = m.A("tags").Select(t => t?.ToString()).Where(t => t != null).ToList()
            }).Where(m => !string.IsNullOrEmpty(m.Guid) && !string.IsNullOrEmpty(m.Repo)).ToList();
        }
    }
}
