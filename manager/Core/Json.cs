// A small JSON reader/writer, so the exe needs nothing outside .NET Framework 4.8.
// Objects are Dictionary<string, object>, arrays List<object>, numbers double, plus string/bool/null.
using System;
using System.Collections;
using System.Collections.Generic;
using System.Globalization;
using System.Text;

namespace GMLManager.Core
{
    public static class Json
    {
        public static object Parse(string s)
        {
            int i = 0;
            object v = Value(s, ref i);
            Ws(s, ref i);
            if (i != s.Length) throw new FormatException($"JSON: unexpected text at {i}");
            return v;
        }

        static void Ws(string s, ref int i) { while (i < s.Length && char.IsWhiteSpace(s[i])) i++; }

        static object Value(string s, ref int i)
        {
            Ws(s, ref i);
            if (i >= s.Length) throw new FormatException("JSON: unexpected end");
            char c = s[i];
            if (c == '{')
            {
                var o = new Dictionary<string, object>(StringComparer.Ordinal);
                i++; Ws(s, ref i);
                if (s[i] == '}') { i++; return o; }
                for (;;)
                {
                    Ws(s, ref i);
                    string k = Str(s, ref i);
                    Ws(s, ref i);
                    if (s[i++] != ':') throw new FormatException($"JSON: ':' expected at {i - 1}");
                    o[k] = Value(s, ref i);
                    Ws(s, ref i);
                    if (s[i] == ',') { i++; continue; }
                    if (s[i] == '}') { i++; return o; }
                    throw new FormatException($"JSON: ',' or '}}' expected at {i}");
                }
            }
            if (c == '[')
            {
                var a = new List<object>();
                i++; Ws(s, ref i);
                if (s[i] == ']') { i++; return a; }
                for (;;)
                {
                    a.Add(Value(s, ref i));
                    Ws(s, ref i);
                    if (s[i] == ',') { i++; continue; }
                    if (s[i] == ']') { i++; return a; }
                    throw new FormatException($"JSON: ',' or ']' expected at {i}");
                }
            }
            if (c == '"') return Str(s, ref i);
            if (string.CompareOrdinal(s, i, "true", 0, 4) == 0) { i += 4; return true; }
            if (string.CompareOrdinal(s, i, "false", 0, 5) == 0) { i += 5; return false; }
            if (string.CompareOrdinal(s, i, "null", 0, 4) == 0) { i += 4; return null; }
            int start = i;
            while (i < s.Length && "+-0123456789.eE".IndexOf(s[i]) >= 0) i++;
            if (start == i) throw new FormatException($"JSON: unexpected '{c}' at {i}");
            return double.Parse(s.Substring(start, i - start), CultureInfo.InvariantCulture);
        }

        static string Str(string s, ref int i)
        {
            if (s[i] != '"') throw new FormatException($"JSON: string expected at {i}");
            i++;
            var sb = new StringBuilder();
            while (s[i] != '"')
            {
                char c = s[i++];
                if (c != '\\') { sb.Append(c); continue; }
                char e = s[i++];
                switch (e)
                {
                    case 'n': sb.Append('\n'); break;
                    case 'r': sb.Append('\r'); break;
                    case 't': sb.Append('\t'); break;
                    case 'b': sb.Append('\b'); break;
                    case 'f': sb.Append('\f'); break;
                    case 'u': sb.Append((char)Convert.ToInt32(s.Substring(i, 4), 16)); i += 4; break;
                    default: sb.Append(e); break;
                }
            }
            i++;
            return sb.ToString();
        }

        public static string Write(object v)
        {
            var sb = new StringBuilder();
            Write(sb, v, 0);
            return sb.ToString();
        }

        static void Write(StringBuilder sb, object v, int depth)
        {
            string pad = new string(' ', (depth + 1) * 2), end = new string(' ', depth * 2);
            switch (v)
            {
                case null: sb.Append("null"); break;
                case string s: Quote(sb, s); break;
                case bool b: sb.Append(b ? "true" : "false"); break;
                case IDictionary d:
                    if (d.Count == 0) { sb.Append("{}"); break; }
                    sb.Append("{\n");
                    bool first = true;
                    foreach (DictionaryEntry kv in d)
                    {
                        if (!first) sb.Append(",\n");
                        first = false;
                        sb.Append(pad); Quote(sb, kv.Key.ToString()); sb.Append(": ");
                        Write(sb, kv.Value, depth + 1);
                    }
                    sb.Append('\n').Append(end).Append('}');
                    break;
                case IEnumerable e:
                    var items = new List<object>();
                    foreach (var x in e) items.Add(x);
                    if (items.Count == 0) { sb.Append("[]"); break; }
                    sb.Append("[\n");
                    for (int k = 0; k < items.Count; k++)
                    {
                        if (k > 0) sb.Append(",\n");
                        sb.Append(pad);
                        Write(sb, items[k], depth + 1);
                    }
                    sb.Append('\n').Append(end).Append(']');
                    break;
                case IFormattable f: sb.Append(f.ToString(null, CultureInfo.InvariantCulture)); break;
                default: Quote(sb, v.ToString()); break;
            }
        }

        static void Quote(StringBuilder sb, string s)
        {
            sb.Append('"');
            foreach (char c in s)
            {
                switch (c)
                {
                    case '"': sb.Append("\\\""); break;
                    case '\\': sb.Append("\\\\"); break;
                    case '\n': sb.Append("\\n"); break;
                    case '\r': sb.Append("\\r"); break;
                    case '\t': sb.Append("\\t"); break;
                    default:
                        if (c < 0x20) sb.Append("\\u").Append(((int)c).ToString("x4"));
                        else sb.Append(c);
                        break;
                }
            }
            sb.Append('"');
        }

        // Lenient accessors for parsed objects.
        public static string S(this object o, string key) =>
            o is Dictionary<string, object> d && d.TryGetValue(key, out var v) && v != null ? Convert.ToString(v, CultureInfo.InvariantCulture) : null;
        public static bool B(this object o, string key, bool def = false) =>
            o is Dictionary<string, object> d && d.TryGetValue(key, out var v) && v is bool b ? b : def;
        public static double N(this object o, string key) =>
            o is Dictionary<string, object> d && d.TryGetValue(key, out var v) && v is double n ? n : 0;
        public static List<object> A(this object o, string key) =>
            o is Dictionary<string, object> d && d.TryGetValue(key, out var v) && v is List<object> a ? a : new List<object>();
        public static Dictionary<string, object> O(this object o, string key) =>
            o is Dictionary<string, object> d && d.TryGetValue(key, out var v) && v is Dictionary<string, object> x ? x : null;
    }
}
