/*****************************************************************************
 * _buffer.h: buffer object private data
 *****************************************************************************
 * Copyright (C) 2016-2017
 *
 * Authors: Marc Chalain <marc.chalain@gmail.com
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

#ifndef ___BUFFER_H__
#define ___BUFFER_H__

typedef struct buffer_s buffer_t;
struct buffer_s
{
	char *data;
	char *offset;
	size_t size;
	size_t length;
	int maxchunks;
};

buffer_t * _buffer_create(int maxchunks);
int _buffer_chunksize(int new);

int _buffer_accept(const buffer_t *buffer, size_t length);
char *_buffer_append(buffer_t *buffer, const char *data, size_t length);

char *_buffer_pop(buffer_t *buffer, size_t length);
void _buffer_shrink(buffer_t *buffer);
void _buffer_reset(buffer_t *buffer, size_t offset);
int _buffer_rewindto(buffer_t *buffer, char needle);

const char *_buffer_get(const buffer_t *buffer, size_t from);
size_t _buffer_length(const buffer_t *buffer);
int _buffer_empty(const buffer_t *buffer);
int _buffer_full(const buffer_t *buffer);

int _buffer_dbentry(buffer_t *storage, dbentry_t **db, char *key, const char * value);
int _buffer_filldb(buffer_t *storage, dbentry_t **db, char separator, char fieldsep);
void _buffer_destroy(buffer_t *buffer);

#endif
