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
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifndef WIN32
# include <pwd.h>
# include <sys/socket.h>
# include <sys/types.h>
# include <unistd.h>
#else
# include <winsock2.h>
#endif

#include "httpserver.h"

int test_client = 0;

#ifdef MBEDTLS
#include "mod_mbedtls.h"
#endif
#ifdef STATIC_FILE
#include "mod_static_file.h"
#endif

char test_content[] = "<html><head><link rel=\"stylesheet\" href=\"styles.css\"></head><body>coucou<br/></body></html>";
struct test_config_s
{
	int unsused;
};
struct test_config_s *test_config = NULL;
int test_func(void *arg, http_message_t *request, http_message_t *response)
{
	char * test = strstr(httpmessage_REQUEST(request, "uri"), "test");
	if (test == NULL)
		return EREJECT;
	printf("test\n");
	httpmessage_addheader(response, "Server", "libhttpserver");
#ifdef TEST_STREAMING
	httpmessage_addcontent(response, "text/html", NULL, 0);
	test_client = httpmessage_keepalive(response);
#else
	httpmessage_addcontent(response, "text/html", test_content, strlen(test_content));
#endif
	return ESUCCESS;
}


int main(int argc, char * const *argv)
{
#ifdef MBEDTLS
	http_server_config_t *config = &(http_server_config_t){ .port = 443, .addr = NULL, .keepalive = 1, .version = HTTP10};
#else
	http_server_config_t *config = NULL;
#endif
#ifndef WIN32
	struct passwd *user;

	user = getpwnam("http");
	if (user == NULL)
		user = getpwnam("apache");
#endif
	setbuf(stdout, NULL);
	http_server_t *server = httpserver_create(config);
	if (server)
	{
		httpserver_addconnector(server, NULL, test_func, test_config);
#ifdef MBEDTLS
		mod_mbedtls_t mbedtlsconfig = 
		{
			.pers = "httpserver-mbedtls",
			.crtfile = "/etc/ssl/private/server.pem",
			.pemfile = NULL,
			.cachain = NULL,
			.dhmfile = "/etc/ssl/private/dhparam.pem",
		};
		void *mod_mbedtls = mod_mbedtls_create(server, &mbedtlsconfig);
#endif
#ifdef STATIC_FILE
		void *mod_static_file = mod_static_file_create(server, NULL);
#endif
#ifndef WIN32
		if (user != NULL)
		{
			setgid(user->pw_gid);
			setuid(user->pw_uid);
		}
#endif
		httpserver_connect(server);
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
#ifdef TEST_STREAMING
			//printf(" %c\n",clock[i]);
			i = (i + 1)%sizeof(clock);
			if (client)
			{
				int ret;
				ret = send(client, data, sizeof(data), 0);
				printf("send stream %d\n",ret);
			}
#endif
		}
		httpserver_disconnect(server);
#ifdef MBEDTLS
		mod_mbedtls_destroy(mod_mbedtls);
#endif
#ifdef STATIC_FILE
		mod_static_file_destroy(mod_static_file);
#endif
	}
	return 0;
}
#endif
