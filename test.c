/*****************************************************************************
 * test.c: test file
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

#ifdef HTTPSERVER
#include <stdio.h>
#include <string.h>

#ifndef WIN32
# include <pwd.h>
# include <sys/socket.h>
# include <sys/types.h>
# include <unistd.h>
#else
# include <winsock2.h>
#endif

#include "httpserver.h"

int client = 0;

int test_func1(void *arg, http_message_t *request, http_message_t *response)
{
	char content[] = "<html><body>coucou<br/></body></html>";
	httpmessage_addheader(response, "Server", "libhttpserver");
	httpmessage_addcontent(response, "text/html", content, strlen(content));
	return 0;
}

int test_func2(void *arg, http_message_t *request, http_message_t *response)
{
	httpmessage_addheader(response, "Server", "libhttpserver");
	httpmessage_addcontent(response, "text/html", NULL, 0);
	client = httpmessage_keepalive(response);
	return 0;
}

int main(int argc, char * const *argv)
{
#ifndef WIN32
	struct passwd *user;

	user = getpwnam("http");
	if (user == NULL)
		user = getpwnam("apache");
#endif
	setbuf(stdout, NULL);
	http_server_config_t config = { 
		.maxclient = 10,
		.chunksize = 64,
		.callback = { NULL, NULL, NULL},
	};
	http_server_t *server = httpserver_create(NULL, 80, &config);
	if (server)
	{
		httpserver_addconnector(server, NULL, test_func1, NULL);
#ifndef WIN32
		if (user != NULL)
		{
			setgid(user->pw_gid);
			setuid(user->pw_uid);
		}
#endif
		httpserver_connect(server);
/*
		char data[1550];
		memset(data, 0xA5, sizeof(data));
		char clock[4] = {'-','\\','|','/'};
		int i = 0;
		while (1)
		{
#ifndef WIN32
			sleep(1);
#else
			Sleep(1000);
#endif
			//printf(" %c\n",clock[i]);
			i = (i + 1)%sizeof(clock);
			if (client)
			{
				int ret;
				ret = send(client, data, sizeof(data), 0);
				printf("send stream %d\n",ret);
			}
		}
*/
		httpserver_disconnect(server);
	}
	return 0;
}
#endif
