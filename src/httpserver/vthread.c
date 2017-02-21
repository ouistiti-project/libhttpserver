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

#ifdef WIN32
#include <windows.h>
#include <tchar.h>
#else
#if defined(HAVE_PTHREAD)
# include <pthread.h>
#endif
#endif

#include "valloc.h"
#include "vthread.h"

struct vthread_s
{
#ifdef WIN32
	HANDLE handle;
	DWORD id;
#else
	pthread_t pthread;
	pthread_attr_t attr;
#endif
};

int vthread_create(vthread_t *thread, vthread_attr_t *attr,
	vthread_routine start_routine, void *arg, int argsize)
{
	int ret = 0;
	vthread_t vthread;
	vthread = vcalloc(1, sizeof(struct vthread_s));
#ifdef WIN32
	/**
	 * the commented lines was found on internet
	 * but after documention HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, argsize);
	 * is the same of calloc(1, argsize); 
	 * with one exception, with calloc the memory MUST be free inside the DLL or Application
	 * that created the memory
	vthread->argument = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, argsize);
	memcpy(vthread->argument, arg, argsize);
	 */
	vthread->handle = CreateThread( NULL, 0, start_routine, arg, 0, &vthread->id);
	SwitchToThread();
#elif defined(HAVE_PTHREAD)
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
#else
	ret = (int)start_routine(arg);
#endif
	*thread = vthread;
	return ret;
}

int vthread_join(vthread_t thread, void **value_ptr)
{
	int ret = 0;
#ifdef WIN32
	CloseHandle(thread->handle);
/**
	if (thread->argument)
		ret = HeapFree(GetProcessHeap(), 0, thread->argument);
*/
#elif defined(HAVE_PTHREAD)
	if (thread->pthread)
	{
		pthread_cancel(thread->pthread);
		ret = pthread_join(thread->pthread, value_ptr);
	}
#endif
	vfree(thread);
	return ret;
}

void vthread_wait(vthread_t threads[], int nbthreads)
{
#ifdef WIN32
	int i;
	HANDLE *threadsArray;

	threadsArray = calloc(nbthreads, sizeof(HANDLE));
	for (i = 0; i < nbthreads; i++) 
	{
		threadsArray[i] = threads[i]->handle;
	}
	WaitForMultipleObjects(nbthreads, threadsArray, TRUE, INFINITE);
#else
	int i;
	for (i = 0; i < nbthreads; i++) 
	{
		void *value_ptr;
		vthread_t thread = threads[i];
		if (thread)
			vthread_join(thread, &value_ptr);
	}
#endif
}

void vthread_yield(vthread_t thread)
{
#ifdef WIN32
	SwitchToThread();
#elif defined(HAVE_PTHREAD)
#if defined(HAVE_PTHREAD_YIELD)
	pthread_yield();
#elif defined(HAVE_SCHED_YIELD)
	sched_yield();
#endif
#endif
}
