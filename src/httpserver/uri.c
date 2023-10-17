/*****************************************************************************
 * uri.c: Simple uri parser
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

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#ifndef WIN32
# include <netdb.h>
#else
# include <ws2tcpip.h>
#endif

#include "uri.h"

#define err(format, ...) fprintf(stderr, "\x1B[31m"format"\x1B[0m\n",  ##__VA_ARGS__)
#define warn(format, ...) fprintf(stderr, "\x1B[35m"format"\x1B[0m\n",  ##__VA_ARGS__)
#ifdef DEBUG
#define dbg(format, ...) fprintf(stderr, "\x1B[32m"format"\x1B[0m\n",  ##__VA_ARGS__)
#else
# define dbg(...)
#endif

const char *g_localhost = "localhost";
const char *g_defaultscheme = "http";

dbentry_t *dbentry_create(char separator, char *string, int storage)
{
	dbentry_t *entry;
	enum
	{
		e_key,
		e_value,
		e_end
	} state = e_key;
	char *it = string;
	char *end = string + strlen(string);

	if (string == NULL)
		return NULL;

	entry = calloc(1, sizeof(*entry));
	if (storage)
	{
		entry->storage = calloc(1, (end - it) + 1);
		memcpy(entry->storage, string, (end - it) + 1);
	}
	string_t *keyvalue = NULL;
	entry->key.data = it;
	keyvalue = &entry->key;
	while (it < end)
	{
		if (*it == 0)
			state = e_end;
		if (*it == '\r')
		{
			*it = '\0';
		}
		if (*it == '\n')
		{
			keyvalue->length = it - keyvalue->data;
			dbentry_t *new = calloc(1, sizeof(*new));
			new->key.data = it + 1;
			new->next = entry;
			entry = new;
			keyvalue = &entry->key;
			*it = 0;
			state = e_key;
		}
		switch(state)
		{
			case e_key:
			{
				if (*it == separator)
				{
					entry->value.data = it + 1;
					entry->key.length = it - entry->key.data;
					keyvalue = &entry->value;
					*it = 0;
					state = e_value;
				}
			}
			break;
			case e_value:
			{
			}
			break;
			case e_end:
				it = end;
			break;
		}
		it++;
	}
	return entry;
}

const char *dbentry_value(dbentry_t *entry, char *key)
{
	while (entry && _string_cmp(&entry->key, key, -1)) entry = entry->next;
	if (entry)
		return (const char *)entry->value.data;
	return NULL;
}

void dbentry_free(dbentry_t *entry)
{
	while (entry)
	{
		dbentry_t *keep = entry->next;
		if (entry->storage)
			free(entry->storage);
		free(entry);
		entry = keep;
	}
}

uri_t *uri_create(char *src)
{
	uri_t *entry;

	if (src == NULL)
		return NULL;

	entry = calloc(1, sizeof(*entry));
	if (entry == NULL)
		return NULL;
	entry->storage = calloc(1, strlen(src) + 1);
	if (entry->storage == NULL)
		return NULL;
	strcpy(entry->storage, src);
	uri_parse(entry, entry->storage);
	return entry;
}

void uri_free(uri_t *uri)
{
	free(uri->storage);
	free(uri);
}

const char *uri_part(uri_t *uri, char *key)
{
	if (strstr(key,"scheme"))
		return uri->scheme;
	if (strstr(key,"user"))
		return uri->user;
	if (strstr(key,"host"))
		return uri->host;
	if (strstr(key,"port"))
		return uri->port_str;
	if (strstr(key,"path"))
		return uri->path;
	return NULL;
}

int uri_parse(uri_t *uri, char *string)
{
	enum
	{
		e_scheme,
		e_protoname,
		e_user,
		e_host,
		e_port,
		e_path,
		e_query,
		e_end
	} state = e_scheme;
	char *it = string;
	char *end = string + strlen(string);

	if (uri == NULL || string == NULL)
		return -1;

	memset(uri, 0, sizeof(*uri));
	uri->scheme = it;
	while (it < end)
	{
		if (*it == 0)
			state = e_end;
		switch(state)
		{
			case e_scheme:
				if (*it == ':')
				{
					*it = 0;
					state = e_protoname;
					break;
				}
				if (*it == '/')
				{
					uri->scheme = g_defaultscheme;
					if (*(it+1) == '/')
					{
						uri->host = it + 2;
						state = e_host;
					}
					else
					{
						uri->path = it - 1;
						state = e_path;
					}
				}
			break;
			case e_protoname:
				if (*it == '/' && *(it + 1) == '/')
				{
					it++;
					uri->user = it + 1;
					state = e_user;
				}
				else
				{
					uri->user = it;
					it--;
					state = e_user;
				}
			break;
			case e_user:
				if (*it == '@')
				{
					uri->host = it + 1;
					*it = 0;
					state = e_host;
				}
				else if (*it == '/')
				{
					uri->host = uri->user;
					uri->user = NULL;
					uri->path = it;
					state = e_path;
				}
				else if (*it == ':')
				{
					uri->host = uri->user;
					uri->user = NULL;
					uri->port_str = it + 1;
					*it = 0;
					state = e_port;
				}
			break;
			case e_host:
				if (*it == ':')
				{
					uri->port_str = it + 1;
					*it = 0;
					state = e_port;
				}
				if (*it == '/')
				{
					uri->path = it;
					state = e_path;
				}
			break;
			case e_port:
				if (*it == '/')
				{
					uri->path = it;
					state = e_path;
				}
			break;
			case e_path:
				if (*it == '?')
				{
					uri->nbqueries = 1;
					uri->query[uri->nbqueries - 1] = it + 1;
					*it = 0;
					state = e_query;
				}
			break;
			case e_query:
				if (*it == '&')
				{
					uri->nbqueries++;
					if (uri->nbqueries > MAX_QUERY)
					{
						state = e_end;
						break;
					}
					uri->query[uri->nbqueries - 1] = it + 1;
					*it = 0;
					state = e_query;
				}
			break;
			case e_end:
				it = end;
			break;
		}
		it++;
	}
	if (uri->port_str)
	{
		uri->port = atoi(uri->port_str);
	}
	if (uri->port == 0 && uri->scheme != NULL)
	{
		struct protoent *servptr;
		servptr = getprotobyname(uri->scheme);
		if (servptr)
			uri->port = servptr->p_proto;
	}
	if (uri->host == NULL || strlen(uri->host) == 0)
	{
		uri->host = g_localhost;
	}
	return 0;
}

const char *uri_query(uri_t *uri, char *key)
{
	int i;
	int len = strlen(key);
	const char *ret = NULL;
	for (i = 0; i < uri->nbqueries; i++)
	{
		if (!strncmp(uri->query[i], key, len))
		{
			ret = &(uri->query[i][len]);
			if (*ret == '=')
				ret++;
			else
				ret = uri->query[i];
		}
	}
	return (const char *)ret;
}

#ifdef TEST
#include <stdio.h>

int main(int argc, char **argv)
{
	int i;
	uri_t uri;
	if (argc < 2)
		return -1;
	uri_parse(&uri, argv[1]);
	if (uri.scheme)
		printf("scheme %s\n", uri.scheme);
	if (uri.user)
		printf("user %s\n", uri.user);
	if (uri.host)
		printf("host %s\n", uri.host);
	if (uri.port)
		printf("port %d\n", uri.port);
	if (uri.path)
		printf("path %s\n", uri.path);
	for (i = 0; i < uri.nbqueries; i++)
		if (uri.query[i])
			printf("query %s\n", uri.query[i]);
	return 0;
}
#endif
