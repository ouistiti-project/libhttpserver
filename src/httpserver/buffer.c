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

#include "valloc.h"
#include "vthread.h"
#include "ouistiti/log.h"
#include "ouistiti/httpserver.h"
#include "_httpserver.h"
#include "_httpmessage.h"
#include "_buffer.h"
#include "_string.h"
#include "dbentry.h"

#define buffer_dbg(...)

static size_t _string_len(string_t *str)
{
	if (str->data && str->length == (size_t) -1)
		str->length = strnlen(str->data, STRING_MAXLENGTH);
	return str->length;
}

int _string_store(string_t *str, const char *pointer, size_t length)
{
	str->data = pointer;
	/// set length and check if value is -1
	str->length = length;
	str->length = _string_len(str);
	str->size = str->length + 1;
	return ESUCCESS;
}

size_t _string_length(const string_t *str)
{
	return str->length;
}

const char *_string_get(const string_t *str)
{
	return str->data;
}

int _string_cmp(const string_t *str, const char *cmp, size_t length)
{
	if (cmp == NULL)
		return -1;
	if ((length != (size_t) -1) && (length != str->length))
		return (length - str->length);
	return strncasecmp(str->data, cmp, str->length);
}

int _string_empty(const string_t *str)
{
	return ! (str->data != NULL && str->data[0] != '\0');
}

static int ChunkSize = HTTPMESSAGE_CHUNKSIZE;
/**
 * the chunksize has to be constant during the life of the application.
 * Two ways are available:
 *  - to store the chunksize into each buffer (takes a lot of place).
 *  - to store into a global variable (looks bad).
 */
buffer_t * _buffer_create(const char *name, int maxchunks)
{
	buffer_t *buffer = vcalloc(1, sizeof(*buffer));
	if (buffer == NULL)
		return NULL;
	buffer->name = name;
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

int _buffer_accept(const buffer_t *buffer, size_t length)
{
	if ((buffer->data + buffer->size < buffer->offset + length) &&
		(buffer->maxchunks * ChunkSize < length))
		return EREJECT;
	return ESUCCESS;
}

int _buffer_append(buffer_t *buffer, const char *data, size_t length)
{
	if (length == (size_t)-1)
		length = strnlen(data, ChunkSize);
	if (length == 0)
		return buffer->offset - buffer->data;

	if (buffer->data + buffer->size <= buffer->offset + length)
	{
		size_t available = buffer->size - (buffer->offset - buffer->data);
		int nbchunks = ((length - available) / ChunkSize) + 1;
		if (buffer->maxchunks > -1 && buffer->maxchunks - nbchunks < 0)
		{
			err("buffer: %s impossible to exceed to %d chunks", buffer->name, buffer->maxchunks);
			nbchunks = buffer->maxchunks;
		}
		size_t chunksize = ChunkSize * nbchunks;

		if (chunksize == 0)
		{
			err("buffer: max chunk: %lu", buffer->size / ChunkSize);
			return -1;
		}

		char *newptr = vrealloc(buffer->data, buffer->size + chunksize);
		if (newptr == NULL)
		{
			buffer->maxchunks = 0;
			warn("buffer: memory allocation error");
		}
		if (buffer->maxchunks == 0)
		{
			err("buffer: out memory block: %lu", buffer->size + chunksize);
			return -1;
		}
		if (buffer->maxchunks > 0)
			buffer->maxchunks -= nbchunks;
		buffer->size += chunksize;
		if (newptr != buffer->data)
		{
			const char *offset = buffer->offset;
			buffer->offset = newptr + (offset - buffer->data);
			buffer->data = newptr;
		}

		available = buffer->size - (buffer->offset - buffer->data);
		length = (length > available)? (available - 1): length;
	}
	char *offset = memcpy(buffer->offset, data, length);
	buffer->length += length;
	buffer->offset += length;
	buffer->data[buffer->length] = '\0';
	return offset - buffer->data;
}

int _buffer_fill(buffer_t *buffer, _buffer_fillcb cb, void * cbarg)
{
	int size = cb(cbarg, buffer->offset, buffer->size - buffer->length - 1);
	if (size > 0)
	{
		buffer->length += size;
		buffer->data[buffer->length] = 0;
	}
	return size;
}

char *_buffer_pop(buffer_t *buffer, size_t length)
{
	length = (length < buffer->length)? length: buffer->length;
	buffer->length -= length;
	buffer->offset -= length;
	*buffer->offset = '\0';
	return buffer->offset;
}

void _buffer_shrink(buffer_t *buffer)
{
	buffer->length -= (buffer->offset - buffer->data);
	while (buffer->length > 0 && (*(buffer->offset) == 0))
	{
		buffer->offset++;
		buffer->length--;
	}
	/// the memory overlap
	memmove(buffer->data, buffer->offset, buffer->length);
	buffer->data[buffer->length] = '\0';
	buffer->offset = buffer->data;
}

void _buffer_reset(buffer_t *buffer, size_t offset)
{
	buffer->offset = buffer->data + offset;
	buffer->length = offset;
	*(buffer->offset) = '\0';
}

int _buffer_rewindto(buffer_t *buffer, char needle)
{
	int ret = EINCOMPLETE;
	char *offset = buffer->data + buffer->length;
	while (offset > buffer->data && *offset != needle)
	{
		offset--;
	}
	if (*offset == needle)
	{
		*offset = '\0';
		buffer->offset = offset;
		buffer->length = buffer->offset - buffer->data;
		ret = ESUCCESS;
	}
	return ret;
}

const char *_buffer_get(const buffer_t *buffer, size_t from)
{
	if (from <= buffer->length)
		return buffer->data + from;
	return NULL;
}

int _buffer_dbentry(const buffer_t *storage, dbentry_t **db, const char *key, size_t keylen, const char * value, size_t end)
{
	size_t valuelen = 0;
	if (key[0] != 0)
	{
		if (value == NULL)
		{
			value = str_true;
			valuelen = 3;
		}
		else
		{
			valuelen = storage->data + end - value;
			storage->data[end] = '\0';
		}
		dbentry_t *entry;
		entry = vcalloc(1, sizeof(dbentry_t));
		if (entry == NULL)
			return -1;
		while (*key == ' ')
			key++;
		entry->storage = storage;
		entry->key.offset = key - storage->data;
		entry->key.length = keylen;
		entry->value.offset = value - storage->data;
		entry->value.length = valuelen;
		entry->next = *db;
		*db = entry;
		buffer_dbg("fill \t%.*s\t%.*s", (int)keylen, key, (int)valuelen, value);
	}
	return 0;
}

int _buffer_filldb(buffer_t *storage, dbentry_t **db, char separator, char fieldsep)
{
	char *key = NULL;
	char *value = NULL;
	int count = 0;
	size_t keylen = 0;

	for (int i = 0; i < storage->length; i++)
	{
		if (key == NULL && storage->data[i] > 0x20 && storage->data[i] < 0x7f)
			key = storage->data + i;
		if ((storage->data[i] == '\r') || (storage->data[i] == '\n'))
			storage->data[i] = '\0';
		if (key != NULL && storage->data[i] == separator && value == NULL)
		{
			keylen = storage->data + i - key;
			value = storage->data + i + 1;
			while (*value == ' ')
				value++;
		}
		else if (storage->data[i] == fieldsep || storage->data[i] == '\0')
		{
			if (key != NULL && keylen == 0)
			{
				keylen = &storage->data[i] - key;
				value = (char *)str_true;
			}
			if (key != NULL && _buffer_dbentry(storage, db, key, keylen, value, i) < 0)
				return -1;
			else
				count++;
			key = NULL;
			keylen = 0;
			value = NULL;
		}
	}
	if (key != NULL && _buffer_dbentry(storage, db, key, keylen, value, storage->length) < 0)
		return -1;
	else
		count++;
	return count;
}

int _buffer_serializedb(buffer_t *storage, dbentry_t *entry, char separator, char fieldsep)
{
	while (entry != NULL)
	{
		const char *key = storage->data + entry->key.offset;
		size_t keylen = entry->key.length;
		const char *value = storage->data + entry->value.offset;
		size_t valuelen = entry->value.length;
		if (key < storage->data || (value + valuelen) > (storage->data + storage->length))
		{
			err("buffer: unserialized db with %.*s", (int)entry->key.length, key);
			err("buffer:     => %.*s", (int)entry->value.length, value);
			return EREJECT;
		}
		if (key[keylen] == '\0')
		{
			size_t keyof = entry->key.offset;
			storage->data[keyof + keylen] = separator;
		}
		size_t valueof = entry->value.offset;
		if ( valueof < storage->length)
		{
			if (valuelen > 0 && (fieldsep == '\r' || fieldsep == '\n') && value[valuelen + 1] == '\0')
			{
				storage->data[valueof + valuelen] = '\r';
				storage->data[valueof + valuelen + 1] = '\n';
			}
			else if (valuelen > 0)
				storage->data[valueof + valuelen] = fieldsep;
		}
		entry = entry->next;
	}
	return ESUCCESS;
}

size_t _buffer_length(const buffer_t *buffer)
{
	return buffer->length;
}

int _buffer_empty(const buffer_t *buffer)
{
	return (buffer->length <= buffer->offset - buffer->data);
}

int _buffer_full(const buffer_t *buffer)
{
	return (buffer->length == buffer->size);
}

void _buffer_destroy(buffer_t *buffer)
{
	if (buffer->data != NULL)
		vfree(buffer->data);
	vfree(buffer);
}

ssize_t dbentry_search(dbentry_t *entry, const char *key, const char **value)
{
	ssize_t valuelen = EREJECT;
	if (value != NULL)
		*value = NULL;
	while (entry != NULL)
	{
		const char *sto_key = entry->storage->data + entry->key.offset;
		if (!strncasecmp(sto_key, key, entry->key.length))
		{
			if (value != NULL)
				*value = entry->storage->data + entry->value.offset;
			valuelen = entry->value.length;
			break;
		}
		entry = entry->next;
	}
	if (entry == NULL)
	{
		buffer_dbg("dbentry %s not found", key);
	}
	return valuelen;
}

dbentry_t *dbentry_get(dbentry_t *entry, const char *key)
{
	while (entry != NULL)
	{
		const char *sto_key = entry->storage->data + entry->key.offset;
		if (!strncasecmp(sto_key, key, entry->key.length))
		{
			break;
		}
		entry = entry->next;
	}
	return entry;
}

#if 0
//used only in httpclient and disabled
int _buffer_deletedb(buffer_t *storage, dbentry_t *entry, int shrink)
{
	int ret = EREJECT;
	size_t length = 0;

	if (storage != entry->storage)
		return ret;

	char *key = storage->data + entry->key.offset;
	size_t keylen = entry->key.length;
	char *value = storage->data + entry->value.offset;
	size_t valuelen = entry->value.length;

	if ((storage->data + storage->length) < (value + valuelen))
	{
		return ret;
	}
	/// remove value
	if (value + valuelen <= storage->data + storage->length)
	{
		storage->length -= valuelen + 1;
		storage->offset -= valuelen + 1;
		length = storage->length - entry->value.offset;
		if (shrink)
			memcpy(value, value + valuelen + 1, length);
		else
			memset(value, 0, length);
	}
	/// remove key
	storage->length -= keylen + 1;
	storage->offset -= keylen + 1;
	length = storage->length - entry->key.offset;
	if (shrink)
		memcpy(key, key + keylen + 1, length);
	else
		memset(key, 0, length);
	ret = ESUCCESS;
	entry = entry->next;
	return ret;
}
#endif

void dbentry_destroy(dbentry_t *entry)
{
	while (entry)
	{
		dbentry_t *next = entry->next;
		free(entry);
		entry = next;
	}
}
