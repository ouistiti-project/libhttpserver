#include <stdio.h>
#include <stdlib.h>

#ifdef WIN32
#include <windows.h>
#include <tchar.h>
#else
#include <pthread.h>
#include <signal.h>
#endif

#include "vthread.h"

struct vthread_s
{
#ifdef WIN32
	void *argument;
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
	*thread = calloc(1, sizeof(struct vthread_s));
#ifdef WIN32
	(*thread)->argument = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, argsize);
	memcpy((*thread)->argument, arg, argsize);
	(*thread)->handle = CreateThread( NULL, 0, start_routine, (*thread)->argument, 0, &(*thread)->id);
#else
	if (attr == NULL)
	{
		attr = &(*thread)->attr;
		pthread_attr_init(attr);
	}
	pthread_attr_setdetachstate(attr, PTHREAD_CREATE_JOINABLE);

	ret = pthread_create(&(*thread)->pthread, attr, start_routine, arg);

#endif
	return ret;
}

int vthread_join(vthread_t thread, void **value_ptr)
{
	int ret;
#ifdef WIN32
	CloseHandle(thread->handle);
	if (thread->argument)
		ret = HeapFree(GetProcessHeap(), 0, thread->argument);
#else
	ret = pthread_join(thread->pthread, value_ptr);
#endif
	free(thread);
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
	sigset_t set;
	sigemptyset(&set);
	sigaddset(&set, SIGINT);
	sigaddset(&set,SIGTERM);

	int sig;
	sigwait(&set, &sig);
	if (sig == SIGINT || sig == SIGTERM)
	{
		int i;
		for (i = 0; i < nbthreads; i++) 
		{
			vthread_t thread = threads[i];
			pthread_kill(thread->pthread, sig);
			//pthread_cancel(thread->pthread);
		}
	}
#endif
}
