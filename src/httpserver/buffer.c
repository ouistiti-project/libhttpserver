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
#include "_string.h"
#include "dbentry.h"

#define buffer_dbg(...)

string_t *_string_create(const char *pointer, size_t length)
{
	string_t *str = calloc(1, sizeof(*str));
	_string_store(str, pointer, length);
	return str;
}

int _string_store(string_t *str, const char *pointer, size_t length)
{
	str->data = pointer;
	if (length == (size_t) -1)
		str->length = strlen(pointer);
	else
		str->length = length;
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

int _string_cmp(const string_t *str, const char *cmp)
{
	return strncasecmp(str->data, cmp, str->length);
}

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

int _buffer_accept(const buffer_t *buffer, size_t length)
{
	if ((buffer->data + buffer->size < buffer->offset + length) &&
		(buffer->maxchunks * ChunkSize < length))
		return EREJECT;
	return ESUCCESS;
}

char *_buffer_append(buffer_t *buffer, const char *data, size_t length)
{
	if (length == (size_t)-1)
		length = strlen(data);
	if (length == 0)
		return buffer->offset;

	if (buffer->data + buffer->size < buffer->offset + length)
	{
		int nbchunks = (length / ChunkSize) + 1;
		if (buffer->maxchunks > -1 && buffer->maxchunks - nbchunks < 0)
			nbchunks = buffer->maxchunks;
		size_t chunksize = ChunkSize * nbchunks;

		if (chunksize == 0)
		{
			err("buffer: max chunk: %lu", buffer->size / ChunkSize);
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
			err("buffer: out memory block: %lu", buffer->size + chunksize);
			return NULL;
		}
		if (buffer->maxchunks > -1)
			buffer->maxchunks -= nbchunks;
		buffer->size += chunksize;
		if (newptr != buffer->data)
		{
			const char *offset = buffer->offset;
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
	memcpy(buffer->data, buffer->offset, buffer->length);
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
		entry->key.data = key;
		entry->key.length = keylen;
		entry->value.data = value;
		entry->value.length = valuelen;
		entry->next = *db;
		*db = entry;
		buffer_dbg("fill \t%.*s\t%.*s", keylen, key, valuelen, value);
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
		if (key == NULL && storage->data[i] > 0x19 && storage->data[i] < 0x7f)
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
			if (key != NULL && _buffer_dbentry(storage, db, key, keylen, value, i) < 0)
				return -1;
			else
				count++;
			key = NULL;
			keylen = 0;
			value = NULL;
		}
	}
	if (key != NULL && _buffer_dbentry(storage, db, key, keylen, value, storage->length - 1) < 0)
		return -1;
	else
		count++;
	return count;
}

int _buffer_serializedb(buffer_t *storage, dbentry_t *entry, char separator, char fieldsep)
{
	while (entry != NULL)
	{
		const char *key = _string_get(&entry->key);
		size_t keylen = _string_length(&entry->key);
		const char *value = _string_get(&entry->value);
		size_t valuelen = _string_length(&entry->value);
		if (key < storage->data || (value + valuelen) > (storage->data + storage->length))
		{
			err("buffer: unserialized db with %.*s", (int)entry->key.length, entry->key.data);
			err("buffer:     => %.*s", (int)entry->value.length, entry->value.data);
			return EREJECT;
		}
		if (key[keylen] == '\0')
		{
			size_t keyof = (size_t)(key - storage->data);
			storage->data[keyof + keylen] = separator;
		}
		size_t valueof = (size_t)(value - storage->data);
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

const char *dbentry_search(dbentry_t *entry, const char *key)
{
	const char *value = NULL;
	while (entry != NULL)
	{
		if (!_string_cmp(&entry->key, key))
		{
			value = _string_get(&entry->value);
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

dbentry_t *dbentry_get(dbentry_t *entry, const char *key)
{
	while (entry != NULL)
	{
		if (!_string_cmp(&entry->key, key))
		{
			break;
		}
		entry = entry->next;
	}
	return entry;
}

int _buffer_deletedb(buffer_t *storage, dbentry_t *entry, int shrink)
{
	int ret = EREJECT;
	size_t length = 0;
	string_t *value = &entry->value;
	string_t *key = &entry->key;
	if (storage->data > key->data ||
		(storage->data + storage->length) < (value->data + value->length))
	{
		return ret;
	}
	/// remove value
	if (value->data >= storage->data &&
		value->data + value->length <= storage->data + storage->length)
	{
		storage->length -= value->length + 1;
		storage->offset -= value->length + 1;
		length = storage->length - (value->data - storage->data);
		char *data = storage->data + (size_t)(value->data - storage->data);
		if (shrink)
			memcpy(data, value->data + value->length + 1, length);
		else
			memset(data, 0, length);
	}
	/// remove key
	storage->length -= key->length + 1;
	storage->offset -= key->length + 1;
	length = storage->length - (key->data - storage->data);
	char *data = storage->data + (size_t)(key->data - storage->data);
	if (shrink)
		memcpy(data, key->data + key->length + 1, length);
	else
		memset(data, 0, length);
	ret = ESUCCESS;
	entry = entry->next;
	return ret;
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
