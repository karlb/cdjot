/* libFuzzer / AFL++ harness for cdjot.
 *
 * Build: see ../Makefile (`make fuzz` for libFuzzer, `make fuzz-afl` for AFL++).
 *
 * Strategy: feed arbitrary bytes to cdjot_convert and let the sanitizers
 * (ASan/UBSan) catch crashes, OOB reads/writes, UAF, and undefined behavior
 * that the unit tests don't exercise. Output is discarded via a /dev/null
 * stream opened once at startup.
 *
 * To replay a corpus under sanitizers without mutation, use libFuzzer's
 * own batch mode: `./fuzz/cdjot-fuzz -runs=0 fuzz/corpus` (see
 * `make fuzz-replay`).
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "../cdjot.h"

static FILE *devnull;

static void
init_devnull(void)
{
	if (devnull) return;
	devnull = fopen("/dev/null", "w");
	if (!devnull) {
		perror("fopen /dev/null");
		exit(1);
	}
	/* Suppress stdio buffering churn; we throw the bytes away anyway. */
	setvbuf(devnull, NULL, _IONBF, 0);
}

#ifdef __AFL_FUZZ_TESTCASE_LEN
/* AFL++ persistent-mode entry point. */
#include <unistd.h>  /* read() expanded by __AFL_FUZZ_TESTCASE_LEN */
__AFL_FUZZ_INIT();

int
main(void)
{
	init_devnull();
#ifdef __AFL_HAVE_MANUAL_CONTROL
	__AFL_INIT();
#endif
	unsigned char *buf = __AFL_FUZZ_TESTCASE_BUF;
	while (__AFL_LOOP(10000)) {
		size_t len = __AFL_FUZZ_TESTCASE_LEN;
		cdjot_convert(devnull, (const char *)buf, len);
	}
	return 0;
}

#else
/* libFuzzer entry point. */
int LLVMFuzzerInitialize(int *argc, char ***argv);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int
LLVMFuzzerInitialize(int *argc, char ***argv)
{
	(void)argc; (void)argv;
	init_devnull();
	return 0;
}

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	cdjot_convert(devnull, (const char *)data, size);
	return 0;
}
#endif
