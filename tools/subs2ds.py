"""Subtitles for the spoken lines and the movies, made from your own disc's audio.

The English PS1 disc has no text for what is spoken: the room messages that
would hold it are empty (docs/engine-notes.md). This tool runs speech
recognition (whisper.cpp's whisper-cli, https://github.com/ggml-org/whisper.cpp)
over the converted clips and writes the result as text with timings. Nothing
of it belongs in the repository: like every converted file it is made
locally from the disc.

Input: the converted DS files (build/ds): voice/*.bin (RVC1, IMA-ADPCM) and
movie/*.bin (RMV1 / RMV2 / RMV3, PCM after the frames). The audio is written as WAV
to a work folder and transcribed with timestamps; results are cached there
(a clip is only transcribed again when its WAV changed).

Output subs/subs.txt, read by source/subs.c:
  @<clip>                   a voice clip (s<stage>_<nn>, c_<nn>) or movie name
  <start ms> <end ms> <text>  one line per segment, ASCII

Usage: python3 -I subs2ds.py build/ds out.txt [work_dir]
  WHISPER_CLI   whisper-cli to run (default: whisper-cli on the PATH)
  WHISPER_MODEL the model (default: ~/.cache/whisper-cpp/ggml-small.en.bin)
"""
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import wave

VOICE_MAGIC = 0x31435652   # "RVC1"
MOVIE_MAGICS = (0x31564D52, 0x32564D52, 0x33564D52)  # "RMV1" 16-bit, "RMV2" / "RMV3" 8-bit audio
MIN_SECONDS = 0.8          # shorter clips are screams and grunts
MAX_CHARS = 110            # one segment fits the 4-line box (30 x 4)

IMA_STEPS = [7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66,
             73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408,
             449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878,
             2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845,
             8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086,
             29794, 32767]
IMA_INDEX = [-1, -1, -1, -1, 2, 4, 6, 8]


def ima_decode(body, samples):
    """The DS ADPCM of tools/xa2ds.py: u16 first sample, u16 index, nibbles low first."""
    pred, index = struct.unpack_from("<hH", body, 0)
    out = [pred]
    for b in body[4:]:
        for nib in (b & 15, b >> 4):
            if len(out) >= samples:
                return out
            step = IMA_STEPS[index]
            delta = step >> 3
            if nib & 4:
                delta += step
            if nib & 2:
                delta += step >> 1
            if nib & 1:
                delta += step >> 2
            pred = max(-32768, min(32767, pred - delta if nib & 8 else pred + delta))
            index = max(0, min(88, index + IMA_INDEX[nib & 7]))
            out.append(pred)
    return out


def write_wav(path, rate, pcm):
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(struct.pack(f"<{len(pcm)}h", *pcm))


def voice_clip(path):
    d = open(path, "rb").read()
    magic, rate, samples = struct.unpack_from("<III", d, 0)
    if magic != VOICE_MAGIC:
        return None
    return rate, ima_decode(d[12:], samples)


def movie_audio(path):
    with open(path, "rb") as f:
        h = f.read(28)
        magic, _w, _h, _fps, _frames, rate, offset, samples, _lz = struct.unpack("<IHHHHIIII", h)
        if magic not in MOVIE_MAGICS:
            return None
        f.seek(offset)
        if magic != MOVIE_MAGICS[0]:
            raw = f.read(samples)
            pcm = [(b - 256 if b > 127 else b) << 8 for b in raw]
        else:
            raw = f.read(samples * 2)
            pcm = list(struct.unpack(f"<{len(raw) // 2}h", raw))
    return rate, pcm


# what whisper "hears" in music, shots and screams
HALLUCINATIONS = {"thanks for watching", "thank you for watching", "thank you", "thanks", "bye", "you",
                  "please subscribe", "subtitles by the amaraorg community"}
SOUND_WORD = re.compile(r"^(a+h*|u+h+|o+h+|h+m+|m+h*m*|h+a+h*|h+e+h*|hu+h+|ugh+|gah+|argh+|bang+|ow+|whoa+|"
                        r"ha+|he+|uh+|eh+|ooh+|aah+|grr+|ngh+|nah+|whew)$")


def clean(text):
    """Drops what whisper writes for sounds ([Music], (screams), *gasps*),
    lines made only of sound words (Uh-huh-huh!) and its known inventions."""
    text = re.sub(r"\[[^\]]*\]|\([^)]*\)|\*[^*]*\*|♪", "", text)
    text = text.replace("’", "'").replace("‘", "'").replace("“", '"').replace("”", '"')
    text = text.replace("…", "...").replace("—", "-").replace("–", "-")
    text = text.encode("ascii", "ignore").decode()
    text = " ".join(text.split())
    words = [w for w in re.split(r"[^a-z]+", text.lower()) if w]
    if not words or all(SOUND_WORD.match(w) for w in words):
        return ""
    if " ".join(words) in HALLUCINATIONS:
        return ""
    return text


def main():
    ds, out_path = sys.argv[1], sys.argv[2]
    work = sys.argv[3] if len(sys.argv) > 3 else os.path.join(os.path.dirname(os.path.abspath(out_path)), "subs_work")
    cli = os.environ.get("WHISPER_CLI") or shutil.which("whisper-cli")
    model = os.environ.get("WHISPER_MODEL") or os.path.expanduser("~/.cache/whisper-cpp/ggml-small.en.bin")
    if not cli or not os.path.exists(model):
        print("subs2ds: whisper-cli or the model is missing; no subtitles (see the README)")
        return
    os.makedirs(work, exist_ok=True)

    clips = []  # (key, wav path)
    for kind, folder, reader in (("voice", "voice", voice_clip), ("movie", "movie", movie_audio)):
        d = os.path.join(ds, folder)
        if not os.path.isdir(d):
            continue
        for name in sorted(os.listdir(d)):
            if not name.endswith(".bin"):
                continue
            src = os.path.join(d, name)
            key = name[:-4]
            wav = os.path.join(work, f"{key}.wav")
            if not os.path.exists(wav) or os.path.getmtime(wav) < os.path.getmtime(src):
                got = reader(src)
                if not got:
                    continue
                rate, pcm = got
                if kind == "voice" and len(pcm) < rate * MIN_SECONDS:
                    continue
                write_wav(wav, rate, pcm)
            clips.append((key, wav))

    # whisper-cli wants 16 kHz: resample with ffmpeg, then transcribe what has
    # no current result yet (one run: the model loads once)
    todo = []
    for key, wav in clips:
        w16 = wav[:-4] + ".16k.wav"
        res = w16 + ".json"
        if not os.path.exists(res) or os.path.getmtime(res) < os.path.getmtime(wav):
            subprocess.run(["ffmpeg", "-loglevel", "error", "-y", "-i", wav, "-ar", "16000", "-ac", "1", w16],
                           check=True)
            todo.append(w16)
    for i in range(0, len(todo), 40):
        batch = todo[i:i + 40]
        cmd = [cli, "-m", model, "-l", "en", "-np", "-oj", "-ml", str(MAX_CHARS), "-sow", "--suppress-nst"]
        for w in batch:
            cmd += ["-f", w]
        subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)
        print(f"subs2ds: transcribed {min(i + 40, len(todo))} / {len(todo)}")

    lines, count = [], 0
    for key, wav in clips:
        res = wav[:-4] + ".16k.wav.json"
        if not os.path.exists(res):
            continue
        segs = []
        for s in json.load(open(res, encoding="utf-8", errors="replace")).get("transcription", []):
            text = clean(s.get("text", ""))
            # whisper can loop on music ("We have to get out of here." 47 times
            # over the end credits): a line said again at once is dropped
            if text and not (segs and segs[-1][2] == text):
                segs.append((s["offsets"]["from"], s["offsets"]["to"], text))
        if segs:
            lines.append(f"@{key}")
            lines += [f"{a} {b} {t}" for a, b, t in segs]
            count += 1
    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    with open(out_path, "w", encoding="ascii") as f:
        f.write("\n".join(lines) + "\n")
    print(f"{out_path}: {count} clips with subtitles")


if __name__ == "__main__":
    main()
