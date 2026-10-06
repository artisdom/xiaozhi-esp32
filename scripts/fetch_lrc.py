#!/usr/bin/env python3
"""Create an .lrc lyrics file next to each MP3 using LRCLIB (https://lrclib.net).

The ATK-DNESP32S3-BOX2 "Now playing" screen reads "<song>.lrc" from the same folder as
"<song>.mp3" on the SD card. This script searches LRCLIB for every MP3 in the given folders
and writes the best match:

  synced     time-stamped lyrics whose length is within 3 s of the MP3
  scaled     time-stamped lyrics from a slightly different recording, stretched to fit
  estimated  plain lyrics only: lines are spread over the part of the MP3 that is not silent

The kind is stored in a "[by:...]" tag. Existing .lrc files are kept unless --force is given.
Songs without a usable match are listed at the end (and with --report in a JSON file).

    python3 scripts/fetch_lrc.py ~/Music/Super_Simple_Songs ~/Music/jdeg100s
"""
import argparse
import json
import os
import re
import struct
import subprocess
import sys
import time
import unicodedata
import urllib.error
import urllib.parse
import urllib.request

API = "https://lrclib.net/api/search"
HEADERS = {"User-Agent": "xiaozhi-esp32 box2 lyrics fetcher (personal use)"}


def clean_title(path):
    name = os.path.splitext(os.path.basename(path))[0]
    name = re.sub(r"^\s*\d+\s*[.\-]\s*", "", name)  # "12.The Wheels On The Bus" -> title
    name = re.sub(r"\(.*?\)|（.*?）|_batch", "", name)
    return re.sub(r"\s+", " ", name).strip()


def normalize(text):
    text = unicodedata.normalize("NFKC", text).lower()
    return re.sub(r"[\W_]+", "", text)


def mp3_duration(path):
    try:
        from mutagen.mp3 import MP3
        return MP3(path).info.length
    except ImportError:
        out = subprocess.run(
            ["ffprobe", "-v", "error", "-show_entries", "format=duration", "-of", "csv=p=0", path],
            capture_output=True, text=True, check=True).stdout
        return float(out.strip())


def request(query, retries=6):
    url = API + "?" + urllib.parse.urlencode(query)
    for attempt in range(retries):
        try:
            with urllib.request.urlopen(urllib.request.Request(url, headers=HEADERS), timeout=30) as r:
                return json.load(r)
        except (urllib.error.URLError, TimeoutError, json.JSONDecodeError):
            time.sleep(3 * (attempt + 1))  # LRCLIB answers 503 when busy.
    return None


def search(title):
    """Candidates for a song title, best query first."""
    seen, results = set(), []
    for query in ({"track_name": title}, {"q": title}):
        found = request(query)
        for item in found or []:
            if item["id"] not in seen:
                seen.add(item["id"])
                results.append(item)
        time.sleep(0.4)
        if any(i.get("syncedLyrics") for i in results):
            break
    return results


def title_matches(wanted, item):
    a, b = normalize(wanted), normalize(item.get("trackName") or "")
    return bool(a) and bool(b) and (a == b or a in b or b in a)


def lrc_stamp(seconds):
    seconds = max(0.0, seconds)
    return "[%02d:%05.2f]" % (int(seconds // 60), seconds % 60)


def scale_synced(lyrics, ratio):
    def fix(match):
        minutes, seconds = int(match.group(1)), float(match.group(2))
        return lrc_stamp((minutes * 60 + seconds) * ratio)
    return re.sub(r"\[(\d+):(\d+(?:\.\d+)?)\]", fix, lyrics)


def natural_seconds(line):
    """Rough singing time of a line (matches the firmware's per-word estimate)."""
    total = 0.0
    for token in re.findall(r"[㐀-鿿぀-ヿ가-힯]|[^\s㐀-鿿぀-ヿ가-힯]+", line):
        total += 0.4 if re.match(r"[㐀-鿿぀-ヿ가-힯]", token) else 0.2 + 0.06 * len(token)
    return max(total, 0.4)


def sound_span(path, duration):
    """Start and end (s) of the non-silent part of an MP3, from 100 ms RMS windows."""
    raw = subprocess.run(
        ["ffmpeg", "-v", "error", "-i", path, "-ac", "1", "-ar", "8000", "-f", "s16le", "-"],
        capture_output=True, check=True).stdout
    samples = struct.unpack("<%dh" % (len(raw) // 2), raw)
    window = 800
    rms = []
    for i in range(0, len(samples) - window, window):
        chunk = samples[i:i + window]
        rms.append((sum(s * s for s in chunk) / window) ** 0.5)
    if not rms:
        return 0.0, duration
    loud = sorted(rms)[int(len(rms) * 0.9)] or 1.0
    active = [i for i, v in enumerate(rms) if v > loud * 0.12]
    if not active:
        return 0.0, duration
    return active[0] * 0.1, min(duration, (active[-1] + 1) * 0.1)


def estimate_lrc(plain, path, duration):
    lines = [l.strip() for l in plain.splitlines()]
    lines = [l for l in lines if l and not re.fullmatch(r"\[.*\]|\(.*\)", l)]  # drop "[Chorus]"
    if not lines:
        return None
    start, end = sound_span(path, duration)
    weights = [natural_seconds(l) for l in lines]
    scale = max(end - start, 1.0) / sum(weights)
    out, cursor = [], start
    for line, weight in zip(lines, weights):
        out.append(lrc_stamp(cursor) + line)
        cursor += weight * scale
    return "\n".join(out)


def choose(title, duration, items):
    """Return (kind, lyrics text, matched item) or None."""
    matching = [i for i in items if title_matches(title, i)]
    synced = sorted((i for i in matching if i.get("syncedLyrics") and i.get("duration")),
                    key=lambda i: abs(i["duration"] - duration))
    if synced:
        best = synced[0]
        diff = abs(best["duration"] - duration)
        if diff <= 3:
            return "synced", best["syncedLyrics"], best
        if diff <= duration * 0.10:
            return "scaled", scale_synced(best["syncedLyrics"], duration / best["duration"]), best
    plain = [i for i in matching if i.get("plainLyrics")]
    if plain:
        best = min(plain, key=lambda i: abs((i.get("duration") or 0) - duration))
        return "estimated", best["plainLyrics"], best
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("folders", nargs="+")
    parser.add_argument("--force", action="store_true", help="overwrite existing .lrc files")
    parser.add_argument("--dry-run", action="store_true", help="search but do not write files")
    parser.add_argument("--report", help="write a JSON report of the matches")
    args = parser.parse_args()

    report, missing = {}, []
    for folder in args.folders:
        for name in sorted(os.listdir(folder)):
            if not name.lower().endswith(".mp3"):
                continue
            path = os.path.join(folder, name)
            target = os.path.splitext(path)[0] + ".lrc"
            if os.path.exists(target) and not args.force:
                print("keep      ", name)
                continue
            title, duration = clean_title(path), mp3_duration(path)
            choice = choose(title, duration, search(title))
            if not choice:
                print("missing   ", name)
                missing.append(path)
                continue
            kind, lyrics, item = choice
            if kind == "estimated":
                lyrics = estimate_lrc(lyrics, path, duration)
                if not lyrics:
                    missing.append(path)
                    continue
            header = "[ti:%s]\n[ar:%s]\n[by:%s]\n[re:fetch_lrc.py from lrclib.net]\n" % (
                item.get("trackName", title), item.get("artistName", ""), kind)
            report[path] = {"kind": kind, "track": item.get("trackName"), "artist": item.get("artistName"),
                            "lrclib_duration": item.get("duration"), "mp3_duration": round(duration, 1)}
            print("%-10s %s  <- %s / %s (%.0fs vs %.0fs)" % (kind, name, item.get("trackName"),
                  item.get("artistName"), item.get("duration") or 0, duration))
            if not args.dry_run:
                with open(target, "w", encoding="utf-8") as f:
                    f.write(header + lyrics.strip() + "\n")
    if args.report:
        with open(args.report, "w", encoding="utf-8") as f:
            json.dump({"matches": report, "missing": missing}, f, ensure_ascii=False, indent=1)
    counts = {}
    for entry in report.values():
        counts[entry["kind"]] = counts.get(entry["kind"], 0) + 1
    print("\n%s; %d without lyrics" % (", ".join("%d %s" % (n, k) for k, n in sorted(counts.items())) or "none written", len(missing)))
    for path in missing:
        print("  no lyrics:", os.path.basename(path))
    return 0


if __name__ == "__main__":
    sys.exit(main())
