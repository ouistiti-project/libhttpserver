/*****************************************************************************
 * utils.c: http utils  for modules
 * this file is part of https://github.com/ouistiti-project/libhttpserver
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
#include <sys/stat.h>

#include "httpserver/httpserver.h"
#include "httpserver/utils.h"

#define err(format, ...) fprintf(stderr, "\x1B[31m"format"\x1B[0m\n",  ##__VA_ARGS__)
#define warn(format, ...) fprintf(stderr, "\x1B[35m"format"\x1B[0m\n",  ##__VA_ARGS__)
#ifdef DEBUG
#define dbg(format, ...) fprintf(stderr, "\x1B[32m"format"\x1B[0m\n",  ##__VA_ARGS__)
#else
#define dbg(...)
#endif

static const char *_mimetype[] =
{
	"text/plain",
	"text/html",
	"text/css",
	"text/json",
	"application/javascript",
	"image/png",
	"image/jpeg",
	"application/octet-stream",
};

typedef struct mime_entry_s
{
	char *ext;
	utils_mimetype_enum type;
} mime_entry_t;

static const mime_entry_t *mime_entry[] =
{
	&(mime_entry_t){
		.ext = ".html,.xhtml,.htm",
		.type = MIME_TEXTHTML,
	},
	&(mime_entry_t){
		.ext = ".css",
		.type = MIME_TEXTCSS,
	},
	&(mime_entry_t){
		.ext = ".json",
		.type = MIME_TEXTJSON,
	},
	&(mime_entry_t){
		.ext = ".js",
		.type = MIME_APPLICATIONJAVASCRIPT,
	},
	&(mime_entry_t){
		.ext = ".text",
		.type = MIME_TEXTPLAIN,
	},
	&(mime_entry_t){
		.ext = ".png",
		.type = MIME_IMAGEPNG,
	},
	&(mime_entry_t){
		.ext = ".jpg",
		.type = MIME_IMAGEJPEG,
	},
	&(mime_entry_t){
		.ext = "*",
		.type = MIME_APPLICATIONOCTETSTREAM,
	},
	NULL
};

const char *utils_getmime(char *filepath)
{
	mime_entry_t *mime = (mime_entry_t *)mime_entry[0];
	char *fileext = strrchr(filepath,'.');
	if (fileext == NULL)
		return NULL;
	while (mime)
	{
		if (utils_searchext(fileext, mime->ext) == ESUCCESS)
		{
			break;
		}
		mime++;
	}
	if (mime)
		return _mimetype[mime->type];
	return NULL;
}

char *str_location = "Location";

char *utils_urldecode(char *encoded)
{
	if (encoded == NULL)
		return NULL;
	int length = strlen(encoded);
	if (length == 0)
		return NULL;
	char *decoded = calloc(1, length + 1);
	char *offset = decoded;
	/** leave the first / **/
	if (*encoded == '/')
		encoded++;
	while (*encoded != '\0')
	{
		if (*encoded == '%')
		{
			encoded++;
			char *end = strchr(encoded, ';');
			if (end == NULL)
			{
				int encval = strtol(encoded, &encoded, 16);
				*offset = (char) encval;
				offset++;
			}
			else
				encoded = end;
		}
		else if (encoded[0] == '.' && encoded[1] == '.' && encoded[2] == '/')
		{
			encoded+=3;
			if (offset > decoded && *(offset - 1) == '/')
			{
				offset--;
				*offset = '\0';
			}
			offset = strrchr(decoded, '/');
			if (offset == NULL)
				offset = decoded;
		}
		else if (*encoded == '?')
		{
			break;
		}
		else
		{
			*offset = *encoded;
			encoded++;
			offset++;
		}
	}
	*offset = 0;
	return decoded;
}

int utils_searchext(char *filepath, char *extlist)
{
	int ret = EREJECT;
	if (filepath != NULL)
	{
		char *filename = strrchr(filepath,'/');
		if (filename != NULL)
			filename++;
		else
			filename = filepath;
		char *ext = extlist;
		while (ext != NULL)
		{
			int i = 0;
			ret = ESUCCESS;
			int wildcard = 0;
			while( *ext != ',' && *ext != '\0')
			{
				if (*ext == '^')
				{
					if (filepath != filename)
					{
						ret = EREJECT;
						ext = strchr(ext, ',');
						break;
					}
					else
					{
						ext++;
						continue;
					}
				}
				if (*ext == '*')
				{
					ext++;
					wildcard = 1;
					break;
				}
				else if (!wildcard)
				{
					if (*ext != filepath[i])
					{
						ret = EREJECT;
						ext = strchr(ext, ',');
						break;
					}
					ext++;
				}
				else if (*ext == filepath[i])
				{
					ext++;
					wildcard = 0;
				}
				i++;
				if (filepath[i] == '\0')
					break;
			}
			if (ext == NULL)
				break;
			else if (*ext == '\0')
				ext = NULL;
			else
				ext++;
			if (ret == ESUCCESS)
				break;
		}
	}
	return ret;
}

char *utils_buildpath(char *docroot, char *path_info, char *filename, char *ext, struct stat *filestat)
{
	char *filepath;
	int length;

	length = strlen(docroot) + 1;
	length += strlen(path_info) + 1;
	length += strlen(filename);
	length += strlen(ext);
	filepath = calloc(1, length + 1);
	snprintf(filepath, length + 1, "%s/%s%s%s", docroot, path_info, filename, ext);

	filepath[length] = '\0';

	if (filestat)
	{
		memset(filestat, 0, sizeof(*filestat));
		if (stat(filepath, filestat))
		{
			free(filepath);
			return NULL;
		}
	}
	return filepath;
}
