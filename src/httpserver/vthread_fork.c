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
#define _XOPEN_SOURCE 700

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sched.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>

#include "log.h"
#include "httpserver.h"
#include "valloc.h"
#include "vthread.h"

struct vthread_s
{
	pid_t pid;
};

#include <signal.h>

int vthread_create(vthread_t *thread, vthread_attr_t *attr,
	vthread_routine start_routine, void *arg, int argsize)
{
	int ret = ESUCCESS;
	vthread_t vthread;

	/**
	 * SIGCHLD must be catched to wake up the server when a client terminated.
	 */
	struct sigaction action;
	action.sa_flags = SA_SIGINFO;
	sigemptyset(&action.sa_mask);
	/**
	 * ignore SIGCHLD allows the child to die without to create a zombie.
	 * But the parent doesn't receive information.
	 * See below about "waitpid"
	 */
	action.sa_handler = SIG_IGN;
	sigaction(SIGCHLD, &action, NULL);

	vthread = vcalloc(1, sizeof(struct vthread_s));

	if (vthread == NULL)
	{
		err("vhtread: memory allocation %s", strerror(errno));
		ret = EREJECT;
	}
	else if ((vthread->pid = fork()) == 0)
	{
#ifdef TIME_PROFILER
		struct timeval date1, date2;
		gettimeofday(&date1, NULL);
#endif
		void *value = start_routine(arg);
#ifdef TIME_PROFILER
		gettimeofday(&date2, NULL);
		date2.tv_sec -= date1.tv_sec;
		if (date2.tv_usec > date1.tv_usec)
			date2.tv_usec -= date1.tv_usec;
		else
		{
			date2.tv_sec += 1;
			date2.tv_usec = date1.tv_usec - date2.tv_usec;
		}
		printf("time %d:%d\n", date2.tv_sec, date2.tv_usec);
#endif
		exit(0);
	}
	else if (vthread->pid == -1)
	{
		err("fork error %s", strerror(errno));
		ret = EREJECT;
	}
	else
	{
		sched_yield();
		*thread = vthread;
	}
	return ret;
}

int vthread_join(vthread_t thread, void **value_ptr)
{
	int ret = 0;
	if (thread->pid > 0)
	{
			pid_t pid;
			pid = waitpid(thread->pid, &ret, 0);
	}
	vfree(thread);
	return WEXITSTATUS(ret);
}

int vthread_exist(vthread_t thread)
{
	int pid = -1;
	if (thread->pid > 0)
	{
		pid = waitpid(thread->pid, NULL, WNOHANG);
		if (pid < 0)
		{
			if (errno == ECHILD)
			{
				/**
				 * SIGCHLD is ignored
				 */
				err("vthread %d previously died", thread->pid);
				thread->pid = 0;
			}
			else
				err("vthread_exist error %s", strerror(errno));
		}
		if (pid == thread->pid)
		{
			/**
			 * thread died previously
			 * Don't try to wait again into vthread_join
			 */
			err("vthread exist NO with SIGCHLD %d", pid == 0);
			thread->pid = 0;
		}
	}
	return (pid == 0);
}

void vthread_wait(vthread_t threads[], int nbthreads)
{
	int ret = 0;
	int i;
	for (i = 0; i < nbthreads; i++)
	{
		vthread_join(threads[i], NULL);
	}
}

void vthread_yield(vthread_t thread)
{
	sched_yield();
}
