#ifndef SM1_CONVERT_H
#define SM1_CONVERT_H
#include <stdbool.h>
#include <stddef.h>
enum { SM1_CONVERT_OK, SM1_CONVERT_EXISTS, SM1_CONVERT_CANCELLED,
       SM1_CONVERT_UNSUPPORTED, SM1_CONVERT_ERROR };
int sm1_convert_track(const char *source, unsigned output_rate, bool (*cancelled)(void));
bool sm1_converted_path(const char *source, char *dest, size_t capacity);
void sm1_convert_init(void);
#endif
