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
#ifndef WIN32
# include <netdb.h>
#else
# include <ws2tcpip.h>
#endif

#include "uri.h"

const char *g_localhost = "localhost";

int uri_parse(uri_t *uri, char *string)
{
	enum
	{
		e_proto,
		e_protoname,
		e_user,
		e_host,
		e_port,
		e_path,
		e_query,
		e_end
	} state = e_proto;
	char *it = string;
	char *end = string + strlen(string);

	if (uri == NULL || string == NULL)
		return -1;

	memset(uri, 0, sizeof(*uri));
	uri->proto = it;
	while (it < end)
	{
		if (*it == 0)
			state = e_end;
		switch(state)
		{
			case e_proto:
				if (*it == ':')
				{
					*it = 0;
					state = e_protoname;
					break;
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
					uri->path = it + 1;
					*it = 0;
					state = e_path;
				}
				if (*it == ':')
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
					uri->path = it + 1;
					*it = 0;
					state = e_path;
				}
			break;
			case e_port:
				if (*it == '/')
				{
					uri->path = it + 1;
					*it = 0;
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
	if (uri->port == 0 && uri->proto != NULL)
	{
		struct protoent *servptr;
		servptr = getprotobyname(uri->proto);
		if (servptr)
			uri->port = servptr->p_proto;
	}
	if (uri->host == NULL || strlen(uri->host) == 0)
	{
		uri->host = g_localhost;
	}
	return 0;
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
	if (uri.proto)
		printf("proto %s\n", uri.proto);
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
