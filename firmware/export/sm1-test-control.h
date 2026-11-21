#ifndef SM1_TEST_CONTROL_H
#define SM1_TEST_CONTROL_H

#define SM1_TEST_USB_REQUEST 0x5d
#define SM1_TEST_USB_INDEX 0x4951
#define SM1_TEST_DEBUG_INDEX 0x4952
#define SM1_TEST_DEBUG_BASE 0x10000u
#define SM1_TEST_DEBUG_NAME_SIZE 128
#define SM1_TEST_RECORD_STATUS_VALUE 1
#define SM1_TEST_RECORD_STATUS_SIZE 96
#define SM1_TEST_RECORD_ERROR_VALUE 2
#define SM1_TEST_RECORD_ERROR_SIZE 20
#define SM1_TEST_RECORD_PATH_SIZE (SM1_TEST_RECORD_STATUS_SIZE - 8)
#define SM1_TEST_TRACK ROCKBOX_DIR "/sm1-test/irq-dma44.flac"
#define SM1_TEST_SEEK_MS 6237

enum sm1_test_command {
    SM1_TEST_REBOOT = 1,
    SM1_TEST_PLAY,
    SM1_TEST_SEEK,
    SM1_TEST_STOP,
    SM1_TEST_OPEN_BENCH,
    SM1_TEST_RECORD_LINE_WAV,
    SM1_TEST_RECORD_MIC_WAV,
    SM1_TEST_RECORD_STOP,
};

enum sm1_test_record_state {
    SM1_RECORD_IDLE,
    SM1_RECORD_ACTIVE,
    SM1_RECORD_READY,
    SM1_RECORD_ERROR,
    SM1_RECORD_UNSUPPORTED,
    SM1_RECORD_BUSY,
};

enum sm1_test_record_stage {
    SM1_RECORD_STAGE_NONE,
    SM1_RECORD_STAGE_DIR,
    SM1_RECORD_STAGE_FILENAME,
    SM1_RECORD_STAGE_INIT,
    SM1_RECORD_STAGE_START,
    SM1_RECORD_STAGE_CAPTURE,
    SM1_RECORD_STAGE_SAVE,
};

extern volatile unsigned sm1_test_submitted;
extern volatile unsigned sm1_test_completed;
extern volatile unsigned sm1_test_dispatch_tick;

const char *sm1_test_debug_name(unsigned index);
void sm1_test_debug_run(unsigned index);
void sm1_test_record_run(bool mic);
bool sm1_test_record_active(void);
bool sm1_test_record_request_stop(void);
void sm1_test_record_status(void *reply);
void sm1_test_record_error(void *reply);

#endif
