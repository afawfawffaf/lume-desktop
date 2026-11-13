"""Re-embeds the cover already inside each audio file at a smaller size.

Lume keeps a page's artwork in memory against a fixed byte budget. Full-size
covers exhaust it partway down a long list, and every row after that point shows
no picture. Shrinking what is already embedded fixes existing files without
downloading anything again.
"""

import os
import subprocess
import sys

SIZE = 320
QUALITY = 6
EXTS = ('.mp3', '.m4a', '.flac', '.ogg')


def cover_size(path):
    """Returns the attached picture's byte size, or 0 when there is none."""
    cmd = ['ffprobe', '-v', 'error', '-select_streams', 'v:0',
           '-show_entries', 'stream=codec_name', '-of', 'csv=p=0', path]
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True)
    except Exception:
        return 0
    if proc.returncode != 0 or not proc.stdout.strip():
        return 0
    return 1


def shrink(path):
    ext = os.path.splitext(path)[1].lower()
    tmp = path + '.cover.jpg'
    cmd = ['ffmpeg', '-y', '-loglevel', 'error', '-i', path, '-map', '0:v:0',
           '-vf', 'scale=%d:%d:force_original_aspect_ratio=decrease' % (SIZE, SIZE),
           '-frames:v', '1', '-q:v', str(QUALITY), tmp]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode != 0 or not os.path.exists(tmp):
        _drop(tmp)
        return 'no-cover'
    before = 0
    try:
        before = _attached_size(path)
    except Exception:
        pass
    out = path + '.re' + ext
    if ext == '.ogg':
        _drop(tmp)
        return 'skip'
    cmd = ['ffmpeg', '-y', '-loglevel', 'error', '-i', path, '-i', tmp,
           '-map', '0:a', '-map', '1:v', '-c:a', 'copy', '-c:v', 'copy',
           '-id3v2_version', '3',
           '-metadata:s:v', 'title=Album cover',
           '-metadata:s:v', 'comment=Cover (front)',
           '-disposition:v', 'attached_pic', out]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    _drop(tmp)
    if proc.returncode != 0 or not os.path.exists(out) or os.path.getsize(out) < 1024:
        _drop(out)
        return 'fail'
    try:
        os.replace(out, path)
    except Exception:
        _drop(out)
        return 'locked'
    after = _attached_size(path)
    return 'ok %d -> %d' % (before, after)


def _attached_size(path):
    cmd = ['ffprobe', '-v', 'error', '-select_streams', 'v:0',
           '-show_entries', 'packet=size', '-of', 'csv=p=0', path]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    total = 0
    for line in proc.stdout.split():
        try:
            total += int(line.strip(','))
        except Exception:
            pass
    return total


def _drop(path):
    try:
        if os.path.exists(path):
            os.remove(path)
    except Exception:
        pass


def main():
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    root = sys.argv[1] if len(sys.argv) > 1 else r'D:\Music\LX music'
    if not os.path.isdir(root):
        sys.exit('目录不存在：' + root)
    counts = {}
    for name in sorted(os.listdir(root)):
        if not name.lower().endswith(EXTS):
            continue
        path = os.path.join(root, name)
        result = shrink(path)
        key = result.split()[0]
        counts[key] = counts.get(key, 0) + 1
        if key == 'ok':
            print('%s  %s' % (result, name))
    print(counts)


if __name__ == '__main__':
    main()
