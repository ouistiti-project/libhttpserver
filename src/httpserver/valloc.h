/*****************************************************************************
 * vmalloc.h: Simple *alloc functions mapper
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

#ifndef VALLOC_H
#define VALLOC_H

#ifdef MBEDTLS_PLATFORM_MEMORY
# define vfree(...)	mbedtls_free(__VA_ARGS__)
# define vcalloc(...) mbedtls_calloc(__VA_ARGS__)
inline char *vrealloc(char *b, size_t s)
{
	char *p = mbedtls_calloc(s);
	memcpy(p, b, s);
	mbedtls_free(b);
	return p;
}
#else
# define vfree(...)	free(__VA_ARGS__)
# define vcalloc(...) calloc(__VA_ARGS__)
# define vrealloc(...) realloc(__VA_ARGS__)
#endif

#endif
