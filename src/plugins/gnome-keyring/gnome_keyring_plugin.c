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
#include <glib/gi18n.h>

#include "gkr-if.h"

/* claws-mail */
#include "common/utils.h"
#include "version.h"
#include "plugin.h"
#include "hooks.h"
#include "account.h"

#define PLUGIN_NAME (_("gnome-keyring"))

static guint get_hookid;

static gboolean password_get_hook(gpointer source, gpointer hook_data) {
	PasswordRequest *req = source;
	gchar *pass;

	/* gkr wants (user, domain, server, object, protocol, authtype, port) */
	debug_print("user: %s, domain: %s, proto: %s, port: %d\n", req->user,
			req->server, req->protocol, req->port);

	pass = gkr_getpass(req->user, req->server, req->protocol, req->port);
	if (pass) {
		req->password = pass;
		return TRUE;
	}

	return FALSE;
}

gint plugin_init(gchar **error)
{
	if (!check_plugin_version(MAKE_NUMERIC_VERSION(3,8,0,0),
				VERSION_NUMERIC, PLUGIN_NAME, error))
		return -1;

	get_hookid = hooks_register_hook(PASSWORD_GET_HOOKLIST, &password_get_hook, NULL);
	return 0;
}

gboolean plugin_done(void)
{
	hooks_unregister_hook(PASSWORD_GET_HOOKLIST, get_hookid);
	return TRUE;
}

const gchar *plugin_name(void)
{
	return PLUGIN_NAME;
}

const gchar *plugin_desc(void)
{
	return _("This plugin adds support for storing passwords in gnome-keyring.");
}

const gchar *plugin_type(void)
{
	return "GTK2";
}

const gchar *plugin_licence(void)
{
	return "GPL3+";
}

const gchar *plugin_version(void)
{
	return VERSION;
}

struct PluginFeature *plugin_provides(void)
{
	static struct PluginFeature features[] = {
		{PLUGIN_OTHER, N_("Password storage")},
		{PLUGIN_NOTHING, NULL}
	};

	return features;
}
