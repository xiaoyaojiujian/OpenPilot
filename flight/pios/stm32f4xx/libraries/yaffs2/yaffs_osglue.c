/*
 * YAFFS: Yet Another Flash File System. A NAND-flash specific file system.
 *
 * Copyright (C) 2002-2011 Aleph One Ltd.
 *   for Toby Churchill Ltd and Brightstar Engineering
 *
 * Created by Charles Manning <charles@aleph1.co.uk>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

/*
 * Example OS glue functions for running on a Linux/POSIX system.
 */

#include "yaffscfg.h"
#include "yaffs_guts.h"
#include "yaffsfs.h"
#include "yaffs_trace.h"
#include <assert.h>

#include <errno.h>
#include <unistd.h>

#include "pios_trace.h"
#include "yaffsfs.h"

#include "pios.h"
#include "pios_mem.h"

/*
 * yaffsfs_SetError() and yaffsfs_GetError()
 * Do whatever to set the system error.
 * yaffsfs_GetError() just fetches the last error.
 */

static int yaffsfs_lastError;

void yaffsfs_SetError(int err)
{
	//Do whatever to set error
	yaffsfs_lastError = err;
	errno = err;
	pios_trace(PIOS_TRACE_ERROR, "yaffsfs_SetError(%d) %s",
					 err, yaffs_error_to_str(err) );
}

int yaffsfs_GetLastError(void)
{
	return yaffsfs_lastError;
}

/*
 * yaffsfs_CheckMemRegion()
 * Check that access to an address is valid.
 * This can check memory is in bounds and is writable etc.
 *
 * Returns 0 if ok, negative if not.
 */
int yaffsfs_CheckMemRegion(const void *addr, size_t size, int write_request)
{
    size = size;
    write_request = write_request;

	if(!addr)
		return -1;
	return 0;
}

/*
 * yaffsfs_Lock()
 * yaffsfs_Unlock()
 * A single mechanism to lock and unlock yaffs. Hook up to a mutex or whatever.
 * Here are two examples, one using POSIX pthreads, the other doing nothing.
 *
 * If we use pthreads then we also start a background gc thread.
 */

#if defined(PIOS_INCLUDE_FREERTOS)

static xSemaphoreHandle _yaffs_sem_lock;

//static pthread_t bc_gc_thread;

void yaffsfs_Lock(void)
{

	if (xSemaphoreTake(_yaffs_sem_lock, portMAX_DELAY) != pdTRUE) {
	        //return -2;
	}
}

void yaffsfs_Unlock(void)
{
    if (xSemaphoreGive(_yaffs_sem_lock) != pdTRUE) {
        //return -2;
    }
}

#if 0
static void *bg_gc_func(void *dummy)
{
	struct yaffs_dev *dev;
	int urgent = 0;
	int result;
	int next_urgent;

	/* Sleep for a bit to allow start up */
	sleep(2);


	while (1) {
		/* Iterate through devices, do bg gc updating ungency */
		yaffs_dev_rewind();
		next_urgent = 0;

		while ((dev = yaffs_next_dev()) != NULL) {
			result = yaffs_do_background_gc_reldev(dev, urgent);
			if (result > 0)
				next_urgent = 1;
		}

		urgent = next_urgent;

		if (next_urgent)
			sleep(1);
		else
			sleep(5);
	}

	/* Don't ever return. */
	return NULL;
}
#endif

void yaffsfs_LockInit(void)
{
	/* Initialise lock */

	_yaffs_sem_lock = xSemaphoreCreateMutex();  // what about deinit?
	/* Sneak in starting a background gc thread too */
	// pthread_create(&bc_gc_thread, NULL, bg_gc_func, NULL);
}

#else

void yaffsfs_Lock(void)
{
}

void yaffsfs_Unlock(void)
{
}

void yaffsfs_LockInit(void)
{
}
#endif

/*
 * yaffsfs_CurrentTime() retrns a 32-bit timestamp.
 *
 * Can return 0 if your system does not care about time.
 */

u32 yaffsfs_CurrentTime(void)
{
	return 0;  // TODO return any increment of time
}


void yaffsfs_OSInitialisation(void)
{
	yaffsfs_LockInit();
}

/*
 * yaffs_bug_fn()
 * Function to report a bug.
 */

void yaffs_bug_fn(const char *file_name, int line_no)
{
	pios_trace(PIOS_TRACE_ERROR, "yaffsfs_SetError(%s) %d",
						 file_name, line_no);
	line_no = line_no;
	file_name = file_name;
	PIOS_Assert(0);
}
