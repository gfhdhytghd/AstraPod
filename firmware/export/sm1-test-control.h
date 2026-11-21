#ifndef SM1_TEST_CONTROL_H
#define SM1_TEST_CONTROL_H

#define SM1_TEST_USB_REQUEST 0x5d
#define SM1_TEST_USB_INDEX 0x4951
#define SM1_TEST_DEBUG_INDEX 0x4952
#define SM1_TEST_DEBUG_BASE 0x10000u
#define SM1_TEST_DEBUG_NAME_SIZE 128
#define SM1_TEST_TRACK ROCKBOX_DIR "/sm1-test/irq-dma44.flac"
#define SM1_TEST_SEEK_MS 6237

enum sm1_test_command {
    SM1_TEST_REBOOT = 1,
    SM1_TEST_PLAY,
    SM1_TEST_SEEK,
    SM1_TEST_STOP,
};

extern volatile unsigned sm1_test_submitted;
extern volatile unsigned sm1_test_completed;
extern volatile unsigned sm1_test_dispatch_tick;

const char *sm1_test_debug_name(unsigned index);
void sm1_test_debug_run(unsigned index);

#endif
