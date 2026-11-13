# -*- coding: utf-8 -*-
"""
Lume -> LX Music 一键同步（由 Lume 原生侧调用，不面向用户直接运行）

做的事：
  1. 读 LX 的 lx.data.db，取出指定歌单
  2. 逐首从解析站拿真实地址并下载（已存在的跳过）
  3. 写同名 .lrc
  4. 内嵌封面
  5. 把 .m4a/.aac 统一转成 192k mp3（保留封面）

进度通过 stdout 逐行输出，供 Lume 读取：
  LVP  {"stage":"download","done":3,"total":99}
  LVEND {"ok":99,"fail":0,"skip":0,"converted":12,"total":99}
  失败时：LVEND {"ok":0,"fail":0,"skip":0,"converted":0,"total":0,"error":"..."}
"""
import base64
import concurrent.futures
import json
import os
import re
import sqlite3
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.parse
import urllib.request

DEFAULT_LIST = 'kg_46915abaf2573ff48bc131bf3e9f2de1'   # 「松清喜欢的音乐」
DEFAULT_OUT = r'D:\Music\LX music'
RESOLVER = 'https://mcp.nianxinxz.com/share/ceshi/{src}.php?id={id}&level={level}&type=mp3'
UA = 'Mozilla/5.0 (Windows NT 10.0; Win64; x64)'
AUDIO_EXT = ('.mp3', '.m4a', '.flac', '.aac', '.ogg')
CONVERT_EXT = ('.m4a', '.aac')
WORKERS = 4

lock = threading.Lock()
done = 0
total = 0


def emit(prefix, payload):
    try:
        sys.stdout.write(prefix + ' ' + json.dumps(payload, ensure_ascii=False) + '\n')
        sys.stdout.flush()
    except Exception:
        pass


def progress(stage, extra=None):
    payload = {'stage': stage, 'done': done, 'total': total}
    if extra:
        payload.update(extra)
    emit('LVP', payload)


def db_path():
    return os.path.join(os.environ.get('APPDATA', ''), 'lx-music-desktop', 'LxDatas', 'lx.data.db')


def open_db():
    path = db_path()
    if not os.path.exists(path):
        raise RuntimeError('找不到 LX 数据库，请先打开一次 LX Music')
    conn = sqlite3.connect('file:' + path + '?mode=ro', uri=True, timeout=10)
    conn.row_factory = sqlite3.Row
    return conn


def sanitize(name):
    s = re.sub(r'[\\/:*?"<>|\r\n\t]', '_', name or '').strip().strip('.')
    return s[:120] or 'unknown'


def plan_names(rows):
    """Give every row a file name that no other row in the same playlist claims.

    The resolver hands out the same song from several albums (different songIds,
    identical title and singer), and "title - singer" alone would let the second
    download overwrite the first. Only the colliding rows grow an album suffix,
    so the ordinary case keeps the short, predictable name.
    """
    bases = []
    for row in rows:
        title = row['name']
        singer = row['singer'] or ''
        bases.append(sanitize('%s - %s' % (title, singer) if singer else title))

    groups = {}
    for index, base in enumerate(bases):
        groups.setdefault(base.lower(), []).append(index)
    for members in groups.values():
        if len(members) < 2:
            continue
        for index in members:
            try:
                album = (json.loads(rows[index]['meta'] or '{}').get('albumName') or '').strip()
            except Exception:
                album = ''
            if album and sanitize(album).lower() != bases[index].lower():
                bases[index] = sanitize('%s (%s)' % (bases[index], album))
            else:
                bases[index] = sanitize('%s (%s)' % (bases[index], rows[index]['source']))

    # Two albums with the same name would collide again; the playlist order is
    # stable, so a counter keeps those apart without touching anything else.
    seen = {}
    for index, base in enumerate(bases):
        key = base.lower()
        seen[key] = seen.get(key, 0) + 1
        if seen[key] > 1:
            bases[index] = sanitize('%s %d' % (base, seen[key]))
    return bases


def resolve(src, ident, level):
    url = RESOLVER.format(src=src, id=urllib.parse.quote(str(ident)), level=level)

    class NoRedirect(urllib.request.HTTPRedirectHandler):
        def redirect_request(self, *a, **k):
            return None

    opener = urllib.request.build_opener(NoRedirect)
    req = urllib.request.Request(url, headers={'User-Agent': UA})
    try:
        return opener.open(req, timeout=20).headers.get('Location')
    except urllib.error.HTTPError as e:
        return e.headers.get('Location')


def download(url, dest):
    tmp = dest + '.part'
    with urllib.request.urlopen(urllib.request.Request(url, headers={'User-Agent': UA}), timeout=90) as r, \
            open(tmp, 'wb') as f:
        while True:
            chunk = r.read(1 << 16)
            if not chunk:
                break
            f.write(chunk)
    os.replace(tmp, dest)


def ext_of(url):
    path = urllib.parse.urlparse(url).path.lower()
    for ext in AUDIO_EXT:
        if ext in path:
            return '.m4a' if ext == '.aac' else ext   # 解析站的 .aac 实际是 mp4 容器
    return '.mp3'


def existing(base):
    for ext in AUDIO_EXT:
        if os.path.exists(base + ext):
            return base + ext
    return None


def has_cover(path):
    # The tag sits at the front for mp3 and near the end for m4a, so the middle
    # of a several-megabyte file never has to be read.
    try:
        size = os.path.getsize(path)
        with open(path, 'rb') as f:
            head = f.read(262144)
            if b'APIC' in head or b'covr' in head:
                return True
            if size > 524288:
                f.seek(-262144, os.SEEK_END)
                return b'APIC' in f.read() or b'covr' in f.read()
            f.seek(len(head))
            return b'APIC' in f.read() or b'covr' in f.read()
    except Exception:
        return True


COVER_SIZE = 320
COVER_QUALITY = 6


def shrink_cover(src):
    """Scales a cover down before embedding.

    Lume keeps every page's artwork in memory against a fixed byte budget, so a
    library of full-size covers runs out of room partway down the list and the
    remaining rows lose their picture. A small cover costs far less than the
    tag it saves.
    """
    small = src + '.small.' + str(os.getpid()) + '.' + str(threading.get_ident()) + '.jpg'
    cmd = ['ffmpeg', '-y', '-loglevel', 'error', '-i', src,
           '-vf', 'scale=%d:%d:force_original_aspect_ratio=decrease' % (COVER_SIZE, COVER_SIZE),
           '-frames:v', '1', '-q:v', str(COVER_QUALITY), small]
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True)
    except Exception:
        return src
    if proc.returncode != 0 or not os.path.exists(small) or os.path.getsize(small) < 512:
        if os.path.exists(small):
            try:
                os.remove(small)
            except Exception:
                pass
        return src
    return small


def embed_cover(path, img):
    # Downloads run on several threads, so the scratch file has to belong to this
    # call alone; a shared name let one thread's cover land in another's track.
    handle, tmp = tempfile.mkstemp(prefix='lume_lx_cover_', suffix='.jpg')
    os.close(handle)
    try:
        with open(tmp, 'wb') as f:
            f.write(img)
    except Exception:
        try:
            os.remove(tmp)
        except Exception:
            pass
        return False
    small = shrink_cover(tmp)
    try:
        return _embed(path, small)
    finally:
        for scratch in {tmp, small}:
            try:
                os.remove(scratch)
            except Exception:
                pass


def _embed(path, tmp):
    ext = os.path.splitext(path)[1].lower()
    out = path + '.embed' + ext
    if ext == '.mp3':
        cmd = ['ffmpeg', '-y', '-loglevel', 'error', '-i', path, '-i', tmp,
               '-map', '0:a', '-map', '1:v', '-c', 'copy', '-id3v2_version', '3',
               '-metadata:s:v', 'title=Album cover',
               '-metadata:s:v', 'comment=Cover (front)', out]
    else:
        cmd = ['ffmpeg', '-y', '-loglevel', 'error', '-i', path, '-i', tmp,
               '-map', '0:a', '-map', '1:v', '-c', 'copy', '-disposition:v', 'attached_pic', out]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode != 0 or not os.path.exists(out):
        if os.path.exists(out):
            try:
                os.remove(out)
            except Exception:
                pass
        return False
    try:
        os.replace(out, path)
    except Exception:
        try:
            os.remove(out)
        except Exception:
            pass
        return False
    return True


def convert(src):
    """把 m4a/aac 转成 192k mp3，成功返回新路径，失败返回 None（保留原文件）。"""
    dest = os.path.splitext(src)[0] + '.mp3'
    cmd = ['ffmpeg', '-y', '-loglevel', 'error', '-i', src, '-map', '0:a']
    if has_cover(src):
        cmd += ['-map', '0:v', '-c:v', 'copy', '-id3v2_version', '3',
                '-metadata:s:v', 'title=Album cover',
                '-metadata:s:v', 'comment=Cover (front)', '-disposition:v', 'attached_pic']
    cmd += ['-c:a', 'libmp3lame', '-b:a', '192k', '-map_metadata', '0', dest]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode != 0 or not os.path.exists(dest) or os.path.getsize(dest) < 1024:
        if os.path.exists(dest):
            try:
                os.remove(dest)
            except Exception:
                pass
        return None
    try:
        os.remove(src)
    except Exception:
        pass
    return dest


def hash_of(meta, quality):
    qualities = meta.get('_qualitys') or {}
    for level in [quality, '320k', '128k', 'flac']:
        h = (qualities.get(level) or {}).get('hash')
        if h:
            return h
    return None


def lookup_cover(meta):
    """Finds a cover for tracks whose playlist entry carries none.

    LX stores only what the list endpoint returned, and some tracks come back
    without a picture. The song-info endpoint knows the album image and can be
    asked with the hash the track already carries.
    """
    hashes = []
    for group in ('_qualitys', 'qualitys'):
        values = meta.get(group) or {}
        if isinstance(values, dict):
            values = values.values()
        for item in values:
            if isinstance(item, dict) and item.get('hash'):
                hashes.append(str(item['hash']))
    if meta.get('hash'):
        hashes.append(str(meta['hash']))
    for h in hashes[:1]:
        url = ('https://m.kugou.com/app/i/getSongInfo.php?cmd=playInfo&hash=' + h)
        try:
            req = urllib.request.Request(url, headers={'User-Agent': UA})
            with urllib.request.urlopen(req, timeout=20) as u:
                info = json.loads(u.read().decode('utf-8', 'replace'))
        except Exception:
            continue
        for key in ('album_img', 'imgUrl'):
            pic = info.get(key)
            if pic:
                return pic.replace('{size}', '480')
    return None


def fetch_lyric(meta, title, singer):
    """Ask Kugou's lyric service when the LX cache has no entry for a track.

    The local cache only covers tracks LX itself has opened, so a freshly added
    song arrives with audio and no lyric. A missing .lrc is silent, not an
    error, which is why this fallback exists at all.
    """
    hashes = []
    for bucket in (meta.get('_qualitys') or {}).values():
        if isinstance(bucket, dict) and bucket.get('hash'):
            hashes.append(bucket['hash'])
    for bucket in meta.get('qualitys') or []:
        if isinstance(bucket, dict) and bucket.get('hash'):
            hashes.append(bucket['hash'])
    params = {'ver': '1', 'man': 'yes', 'client': 'pc',
              'keyword': ('%s %s' % (title, singer)).strip()}
    if hashes:
        params['hash'] = hashes[0]
    try:
        search = 'https://lyrics.kugou.com/search?' + urllib.parse.urlencode(params)
        payload = json.load(urllib.request.urlopen(
            urllib.request.Request(search, headers={'User-Agent': UA}), timeout=20))
    except Exception:
        return None
    wanted = (title or '').strip()
    for candidate in payload.get('candidates') or []:
        if (candidate.get('song') or '').strip() != wanted:
            continue
        try:
            query = urllib.parse.urlencode({
                'ver': '1', 'client': 'pc', 'fmt': 'lrc', 'charset': 'utf8',
                'id': candidate['id'], 'accesskey': candidate['accesskey']})
            body = json.load(urllib.request.urlopen(
                urllib.request.Request('https://lyrics.kugou.com/download?' + query,
                                       headers={'User-Agent': UA}), timeout=20))
            text = base64.b64decode(body.get('content') or '').decode('utf-8', 'replace')
            if '[00:' in text:
                return text.lstrip('\ufeff')
        except Exception:
            continue
    return None


def process(index, row, out_dir, lyrics, quality, want_lyric, want_cover, counters, base):
    global done
    try:
        meta = json.loads(row['meta'] or '{}')
    except Exception:
        meta = {}
    src = row['source']
    title = row['name']
    singer = row['singer'] or ''
    audio = existing(os.path.join(out_dir, base))
    result = 'ok'

    if audio is None:
        ident = hash_of(meta, quality) if src == 'kg' else meta.get('songId')
        if not ident:
            result = 'noid'
        else:
            url = None
            for _ in range(3):
                try:
                    url = resolve(src, ident, quality)
                except Exception:
                    url = None
                if url and '/None' not in url:
                    break
                time.sleep(1.5)
            if not url or '/None' in url:
                result = 'fail'
            else:
                audio = os.path.join(out_dir, base + ext_of(url))
                try:
                    download(url, audio)
                except Exception:
                    if os.path.exists(audio + '.part'):
                        try:
                            os.remove(audio + '.part')
                        except Exception:
                            pass
                    result = 'fail'
    else:
        result = 'skip'

    if result in ('ok', 'skip') and audio:
        if want_lyric:
            main_lyric = (lyrics.get(row['id']) or {}).get('lyric')
            if not main_lyric and not os.path.exists(os.path.splitext(audio)[0] + '.lrc'):
                main_lyric = fetch_lyric(meta, title, singer)
            if main_lyric:
                lrc = os.path.splitext(audio)[0] + '.lrc'
                try:
                    with open(lrc, 'w', encoding='utf-8', newline='\n') as f:
                        f.write(main_lyric if main_lyric.endswith('\n') else main_lyric + '\n')
                except Exception:
                    pass
        # A track that is already downloaded may still be missing its picture,
        # so the cover is repaired for skipped rows too.
        if want_cover and result in ('ok', 'skip'):
            pic = meta.get('picUrl') or meta.get('img')
            if not pic:
                pic = lookup_cover(meta)
            if pic and not has_cover(audio):
                try:
                    req = urllib.request.Request(
                        pic, headers={'User-Agent': UA, 'Referer': 'http://imge.kugou.com/'})
                    with urllib.request.urlopen(req, timeout=30) as u:
                        embed_cover(audio, u.read())
                except Exception:
                    pass

    with lock:
        done += 1
        counters[result] = counters.get(result, 0) + 1
        progress('download')
    return result


def main():
    try:
        sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    except Exception:
        pass

    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument('--list', default=DEFAULT_LIST)
    ap.add_argument('--out', default=DEFAULT_OUT)
    ap.add_argument('--quality', default='standard')
    ap.add_argument('--no-lyric', action='store_true')
    ap.add_argument('--no-cover', action='store_true')
    ap.add_argument('--no-convert', action='store_true')
    args = ap.parse_args()

    global total
    counters = {}
    try:
        conn = open_db()
        rows = [dict(r) for r in conn.execute(
            'select id,listId,name,singer,source,meta from my_list_music_info where listId=? order by rowid',
            (args.list,))]
        lyrics = {}
        if not args.no_lyric:
            for r in conn.execute('select id,type,text from lyric'):
                if not r['text']:
                    continue
                try:
                    lyrics.setdefault(r['id'], {})[r['type']] = base64.b64decode(r['text']).decode('utf-8', 'replace')
                except Exception:
                    pass
        conn.close()
    except Exception as error:
        emit('LVEND', {'ok': 0, 'fail': 0, 'skip': 0, 'converted': 0, 'total': 0, 'error': str(error)})
        return 1

    total = len(rows)
    if not total:
        emit('LVEND', {'ok': 0, 'fail': 0, 'skip': 0, 'converted': 0, 'total': 0,
                       'error': '歌单是空的，先在 LX 里加歌'})
        return 1

    try:
        os.makedirs(args.out, exist_ok=True)
    except Exception as error:
        emit('LVEND', {'ok': 0, 'fail': 0, 'skip': 0, 'converted': 0, 'total': 0,
                       'error': '无法创建目录：%s' % error})
        return 1

    names = plan_names(rows)
    progress('download')
    with concurrent.futures.ThreadPoolExecutor(WORKERS) as pool:
        list(pool.map(lambda pair: process(pair[0], pair[1], args.out, lyrics, args.quality,
                                           not args.no_lyric, not args.no_cover, counters,
                                           names[pair[0]]),
                      enumerate(rows)))

    converted = 0
    if not args.no_convert:
        pending = [os.path.join(args.out, name) for name in sorted(os.listdir(args.out))
                   if os.path.splitext(name)[1].lower() in CONVERT_EXT]
        for index, path in enumerate(pending):
            if convert(path):
                converted += 1
            emit('LVP', {'stage': 'convert', 'done': index + 1, 'total': len(pending)})

    emit('LVEND', {'ok': counters.get('ok', 0), 'fail': counters.get('fail', 0),
                   'skip': counters.get('skip', 0), 'noid': counters.get('noid', 0),
                   'converted': converted, 'total': total})
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except Exception as error:
        emit('LVEND', {'ok': 0, 'fail': 0, 'skip': 0, 'converted': 0, 'total': 0, 'error': str(error)})
        sys.exit(1)
