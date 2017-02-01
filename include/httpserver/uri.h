/*****************************************************************************
 * uri.h: Simple uri parser
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

#ifndef __URI_H__
#define __URI_H__

#define MAX_QUERY 10

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct uri_s uri_t;
struct uri_s
{
	char *storage;
	const char	*scheme;
	const char	*user;
	const char	*host;
	const char	*port_str;
	int		port;
	const char	*path;
	const char	*query[MAX_QUERY];
	int		nbqueries;
};

uri_t *uri_create(char *src);
void uri_free(uri_t *uri);
int uri_parse(uri_t *dst, char *src);
const char *uri_query(uri_t *uri, char *key);
const char *uri_part(uri_t *uri, char *key);

typedef struct dbentry_s dbentry_t;


dbentry_t *dbentry_create(char separator, char *string, int storage);
const char *dbentry_value(dbentry_t *entry, char *key);
void dbentry_free(dbentry_t *entry);

#define post_create(string, storage) dbentry_create('=', string, storage)
#define post_value dbentry_value
#define post_free dbentry_free
#define header_create(string, storage) dbentry_create(':', string, storage)
#define header_value dbentry_value
#define header_free dbentry_free


#ifdef __cplusplus
}
#endif

#endif
