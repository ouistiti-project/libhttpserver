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

int client = 0;

#ifdef MBEDTLS
#include "mod_mbedtls.h"
#endif

#define TEST_FILE

char test_content[] = "<html><head><link rel=\"stylesheet\" href=\"styles.css\"></head><body>coucou<br/></body></html>";
#ifdef TEST_STREAMING
struct test_config_s
{
	int unsused;
};
struct test_config_s *test_config = NULL;
int test_func(void *arg, http_message_t *request, http_message_t *response)
{
	httpmessage_addheader(response, "Server", "libhttpserver");
	httpmessage_addcontent(response, "text/html", NULL, 0);
	client = httpmessage_keepalive(response);
	return 0;
}
#elif defined(TEST_FILE)
struct test_config_s
{
	char *rootdoc;
};
struct test_config_s g_test_config = 
{
	.rootdoc = "/srv/www/htdocs",
};
struct test_connection_s
{
	int type;
	FILE *fileno;
};
struct test_config_s *test_config = &g_test_config;
int test_func(void *arg, http_message_t *request, http_message_t *response)
{
	struct test_config_s *config = (struct test_config_s *)arg;
	char content[64];
	int size;
	struct test_connection_s *private = httpmessage_private(request, NULL);
	if (!private)
	{
		private = calloc(1, sizeof(*private));
		private->type = 0xAABBCCDD;

		char filepath[512];
		snprintf(filepath, 511, "%s%s", config->rootdoc, httpmessage_REQUEST(request, "uri"));
		struct stat filestat;
		int ret = stat(filepath, &filestat);
		if (S_ISDIR(filestat.st_mode))
		{
			snprintf(filepath, 511, "%s%s/index.html", config->rootdoc, httpmessage_REQUEST(request, "uri"));
			stat(filepath, &filestat);
		}
		if (ret != 0)
		{
			printf("file: %s not found\n", filepath);
			return EREJECT;
		}
		printf("open file %d %s\n", filestat.st_size, filepath);
		httpmessage_addheader(response, "Server", "libhttpserver");
		private->fileno = fopen(filepath, "rb");
		httpmessage_private(request, (void *)private);
	}
	/**
	 * TODO support of private from another callback
	 */
	if (private->type != 0xAABBCCDD)
		return EREJECT;
	if (feof(private->fileno))
	{
		printf("end of file\n");
		fclose(private->fileno);
		private->fileno = NULL;
		free(private);
		return ESUCCESS;
	}
	size = fread(content, 1, sizeof(content), private->fileno);
	printf("read %d %d\n", size, sizeof(content));
	httpmessage_addcontent(response, "text/html", content, size);
	return ECONTINUE;
}
#else
struct test_config_s
{
	int unsused;
};
struct test_config_s *test_config = NULL;
int test_func(void *arg, http_message_t *request, http_message_t *response)
{
	httpmessage_addheader(response, "Server", "libhttpserver");
	httpmessage_addcontent(response, "text/html", test_content, strlen(test_content));
	return 0;
}
#endif

int main(int argc, char * const *argv)
{
	http_server_config_t *config = NULL;
#ifndef WIN32
	struct passwd *user;

	user = getpwnam("http");
	if (user == NULL)
		user = getpwnam("apache");
#endif
	setbuf(stdout, NULL);
#ifdef MBEDTLS
	mod_mbedtls_t mbedtlsconfig = 
	{
		.pers = "httpserver-mbedtls",
		.crtfile = "/etc/ssl/private/server.pem",
		.pemfile = NULL,
		.cachain = NULL,
		.dhmfile = "/etc/ssl/private/dhparam.pem",
	};
	void * mod = mod_mbedtls_create(&mbedtlsconfig);
	config = mod_mbedtls_getmod(mod);
#endif
	http_server_t *server = httpserver_create(config);
	if (server)
	{
		
		httpserver_addconnector(server, NULL, test_func, test_config);
#ifndef WIN32
		if (user != NULL)
		{
			setgid(user->pw_gid);
			setuid(user->pw_uid);
		}
#endif
		httpserver_connect(server);
#ifdef TEST_STREAMING
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
#endif
		httpserver_disconnect(server);
	}
#ifdef MBEDTLS
	mod_mbedtls_destroy(mod);
#endif
	return 0;
}
#endif
