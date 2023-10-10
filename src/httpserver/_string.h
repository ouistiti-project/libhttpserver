/*****************************************************************************
 * _string.h: string object private data
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

#ifndef __STRING_H__
#define __STRING_H__

typedef struct string_s string_t;
struct string_s
{
	const char *data;
	size_t length;
	size_t size;
};

#define STRING_REF(string) string, sizeof(string)-1
#define STRING_DCL(string) {.data=string, .size=sizeof(string), .length=sizeof(string)-1}
string_t *_string_create(const char *pointer, size_t length);
int _string_alloc(string_t *str, const char *pointer, size_t length);
int _string_store(string_t *str, const char *pointer, size_t length);
int _string_cpy(string_t *str, const char *source);
size_t _string_length(const string_t *str);
const char *_string_get(const string_t *str);
int _string_cmp(const string_t *str, const char *cmp, size_t length);
int _string_empty(const string_t *str);

#endif
