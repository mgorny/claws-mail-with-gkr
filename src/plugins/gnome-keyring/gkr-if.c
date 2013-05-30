/*
 * gnome-keyring plugin for Claws Mail
 *
 * Copyright (C) 2013 Michał Górny <mgorny@gentoo.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
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
#	include "config.h"
#	include "claws-features.h"
#endif

#include <glib.h>
#include <gnome-keyring.h>

/* claws-mail */
#include "alertpanel.h"
#include "common/utils.h"
#include "gtk/inputdialog.h"

gchar *gkr_getpass(const gchar *user, const gchar *server, const gchar *proto,
		guint16 port)
{
	GnomeKeyringResult ret;
	GList *items;
	gchar *pass = NULL;

	if (!gnome_keyring_is_available())
		return NULL;

	ret = gnome_keyring_find_network_password_sync(user, NULL, server, NULL,
			NULL, NULL, 0, &items);
	if (ret == GNOME_KEYRING_RESULT_OK) {
		GList *i;
		int best_pass = 0;

		for (i = items; i; i = g_list_next(i)) {
			GnomeKeyringNetworkPasswordData *item = i->data;

			if (item->password && *(item->password)) {
				enum {
					PASS_ANY = 1,
					PASS_PROTO = 2,
					PASS_PORT = 4
				} pass_qual = PASS_ANY;

				if (item->protocol && !strcmp(proto, item->protocol))
					pass_qual |= PASS_PROTO;
				if (item->port && port == item->port)
					pass_qual |= PASS_PORT;

				if (pass_qual > best_pass) {
					if (best_pass)
						g_free(pass);
					pass = g_strdup(item->password);
					best_pass = pass_qual;
				}
			}
		}
		gnome_keyring_network_password_list_free(items);
	} else {
		debug_print("gnome_keyring_find_network_password_sync() failed: %s\n",
				gnome_keyring_result_to_message(ret));

		if (ret != GNOME_KEYRING_RESULT_NO_MATCH
				&& ret != GNOME_KEYRING_RESULT_CANCELLED)
			alertpanel_error("Unable to query gnome-keyring: %s\n",
					gnome_keyring_result_to_message(ret));
	}

	return pass;
}
