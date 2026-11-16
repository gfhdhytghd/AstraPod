#ifndef PODLINK_RUST_SERVICE_TEST_H
#define PODLINK_RUST_SERVICE_TEST_H
/* Deterministic worker adapter. All protocol state belongs to Rust. */
#include "remote_log.h"
static char log_line[768];
static void service_step(void) {podlink_rust_step();podlink_rust_log((unsigned char *)log_line);}
#endif
