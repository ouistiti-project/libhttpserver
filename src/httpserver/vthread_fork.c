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
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sched.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>

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
	pid_t pid;
};

#include <signal.h>
static void handler(int sig, siginfo_t *si, void *arg)
{
}

int vthread_create(vthread_t *thread, vthread_attr_t *attr,
	vthread_routine start_routine, void *arg, int argsize)
{
	int ret = 0;
	vthread_t vthread;

	struct sigaction action;
	action.sa_flags = SA_SIGINFO;
	sigemptyset(&action.sa_mask);
	action.sa_sigaction = handler;
	sigaction(SIGCHLD, &action, NULL);
	vthread = vcalloc(1, sizeof(struct vthread_s));

	if ((vthread->pid = fork()) == 0)
	{
		ret = (int)start_routine(arg);
		exit(ret);
	}
	sched_yield();
	*thread = vthread;
	return ret;
}

int vthread_join(vthread_t thread, void **value_ptr)
{
	int ret = 0;
	if (thread->pid > 0)
	{
		kill( thread->pid, SIGKILL);
		do
		{
			pid_t pid;
			pid = waitpid(thread->pid, &ret, 0);
			if (pid < 0 && errno == EINTR)
				continue;
			if (pid != thread->pid)
				break;
		} while (!WIFEXITED(ret));
	}
	vfree(thread);
	return WEXITSTATUS(ret);
}

int vthread_exist(vthread_t thread)
{
	int ret = 0;
	int pid;
	if (thread->pid > 0)
	{
		pid = waitpid(thread->pid, &ret, WNOHANG);
		if (pid == 0)
		{
			return 1;
		}
		if (pid == thread->pid)
		{
			if (WIFEXITED(ret))
				thread->pid = 0;
			return !WIFEXITED(ret);
		}
		err("vthread_exist error %s", strerror(errno));
		if (errno == ECHILD)
			thread->pid = 0;
	}
	return 0;
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
