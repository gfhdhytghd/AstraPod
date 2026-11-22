#!/usr/bin/env python3
"""Bind the upstream audio utilities to the AstraPod foreground file APIs."""
from hashlib import sha256
from pathlib import Path
import sys


DIGESTS = {
    "mp3_encoder.c": "e5193d8cea64cc5d1999c1854a6f10832a8edd1cf466d66db18b6adeed136f5a",
    "wav2wv.c": "64f765b9ec66fe65267dc8ad0f2df36259fe99bed57626f2bf88c8a549d437d8",
    "vbrfix.c": "96a4cba0edf2705d18e62d65defd290e9fced2f05a4b596495c54ed2e3b46fe9",
    "mp3data.c": "ed262a29d0ff36e06f080990c90a8adfde44122d0e3e239fbc2920d40a5ed38d",
}


def checked(path):
    data = path.read_bytes()
    if path.name in DIGESTS and sha256(data).hexdigest() != DIGESTS[path.name]:
        raise SystemExit(f"{path}: review the AstraPod encoder adapter")
    return data.decode()


def mp3_encoder(source, adapter, wave):
    header = source[:source.index("static char* mp3_enc_err")]
    settings = source[source.index("static int         mp3file,"):
                      source.index("static bool checkString")]
    settings = settings.replace("mp3file, wavfile, wav_size, frames", "frames")
    settings = settings.replace("void        *enc_buffer", "unsigned char *enc_buffer")
    engine = source[source.index("static inline uint32_t myswap32"):
                    source.index("int  num_file;")]
    engine = engine.replace("static void compress(void)",
                            "static bool compress(struct encoder_file *file)")
    engine = engine.replace("wav_size/cfg.smpl_per_frm/cfg.channels/2",
                            "total_frames")
    engine = engine.replace(
        "    if(read_samples((mfbuf + 2*512), cfg.smpl_per_frm) == 0)\n      break;",
        """    if (encoder_cancel(file))
    {
        cancelled = true;
        return false;
    }
    if (frames >= total_frames)
        break;
    if (!read_samples(file->input, mfbuf + 2 * 512, cfg.smpl_per_frm))
        return false;""")
    # The retained filter and frame writer need neither codec IRAM nor audio RAM.
    engine = engine.replace(
        """      rb->write(mp3file, enc_buffer, enc_chunk & ~3);
      memcpy(enc_buffer, enc_buffer + (enc_chunk >> 2), enc_chunk & 3);
      enc_chunk &= 3;""",
        """      if (rb->write(file->output, enc_buffer, enc_chunk & ~3) !=
          (enc_chunk & ~3))
          return false;
      rb->memmove(enc_buffer, enc_buffer + (enc_chunk & ~3), enc_chunk & 3);
      enc_chunk &= 3;""")
    engine = engine.replace("  rb->write(mp3file, enc_buffer, enc_chunk);",
                            "  return rb->write(file->output, enc_buffer, enc_chunk) == enc_chunk;")
    prelude, entry = adapter.split("/* ENCODER ENTRY */\n")
    parser = wave[wave.index("static unsigned int le16"):
                  wave.index("enum wave_result")]
    return header + settings + parser + prelude + engine + entry


def wav2wv(source, adapter):
    return source[:source.index("#define SAMPLES_PER_BLOCK")] + adapter


def vbrfix(source, adapter, library):
    header = source[:source.index("static char   *audiobuf")]
    bitfields = library[library.index("#define SYNC_MASK"):
                        library.index("/* Helper function to read 4-byte")]
    bitfields = bitfields.replace("unsigned long bytes2int(", "static unsigned long rock_mp3_bytes2int(")
    return header + '#include "mp3data.h"\n#define VDEBUGF(...) do {} while (0)\n' + bitfields + adapter


source, adapter, output, *extras = map(Path, sys.argv[1:])
transform = {"mp3_encoder": mp3_encoder, "wav2wv": wav2wv, "vbrfix": vbrfix}
output.write_text(transform[source.stem](checked(source), adapter.read_text(),
                                        *(checked(extra) for extra in extras)))
