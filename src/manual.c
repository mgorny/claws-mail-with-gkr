/*
 * Sylpheed -- a GTK+ based, lightweight, and fast e-mail client
 * Copyright (C) 1999,2000 Hiroyuki Yamamoto
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "defs.h"

#include <glib.h>
#include <string.h>

#include "prefs_common.h"
#include "manual.h"
#include "utils.h"

void manual_open(ManualLang lang)
{
	gchar *lang_str;
	gchar *file_uri;

	switch (lang) {
	case MANUAL_LANG_EN:
		lang_str = "en";
		break;
	case MANUAL_LANG_JA:
		lang_str = "ja";
		break;
	default:
		return;
	}

	file_uri = g_strconcat("file://", MANUALDIR,
			       G_DIR_SEPARATOR_S, lang_str, G_DIR_SEPARATOR_S,
			       MANUAL_HTML_INDEX, NULL);
	debug_print("Opening manual: %s\n", file_uri);
	open_uri(file_uri, prefs_common.uri_cmd);
	g_free(file_uri);
}

void faq_open(FaqLang lang)
{
	gchar *lang_str;
	gchar *file_uri;

	switch (lang) {
	case FAQ_LANG_EN:
		lang_str = "en";
		break;
	case FAQ_LANG_ES:
		lang_str = "es";
		break;
	case FAQ_LANG_FR:
		lang_str = "fr";
		break;
	default:
		return;
	}

	file_uri = g_strconcat("file://", FAQDIR,
			       G_DIR_SEPARATOR_S, lang_str, G_DIR_SEPARATOR_S,
			       FAQ_HTML_INDEX, NULL);
	debug_print("Opening FAQ: %s\n", file_uri);
	open_uri(file_uri, prefs_common.uri_cmd);
	g_free(file_uri);
}
