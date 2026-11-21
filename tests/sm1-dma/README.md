# iPod 6G SM1 PCM DMA probe

Compile normal firmware with the additional C define PODBOX_SM1_DMA_PROBE
to enable up to sixteen transfers after boot. The ordinary image uses the CPU
copy. The experiment runs only in the stereo 16-bit FLAC playback route at
44.1/48 kHz with EQ off. ALAC and conversion callbacks do not opt in.

After a completed SM1 frame the ARM claims Y and validates its status, slot
and word count. DMA0 channel 7, when free, copies interleaved halfwords from
physical address 0x22038100 to a private, cache-line aligned PCM staging area.
The 12-bit DMA count is limited to 4094 words to keep stereo pairs intact.
Channel interrupt masks stay clear; polling reads raw terminal count/error.
The mixer and the existing I²S channel 2 still own output to the DAC.

Before each transfer, the probe writes the complement of each source word to
the destination and cleans/invalidates the private cache lines. After confirming
channel inactivity, it invalidates those lines, compares every requested word
against the still-held Y slot, and checks the unused tail and both guards.
Playback debug logs include word count and verified=1/0. On mismatch or a
quiesced timeout, it disables further probes and the ordinary CPU copies the
entire frame. If halting cannot drain the DMA or disabling fails to clear the
enabled/active flags, the probe pins its SRAM slot, DMA channel, clocks and
destination until reset; the frame fails without handoff or new SM1 work.

The host register model exercises production control flow. Run from repository root:

```sh
cc -std=c11 -Wall -Wextra -Werror -Ifirmware/export -Ifirmware/target/arm/s5l8702 tests/sm1-dma/probe_test.c -o /tmp/sm1-dma-test
for n in 2 346 8192 9216; do /tmp/sm1-dma-test 0 "$n" || exit 1; done
for m in 1 2 3 4 5; do /tmp/sm1-dma-test "$m" 8192 || exit 1; done
```

Cases cover shortest stereo, short tail, normal and maximum output, channel
busy, a second-segment bus error, timeout with graceful halt, stuck active DMA,
and one corrupted output word. The simulated timer crosses 32-bit rollover.
The model cannot establish actual PL080 access to Y SRAM or timing on silicon.
A target check needs a diagnostic image, a matching known stereo PCM fixture,
the DMA word-verification logs and a byte-for-byte playback capture including
the final short frame. Stock output DMA and mixed-output timing remain separate.
