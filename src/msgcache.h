/*
 * Sylpheed -- a GTK+ based, lightweight, and fast e-mail client
 * Copyright (C) 1999-2001 Hiroyuki Yamamoto
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

#ifndef __MSGCACHE_H__
#define __MSGCACHE_H__

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include <glib.h>

typedef GHashTable MsgCache;

#include "procmsg.h"
#include "folder.h"

MsgCache   *msgcache_new			();
void	    msgcache_destroy			(MsgCache *cache);
MsgCache   *msgcache_read			(const gchar *cache_file, const gchar *mark_file, FolderItem *item);
gint	    msgcache_write			(const gchar *cache_file, const gchar *mark_file, MsgCache *cache);
void 	    msgcache_add_msg			(MsgCache *cache, MsgInfo *msginfo);
void 	    msgcache_remove_msg			(MsgCache *cache, guint num);
void 	    msgcache_update_msg			(MsgCache *cache, MsgInfo *msginfo);
MsgInfo	   *msgcache_get_msg			(MsgCache *cache, guint num);
GSList	   *msgcache_get_msg_list		(MsgCache *cache);

#endif
