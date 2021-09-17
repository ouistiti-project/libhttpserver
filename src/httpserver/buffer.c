/*****************************************************************************
 * httpserver.c: Simple HTTP server
 * this file is part of https://github.com/ouistiti-project/libhttpserver
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

#if defined(__GNUC__) && !defined(_GNU_SOURCE)
# define _GNU_SOURCE
#else
# define strcasestr strstr
#endif
#define _XOPEN_SOURCE 700

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#ifdef USE_STDARG
#include <stdarg.h>
#endif

#include "valloc.h"
#include "vthread.h"
#include "log.h"
#include "httpserver.h"
#include "_httpserver.h"
#include "_httpmessage.h"
#include "_buffer.h"
#include "dbentry.h"

#define buffer_dbg(...)

static int ChunkSize = HTTPMESSAGE_CHUNKSIZE;
/**
 * the chunksize has to be constant during the life of the application.
 * Two ways are available:
 *  - to store the chunksize into each buffer (takes a lot of place).
 *  - to store into a global variable (looks bad).
 */
buffer_t * _buffer_create(int maxchunks)
{
	buffer_t *buffer = vcalloc(1, sizeof(*buffer));
	if (buffer == NULL)
		return NULL;
	/**
	 * nbchunks is unused here, because is it possible to realloc.
	 * Embeded version may use the nbchunk with special vcalloc.
	 * The idea is to create a pool of chunks into the stack.
	 */
	buffer->data = vcalloc(1, ChunkSize + 1);
	if (buffer->data == NULL)
	{
		free(buffer);
		return NULL;
	}
	buffer->maxchunks = maxchunks - 1;
	buffer->size = ChunkSize + 1;
	buffer->offset = buffer->data;
	return buffer;
}

int _buffer_chunksize(int new)
{
	if (new > 0)
		ChunkSize = new;
	return ChunkSize;
}

int _buffer_accept(buffer_t *buffer, int length)
{
	if ((buffer->data + buffer->size < buffer->offset + length) &&
		(buffer->maxchunks * ChunkSize < length))
		return EREJECT;
	return ESUCCESS;
}

char *_buffer_append(buffer_t *buffer, const char *data, int length)
{
	if (length == -1)
		length = strlen(data);
	if (length == 0)
		return buffer->offset;

	if (buffer->data + buffer->size < buffer->offset + length)
	{
		int nbchunks = (length / ChunkSize) + 1;
		if (buffer->maxchunks > -1 && buffer->maxchunks - nbchunks < 0)
			nbchunks = buffer->maxchunks;
		int chunksize = ChunkSize * nbchunks;

		if (chunksize == 0)
		{
			err("buffer: max chunk: %d", buffer->size / ChunkSize);
			return NULL;
		}

		char *newptr = vrealloc(buffer->data, buffer->size + chunksize);
		if (newptr == NULL)
		{
			buffer->maxchunks = 0;
			warn("buffer: memory allocation error");
		}
		if (buffer->maxchunks == 0)
		{
			err("buffer: out memory block: %d", buffer->size + chunksize);
			return NULL;
		}
		if (buffer->maxchunks > -1)
			buffer->maxchunks -= nbchunks;
		buffer->size += chunksize;
		if (newptr != buffer->data)
		{
			char *offset = buffer->offset;
			buffer->offset = newptr + (offset - buffer->data);
			buffer->data = newptr;
		}

		length = (length > chunksize)? (chunksize - 1): length;
	}
	char *offset = memcpy(buffer->offset, data, length);
	buffer->length += length;
	buffer->offset += length;
	buffer->data[buffer->length] = '\0';
	return offset;
}

char *_buffer_pop(buffer_t *buffer, int length)
{
	length = (length < buffer->length)? length: buffer->length;
	buffer->length -= length;
	buffer->offset -= length;
	*buffer->offset = '\0';
	return buffer->offset;
}

void _buffer_shrink(buffer_t *buffer, int reset)
{
	buffer->length -= (buffer->offset - buffer->data);
	while (buffer->length > 0 && (*(buffer->offset) == 0))
	{
		buffer->offset++;
		buffer->length--;
	}
	memcpy(buffer->data, buffer->offset, buffer->length);
	buffer->data[buffer->length] = '\0';
	if (!reset)
		buffer->offset = buffer->data + buffer->length;
	else
		buffer->offset = buffer->data;
}

void _buffer_reset(buffer_t *buffer)
{
	buffer->offset = buffer->data;
	buffer->length = 0;
}

int _buffer_rewindto(buffer_t *buffer, char needle)
{
	int ret = EINCOMPLETE;
	while (buffer->offset > buffer->data && *(buffer->offset) != needle)
	{
		buffer->offset--;
		buffer->length--;
	}
	if (*(buffer->offset) == needle)
	{
		*buffer->offset = '\0';
		ret = ESUCCESS;
	}
	return ret;
}

int _buffer_dbentry(buffer_t *storage, dbentry_t **db, char *key, const char * value)
{
	if (key[0] != 0)
	{
		if (value == NULL)
		{
			value = str_true;
		}
		dbentry_t *entry;
		entry = vcalloc(1, sizeof(dbentry_t));
		if (entry == NULL)
			return -1;
		while (*key == ' ')
			key++;
		entry->key = key;
		entry->value = value;
		entry->next = *db;
		*db = entry;
		buffer_dbg("fill \t%s\t%s", key, value);
	}
	return 0;
}

int _buffer_filldb(buffer_t *storage, dbentry_t **db, char separator, char fieldsep)
{
	int i;
	char *key = storage->data;
	const char *value = NULL;
	int count = 0;

	for (i = 0; i < storage->length; i++)
	{
		if (storage->data[i] == '\n')
			storage->data[i] = '\0';
		if (storage->data[i] == separator && value == NULL)
		{
			storage->data[i] = '\0';
			value = storage->data + i + 1;
			while (*value == ' ')
				value++;
		}
		else if ((storage->data[i] == '\0') ||
				(storage->data[i] == fieldsep))
		{
			storage->data[i] = '\0';
			if (_buffer_dbentry(storage, db, key, value) < 0)
				return -1;
			else
				count++;
			key = storage->data + i + 1;
			value = NULL;
		}
	}
	if (_buffer_dbentry(storage, db, key, value) < 0)
		return -1;
	else
		count++;
	return count;
}

int _buffer_empty(buffer_t *buffer)
{
	return (buffer->length <= buffer->offset - buffer->data);
}

int _buffer_full(buffer_t *buffer)
{
	return (buffer->length == buffer->size);
}

char _buffer_last(buffer_t *buffer)
{
	if (buffer->length > 0)
		return *(buffer->offset - 1);
	else
		return 0;
}

void _buffer_destroy(buffer_t *buffer)
{
	if (buffer->data != NULL)
		vfree(buffer->data);
	vfree(buffer);
}

const char *dbentry_search(dbentry_t *entry, const char *key)
{
	const char *value = NULL;
	while (entry != NULL)
	{
		if (!strcasecmp(entry->key, key))
		{
			value = entry->value;
			break;
		}
		entry = entry->next;
	}
	if (entry == NULL)
	{
		buffer_dbg("dbentry %s not found", key);
	}
	return value;
}

void dbentry_revert(dbentry_t *constentry, char separator, char fieldsep)
{
	dbentry_revert_t *entry = (dbentry_revert_t *)constentry;
	while (entry != NULL)
	{
		int i = 0;
		while ((entry->key[i]) != '\0') i++;
		(entry->key)[i] = separator;

		if (entry->key < entry->value)
		{
			int i = 0;
			while ((entry->value[i]) != '\0') i++;
			if ((fieldsep == '\r' || fieldsep == '\n') && entry->value[i + 1] == '\0')
			{
				(entry->value)[i] = '\r';
				(entry->value)[i + 1] = '\n';
			}
			else
				(entry->value)[i] = fieldsep;
		}
		else
		{
			if ((fieldsep == '\r' || fieldsep == '\n') && entry->key[i + 1] == '\0')
			{
				(entry->key)[i] = '\r';
				(entry->key)[i + 1] = '\n';
			}
		}

		entry = entry->next;
	}
}

void dbentry_destroy(dbentry_t *entry)
{
	while (entry)
	{
		dbentry_t *next = entry->next;
		free(entry);
		entry = next;
	}
}
