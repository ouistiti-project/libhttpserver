/*****************************************************************************
 * vthread.c: multiplatform thread
 *****************************************************************************
 * Copyright (C) 2016-2017
 *
 * Authors: Marc Chalain <marc.chalain@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject
 * to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

#define _GNU_SOURCE
#include <pthread.h>
#include <signal.h>
#include <sched.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "valloc.h"
#include "vthread.h"

#define err(format, ...) fprintf(stderr, "\x1B[31m"format"\x1B[0m\n",  ##__VA_ARGS__)
#define warn(format, ...) fprintf(stderr, "\x1B[35m"format"\x1B[0m\n",  ##__VA_ARGS__)
#ifdef DEBUG
#define dbg(format, ...) fprintf(stderr, "\x1B[32m"format"\x1B[0m\n",  ##__VA_ARGS__)
#else
# define dbg(...)
#endif

struct vthread_s
{
	pthread_t pthread;
	pthread_attr_t attr;
};

int vthread_create(vthread_t *thread, vthread_attr_t *attr,
	vthread_routine start_routine, void *arg, int argsize)
{
	int ret = 0;
	vthread_t vthread;
	vthread = vcalloc(1, sizeof(struct vthread_s));
	if (attr == NULL)
	{
		attr = &vthread->attr;
	}
	pthread_attr_init(attr);
	pthread_attr_setdetachstate(attr, PTHREAD_CREATE_JOINABLE);

	ret = pthread_create(&(vthread->pthread), attr, start_routine, arg);
#if defined(HAVE_PTHREAD_YIELD)
	pthread_yield();
#elif defined(HAVE_SCHED_YIELD)
	sched_yield();
#endif
	*thread = vthread;
	return ret;
}

int vthread_join(vthread_t thread, void **value_ptr)
{
	int ret = 0;
	if (thread->pthread)
	{
		pthread_cancel(thread->pthread);
		ret = pthread_join(thread->pthread, value_ptr);
	}
	return ret;
}

int vthread_exist(vthread_t thread)
{
	return 1;
}

void vthread_wait(vthread_t threads[], int nbthreads)
{
	int i;
	for (i = 0; i < nbthreads; i++) 
	{
		void *value_ptr;
		vthread_t thread = threads[i];
		if (thread && thread->pthread)
		{
			pthread_join(thread->pthread, value_ptr);
		}
	}
}

void vthread_yield(vthread_t thread)
{
#if defined(HAVE_PTHREAD_YIELD)
	pthread_yield();
#elif defined(HAVE_SCHED_YIELD)
	sched_yield();
#endif
}
