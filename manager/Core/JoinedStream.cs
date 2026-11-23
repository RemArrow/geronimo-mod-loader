// Several files read as one, without joining them on disk: a zip split into parts (Name.zip.001,
// .002, ...) is opened straight from its pieces, so a 3.5 GB map needs no second 3.5 GB copy.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;

namespace GMLManager.Core
{
    public sealed class JoinedStream : Stream
    {
        readonly List<FileStream> _parts;
        readonly long[] _start;
        long _pos;

        public JoinedStream(IEnumerable<string> files)
        {
            _parts = files.Select(f => new FileStream(f, FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 16)).ToList();
            _start = new long[_parts.Count + 1];
            for (int i = 0; i < _parts.Count; i++) _start[i + 1] = _start[i] + _parts[i].Length;
        }

        public override bool CanRead => true;
        public override bool CanSeek => true;
        public override bool CanWrite => false;
        public override long Length => _start[_parts.Count];
        public override long Position { get => _pos; set => _pos = value; }

        public override int Read(byte[] buffer, int offset, int count)
        {
            if (_pos >= Length || count <= 0) return 0;
            int i = Array.BinarySearch(_start, _pos);
            if (i < 0) i = ~i - 1;
            while (i < _parts.Count - 1 && _start[i + 1] <= _pos) i++;   // skip empty parts
            var p = _parts[i];
            p.Position = _pos - _start[i];
            int n = p.Read(buffer, offset, (int)Math.Min(count, _start[i + 1] - _pos));
            _pos += n;
            return n;
        }

        public override long Seek(long offset, SeekOrigin origin)
        {
            _pos = origin == SeekOrigin.Begin ? offset : origin == SeekOrigin.Current ? _pos + offset : Length + offset;
            if (_pos < 0) throw new IOException("Seek before the start.");
            return _pos;
        }

        public override void Flush() { }
        public override void SetLength(long value) => throw new NotSupportedException();
        public override void Write(byte[] buffer, int offset, int count) => throw new NotSupportedException();

        protected override void Dispose(bool disposing)
        {
            if (disposing) foreach (var p in _parts) p.Dispose();
            base.Dispose(disposing);
        }
    }
}
