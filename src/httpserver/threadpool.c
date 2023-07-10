/*****************************************************************************
 * threadpool.c: pthread pool
 * this file is part of https://github.com/ouistiti-project/libhttpserver
 *****************************************************************************
 * Copyright (C) 2021-2022
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

#include <pthread.h>
#include <stdlib.h>

#include "log.h"
#include "threadpool.h"

typedef struct thread_s thread_t;
struct thread_s
{
	pthread_t id;
	pthread_mutex_t mutex;
	pthread_cond_t cond;
	enum
	{
		E_STOPPED = 0,
		E_WAITING,
		E_RUNNING,
	} state;
	threadhandler_t hdl;
	void *hdldata;
	void *userdata;
	thread_t *next;
};

typedef struct threadpool_s threadpool_t;
struct threadpool_s
{
	thread_t *threads;
};

static void * _thread_run(void *data)
{
	thread_t *thread = (thread_t *)data;

	warn("thread start");
	while (thread->state > E_STOPPED)
	{
		pthread_mutex_lock(&thread->mutex);
		while (thread->state == E_WAITING)
			pthread_cond_wait(&thread->cond, &thread->mutex);
		pthread_mutex_unlock(&thread->mutex);
		if (thread->state == E_STOPPED)
			break;
		thread->hdl(thread->hdldata, thread->userdata);

		pthread_mutex_lock(&thread->mutex);
		thread->state = E_WAITING;
		pthread_mutex_unlock(&thread->mutex);
		pthread_cond_signal(&thread->cond);
	}
	warn("thread end");
}

int threadpool_grow(threadpool_t *pool)
{
	thread_t *it = calloc(1, sizeof(*it));

	it->next = pool->threads;
	pool->threads = it;

	pthread_mutex_init(&it->mutex, NULL);
	pthread_cond_init(&it->cond, NULL);
	it->state = E_WAITING;
	return pthread_create(&it->id, NULL, _thread_run, it);
}

threadpool_t *threadpool_init(int pooldepth)
{
	pthread_setcanceltype(PTHREAD_CANCEL_ENABLE, NULL);
	threadpool_t *pool = calloc(1, sizeof(*pool));

	for (int i = 0; i < pooldepth; i++)
	{
		threadpool_grow(pool);
	}
	return pool;
}

int threadpool_get(threadpool_t *pool, threadhandler_t hdl, void *hdldata, void *userdata)
{
	int ret = -1;
	int pressure = 0;
	for (thread_t *it = pool->threads; it != NULL; it = it->next)
	{
		if (it->state == E_WAITING)
		{
			pthread_mutex_lock(&it->mutex);
			it->hdl = hdl;
			it->hdldata = hdldata;
			it->userdata = userdata;
			it->state = E_RUNNING;
			pthread_mutex_unlock(&it->mutex);
			pthread_cond_signal(&it->cond);
			ret = (int) it->id;
			break;
		}
		else
			pressure++;
	}
	dbg("threadpool pressure %d", pressure);
	return ret;
}

int threadpool_wait(threadpool_t *pool, int id)
{
	thread_t *it = NULL;
	for (it = pool->threads; it != NULL && it->id != id; it = it->next);
	if (it == NULL)
		return -1;

	pthread_mutex_lock(&it->mutex);
	while (it->state == E_RUNNING)
		pthread_cond_wait(&it->cond, &it->mutex);
	pthread_mutex_unlock(&it->mutex);
	return -(it->state == E_STOPPED);
}

int threadpool_isrunning(threadpool_t *pool, int id)
{
	thread_t *it;
	for (it = pool->threads; it != NULL && it->id != id; it = it->next);
	if (it == NULL)
		return -1;
	return (it->state == E_RUNNING);
}

void threadpool_destroy(threadpool_t *pool)
{
	for (thread_t *it = pool->threads; it == NULL; it = it->next)
	{
		pthread_mutex_lock(&it->mutex);
		it->state = E_STOPPED;
		pthread_mutex_unlock(&it->mutex);
		pthread_cond_signal(&it->cond);
		pthread_cancel(it->id);
	}
}
