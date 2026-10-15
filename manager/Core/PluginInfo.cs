// Reads a GML plugin's metadata (the exported GML_PluginMetadata struct) straight from the DLL
// file, the same way GML's chainloader does: no LoadLibrary, nothing in the DLL runs.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;

namespace GMLManager.Core
{
    public class Dependency
    {
        public string Guid, MinVersion;
        public bool Soft;
    }

    public class PluginInfo
    {
        public string Path;          // the DLL
        public string Dir;           // its folder
        public string Guid, Name, Version;
        public uint ApiVersion;
        public bool IsPlugin, IsPatcher;
        public bool Disabled;        // disabled.txt in its folder or a parent folder under plugins\
        public bool ContentOnly;     // a plugin folder with no DLL (an Unreal mod plugin or paks)
        public List<Dependency> Dependencies = new List<Dependency>();
        public List<string> Incompatibilities = new List<string>();

        // GML_PluginInfo: size, apiVersion, guid[128], name[128], version[32],
        // dependencies[16] {guid[128], minVersion[32], flags}, incompatibilities[8][128]
        const int GuidLen = 128, NameLen = 128, VerLen = 32, MaxDeps = 16, MaxIncompat = 8;
        const int DepSize = GuidLen + VerLen + 4;

        public static PluginInfo Read(string dll)
        {
            using (var f = new FileStream(dll, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete))
            using (var r = new BinaryReader(f))
            {
                if (f.Length < 0x100 || r.ReadUInt16() != 0x5A4D) return null;   // MZ
                f.Position = 0x3C;
                int pe = r.ReadInt32();
                f.Position = pe;
                if (r.ReadUInt32() != 0x4550) return null;                    // PE\0\0
                ushort machine = r.ReadUInt16(), nSections = r.ReadUInt16();
                f.Position = pe + 20;
                ushort optSize = r.ReadUInt16();
                long opt = pe + 24;
                f.Position = opt;
                if (machine != 0x8664 || r.ReadUInt16() != 0x20B) return null; // x64, PE32+
                f.Position = opt + 112;                                       // DataDirectory[0] = exports
                uint expRva = r.ReadUInt32(), expSize = r.ReadUInt32();
                if (expRva == 0 || expSize == 0) return null;

                var sections = new List<(uint va, uint vsize, uint raw, uint rawSize)>();
                f.Position = opt + optSize;
                for (int i = 0; i < nSections; i++)
                {
                    f.Position = opt + optSize + i * 40 + 8;
                    uint vsize = r.ReadUInt32(), va = r.ReadUInt32(), rawSize = r.ReadUInt32(), raw = r.ReadUInt32();
                    sections.Add((va, vsize, raw, rawSize));
                }
                long Off(uint rva)
                {
                    foreach (var s in sections)
                        if (rva >= s.va && rva < s.va + Math.Max(s.vsize, s.rawSize)) return s.raw + (rva - s.va);
                    return -1;
                }
                string CStr(long off, int max)
                {
                    if (off < 0) return "";
                    f.Position = off;
                    var b = r.ReadBytes(max);
                    int n = Array.IndexOf(b, (byte)0);
                    return Encoding.UTF8.GetString(b, 0, n < 0 ? b.Length : n);
                }

                long e = Off(expRva);
                if (e < 0) return null;
                f.Position = e + 20;
                uint nFuncs = r.ReadUInt32(), nNames = r.ReadUInt32();
                uint aFuncs = r.ReadUInt32(), aNames = r.ReadUInt32(), aOrds = r.ReadUInt32();
                var p = new PluginInfo { Path = dll, Dir = System.IO.Path.GetDirectoryName(dll) };
                long metaOff = -1;
                for (uint i = 0; i < nNames && i < 100000; i++)
                {
                    f.Position = Off(aNames) + i * 4;
                    uint nameRva = r.ReadUInt32();
                    string name = CStr(Off(nameRva), 64);
                    if (name == "GML_Awake") p.IsPlugin = true;
                    else if (name == "GML_Patch") p.IsPatcher = true;
                    else if (name == "GML_PluginMetadata")
                    {
                        f.Position = Off(aOrds) + i * 2;
                        ushort ord = r.ReadUInt16();
                        f.Position = Off(aFuncs) + ord * 4L;
                        metaOff = Off(r.ReadUInt32());
                    }
                }
                if (metaOff < 0)
                {
                    if (!p.IsPlugin && !p.IsPatcher) return null;              // a helper DLL
                    p.Guid = p.Name = System.IO.Path.GetFileNameWithoutExtension(dll);
                    p.Version = "0.0.0";
                    return p;
                }
                f.Position = metaOff;
                uint size = r.ReadUInt32();
                p.ApiVersion = r.ReadUInt32();
                long b0 = metaOff + 8;
                p.Guid = CStr(b0, GuidLen);
                p.Name = CStr(b0 + GuidLen, NameLen);
                p.Version = CStr(b0 + GuidLen + NameLen, VerLen);
                long deps = b0 + GuidLen + NameLen + VerLen;
                if (size >= deps - metaOff + MaxDeps * DepSize)
                    for (int d = 0; d < MaxDeps; d++)
                    {
                        long o = deps + d * DepSize;
                        string g = CStr(o, GuidLen);
                        if (g.Length == 0) continue;
                        f.Position = o + GuidLen + VerLen;
                        p.Dependencies.Add(new Dependency { Guid = g, MinVersion = CStr(o + GuidLen, VerLen), Soft = (r.ReadUInt32() & 1) != 0 });
                    }
                long inc = deps + MaxDeps * DepSize;
                if (size >= inc - metaOff + MaxIncompat * GuidLen)
                    for (int k = 0; k < MaxIncompat; k++)
                    {
                        string g = CStr(inc + k * GuidLen, GuidLen);
                        if (g.Length > 0) p.Incompatibilities.Add(g);
                    }
                if (string.IsNullOrEmpty(p.Name)) p.Name = p.Guid;
                if (string.IsNullOrEmpty(p.Version)) p.Version = "0.0.0";
                return p;
            }
        }

        // Every plugin under GML\plugins and patcher under GML\patchers, enabled or not.
        public static List<PluginInfo> Scan(string win64)
        {
            var list = new List<PluginInfo>();
            var plugins = System.IO.Path.Combine(win64, "GML", "plugins");
            if (Directory.Exists(plugins))
            {
                Walk(plugins, false, list);
                // Top-level folders holding no plugin DLL but content (an Unreal mod plugin, paks)
                foreach (var dir in Directory.GetDirectories(plugins))
                {
                    if (list.Any(p => IsUnder(p.Path, dir))) continue;
                    bool content = Directory.EnumerateFiles(dir, "*.uplugin", SearchOption.AllDirectories).Any()
                                   || Directory.EnumerateFiles(dir, "*.utoc", SearchOption.AllDirectories).Any();
                    if (!content) continue;
                    var n = System.IO.Path.GetFileName(dir);
                    list.Add(new PluginInfo
                    {
                        Path = dir, Dir = dir, Guid = "content:" + n, Name = n, Version = "",
                        ContentOnly = true, IsPlugin = true, Disabled = File.Exists(System.IO.Path.Combine(dir, "disabled.txt"))
                    });
                }
            }
            var patchers = System.IO.Path.Combine(win64, "GML", "patchers");
            if (Directory.Exists(patchers)) Walk(patchers, false, list);
            return list;
        }

        static void Walk(string dir, bool disabled, List<PluginInfo> list)
        {
            disabled |= File.Exists(System.IO.Path.Combine(dir, "disabled.txt"));
            foreach (var dll in Directory.GetFiles(dir, "*.dll"))
            {
                PluginInfo p = null;
                try { p = Read(dll); } catch { }
                if (p == null || !(p.IsPlugin || p.IsPatcher)) continue;
                p.Disabled = disabled;
                list.Add(p);
            }
            foreach (var sub in Directory.GetDirectories(dir).OrderBy(x => x, StringComparer.OrdinalIgnoreCase))
                Walk(sub, disabled, list);
        }

        public static bool IsUnder(string path, string dir) =>
            path.StartsWith(dir.TrimEnd('\\') + "\\", StringComparison.OrdinalIgnoreCase);
    }

    public static class Versions
    {
        // "1.2.10" > "1.2.9"; a leading "v" and missing parts are ignored, as in GML.
        public static int Compare(string a, string b)
        {
            var x = Parts(a); var y = Parts(b);
            for (int i = 0; i < 4; i++)
                if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
            return 0;
        }

        static long[] Parts(string v)
        {
            var r = new long[4];
            if (string.IsNullOrEmpty(v)) return r;
            v = v.Trim().TrimStart('v', 'V');
            var bits = v.Split('.', '-', '+', ' ');
            for (int i = 0; i < 4 && i < bits.Length; i++)
            {
                var digits = new string(bits[i].TakeWhile(char.IsDigit).ToArray());
                long.TryParse(digits, out r[i]);
            }
            return r;
        }

        public static string Clean(string tag) => string.IsNullOrEmpty(tag) ? tag : tag.Trim().TrimStart('v', 'V');
    }
}
