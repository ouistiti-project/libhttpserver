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

typedef struct mime_entry_s mime_entry_t;
struct mime_entry_s
{
	const char *ext;
	const char *mime;
	mime_entry_t *next;
};

static const mime_entry_t *mime_entry =
&(mime_entry_t){
	.ext = ".text",
	.mime = "text/plain",
	.next = 
&(mime_entry_t){
	.ext = ".html,.xhtml,.htm",
	.mime = "text/html",
	.next =
&(mime_entry_t){
	.ext = ".css",
	.mime = "text/css",
	.next =
&(mime_entry_t){
	.ext = ".json",
	.mime = "text/json",
	.next =
&(mime_entry_t){
	.ext = ".js",
	.mime = "application/javascript",
	.next =
&(mime_entry_t){
	.ext = ".png",
	.mime = "image/png",
	.next =
&(mime_entry_t){
	.ext = ".jpg",
	.mime = "image/jpeg",
	.next =
&(mime_entry_t){
	.ext = "*",
	.mime = "application/octet-stream",
	.next = NULL
}}}}}}}};

static int _utils_searchexp(const char *haystack, const char *needleslist, int ignore_case);

void utils_addmime(const char *ext, const char*mime)
{
	mime_entry_t *entry = calloc(1, sizeof(*entry));
	entry->ext = ext;
	entry->mime = mime;
	mime_entry_t *last = (mime_entry_t *)mime_entry;
	if (last)
	{
		while (last->next) last = last->next;
		last->next = entry;
	}
	else
	{
		mime_entry = last;
	}
}

const char *utils_getmime(char *filepath)
{
	mime_entry_t *mime = (mime_entry_t *)mime_entry;
	while (mime)
	{
		if (_utils_searchexp(filepath, mime->ext, 1) == ESUCCESS)
		{
			break;
		}
		mime = mime->next;
	}
	if (mime)
		return mime->mime;
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
				int encval = 0;
				int i;
				for (i = 0; i < 2; i++)
				{
					encval = encval << 4;
					if (*encoded < 0x40)
						encval += (*encoded - 0x30);
					else if (*encoded < 0x47)
						encval += (*encoded - 0x41 + 10);
					else if (*encoded < 0x67)
						encval += (*encoded - 0x61 + 10);
					encoded ++;
				}
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

int utils_searchexp(const char *haystack, const char *needleslist)
{
	return _utils_searchexp(haystack, needleslist, 0);
}

static int _utils_searchexp(const char *haystack, const char *needleslist, int ignore_case)
{
	int ret = EREJECT;
	if (haystack != NULL)
	{
		int i = -1;
		char *needle = (char *)needleslist;
		while (needle != NULL)
		{
			i = -1;
			ret = ECONTINUE;
			int wildcard = 1;
			if (*needle == '^')
			{
				wildcard = 0;
			}
			char *needle_entry = needle;
			do
			{
				if (*needle == '*')
				{
					wildcard = 1;
					needle++;
				}
				i++;
				char hay = haystack[i];
				// lowercase
				if (ignore_case && hay > 0x40 && hay < 0x5b)
					hay += 0x20;
				if (!wildcard)
				{
					needle++;
					char lneedle = *needle;
					if (ignore_case && lneedle > 0x40 && lneedle < 0x5b)
						lneedle += 0x20;
					if (*needle == ',' || *needle == '\0')
					{
						if (hay != '\0')
							ret = EREJECT;
						else
							ret = ESUCCESS;
						break;
					}
					else if (lneedle != hay && *needle != '*')
					{
						needle = needle_entry;
						ret = EREJECT;
						if (*needle == '^')
						{
							break;
						}
					}
					else if (lneedle == hay)
					{
						ret = ESUCCESS;
					}
				}
				else
				{
					char lneedle = *needle;
					if (ignore_case && lneedle > 0x40 && lneedle < 0x5b)
						lneedle += 0x20;
					needle_entry = needle;
					ret = ESUCCESS;
					if (lneedle == hay)
					{
						wildcard = 0;
					}
					else if (*needle == ',' || *needle == '\0')
					{
						wildcard = 0;
						break;
					}
				}
			}
			while(haystack[i] != '\0');
			if (*needle == '*')
			{
				ret = ESUCCESS;
				needle++;
			}
			//dbg("searchexp %d %c %c", ret, *needle, haystack[i]);
			if (ret == ESUCCESS)
			{
				if (*needle == ',' || *needle == '\0')
					break;
				else
					ret = EREJECT;
			}
			needle = strchr(needle, ',');
			if (needle != NULL)
				needle++;
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

#ifdef TEST
int main()
{
	int ret = 0;
	ret = utils_searchexp("toto.js", ".txt,.js,.css");
	warn(" 1 ret %d/ESUCCESS", ret);
	ret = utils_searchexp("toto.js", ".txt,.json,.css");
	warn(" 2 ret %d/EREJECT", ret);
	ret = utils_searchexp("toto.json", ".txt,.js,.css");
	warn(" 3 ret %d/EREJECT", ret);
	ret = utils_searchexp("toto.json", ".txt,.json,.css");
	warn(" 4 ret %d/ESUCCESS", ret);
	ret = utils_searchexp("toto.json", ".txt,.css,.json");
	warn(" 5 ret %d/ESUCCESS", ret);
	ret = utils_searchexp("toto.json", ".txt,.css,.js*");
	warn(" 6 ret %d/ESUCCESS", ret);
	ret = utils_searchexp("toto.js", ".txt,.css,.js*");
	warn(" 7 ret %d/ESUCCESS", ret);
	ret = utils_searchexp("toto.json", "^.json,.css");
	warn(" 8 ret %d/EREJECT", ret);
	ret = utils_searchexp("public/toto.json", ".json,.css");
	warn(" 9 ret %d/ESUCCESS", ret);
	ret = utils_searchexp("public/toto.json", "public/*.json,public/*.css");
	warn("10 ret %d/ESUCCESS", ret);
	ret = utils_searchexp("test/public/toto.json", "public/*.json,public/*.css");
	warn("11 ret %d/ESUCCESS", ret);
	ret = utils_searchexp("public/toto.json", "^public/*.json,^public/*.css");
	warn("12 ret %d/ESUCCESS", ret);
	ret = utils_searchexp("test/public/toto.json", "^public/*.json,^public/*.css");
	warn("13 ret %d/EREJECT", ret);
	ret = utils_searchexp("public/", "public/*,public/*.css");
	warn("14 ret %d/ESUCCESS", ret);
	ret = utils_searchexp("public/", ".json,.css");
	warn("15 ret %d/EREJECT", ret);
	ret = utils_searchexp("public/", ".json,.css,*");
	warn("16 ret %d/ESUCCESS", ret);
	ret = utils_searchexp("public/toto.jpg", ".json,.css,*");
	warn("17 ret %d/ESUCCESS", ret);
	ret = utils_searchexp("public/to.to.jpg", ".json,.css,*");
	warn("18 ret %d/ESUCCESS", ret);
	ret = utils_searchexp("test/public/toto.min.js", "public/*.js,public/*.css");
	warn("19 ret %d/ESUCCESS", ret);
	ret = utils_searchexp("test/public/toto.min.css", "public/*.js,public/*.css");
	warn("20 ret %d/ESUCCESS", ret);
	ret = utils_searchexp("test/public/toto.css.none", "public/*.js,public/*.css");
	warn("21 ret %d/EREJECT", ret);
	return 0;
}
#endif
