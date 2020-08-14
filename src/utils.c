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
#include <errno.h>
#include <unistd.h>

#include "httpserver/log.h"
#include "httpserver/httpserver.h"
#include "httpserver/utils.h"

#define utils_dbg(...)

const char str_location[] = "Location";
const char str_textplain[] = "text/plain";
const char str_texthtml[] = "text/html";
const char str_textcss[] = "text/css";
const char str_textjson[] = "text/json";
const char str_imagepng[] = "image/png";
const char str_imagejpeg[] = "image/jpeg";
const char str_applicationjavascript[] = "application/javascript";
const char str_applicationoctetstream[] = "application/octet-stream";

typedef struct mime_entry_s mime_entry_t;
struct mime_entry_s
{
	const char *ext;
	const char *mime;
	mime_entry_t *next;
};

static mime_entry_t *mime_entry = NULL;

static const mime_entry_t *mime_default =
(mime_entry_t *)&(const mime_entry_t){
	.ext = "*",
	.mime = str_applicationoctetstream,
	.next =
(mime_entry_t *)&(const mime_entry_t){
	.ext = ".text",
	.mime = str_textplain,
	.next =
(mime_entry_t *)&(const mime_entry_t){
	.ext = ".html,.xhtml,.htm",
	.mime = str_texthtml,
	.next =
(mime_entry_t *)&(const mime_entry_t){
	.ext = ".css",
	.mime = str_textcss,
	.next =
(mime_entry_t *)&(const mime_entry_t){
	.ext = ".json",
	.mime = str_textjson,
	.next =
(mime_entry_t *)&(const mime_entry_t){
	.ext = ".js",
	.mime = str_applicationjavascript,
	.next =
(mime_entry_t *)&(const mime_entry_t){
	.ext = ".png",
	.mime = str_imagepng,
	.next =
(mime_entry_t *)&(const mime_entry_t){
	.ext = ".jpg",
	.mime = str_imagejpeg,
	.next = NULL
}}}}}}}};

static int _utils_searchexp(const char *haystack, const char *needleslist, int ignore_case, const char **rest);

void utils_addmime(const char *ext, const char*mime)
{
	mime_entry_t *entry = calloc(1, sizeof(*entry));
	entry->ext = ext;
	entry->mime = mime;
	mime_entry_t *last = mime_entry;
	if (last)
	{
		while (last->next) last = last->next;
		last->next = entry;
	}
	else
	{
		mime_entry = entry;
	}
}

static const mime_entry_t *_utils_getmime(const mime_entry_t *entry, const char *filepath)
{
	while (entry)
	{
		if (_utils_searchexp(filepath, entry->ext, 1, NULL) == ESUCCESS)
		{
			break;
		}
		entry = entry->next;
	}
	return entry;
}

const char *utils_getmime(const char *filepath)
{
	const mime_entry_t *mime = _utils_getmime(mime_default->next, filepath);
	if (mime == NULL)
		mime = _utils_getmime(mime_entry, filepath);
	if (mime)
		return mime->mime;
	return mime_default->mime;
}


char *utils_urldecode(const char *encoded)
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
			// back into previous directory
			encoded+=3;
			if ((offset > decoded) && (offset < decoded + length) && (*(offset - 1) == '/'))
			{
				offset--;
				*offset = '\0';
			}
			offset = strrchr(decoded, '/');
			if (offset == NULL)
			{
				if (decoded[0] == 0)
				{
					free(decoded);
					return NULL;
				}
				else
					offset = decoded;
			}
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

int utils_searchexp(const char *haystack, const char *needleslist, const char **rest)
{
	return _utils_searchexp(haystack, needleslist, 0, rest);
}

static int _utils_searchexp(const char *haystack, const char *needleslist, int ignore_case, const char **rest)
{
	int ret = EREJECT;
	if (haystack != NULL)
	{
		int i = -1;
		const char *needle = needleslist;
		while (needle != NULL)
		{
			i = -1;
			ret = ECONTINUE;
			int wildcard = 1;
			if (*needle == '^')
			{
				wildcard = 0;
			}
			const char *needle_entry = needle;
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
				utils_dbg("wildcard %d, needle %s, haystack %s", wildcard, needle, &haystack[i]);
				if (!wildcard)
				{
					needle++;
					char lneedle = *needle;
					if (ignore_case && lneedle > 0x40 && lneedle < 0x5b)
						lneedle += 0x20;
					if (*needle == ',' || *needle == '\0' || *needle == '$')
					{
						utils_dbg("end of needle, hay %c", hay);
						break;
					}
					else if (lneedle != hay && *needle != '*')
					{
						utils_dbg("end of needle %c, hay %c",lneedle, hay);
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
						break;
					}
				}
			}
			while(haystack[i] != '\0');
			if (*needle == '*' && haystack[i] != '\0')
			{
				ret = ESUCCESS;
				needle++;
			}
			else if (*needle == '$' && haystack[i] != '\0')
			{
				ret = EREJECT;
				needle++;
			}
			utils_dbg("searchexp %d %c %c", ret, *needle, haystack[i]);
			if (ret == ESUCCESS)
			{
				if (*needle == ',' || *needle == '\0' || *needle == '$')
				{
					if (wildcard && (rest != NULL))
						*rest = &haystack[i - 1];
					break;
				}
				else if (*needle != '*')
					ret = EREJECT;
			}
			needle = strchr(needle, ',');
			if (needle != NULL)
				needle++;
			wildcard = 0;
		}
	}
	return ret;
}

static int utils_searchstring(const char **result, const char *haystack, const char *needle, int *length)
{
	if (*length != 0)
		return 0;

	int needlelen = strlen(needle);

	if ((*result == NULL || *result[0] == '\0') &&
		!strncmp(haystack, needle, needlelen) && haystack[needlelen] == '=')
	{
		const char *end = NULL;
		*result = strchr(haystack, '=');
		if (*result == NULL)
			return 0;
		*result += 1;
		if (**result == '"')
		{
			*result = *result + 1;
			end = strchr(*result, '"');
			if (end != NULL)
				*length = end - *result;
		}
		end = *result;
		while(*end != 0 && *end != ' ' && *end != ',')
		{
			end++;
		}
		if (*length == 0)
			*length = end - *result;
		return needlelen + sizeof("=") + end - *result;
	}
	return 0;
}

static int utils_runentry(utils_parsestring_t *entry, const char *value, size_t valuelen)
{
	if (entry->result == EINCOMPLETE)
	{
		entry->result = entry->cb(entry->cbdata, value, valuelen);
	}
	return entry->result;
}

int utils_parsestring(const char *string, int listlength, utils_parsestring_t list[])
{
	int ret = ESUCCESS;
	int listit;

	for (listit = 0; listit < listlength; listit++)
	{
		list[listit].result = EINCOMPLETE;
	}

	int length, i;
	length = strlen(string);
	for (i = 0; i < length; i++)
	{
		//dbg("search %s", string + i);
		for (listit = 0; listit < listlength; listit++)
		{
			const char *value = NULL;
			int valuelen = 0;
			int ret = utils_searchstring(&value, string + i, list[listit].field, &valuelen);
			i += ret;
			if (ret > 0)
			{
				ret = utils_runentry(&list[listit], value, valuelen);
				break;
			}
		}
		if (ret == EREJECT)
			break;
	}
	for (listit = 0; listit < listlength; listit++)
	{
		if ((ret = utils_runentry(&list[listit], NULL, 0)) == EREJECT)
		{
			break;
		}
	}
	return ret;
}

#ifndef COOKIE
static const char str_Cookie[] = "Cookie";
static const char str_SetCookie[] = "Set-Cookie";

const char *cookie_get(http_message_t *request, const char *key)
{
	const char *value = NULL;
	const char *cookie = NULL;
	cookie = httpmessage_REQUEST(request, str_Cookie);
	if (cookie != NULL)
	{
		value = strstr(cookie, key);
	}
	return value;
}

int cookie_set(http_message_t *response, const char *key, const char *value, ...)
{
	httpmessage_addheader(response, str_SetCookie, key);
	return httpmessage_appendheader(response, str_SetCookie, "=", value, NULL);
}
#endif
