# iPod Classic SM1 FLAC playback

The ordinary codec thread selects the profile in apps-ipod/screens/system/sm1_flac_profile.h. The six assembled programs and their SHA-256 pins are in images/ and profiles.json. tools/buildzip.pl installs them under .rockbox/sm1 on ipod6g. The package contains no diagnostic controller, USB test commands or private music.

## Building

Use normal Rockbox build dependencies, arm-elf-eabi-gcc (tested with 9.5.0) and a new build directory inside the source tree.

    mkdir build-ipod
    cd build-ipod
    ../tools/configure --target=ipod6g --type=n --appsdir=apps-ipod
    make -j8
    make zip

To reassemble and check every packaged image, set ZIG to Zig 0.16.0 and run python3 firmware/target/arm/s5l8702/sm1/verify-images.py from the project root. This uses the included x17 assembler subset and the included sosumi assembly dependencies. Their license files are alongside the sources.

## Contracts

| Stereo input | Max block in source pairs | Max compressed bytes | Output |
|---|---:|---:|---|
| 16 or 24 bit, 44.1 or 48 kHz | 4608 | 32768 | s16 stereo at source rate |
| 24 bit, 96 kHz | 8192 | 65535 | s16 stereo at 48 kHz |
| 24 bit, 192 kHz | 16384 | 65535 | s16 stereo at 48 kHz |

The native and supported fragmented MP4 FLAC profiles share the decoder. True 25-bit joint side values are reconstructed in the 24-bit path. Only the native files use the high-rate read-ahead and persistent cache. On a high-rate miss, SM1 generates the 48 kHz PCM. Cache publication requires a complete file. After reboot the player reads and hashes the source and image before it publishes the first cached block to the mixer. Direct seeks read the corresponding checked block. A different image creates a different key. First-time decoding can exceed audio duration. The optimized 192 kHz hardware long-block example took 297.342 ms for 85.333 ms of audio.

Crash recovery requires retained DRAM. SM1 owns the SRAM previously used as a secondary crash record. The inherited diagnostic screen can still print an IRAM1 copy label; physically both status pointers refer to DRAM. The former dual-store source and emulator tests need adjustment for this layout. The policy counts three consecutive fatal starts within 30 seconds. Minimal recovery attempts crash export and USB disk mode. A recovery fault parks the device. Warm-reset retention depends on power remaining available.

## Representative release gates

Play files from the ordinary browser or a playlist with the six installed images. Use a short 48 kHz native file with a real 25-bit side and partial final frame to check mixer startup and EOF. Use 44.1 kHz fragmented MP4 FLAC with a known joint-side boundary to check demux and seek. On the Sweet Design 96 kHz excerpt, check the first full generation, cold reboot, and a 12013 ms seek with a 143535-pair suffix. On the Rainy Days 192 kHz excerpt, check both the 16384-sample frame and its 4460-sample tail. The expected 5211 output stereo pairs have SHA-256 5f19bb8f4c5881612f2a11101ede1fb3c3183f66d539b6404c1bf1bb50498c5d. A rebooted hit must submit zero frames to SM1. Check short EOF, manual STOP, seek and transition to an ordinary non-FLAC track. Record codec EOF, mixer idle, playlist restoration and USB controller state separately.

These representative captures came from instrumented firmware SHA-256 3b6868b902265cb92d8837d788c4f1c37e8fab811b0ff62e85bfb965b306bbf0 for the 96/192 cache gates. Project firmware 7507f39bfc967a975b992151a42c6ad08e0e32451bdbd5adedd41a50a9662ad6 was cross-compiled. The combined project binary still needs representative device acceptance. The 192 kHz cache proof covers a 109 ms excerpt. Original source integrity was checked independently of playback. Out of 1486 sources, 1409 passed original integrity or full MP4 decode, 41 have verified exact prefixes and 16 have exact alternates. One 16/44.1 original lacks 3456 stereo pairs and 19 files at 24/96 fail decoded PCM MD5. All twenty originals still matched the damaged source-file hashes during the targeted follow-up.
