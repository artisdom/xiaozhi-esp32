#!/usr/bin/env python3
"""Time-stamp song lyrics against the real recording with speech recognition.

Two steps, both optional extras on top of scripts/fetch_lrc.py:

  transcribe  run faster-whisper (pip install faster-whisper) on every MP3 and store the words
              with their times in a JSON file per song. English or Chinese is picked from the
              file name (Chinese needs a CJK character in it).
  import-text split text files with "[Song title]" sections into "<song>.txt" reference lyrics
              next to the MP3 whose title matches best (credit lines such as "词/曲/领唱" are
              dropped; existing .txt files are kept).
  align       build "<song>.lrc" from those words. If a reference text exists next to the MP3
              ("<song>.txt", else an .lrc made by fetch_lrc.py) its words are matched against
              what was recognised, so the text is right and the times come from the audio;
              verses that repeat in the recording are repeated. Without a reference the
              recognised text is used (tag [by:asr]) and should be proof-read.

Every word of the result carries its own "<mm:ss.xx>" stamp, which the BOX2 lyrics screen
uses for word-by-word highlighting.

    python3 scripts/asr_lrc.py transcribe ~/Music/Super_Simple_Songs --out asr/
    python3 scripts/asr_lrc.py import-text ~/Music/jdeg100s --texts ~/Music/lyrics/*.txt
    python3 scripts/asr_lrc.py align ~/Music/Super_Simple_Songs --asr asr/
"""
import argparse
import difflib
import glob
import json
import os
import re
import subprocess
import sys

CJK = r"㐀-鿿぀-ヿ가-힯"
HALLUCINATIONS = ("thank you for watching", "subscribe", "字幕", "请不吝点赞", "謝謝觀看")


def song_key(path):
    return os.path.basename(os.path.dirname(os.path.abspath(path)))[:3] + "__" + os.path.basename(path)


def is_chinese(path):
    return bool(re.search("[%s]" % CJK, os.path.basename(path)))


def transcribe(args):
    import numpy as np
    from faster_whisper import WhisperModel

    os.makedirs(args.out, exist_ok=True)
    model = WhisperModel(args.model, device="cpu", compute_type="int8", cpu_threads=args.threads)
    files = [f for d in args.folders for f in sorted(glob.glob(os.path.join(d, "*.[mM][pP]3")))]
    for path in files[args.part::args.parts]:
        out = os.path.join(args.out, song_key(path) + ".json")
        if os.path.exists(out):
            continue
        raw = subprocess.run(["ffmpeg", "-v", "error", "-i", path, "-ac", "1", "-ar", "16000",
                              "-f", "f32le", "-"], capture_output=True, check=True).stdout
        zh = is_chinese(path)
        segments, info = model.transcribe(
            np.frombuffer(raw, dtype=np.float32), word_timestamps=True, beam_size=1,
            condition_on_previous_text=False, language="zh" if zh else "en",
            initial_prompt="以下是普通话儿歌的歌词，使用简体中文。" if zh else None,
            temperature=0.0, compression_ratio_threshold=2.0)
        data = [{"start": s.start, "end": s.end, "text": s.text,
                 "words": [{"w": w.word, "s": w.start, "e": w.end, "p": w.probability} for w in s.words or []]}
                for s in segments]
        with open(out, "w", encoding="utf-8") as f:
            json.dump({"file": path, "lang": info.language, "duration": info.duration, "segments": data},
                      f, ensure_ascii=False)
        print("transcribed", os.path.basename(path), flush=True)


# --- alignment -------------------------------------------------------------------------------

try:
    from opencc import OpenCC
    _t2s = OpenCC("t2s").convert
except ImportError:  # Traditional/simplified differences then count as mismatches.
    def _t2s(text):
        return text


def tokenize(text, zh):
    """Split text into display tokens: words, or single characters for Chinese."""
    pattern = "[%s][^%s\\w\\s]*|[^\\s%s]+" % (CJK, CJK, CJK) if zh else r"\S+"
    tokens = re.findall(pattern, text)
    result = []
    for tok in tokens:
        # A Latin word glued to CJK text (e.g. "ABC字母") is split by the pattern above.
        result.append(tok)
    return result


def key_of(token, zh):
    token = _t2s(token).lower()
    return re.sub(r"[^\w]", "", token).replace("_", "")


def asr_tokens(data, zh):
    """Recognised words as (key, start, end, shown text); Chinese is split into characters."""
    out = []
    for seg in data["segments"]:
        text = seg["text"].strip().lower()
        if any(h in text for h in HALLUCINATIONS):
            continue
        words = seg["words"] or [{"w": seg["text"], "s": seg["start"], "e": seg["end"], "p": 1.0}]
        for w in words:
            if zh:
                pieces = [c for c in _t2s(w["w"]) if re.match(r"\w", c)]
                shown = pieces
            else:
                shown = [w["w"].strip()]
                pieces = [key_of(shown[0], zh)]
            if not pieces or not pieces[0]:
                continue
            step = (w["e"] - w["s"]) / len(pieces)
            for i, (key, text) in enumerate(zip(pieces, shown)):
                out.append((key, w["s"] + i * step, w["s"] + (i + 1) * step, text))
    return out


def segment_lines(tokens, zh):
    """Lines of (shown text, start) from recognised words.

    A new line starts after a pause, after a sentence end, after a comma once the line is
    already long, or when the line gets too long."""
    limit = 12 if zh else 9
    lines, current, last_end = [], [], 0.0
    for _, start, end, shown in tokens:
        if current:
            previous = current[-1][0]
            sentence_end = re.search(r"[.!?。！？]$", previous) and len(current) >= 2
            comma_break = re.search(r"[,，、;；]$", previous) and len(current) >= (7 if zh else 5)
            if start - last_end > 0.7 or sentence_end or comma_break or len(current) >= limit:
                lines.append(current)
                current = []
        current.append((shown, start))
        last_end = end
    if current:
        lines.append(current)
    return lines


def read_reference(path):
    base = os.path.splitext(path)[0]
    for ext in (".txt", ".lrc"):
        if os.path.exists(base + ext):
            text = open(base + ext, encoding="utf-8-sig").read()
            lines = []
            for line in text.splitlines():
                if re.match(r"\s*\[(ti|ar|al|by|re|ve|offset|length|au|la):", line):
                    continue
                line = re.sub(r"\[\d+:\d+(?:[.:]\d+)?\]|<\d+:\d+(?:[.:]\d+)?>", "", line).strip()
                if line and not re.fullmatch(r"\[.*\]|\(.*\)", line):
                    lines.append(line)
            if lines:
                return lines
    return None


def align_reference(ref_lines, toks, zh, duration):
    """Place the reference lines on the recognised timeline, repeating them for repeated verses.

    Returns a list of lines, each a list of (display token, start seconds)."""
    ref = [(tok, key_of(tok, zh)) for line in ref_lines for tok in tokenize(line, zh)]
    line_of = [i for i, line in enumerate(ref_lines) for _ in tokenize(line, zh)]
    keys = [k for _, k in ref]
    out, pos = [], 0
    default_step = 0.4 if zh else 0.35
    first_keys = keys[:min(len(keys), len(tokenize(ref_lines[0], zh)), 8)]

    def find_start(begin):
        """Earliest recognised position where the first reference line is heard."""
        n = len(first_keys)
        for s in range(begin, len(toks)):
            seg = [t[0] for t in toks[s:s + n + 3]]
            blocks = difflib.SequenceMatcher(None, first_keys, seg, autojunk=False).get_matching_blocks()
            if sum(b.size for b in blocks) >= max(2, 0.5 * n):
                return s
        return None

    while pos < len(toks):
        start = find_start(pos)
        if start is None:
            if out:
                break
            start = pos
        # Only about one verse of recognised words, so a later repeat cannot steal the match.
        window = toks[start:start + int(len(ref) * 1.15) + 2]
        sm = difflib.SequenceMatcher(None, keys, [t[0] for t in window], autojunk=False)
        blocks = [b for b in sm.get_matching_blocks() if b.size]
        matched = sum(b.size for b in blocks)
        if matched < max(3, 0.25 * len(ref)):
            break
        anchor = {}
        for b in blocks:
            for i in range(b.size):
                anchor[b.a + i] = window[b.b + i]
        times = [None] * len(ref)
        for i, token in anchor.items():
            times[i] = token[1]
        last_asr = max(b.b + b.size for b in blocks)
        # Fill the gaps between anchors evenly; extend before the first and after the last.
        known = [i for i, t in enumerate(times) if t is not None]
        for a, b in zip(known, known[1:]):
            for i in range(a + 1, b):
                times[i] = times[a] + (times[b] - times[a]) * (i - a) / (b - a)
        first, last = known[0], known[-1]
        for i in range(first - 1, -1, -1):
            times[i] = max(0.0, times[i + 1] - default_step)
        for i in range(last + 1, len(ref)):
            times[i] = min(duration, times[i - 1] + default_step)
        lines, heard = {}, {}
        for idx, ((tok, _), t, li) in enumerate(zip(ref, times, line_of)):
            lines.setdefault(li, []).append((tok, t))
            heard[li] = heard.get(li, 0) + (idx in anchor)
        # A reference line with (almost) nothing heard belongs to another song or verse.
        out.extend(lines[i] for i in sorted(lines)
                   if heard[i] >= max(1, 0.15 * len(lines[i])))
        new_pos = start + last_asr
        if new_pos <= pos:
            break
        pos = new_pos
    return out


def norm_title(title):
    title = re.sub(r"[(（\[【].*?[)）\]】]", "", title)
    return re.sub(r"[\W_]+", "", _t2s(title)).lower()


SENTENCE_PUNCTUATION = "，。,.!?！？；;：:、"
CREDIT = re.compile(r"(作词|作曲|编曲|词|曲|配器|领唱|独唱|合唱|演唱)\s*[:：]|\S+词\s+\S+曲|配器|领唱|独唱|"
                    r"[Cc]opyright|^\s*[Ww]ords?\b|^\s*[Mm]usic\b")


def title_of_line(raw, known):
    """Normalised title if `raw` looks like a song title line, else None.

    Recognised: "[Title]", "12. Title" (short, no sentence punctuation) or a line that is
    exactly one of the MP3 titles being looked for."""
    line = raw.strip()
    m = re.match(r"^(?:\d+\s*[.、．)]\s*)?[\[【《](.+?)[\]】》]\s*$", line)
    if m:
        return norm_title(m.group(1))
    m = re.match(r"^\d+\s*[.、．)]\s*(.+)$", line)
    if m and len(m.group(1)) <= 16 and not any(c in m.group(1) for c in SENTENCE_PUNCTUATION):
        return norm_title(m.group(1))
    if line and len(line) <= 24 and norm_title(line) in known:
        return norm_title(line)
    return None


def parse_sections(path, known=()):
    """{normalized title: lines}; the longest section wins when a title occurs twice.

    A short single-line paragraph without punctuation followed by a longer paragraph also
    starts a section, which covers texts that list a bare title above each song."""
    raw_lines = open(path, encoding="utf-8-sig").read().splitlines()
    sections, title, lines = {}, None, []

    def close():
        if title and lines and len(lines) > len(sections.get(title, ())):
            sections[title] = list(lines)

    for i, raw in enumerate(raw_lines):
        found = title_of_line(raw, known)
        stripped = raw.strip()
        if (not found and stripped and 1 < len(stripped) <= 10
                and not any(c in stripped for c in SENTENCE_PUNCTUATION)
                and (i == 0 or not raw_lines[i - 1].strip())
                and i + 2 < len(raw_lines) and not raw_lines[i + 1].strip()
                and len(raw_lines[i + 2].strip()) > len(stripped)):
            found = norm_title(stripped)
        if found:
            close()
            title, lines = found, []
        elif title and stripped and not CREDIT.search(raw):
            lines.append(stripped)
    close()
    return sections


def import_text(args):
    mp3s = [p for folder in args.folders for p in sorted(glob.glob(os.path.join(folder, "*.[mM][pP]3")))]
    known = {norm_title(os.path.splitext(os.path.basename(p))[0]) for p in mp3s}
    sections = {}
    for path in args.texts:
        for title, lines in parse_sections(path, known).items():
            if len(lines) > len(sections.get(title, ())):
                sections[title] = lines
    for path in mp3s:
        name = norm_title(os.path.splitext(os.path.basename(path))[0])
        target = os.path.splitext(path)[0] + ".txt"
        if os.path.exists(target):
            continue
        best = None
        for title in sections:
            if title == name:
                score = 2.0
            elif min(len(title), len(name)) >= 2 and (title in name or name in title):
                score = 1.0 + min(len(title), len(name)) / max(len(title), len(name))
            else:
                score = difflib.SequenceMatcher(None, title, name).ratio()
                if score < 0.8:
                    continue
            if best is None or score > best[0]:
                best = (score, title)
        if best:
            with open(target, "w", encoding="utf-8") as f:
                # Traditional Chinese text is shown in simplified characters, like the font.
                f.write("\n".join(_t2s(line) for line in sections[best[1]]) + "\n")
            print("text      %s <- [%s] (%d lines)" % (os.path.basename(path), best[1], len(sections[best[1]])))
        else:
            print("no text   ", os.path.basename(path))


def fmt(seconds):
    seconds = max(0.0, seconds)
    return "<%02d:%05.2f>" % (int(seconds // 60), seconds % 60)


def write_lrc(path, lines, zh, by, title):
    rows = []
    last_start = -1.0
    for line in lines:
        if not line:
            continue
        start = max(line[0][1], last_start + 0.05)
        parts = []
        for tok, t in line:
            parts.append(fmt(max(t, start)) + tok + ("" if zh else " "))
        rows.append("[%02d:%05.2f]%s" % (int(start // 60), start % 60, "".join(parts).rstrip()))
        last_start = start
    with open(os.path.splitext(path)[0] + ".lrc", "w", encoding="utf-8") as f:
        f.write("[ti:%s]\n[by:%s]\n[re:asr_lrc.py]\n%s\n" % (title, by, "\n".join(rows)))
    return len(rows)


def align(args):
    files = [f for d in args.folders for f in sorted(glob.glob(os.path.join(d, "*.[mM][pP]3")))]
    for path in files:
        name = os.path.basename(path)
        asr_file = os.path.join(args.asr, song_key(path) + ".json")
        if not os.path.exists(asr_file):
            print("no asr   ", name)
            continue
        data = json.load(open(asr_file, encoding="utf-8"))
        zh = is_chinese(path) or data.get("lang") == "zh"
        toks = asr_tokens(data, zh)
        if not toks:
            print("silent   ", name)
            continue
        ref = read_reference(path)
        title = os.path.splitext(name)[0]
        if ref:
            lines = align_reference(ref, toks, zh, data["duration"])
            by = "asr-aligned"
            if not lines:  # Reference does not match the recording: fall back to what was heard.
                ref = None
        if not ref:
            lines, by = segment_lines(toks, zh), "asr"
        lrc = os.path.splitext(path)[0] + ".lrc"
        if os.path.exists(lrc) and not args.force and "[by:asr" in open(lrc, encoding="utf-8").read():
            print("keep     ", name)  # Already produced by this tool (possibly proof-read).
            continue
        count = write_lrc(path, lines, zh, by, title)
        print("%-12s %s (%d lines)" % (by, name, count))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    t = sub.add_parser("transcribe")
    t.add_argument("folders", nargs="+")
    t.add_argument("--out", required=True)
    t.add_argument("--model", default="small")
    t.add_argument("--threads", type=int, default=8)
    t.add_argument("--part", type=int, default=0, help="process every PARTS-th file starting here")
    t.add_argument("--parts", type=int, default=1)
    i = sub.add_parser("import-text")
    i.add_argument("folders", nargs="+")
    i.add_argument("--texts", nargs="+", required=True, help="text files with [Title] sections")
    a = sub.add_parser("align")
    a.add_argument("folders", nargs="+")
    a.add_argument("--asr", required=True)
    a.add_argument("--force", action="store_true", help="also redo files made by this tool")
    args = parser.parse_args()
    {"transcribe": transcribe, "align": align, "import-text": import_text}[args.command](args)
    return 0


if __name__ == "__main__":
    sys.exit(main())
