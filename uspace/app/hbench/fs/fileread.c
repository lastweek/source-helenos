/*
 * Copyright (c) 2011 Martin Sucha
 * Copyright (c) 2019 Vojtech Horky
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * - Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 * - Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in the
 *   documentation and/or other materials provided with the distribution.
 * - The name of the author may not be used to endorse or promote products
 *   derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/** @addtogroup hbench
 * @{
 */

#include <str_error.h>
#include <stdio.h>
#include <stdlib.h>
#include "../hbench.h"

#define BUFFER_SIZE 4096

/*
 * Note that while this benchmark tries to measure speed of file reading,
 * it rather measures speed of FS cache as it is highly probable that the
 * corresponding blocks would be cached after first run.
 */
static bool runner(benchmeter_t *meter, uint64_t size,
    char *error, size_t error_size)
{
	const char *path = bench_param_get("filename", "/data/web/helenos.png");

	char *buf = malloc(BUFFER_SIZE);
	if (buf == NULL) {
		snprintf(error, error_size, "failed to allocate %dB buffer", BUFFER_SIZE);
		return false;
	}

	bool ret = true;

	FILE *file = fopen(path, "r");
	if (file == NULL) {
		snprintf(error, error_size, "failed to open %s for reading: %s",
		    path, str_error(errno));
		ret = false;
		goto leave_free_buf;
	}

	benchmeter_start(meter);
	for (uint64_t i = 0; i < size; i++) {
		int rc = fseek(file, 0, SEEK_SET);
		if (rc != 0) {
			snprintf(error, error_size, "failed to rewind %s: %s",
			    path, str_error(errno));
			ret = false;
			goto leave_close;
		}
		while (!feof(file)) {
			fread(buf, 1, BUFFER_SIZE, file);
			if (ferror(file)) {
				snprintf(error, error_size, "failed to read from %s: %s",
				    path, str_error(errno));
				ret = false;
				goto leave_close;
			}
		}
	}
	benchmeter_stop(meter);

leave_close:
	fclose(file);

leave_free_buf:
	free(buf);

	return ret;
}

benchmark_t benchmark_file_read = {
	.name = "file_read",
	.desc = "Sequentially read contents of a file (use 'filename' param to alter the default).",
	.entry = &runner,
	.setup = NULL,
	.teardown = NULL
};

/**
 * @}
 */
