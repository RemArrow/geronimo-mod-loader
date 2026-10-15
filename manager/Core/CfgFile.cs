// GML's .cfg files (BepInEx format). Only the "Key = value" lines change on save, so comments,
// unknown keys and ordering stay exactly as the plugin wrote them.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;

namespace GMLManager.Core
{
    public class CfgEntry
    {
        public string Section, Key, Value, Type, Default, Description;
        public List<string> AcceptableValues = new List<string>();
        internal int Line;
    }

    public class CfgFile
    {
        public string Path;
        public string Header;   // "Settings file was created by plugin X v1"
        public List<CfgEntry> Entries = new List<CfgEntry>();
        List<string> _lines;
        string _newline = "\n";     // GML writes LF; a file someone saved with CRLF keeps CRLF
        bool _finalNewline = true;

        public static CfgFile Load(string path)
        {
            var text = File.ReadAllText(path);
            var c = new CfgFile { Path = path };
            c._newline = text.Contains("\r\n") ? "\r\n" : "\n";
            c._finalNewline = text.EndsWith("\n");
            if (c._finalNewline) text = text.Substring(0, text.Length - c._newline.Length);
            c._lines = text.Split(new[] { c._newline }, StringSplitOptions.None).ToList();
            string section = "";
            var desc = new List<string>();
            string type = null, def = null;
            var acceptable = new List<string>();
            for (int i = 0; i < c._lines.Count; i++)
            {
                var t = c._lines[i].Trim();
                if (t.Length == 0) continue;
                if (t.StartsWith("[") && t.EndsWith("]"))
                {
                    section = t.Substring(1, t.Length - 2);
                    desc.Clear(); type = def = null; acceptable = new List<string>();
                    continue;
                }
                if (t.StartsWith("##"))
                {
                    var d = t.Substring(2).Trim();
                    if (section.Length == 0)
                    {
                        if (c.Header == null && d.StartsWith("Settings file was created")) c.Header = d;
                    }
                    else desc.Add(d);
                    continue;
                }
                if (t.StartsWith("#"))
                {
                    var d = t.Substring(1).Trim();
                    if (d.StartsWith("Setting type:")) type = d.Substring(13).Trim();
                    else if (d.StartsWith("Default value:")) def = d.Substring(14).Trim();
                    else if (d.StartsWith("Acceptable values:"))
                        acceptable = d.Substring(18).Split(',').Select(x => x.Trim()).Where(x => x.Length > 0).ToList();
                    continue;
                }
                int eq = t.IndexOf('=');
                if (eq <= 0) continue;
                c.Entries.Add(new CfgEntry
                {
                    Section = section, Key = t.Substring(0, eq).Trim(), Value = t.Substring(eq + 1).Trim(),
                    Type = type ?? "String", Default = def, AcceptableValues = acceptable,
                    Description = string.Join(" ", desc), Line = i
                });
                desc.Clear(); type = def = null; acceptable = new List<string>();
            }
            return c;
        }

        public CfgEntry Get(string section, string key) =>
            Entries.FirstOrDefault(e => string.Equals(e.Section, section, StringComparison.OrdinalIgnoreCase) &&
                                        string.Equals(e.Key, key, StringComparison.OrdinalIgnoreCase));

        public void Set(CfgEntry e, string value)
        {
            e.Value = value;
            _lines[e.Line] = $"{e.Key} = {value}";
        }

        public void Save()
        {
            // UTF-8 without BOM, as GML writes it
            File.WriteAllText(Path, string.Join(_newline, _lines) + (_finalNewline ? _newline : ""), new UTF8Encoding(false));
        }

        // A one-off edit of a file that may not exist yet (GML writes its .cfg on first launch).
        public static bool TrySet(string path, string section, string key, string value)
        {
            if (!File.Exists(path)) return false;
            var c = Load(path);
            var e = c.Get(section, key);
            if (e == null) return false;
            c.Set(e, value);
            c.Save();
            return true;
        }
    }
}
