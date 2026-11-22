These excerpts are unmodified 44.1 kHz signed 16-bit mono PCM from the
iPod Classic 6G native recorder's headset mic. The tone files capture a
440 Hz sine played from a computer speaker. The silence files capture the
room and the headset-mic noise floor without the tone.

| Fixture | Source | First 4096-frame block (zero-based) | Blocks |
| --- | --- | ---: | ---: |
| b4b7-silence.wav | mic/b4b7.wav | 13 | 6 |
| b4b7-tone.wav | mic/b4b7.wav | 36 | 8 |
| v26-silence.wav | pitch-v26/rec/mic.wav | 5 | 5 |
| v26-tone.wav | pitch-v26/rec/mic.wav | 29 | 8 |

Run `python3 tools/tests/rock_pitch_replay.py /tmp/pitch-replay` from the
repository root. Append paths to complete WAV files to print the result
of each complete block at the default and diagnostic thresholds.
