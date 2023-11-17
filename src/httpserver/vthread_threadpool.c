/*****************************************************************************
 * vthread_poolthread.c: pthread pool manager
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

#include "ouistiti/log.h"
#include "ouistiti/httpserver.h"
#include "valloc.h"
#include "vthread.h"
#include "threadpool.h"

#include "threadpool.c"

struct vthread_s
{
	int id;
	vthread_routine routine;
	void *data;
	void *rdata;
};

static threadpool_t *g_pool = NULL;

static int threadhandler(void *data, void *userdata)
{
	vthread_t vthread = (vthread_t )data;

	vthread->rdata = vthread->routine(userdata);
	if ( vthread->rdata != NULL)
		return -1;
	return 0;
}

void vthread_init(int maxthreads)
{
	if (g_pool == NULL)
		g_pool = threadpool_init(maxthreads);
	else
	{
		for (int i = 0; i < maxthreads; i++)
			threadpool_grow(g_pool);
	}
}

void vthread_uninit(vthread_t thread)
{
	if (g_pool && ! threadpool_isrunning(g_pool, -1))
	{
		threadpool_destroy(g_pool);
		g_pool = NULL;
	}
}

int vthread_create(vthread_t *thread, vthread_attr_t *attr,
	vthread_routine start_routine, void *arg, int argsize)
{
	*thread = NULL;
	if (g_pool == NULL)
		return EREJECT;
	int ret = ESUCCESS;
	vthread_t vthread;
	vthread = vcalloc(1, sizeof(struct vthread_s));

	vthread->routine = start_routine;
	vthread->data = arg;
	vthread->id = threadpool_get(g_pool, threadhandler, vthread, arg);
	*thread = vthread;
	if (vthread->id == 0)
	{
		free(vthread);
		ret = EREJECT;
		*thread = NULL;
	}
	return ret;
}

int vthread_join(vthread_t vthread, void **value_ptr)
{
	if (g_pool == NULL || vthread == NULL)
		return -1;
	int ret = 0;
	if (vthread->id != 0)
	{
		if (threadpool_wait(g_pool, vthread->id) == -1)
		{
			return EREJECT;
		}
		if (value_ptr)
			*value_ptr = vthread->rdata;
		free(vthread);
	}
	return ret;
}

int vthread_exist(vthread_t vthread)
{
	if (g_pool == NULL || vthread == NULL)
		return -1;
	return threadpool_isrunning(g_pool, vthread->id);
}

void vthread_wait(vthread_t threads[], int nbthreads)
{
	if (g_pool == NULL)
		return;
	int i;
	for (i = 0; i < nbthreads; i++) 
	{
		void *value_ptr;
		vthread_t vthread = threads[i];
		if (vthread)
		{
			vthread_join(vthread, &value_ptr);
		}
	}
}

void vthread_yield(vthread_t vthread)
{
	sched_yield();
}

int vthread_self(vthread_t vthread)
{
	if (g_pool == NULL || vthread == NULL)
		return -1;
	return vthread->id;
}

int vthread_sharedmemory(vthread_t thread)
{
	return 1;
}
