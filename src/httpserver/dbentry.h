/*****************************************************************************
 * dbentry.h: Simple Database collection
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

#ifndef DBENTRY_H
#define DBENTRY_H

typedef struct dbentry_s dbentry_t;

#include "_buffer.h"

struct dbentry_s
{
	const buffer_t *storage;
	struct{
		size_t offset;
		size_t length;
	} key;
	struct{
		size_t offset;
		size_t length;
	} value;
	struct dbentry_s *next;
};

ssize_t dbentry_search(dbentry_t *entry, const char *key, const char **value);
void dbentry_destroy(dbentry_t *entry);
void dbentry_revert(dbentry_t *constentry, char separator, char fieldsep);
dbentry_t *dbentry_get(dbentry_t *entry, const char *key);

#endif
