/*****************************************************************************
 * mod_document.c: callbacks and management of files
 * this file is part of https://github.com/ouistiti-project/ouistiti
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

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <errno.h>
#include <time.h>

#include "httpserver/httpserver.h"
#include "httpserver/utils.h"
#include "httpserver/log.h"
#include "mod_document.h"
#include "mod_auth.h"

#define document_dbg(...)

/**
 * transfer function for getfile_connector
 */
static int mod_send_read(document_connector_t *private, http_message_t *response);
#ifdef SENDFILE
extern int mod_send_sendfile(document_connector_t *private, http_message_t *response);
#endif
static int _mime_connector(void *arg, http_message_t *request, http_message_t *response);

static const char str_put[] = "PUT";
static const char str_delete[] = "DELETE";

static const char str_document[] = "document";

/**
 * USE_PRIVATE is used to keep a sample of cade which uses
 * the httpmessage_private function
 */
typedef struct _document_connector_s document_connector_t;

int mod_send(document_connector_t *private, http_message_t *response);

int document_close(document_connector_t *private, http_message_t *request)
{
	if (private->fdfile > 0)
		close(private->fdfile);
	private->fdfile = 0;
	if (private->fdroot > 0)
		close(private->fdroot);
	private->fdroot = 0;
	private->func = NULL;
	private->dir = NULL;
	httpmessage_private(request, NULL);
	free(private);
}

static int document_checkname(const _mod_document_mod_t *mod, const char *uri)
{
	const mod_document_t *config = mod->config;

	if (utils_searchexp(uri, config->deny, NULL) == ESUCCESS)
	{
		return  EREJECT;
	}
	if (utils_searchexp(uri, config->allow, NULL) != ESUCCESS)
	{
		return  EREJECT;
	}
	return ESUCCESS;
}

static int _document_docroot(_mod_document_mod_t *mod,
		http_message_t *request, const char *url)
{
	const mod_document_t *config = mod->config;
	int fdroot = mod->fdroot;

#ifdef DOCUMENTHOME
	if (url[0] == '~' && mod->fdhome != 0)
	{
		fdroot = mod->fdhome;
#ifdef AUTH
		if (url[1] == '/')
		{
			const char *userhome = auth_info(request, "user");
			fdroot = openat(fdroot, userhome, O_DIRECTORY);
			if (fdroot == -1)
			{
				err("document: user %s home directory not found", userhome);
				fdroot = mod->fdhome;
			}
		}
#endif
	}
#ifdef AUTH
	else if (config->options & DOCUMENT_HOME)
	{
		const char *home = auth_info(request, "home");
		if (home != NULL)
		{
			fdroot = open(home, O_DIRECTORY);
			if (fdroot == -1)
			{
				err("document: user %s home directory not found", home);
				fdroot = mod->fdroot;
			}
		}
	}
#endif
#endif
	return fdroot;
}

#ifdef DOCUMENTREST
static int _document_getconnnectorput(_mod_document_mod_t *mod,
		int fdroot, int fdfile, const char *url,
		http_message_t *request, http_message_t *response,
		http_connector_t *connector, struct stat *filestat)
{
	const mod_document_t *config = mod->config;

	if ((config->options & DOCUMENT_REST) == 0)
	{
		err("document: file %s rest not allowed", url);
		close(fdfile);
		return -1;
	}

	int length = strlen(url);
	if (fdfile > 0 || length < 1)
	{
		close(fdfile);
		const char *uri = httpmessage_REQUEST(request,"uri");

		err("document: file %s already exists", url);
		httpmessage_addcontent(response, "text/json", "{\"method\":\"PUT\",\"result\":\"KO\",\"name\":\"", -1);
		httpmessage_appendcontent(response, uri, -1);
		httpmessage_appendcontent(response, "\"}\n", -1);
#if defined RESULT_405
		httpmessage_result(response, RESULT_405);
#else
		httpmessage_result(response, RESULT_400);
#endif
		return 0;
	}
	else if (url[length - 1] == '/')
	{
		err("document: %s found dir", url);
		fdfile = dup(fdroot);
	}
	else
		fdfile = openat(fdroot, url, O_WRONLY | O_CREAT | O_EXCL, 0644);
	if (fstat(fdfile, filestat) == -1)
	{
		err("document: spurious error on fstat %s", strerror(errno));
		close(fdfile);
		return -1;
	}
	char range[20];
	snprintf(range, 20, "bytes %.9ld/*", (long)filestat->st_size);
	httpmessage_addheader(response, "Content-Range", range);
	*connector = putfile_connector;
	return fdfile;
}
#endif

#ifdef DOCUMENTREST
static int _document_getconnnectorpost(_mod_document_mod_t *mod,
		int fdroot, int fdfile, const char *url,
		http_message_t *UNUSED(request), http_message_t *response,
		http_connector_t *connector, struct stat *filestat)
{
	const mod_document_t *config = mod->config;

	if ((config->options & DOCUMENT_REST) == 0)
	{
		err("document: file %s rest not allowed", url);
		close(fdfile);
		return -1;
	}
	if (fstat(fdfile, filestat) == -1)
	{
		err("document: spurious error on fstat %s", strerror(errno));
		close(fdfile);
		return -1;
	}
	*connector = postfile_connector;
	char range[20];
	snprintf(range, 20, "bytes %.9ld/*", (long)filestat->st_size);
	httpmessage_addheader(response, "Content-Range", range);
	close(fdfile);
	return openat(fdroot, url, O_RDWR | O_TRUNC);
}
#endif

#ifdef DOCUMENTREST
static int _document_getconnnectordelete(_mod_document_mod_t *mod,
		int fdroot, int fdfile, const char *url,
		http_message_t *UNUSED(request), http_message_t *response,
		http_connector_t *connector, struct stat *filestat)
{
	const mod_document_t *config = mod->config;

	if ((config->options & DOCUMENT_REST) == 0)
	{
		err("document: file %s rest not allowed", url);
		close(fdfile);
		return -1;
	}
	if (fstat(fdfile, filestat) == -1)
	{
		err("document: spurious error on fstat %s", strerror(errno));
		close(fdfile);
		return -1;
	}
	*connector = deletefile_connector;
	char range[20];
	snprintf(range, 20, "bytes %.9ld/*", (long)filestat->st_size);
	httpmessage_addheader(response, "Content-Range", range);
	close(fdfile);
	/**
	 * delete use only the url fdfile is useless
	 */
	return dup(fdroot);
}
#endif

static int _document_getdefaultpage(_mod_document_mod_t *mod, int fdroot, const char *url, http_message_t *response)
{
	const mod_document_t *config = mod->config;
	int fdfile = openat(fdroot, config->defaultpage, O_RDONLY);
	if (fdfile > 0)
	{
		dbg("document: move to %s/%s", url, config->defaultpage);
		/**
		 * Check uri is only one character.
		 * It should be "/"
		 */
		if (url[0] != '\0')
			httpmessage_addheader(response, str_location, "/");
		else
			httpmessage_addheader(response, str_location, "");
		httpmessage_appendheader(response, str_location, url, "/", config->defaultpage, NULL);
	}
	return fdfile;
}

static int _document_getconnnectorget(_mod_document_mod_t *mod,
		int fdroot, int fdfile, const char *url,
		http_message_t *request, http_message_t *response,
		http_connector_t *connector, struct stat *filestat)
{
	const mod_document_t *config = mod->config;

	if (fstat(fdfile, filestat) == -1)
	{
		err("document: spurious error on fstat %s", strerror(errno));
		close(fdfile);
		return -1;
	}
	if (S_ISDIR(filestat->st_mode))
	{
#ifdef DIRLISTING
		const char *X_Requested_With = httpmessage_REQUEST(request, "X-Requested-With");
		if ((X_Requested_With && strstr(X_Requested_With, "XMLHttpRequest") != NULL) &&
			(config->options & DOCUMENT_DIRLISTING))
		{
			close(fdfile);
			fdfile = openat(fdroot, url, O_DIRECTORY);
			*connector = dirlisting_connector;
		}
		else
#endif
		{
			close(fdroot);
			fdroot = fdfile;
			fdfile = _document_getdefaultpage(mod, fdroot, url, response);
			if (fdfile > 0)
			{
#if defined(RESULT_301)
				httpmessage_result(response, RESULT_301);
				close(fdfile);
				return 0;
#endif
			}
			else
			{
				dbg("document: %s is directory", url);
				close(fdfile);
				return -1;
			}
		}
	}
	else if (filestat->st_size == 0)
	{
		dbg("document: empty file");
#if defined(RESULT_204)
		httpmessage_result(response, RESULT_204);
#else
		const char *mime = NULL;
		mime = utils_getmime(private->url);
		httpmessage_addcontent(response, (char *)mime, NULL, 0);
#endif
		close(fdfile);
		return 0;
	}
	else
	{
		mod->transfer = mod_send_read;
#ifdef SENDFILE
		if (config->options & DOCUMENT_SENDFILE)
			mod->transfer = mod_send_sendfile;
#endif
	}
	return fdfile;
}

static int _document_getconnnectorheader(_mod_document_mod_t *mod,
		int fdroot, int fdfile, const char *url,
		http_message_t *request, http_message_t *response,
		http_connector_t *connector, struct stat *filestat)
{
	fdfile = _document_getconnnectorget(mod, fdroot, fdfile, url,
				request, response, connector, filestat);

	if (fdfile > 0)
	{
		document_connector_t *private = NULL;
		const mod_document_t *config = mod->config;

		private = calloc(1, sizeof(*private));
		httpmessage_private(request, private);

		private->mod = mod;
		private->fdfile = fdfile;
		private->url = url;
		private->size = filestat->st_size;
		private->offset = 0;
		_mime_connector(mod, request, response);
#ifdef RANGEREQUEST
		if (config->options & DOCUMENT_RANGE)
			range_connector(mod, request, response);
#endif
		close(fdfile);
		free(private);
	}
	return 0;
}

static int _document_connector(void *arg, http_message_t *request, http_message_t *response)
{
	int ret =  EREJECT;
	document_connector_t *private = httpmessage_private(request, NULL);
	_mod_document_mod_t *mod = (_mod_document_mod_t *)arg;
	http_connector_t connector = getfile_connector;

	if (private != NULL)
	{
		err("document: client should be uninitialized");
		return EREJECT;
	}
	const char *uri = httpmessage_REQUEST(request,"uri");
	if (uri[0] == '/')
		uri++;

	if (document_checkname(mod, uri) == EREJECT)
	{
		dbg("document: %s forbidden extension", uri);
		/**
		 * Another module may have the same docroot and
		 * accept the name of the uri.
		 * The module has not to return an error.
		 */
		return  EREJECT;
	}

	int fdroot = dup(_document_docroot(mod, request, uri));

	const char *url = uri;
	if (url[0] == '~')
		url++;

	while (url[0] == '/' && url[0] != '\0')
		url++;

	int length = strlen(url);

	int fdfile = 0;
	if (length > 0)
	{
		dbg("document: open %s", url);
		fdfile = openat(fdroot, url, O_RDONLY );
	}
	else
		fdfile = dup(fdroot);

	struct stat filestat;

	int type = 0;
	const char *method = httpmessage_REQUEST(request, "method");
#ifdef DOCUMENTREST
	if (!strcmp(method, str_put))
	{
		fdfile = _document_getconnnectorput(mod, fdroot, fdfile, url,
					request, response, &connector, &filestat);
		type |= DOCUMENT_REST;
	}
	else if (!strcmp(method, "POST"))
	{
		fdfile = _document_getconnnectorpost(mod, fdroot, fdfile, url,
					request, response, &connector, &filestat);
		type |= DOCUMENT_REST;
	}
	else if (!strcmp(method, str_delete))
	{
		fdfile = _document_getconnnectordelete(mod, fdroot, fdfile, url,
					request, response, &connector, &filestat);
		type |= DOCUMENT_REST;
	}
	else
#endif
	if (!strcmp(method, str_get))
	{
		fdfile = _document_getconnnectorget(mod, fdroot, fdfile, url,
					request, response, &connector, &filestat);
	}
	else if (!strcmp(method, str_head))
	{
		fdfile = _document_getconnnectorheader(mod, fdroot, fdfile, url,
					request, response, &connector, &filestat);
	}
	else
	{
		close(fdfile);
		close(fdroot);
		return EREJECT;
	}
	if (fdfile == 0)
	{
		close(fdroot);
		return  ESUCCESS;
	}
	else if (fdfile == -1)
	{
		dbg("document: %s not exist %s", uri, strerror(errno));
		close(fdroot);
		return  EREJECT;
	}
	if (S_ISDIR(filestat.st_mode))
	{
		type |= DOCUMENT_DIRLISTING;
	}

	private = calloc(1, sizeof(*private));
	httpmessage_private(request, private);

	private->mod = mod;
	private->ctl = httpmessage_client(request);
	private->fdfile = fdfile;
	private->fdroot = fdroot;
	private->url = url;
	private->func = connector;
	private->size = filestat.st_size;
	private->offset = 0;
	private->type = type;
#ifdef DEBUG
	clock_gettime(CLOCK_REALTIME, &private->start);
	private->datasize = private->size;
#endif

	return EREJECT;
}

int getfile_connector(void *arg, http_message_t *request, http_message_t *response)
{
	document_connector_t *private = httpmessage_private(request, NULL);
	const _mod_document_mod_t *mod = (_mod_document_mod_t *)arg;
	int ret;

	ret = mod->transfer(private, response);
	if (ret < 0)
	{
		if (errno == EAGAIN)
			return ECONTINUE;
		err("document: send %s (%d,%s)", private->url, ret, strerror(errno));
		document_close(private, request);
		/**
		 * it is too late to set an error here
		 */
		return EREJECT;
	}
	private->offset += ret;
	private->size -= ret;
	if (ret == 0 || private->size <= 0)
	{
#ifdef DEBUG
		struct timespec stop;
		struct timespec value;
		clock_gettime(CLOCK_REALTIME, &stop);

		value.tv_sec = stop.tv_sec - private->start.tv_sec;
		value.tv_nsec = stop.tv_nsec - private->start.tv_nsec;
		if (value.tv_sec == 0 && ((long)value.tv_nsec/1000000) == 0)
			dbg("document: (%llu bytes) %03ld ns", private->datasize, value.tv_nsec);
		else
			dbg("document: (%llu bytes) time %ld:%03ld", private->datasize, value.tv_sec, value.tv_nsec/1000000);
#endif
		warn("document: send %s", private->url);
		document_close(private, request);
		return ESUCCESS;
	}
	return ECONTINUE;
}

static int mod_send_read(document_connector_t *private, http_message_t *response)
{
	int ret = 0;
	int size;
	int chunksize;
	char content[CONTENTCHUNK];

	/**
	 * check the size for the range support
	 * the size may be different of the real size file
	 */
	chunksize = (CONTENTCHUNK > private->size)?private->size:CONTENTCHUNK;
	size = read(private->fdfile, content, chunksize);
	if (size > 0)
	{
		ret = size;
		content[size] = 0;
		httpmessage_addcontent(response, "none", content, size);
	}
	else if (ret == 0)
	{
		ret = ESUCCESS;
	}
	return ret;
}

static int _transfer_connector(void *arg, http_message_t *request, http_message_t *response)
{
	const document_connector_t *private = httpmessage_private(request, NULL);

	if (private && private->func)
	{
		return private->func(arg, request, response);
	}
	return EREJECT;
}

static int _mime_connector(void *arg, http_message_t *request, http_message_t *response)
{
	const document_connector_t *private = httpmessage_private(request, NULL);

	if (private == NULL ||
		(private->type & DOCUMENT_DIRLISTING) ||
		(private->type & DOCUMENT_REST) ||
		!(private->fdfile > 0))
	{
		return EREJECT;
	}

	const char *mime = NULL;
	mime = utils_getmime(private->url);
	httpmessage_addcontent(response, mime, NULL, private->size);

	return EREJECT;
}

void *mod_document_create(http_server_t *server, mod_document_t *config)
{
	if (!config)
	{
		err("document: configuration empty");
		return NULL;
	}
	_mod_document_mod_t *mod = calloc(1, sizeof(*mod));

	mod->config = config;
	mod->fdroot = open(config->docroot, O_DIRECTORY);
	if (mod->fdroot == -1)
	{
		err("document: docroot %s not found", config->docroot);
	}
#ifdef DOCUMENTHOME
	if (config->dochome != NULL)
	{
		mod->fdhome = open(config->dochome, O_DIRECTORY);
		if (mod->fdhome == -1)
		{
			err("document: dochome %s not found", config->dochome);
		}
	}
#endif
	httpserver_addconnector(server, _document_connector, mod, CONNECTOR_DOCUMENT, str_document);
#ifdef RANGEREQUEST
	if (config->options & DOCUMENT_RANGE)
		httpserver_addconnector(server, range_connector, mod, CONNECTOR_DOCUMENT, str_document);
#endif
	httpserver_addconnector(server, _mime_connector, mod, CONNECTOR_DOCUMENT, str_document);
	httpserver_addconnector(server, _transfer_connector, mod, CONNECTOR_DOCUMENT, str_document);

#ifdef DOCUMENTREST
	if (config->options & DOCUMENT_REST)
	{
		httpserver_addmethod(server, str_put, 1);
		httpserver_addmethod(server, str_delete, 1);
	}
#endif
	return mod;
}

void mod_document_destroy(void *data)
{
	free(data);
}

const module_t mod_document =
{
	.name = str_document,
	.create = (module_create_t)&mod_document_create,
	.destroy = &mod_document_destroy
};
#ifdef MODULES
extern module_t mod_info __attribute__ ((weak, alias ("mod_document")));
#endif
