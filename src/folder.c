/*
 * Sylpheed -- a GTK+ based, lightweight, and fast e-mail client
 * Copyright (C) 1999-2006 Hiroyuki Yamamoto and the Sylpheed-Claws team
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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "defs.h"

#include <glib.h>
#include <glib/gi18n.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdlib.h>
#ifdef WIN32
#include <w32lib.h>
#endif

#include "folder.h"
#include "session.h"
#include "inc.h"
#include "imap.h"
#include "news.h"
#include "mh.h"
#include "utils.h"
#include "xml.h"
#include "codeconv.h"
#include "prefs_gtk.h"
#include "account.h"
#include "filtering.h"
#include "procheader.h"
#include "hooks.h"
#include "log.h"
#include "folder_item_prefs.h"
#include "remotefolder.h"
#include "partial_download.h"
#include "statusbar.h"
#include "gtkutils.h"
#include "timing.h"
#include "compose.h"

/* Dependecies to be removed ?! */
#include "prefs_common.h"
#include "prefs_account.h"

/* Define possible missing constants for Windows. */
#ifdef G_OS_WIN32
# ifndef S_IRGRP
# define S_IRGRP 0
# define S_IWGRP 0
# endif
# ifndef S_IROTH
# define S_IROTH 0
# define S_IWOTH 0
# endif
#endif

static GList *folder_list = NULL;
static GSList *class_list = NULL;
static GSList *folder_unloaded_list = NULL;

void folder_init		(Folder		*folder,
				 const gchar	*name);

static gchar *folder_get_list_path	(void);
static GNode *folder_get_xml_node	(Folder 	*folder);
static Folder *folder_get_from_xml	(GNode 		*node);
static void folder_update_op_count_rec	(GNode		*node);


static void folder_get_persist_prefs_recursive
					(GNode *node, GHashTable *pptable);
static gboolean persist_prefs_free	(gpointer key, gpointer val, gpointer data);
void folder_item_read_cache		(FolderItem *item);
gint folder_item_scan_full		(FolderItem *item, gboolean filtering);
static void folder_item_update_with_msg (FolderItem *item, FolderItemUpdateFlags update_flags,
                                         MsgInfo *msg);

void folder_system_init(void)
{
	folder_register_class(mh_get_class());
	folder_register_class(imap_get_class());
	folder_register_class(news_get_class());
}

GSList *folder_get_class_list(void)
{
	return class_list;
}

void folder_register_class(FolderClass *klass)
{
	GSList *xmllist, *cur;

	debug_print("registering folder class %s\n", klass->idstr);

	class_list = g_slist_append(class_list, klass);

	xmllist = g_slist_copy(folder_unloaded_list);
	for (cur = xmllist; cur != NULL; cur = g_slist_next(cur)) {
		GNode *node = (GNode *) cur->data;
		XMLNode *xmlnode = (XMLNode *) node->data;
		GList *cur = xmlnode->tag->attr;

		for (; cur != NULL; cur = g_list_next(cur)) {
			XMLAttr *attr = (XMLAttr *) cur->data;

			if (!attr || !attr->name || !attr->value) continue;
			if (!strcmp(attr->name, "type") && !strcmp(attr->value, klass->idstr)) {
				Folder *folder;

				folder = folder_get_from_xml(node);
				folder_add(folder);
				folder_unloaded_list = g_slist_remove(folder_unloaded_list, node);

				cur = NULL;
				continue;
			}
		}
	}
	g_slist_free(xmllist);
}

void folder_unregister_class(FolderClass *klass)
{
	GList *folderlist, *cur;

	debug_print("unregistering folder class %s\n", klass->idstr);

	class_list = g_slist_remove(class_list, klass);

	folderlist = g_list_copy(folder_get_list());
	for (cur = folderlist; cur != NULL; cur = g_list_next(cur)) {
		Folder *folder = (Folder *) cur->data;

		if (folder->klass == klass) {
			GNode *xmlnode = folder_get_xml_node(folder);
			folder_unloaded_list = g_slist_append(folder_unloaded_list, xmlnode);
			folder_destroy(folder);
		}
	}
	g_list_free(folderlist);
}

Folder *folder_new(FolderClass *klass, const gchar *name, const gchar *path)
{
	Folder *folder = NULL;
	FolderItem *item;

	g_return_val_if_fail(klass != NULL, NULL);

	name = name ? name : path;
	folder = klass->new_folder(name, path);

	/* Create root folder item */
	item = folder_item_new(folder, name, NULL);
	if (item == NULL) {
		return NULL;
	}
	item->folder = folder;
	folder->node = item->node = g_node_new(item);
	folder->data = NULL;

	return folder;
}

void folder_init(Folder *folder, const gchar *name)
{
	g_return_if_fail(folder != NULL);

	folder_set_name(folder, name);

	/* Init folder data */
	folder->account = NULL;
	folder->sort = 0;
	folder->inbox = NULL;
	folder->outbox = NULL;
	folder->draft = NULL;
	folder->queue = NULL;
	folder->trash = NULL;
}

static void reset_parent_type(FolderItem *item, gpointer data) {
	item->parent_stype = -1;
}

void folder_item_change_type(FolderItem *item, SpecialFolderItemType newtype)
{
	Folder *folder = NULL;
	FolderUpdateData hookdata;

	folder = item->folder;
	/* unset previous root of newtype */
	switch(newtype) {
	case F_INBOX:
		folder_item_change_type(folder->inbox, F_NORMAL);
		folder->inbox = item;
		break;
	case F_OUTBOX:
		folder_item_change_type(folder->outbox, F_NORMAL);
		folder->outbox = item;
		break;
	case F_QUEUE:
		folder_item_change_type(folder->queue, F_NORMAL);
		folder->queue = item;
		break;
	case F_DRAFT:
		folder_item_change_type(folder->draft, F_NORMAL);
		folder->draft = item;
		break;
	case F_TRASH:
		folder_item_change_type(folder->trash, F_NORMAL);
		folder->trash = item;
		break;
	case F_NORMAL:
	default:
		break;
	}
	/* set new type for current folder and sons */
	item->stype = newtype;
	folder_func_to_all_folders(reset_parent_type, NULL);
	
	hookdata.folder = folder;
	hookdata.update_flags = FOLDER_TREE_CHANGED;
	hookdata.item = NULL;
	hooks_invoke(FOLDER_UPDATE_HOOKLIST, &hookdata);
}

void folder_destroy(Folder *folder)
{
	g_return_if_fail(folder != NULL);
	g_return_if_fail(folder->klass->destroy_folder != NULL);

	folder_remove(folder);

	folder_tree_destroy(folder);

	folder->klass->destroy_folder(folder);

	g_free(folder->name);
	g_free(folder);
}

void folder_set_xml(Folder *folder, XMLTag *tag)
{
	GList *cur;
	FolderItem *rootitem = NULL;

	if ((folder->node != NULL) && (folder->node->data != NULL))
		rootitem = (FolderItem *) folder->node->data;

	for (cur = tag->attr; cur != NULL; cur = g_list_next(cur)) {
		XMLAttr *attr = (XMLAttr *) cur->data;

		if (!attr || !attr->name || !attr->value) continue;
		if (!strcmp(attr->name, "name")) {
			g_free(folder->name);
			folder->name = g_strdup(attr->value);
			if (rootitem != NULL) {
				g_free(rootitem->name);
				rootitem->name = g_strdup(attr->value);
			}
		} else if (!strcmp(attr->name, "account_id")) {
			PrefsAccount *account;

			account = account_find_from_id(atoi(attr->value));
			if (!account)
				g_warning("account_id: %s not found\n", attr->value);
			else {
				folder->account = account;
				account->folder = folder;
			}
		} else if (!strcmp(attr->name, "collapsed")) {
			if (rootitem != NULL)
				rootitem->collapsed = *attr->value == '1' ? TRUE : FALSE;
		} else if (!strcmp(attr->name, "sort")) {
			folder->sort = atoi(attr->value);
		}
	}
}

XMLTag *folder_get_xml(Folder *folder)
{
	XMLTag *tag;

	tag = xml_tag_new("folder");

	if (folder->name)
		xml_tag_add_attr(tag, xml_attr_new("name", folder->name));
	if (folder->account)
		xml_tag_add_attr(tag, xml_attr_new_int("account_id", folder->account->account_id));
	if (folder->node && folder->node->data) {
		FolderItem *rootitem = (FolderItem *) folder->node->data;

		xml_tag_add_attr(tag, xml_attr_new("collapsed", rootitem->collapsed ? "1" : "0"));
	}
	xml_tag_add_attr(tag, xml_attr_new_int("sort", folder->sort));

	return tag;
}

FolderItem *folder_item_new(Folder *folder, const gchar *name, const gchar *path)
{
	FolderItem *item = NULL;
	
	g_return_val_if_fail(folder != NULL, NULL);
	
	if (folder->klass->item_new) {
		item = folder->klass->item_new(folder);
	} else {
		item = g_new0(FolderItem, 1);
	}

	g_return_val_if_fail(item != NULL, NULL);

	item->stype = F_NORMAL;
	item->name = g_strdup(name);
	item->path = g_strdup(path);
	item->mtime = 0;
	item->new_msgs = 0;
	item->unread_msgs = 0;
	item->unreadmarked_msgs = 0;
	item->marked_msgs = 0;
	item->total_msgs = 0;
	item->last_num = -1;
	item->cache = NULL;
	item->no_sub = FALSE;
	item->no_select = FALSE;
	item->collapsed = FALSE;
	item->thread_collapsed = FALSE;
	item->threaded  = TRUE;
	item->ret_rcpt  = FALSE;
	item->opened    = FALSE;
	item->node = g_node_new(item);
	item->folder = NULL;
	item->account = NULL;
	item->apply_sub = FALSE;
	item->mark_queue = NULL;
	item->data = NULL;
	item->parent_stype = -1;

	item->sort_key = SORT_BY_DATE;
	item->sort_type = SORT_ASCENDING;

	item->prefs = folder_item_prefs_new();

	return item;
}

void folder_item_append(FolderItem *parent, FolderItem *item)
{
	g_return_if_fail(parent != NULL);
	g_return_if_fail(parent->folder != NULL);
	g_return_if_fail(parent->node != NULL);
	g_return_if_fail(item != NULL);

	item->folder = parent->folder;
	g_node_append(parent->node, item->node);
}

void folder_item_remove(FolderItem *item)
{
	GNode *node, *start_node;
	FolderUpdateData hookdata;

	g_return_if_fail(item != NULL);
	g_return_if_fail(item->folder != NULL);
	g_return_if_fail(item->folder->node != NULL);

	start_node = node = item->node;
	
	node = item->folder->node;
	
	node = g_node_find(node, G_PRE_ORDER, G_TRAVERSE_ALL, item);
	node = node->children;

	/* remove my children */
	while (node != NULL) {
		if (node && node->data) {
			FolderItem *sub_item = (FolderItem*) node->data;
			node = node->next;
			folder_item_remove(sub_item);
		}
	}
	
	/* remove myself */
	if (item->cache != NULL) {
		msgcache_destroy(item->cache);
		item->cache = NULL;
	}

	hookdata.folder = item->folder;
	hookdata.update_flags = FOLDER_TREE_CHANGED | FOLDER_REMOVE_FOLDERITEM;
	hookdata.item = item;
	hooks_invoke(FOLDER_UPDATE_HOOKLIST, &hookdata);

	node = start_node;

	if (item->folder->node == node)
		item->folder->node = NULL;

	folder_item_destroy(item);

	g_node_destroy(node);
}

void folder_item_remove_children(FolderItem *item)
{
	GNode *node, *next;

	g_return_if_fail(item != NULL);
	g_return_if_fail(item->folder != NULL);
	g_return_if_fail(item->node != NULL);

	node = item->node->children;
	while (node != NULL) {
		next = node->next;
		folder_item_remove(FOLDER_ITEM(node->data));
		node = next;
	}
}

void folder_item_destroy(FolderItem *item)
{
	Folder *folder;

	g_return_if_fail(item != NULL);

	folder = item->folder;
	if (folder) {
		if (folder->inbox == item)
			folder->inbox = NULL;
		else if (folder->outbox == item)
			folder->outbox = NULL;
		else if (folder->draft == item)
			folder->draft = NULL;
		else if (folder->queue == item)
			folder->queue = NULL;
		else if (folder->trash == item)
			folder->trash = NULL;
	}

	if (item->cache)
		folder_item_free_cache(item, TRUE);
	if (item->prefs)
		folder_item_prefs_free(item->prefs);
	g_free(item->name);
	g_free(item->path);

	if (item->folder != NULL) {
		if(item->folder->klass->item_destroy) {
			item->folder->klass->item_destroy(item->folder, item);
		} else {
			g_free(item);
		}
	}
}

FolderItem *folder_item_parent(FolderItem *item)
{
	g_return_val_if_fail(item != NULL, NULL);
	g_return_val_if_fail(item->node != NULL, NULL);

	if (item->node->parent == NULL)
		return NULL;
	return (FolderItem *) item->node->parent->data;
}

void folder_item_set_xml(Folder *folder, FolderItem *item, XMLTag *tag)
{
	GList *cur;

	for (cur = tag->attr; cur != NULL; cur = g_list_next(cur)) {
		XMLAttr *attr = (XMLAttr *) cur->data;

		if (!attr || !attr->name || !attr->value) continue;
		if (!strcmp(attr->name, "type")) {
			if (!g_ascii_strcasecmp(attr->value, "normal"))
				item->stype = F_NORMAL;
			else if (!g_ascii_strcasecmp(attr->value, "inbox"))
				item->stype = F_INBOX;
			else if (!g_ascii_strcasecmp(attr->value, "outbox"))
				item->stype = F_OUTBOX;
			else if (!g_ascii_strcasecmp(attr->value, "draft"))
				item->stype = F_DRAFT;
			else if (!g_ascii_strcasecmp(attr->value, "queue"))
				item->stype = F_QUEUE;
			else if (!g_ascii_strcasecmp(attr->value, "trash"))
				item->stype = F_TRASH;
		} else if (!strcmp(attr->name, "name")) {
			g_free(item->name);
			item->name = g_strdup(attr->value);
		} else if (!strcmp(attr->name, "path")) {
			g_free(item->path);
			item->path = g_strdup(attr->value);
		} else if (!strcmp(attr->name, "mtime"))
			item->mtime = strtoul(attr->value, NULL, 10);
		else if (!strcmp(attr->name, "new"))
			item->new_msgs = atoi(attr->value);
		else if (!strcmp(attr->name, "unread"))
			item->unread_msgs = atoi(attr->value);
		else if (!strcmp(attr->name, "unreadmarked"))
			item->unreadmarked_msgs = atoi(attr->value);
		else if (!strcmp(attr->name, "marked"))
			item->marked_msgs = atoi(attr->value);
		else if (!strcmp(attr->name, "total"))
			item->total_msgs = atoi(attr->value);
		else if (!strcmp(attr->name, "no_sub"))
			item->no_sub = *attr->value == '1' ? TRUE : FALSE;
		else if (!strcmp(attr->name, "no_select"))
			item->no_select = *attr->value == '1' ? TRUE : FALSE;
		else if (!strcmp(attr->name, "collapsed"))
			item->collapsed = *attr->value == '1' ? TRUE : FALSE;
		else if (!strcmp(attr->name, "thread_collapsed"))
			item->thread_collapsed =  *attr->value == '1' ? TRUE : FALSE;
		else if (!strcmp(attr->name, "threaded"))
			item->threaded =  *attr->value == '1' ? TRUE : FALSE;
		else if (!strcmp(attr->name, "hidereadmsgs"))
			item->hide_read_msgs =  *attr->value == '1' ? TRUE : FALSE;
		else if (!strcmp(attr->name, "reqretrcpt"))
			item->ret_rcpt =  *attr->value == '1' ? TRUE : FALSE;
		else if (!strcmp(attr->name, "sort_key")) {
			if (!strcmp(attr->value, "none"))
				item->sort_key = SORT_BY_NONE;
			else if (!strcmp(attr->value, "number"))
				item->sort_key = SORT_BY_NUMBER;
			else if (!strcmp(attr->value, "size"))
				item->sort_key = SORT_BY_SIZE;
			else if (!strcmp(attr->value, "date"))
				item->sort_key = SORT_BY_DATE;
			else if (!strcmp(attr->value, "from"))
				item->sort_key = SORT_BY_FROM;
			else if (!strcmp(attr->value, "subject"))
				item->sort_key = SORT_BY_SUBJECT;
			else if (!strcmp(attr->value, "score"))
				item->sort_key = SORT_BY_SCORE;
			else if (!strcmp(attr->value, "label"))
				item->sort_key = SORT_BY_LABEL;
			else if (!strcmp(attr->value, "mark"))
				item->sort_key = SORT_BY_MARK;
			else if (!strcmp(attr->value, "unread"))
				item->sort_key = SORT_BY_STATUS;
			else if (!strcmp(attr->value, "mime"))
				item->sort_key = SORT_BY_MIME;
			else if (!strcmp(attr->value, "to"))
				item->sort_key = SORT_BY_TO;
			else if (!strcmp(attr->value, "locked"))
				item->sort_key = SORT_BY_LOCKED;
		} else if (!strcmp(attr->name, "sort_type")) {
			if (!strcmp(attr->value, "ascending"))
				item->sort_type = SORT_ASCENDING;
			else
				item->sort_type = SORT_DESCENDING;
		} else if (!strcmp(attr->name, "account_id")) {
			PrefsAccount *account;

			account = account_find_from_id(atoi(attr->value));
			if (!account)
				g_warning("account_id: %s not found\n", attr->value);
			else
				item->account = account;
		} else if (!strcmp(attr->name, "apply_sub"))
			item->apply_sub = *attr->value == '1' ? TRUE : FALSE;
	}
}

XMLTag *folder_item_get_xml(Folder *folder, FolderItem *item)
{
	static gchar *folder_item_stype_str[] = {"normal", "inbox", "outbox",
						 "draft", "queue", "trash"};
	static gchar *sort_key_str[] = {"none", "number", "size", "date",
					"from", "subject", "score", "label",
					"mark", "unread", "mime", "to", 
					"locked"};
	XMLTag *tag;
	gchar *value;

	tag = xml_tag_new("folderitem");

	xml_tag_add_attr(tag, xml_attr_new("type", folder_item_stype_str[item->stype]));
	if (item->name)
		xml_tag_add_attr(tag, xml_attr_new("name", item->name));
	if (item->path)
		xml_tag_add_attr(tag, xml_attr_new("path", item->path));
	if (item->no_sub)
		xml_tag_add_attr(tag, xml_attr_new("no_sub", "1"));
	if (item->no_select)
		xml_tag_add_attr(tag, xml_attr_new("no_select", "1"));
	xml_tag_add_attr(tag, xml_attr_new("collapsed", item->collapsed && item->node->children ? "1" : "0"));
	xml_tag_add_attr(tag, xml_attr_new("thread_collapsed", item->thread_collapsed ? "1" : "0"));
	xml_tag_add_attr(tag, xml_attr_new("threaded", item->threaded ? "1" : "0"));
	xml_tag_add_attr(tag, xml_attr_new("hidereadmsgs", item->hide_read_msgs ? "1" : "0"));
	if (item->ret_rcpt)
		xml_tag_add_attr(tag, xml_attr_new("reqretrcpt", "1"));

	if (item->sort_key != SORT_BY_NONE) {
		xml_tag_add_attr(tag, xml_attr_new("sort_key", sort_key_str[item->sort_key]));
		xml_tag_add_attr(tag, xml_attr_new("sort_type", item->sort_type == SORT_ASCENDING ? "ascending" : "descending"));
	}

	value = g_strdup_printf("%ld", (unsigned long int) item->mtime);
	xml_tag_add_attr(tag, xml_attr_new("mtime", value));
	g_free(value);
	xml_tag_add_attr(tag, xml_attr_new_int("new", item->new_msgs));
	xml_tag_add_attr(tag, xml_attr_new_int("unread", item->unread_msgs));
	xml_tag_add_attr(tag, xml_attr_new_int("unreadmarked", item->unreadmarked_msgs));
	xml_tag_add_attr(tag, xml_attr_new_int("marked", item->marked_msgs));
	xml_tag_add_attr(tag, xml_attr_new_int("total", item->total_msgs));

	if (item->account)
		xml_tag_add_attr(tag, xml_attr_new_int("account_id", item->account->account_id));
	if (item->apply_sub)
		xml_tag_add_attr(tag, xml_attr_new("apply_sub", "1"));

	return tag;
}

void folder_set_ui_func(Folder *folder, FolderUIFunc func, gpointer data)
{
	g_return_if_fail(folder != NULL);

	folder->ui_func = func;
	folder->ui_func_data = data;
}

void folder_set_name(Folder *folder, const gchar *name)
{
	g_return_if_fail(folder != NULL);

	g_free(folder->name);
	folder->name = name ? g_strdup(name) : NULL;
	if (folder->node && folder->node->data) {
		FolderItem *item = (FolderItem *)folder->node->data;

		g_free(item->name);
		item->name = name ? g_strdup(name) : NULL;
	}
}

void folder_set_sort(Folder *folder, guint sort)
{
	g_return_if_fail(folder != NULL);

	if (folder->sort != sort) {
		folder_remove(folder);
		folder->sort = sort;
		folder_add(folder);
	}
}

gboolean folder_tree_destroy_func(GNode *node, gpointer data) {
	FolderItem *item = (FolderItem *) node->data;

	folder_item_destroy(item);
	return FALSE;
}

void folder_tree_destroy(Folder *folder)
{
	GNode *node;

	g_return_if_fail(folder != NULL);

	node = folder->node;
	
	prefs_filtering_clear_folder(folder);

	if (node != NULL) {
		g_node_traverse(node, G_POST_ORDER, G_TRAVERSE_ALL, -1,
				folder_tree_destroy_func, NULL);
		g_node_destroy(node);
		folder->node = NULL;
	}
}

void folder_add(Folder *folder)
{
	Folder *cur_folder;
	GList *cur;
	gint i;
	FolderUpdateData hookdata;

	g_return_if_fail(folder != NULL);

	for (i = 0, cur = folder_list; cur != NULL; cur = cur->next, i++) {
		cur_folder = FOLDER(cur->data);
		if (cur_folder->sort < folder->sort)
			break;
	}

	folder_list = g_list_insert(folder_list, folder, i);

	hookdata.folder = folder;
	hookdata.update_flags = FOLDER_ADD_FOLDER;
	hookdata.item = NULL;
	hooks_invoke(FOLDER_UPDATE_HOOKLIST, &hookdata);
}

void folder_remove(Folder *folder)
{
	FolderUpdateData hookdata;

	g_return_if_fail(folder != NULL);

	folder_list = g_list_remove(folder_list, folder);

	hookdata.folder = folder;
	hookdata.update_flags = FOLDER_REMOVE_FOLDER;
	hookdata.item = NULL;
	hooks_invoke(FOLDER_UPDATE_HOOKLIST, &hookdata);
}

GList *folder_get_list(void)
{
	return folder_list;
}

gint folder_read_list(void)
{
	GNode *node, *cur;
	XMLNode *xmlnode;
	gchar *path;

	path = folder_get_list_path();
	if (!is_file_exist(path)) return -1;
	node = xml_parse_file(path);
	if (!node) return -1;

	xmlnode = node->data;
	if (strcmp2(xmlnode->tag->tag, "folderlist") != 0) {
		g_warning("wrong folder list\n");
		xml_free_tree(node);
		return -1;
	}

	cur = node->children;
	while (cur != NULL) {
		Folder *folder;

		folder = folder_get_from_xml(cur);
		if (folder != NULL)
			folder_add(folder);
		else
			folder_unloaded_list = g_slist_append(folder_unloaded_list,
				(gpointer) xml_copy_tree(cur));
		cur = cur->next;
	}

	xml_free_tree(node);
	if (folder_list || folder_unloaded_list)
		return 0;
	else
		return -1;
}

void folder_write_list(void)
{
	GList *list;
	GSList *slist;
	Folder *folder;
	gchar *path;
	PrefFile *pfile;
	GNode *rootnode;
	XMLNode *xmlnode;
	XMLTag *tag;

	path = folder_get_list_path();
	if ((pfile = prefs_write_open(path)) == NULL) return;

	xml_file_put_xml_decl(pfile->fp);
	tag = xml_tag_new("folderlist");

	xmlnode = xml_node_new(tag, NULL);

	rootnode = g_node_new(xmlnode);

	for (list = folder_list; list != NULL; list = list->next) {
		GNode *node;

		folder = list->data;
		node = folder_get_xml_node(folder);
		if (node != NULL)
			g_node_append(rootnode, node);
	}

	for (slist = folder_unloaded_list; slist != NULL; slist = g_slist_next(slist)) {
		GNode *node = (GNode *) slist->data;

		g_node_append(rootnode, (gpointer) xml_copy_tree(node));
	}

	xml_write_tree(rootnode, pfile->fp);

	if (prefs_file_close(pfile) < 0)
		g_warning("failed to write folder list.\n");

	xml_free_tree(rootnode);
}

gboolean folder_scan_tree_func(GNode *node, gpointer data)
{
	GHashTable *pptable = (GHashTable *)data;
	FolderItem *item = (FolderItem *)node->data;
	
	folder_item_restore_persist_prefs(item, pptable);
	folder_item_scan_full(item, FALSE);

	return FALSE;
}

void folder_scan_tree(Folder *folder, gboolean rebuild)
{
	GHashTable *pptable;
	FolderUpdateData hookdata;
	Folder *old_folder = folder;

	if (!folder->klass->scan_tree)
		return;
	
	pptable = folder_persist_prefs_new(folder);

	if (rebuild)
		folder_remove(folder);

	if (folder->klass->scan_tree(folder) < 0) {
		if (rebuild)
			folder_add(old_folder);
		return;
	} else if (rebuild)
		folder_add(folder);

	hookdata.folder = folder;
	hookdata.update_flags = FOLDER_TREE_CHANGED;
	hookdata.item = NULL;
	hooks_invoke(FOLDER_UPDATE_HOOKLIST, &hookdata);

	g_node_traverse(folder->node, G_POST_ORDER, G_TRAVERSE_ALL, -1, folder_scan_tree_func, pptable);
	folder_persist_prefs_free(pptable);

	prefs_matcher_read_config();

	folder_write_list();
}

FolderItem *folder_create_folder(FolderItem *parent, const gchar *name)
{
	FolderItem *new_item;

	new_item = parent->folder->klass->create_folder(parent->folder, parent, name);
	if (new_item) {
		FolderUpdateData hookdata;

		new_item->cache = msgcache_new();

		hookdata.folder = new_item->folder;
		hookdata.update_flags = FOLDER_TREE_CHANGED | FOLDER_ADD_FOLDERITEM;
		hookdata.item = new_item;
		hooks_invoke(FOLDER_UPDATE_HOOKLIST, &hookdata);
	}

	return new_item;
}

gint folder_item_rename(FolderItem *item, gchar *newname)
{
	gint retval;

	g_return_val_if_fail(item != NULL, -1);
	g_return_val_if_fail(newname != NULL, -1);

	retval = item->folder->klass->rename_folder(item->folder, item, newname);

	if (retval >= 0) {
		FolderItemUpdateData hookdata;
		FolderUpdateData hookdata2;

		hookdata.item = item;
		hookdata.update_flags = F_ITEM_UPDATE_NAME;
		hookdata.msg = NULL;
		hooks_invoke(FOLDER_ITEM_UPDATE_HOOKLIST, &hookdata);

		hookdata2.folder = item->folder;
		hookdata2.item = item;
		hookdata2.update_flags = FOLDER_RENAME_FOLDERITEM;
		hooks_invoke(FOLDER_UPDATE_HOOKLIST, &hookdata2);
	}

	return retval;
}

struct TotalMsgCount
{
	guint new_msgs;
	guint unread_msgs;
	guint unreadmarked_msgs;
	guint marked_msgs;
	guint total_msgs;
};

struct FuncToAllFoldersData
{
	FolderItemFunc	function;
	gpointer	data;
};

static gboolean folder_func_to_all_folders_func(GNode *node, gpointer data)
{
	FolderItem *item;
	struct FuncToAllFoldersData *function_data = (struct FuncToAllFoldersData *) data;

	g_return_val_if_fail(node->data != NULL, FALSE);

	item = FOLDER_ITEM(node->data);
	g_return_val_if_fail(item != NULL, FALSE);

	function_data->function(item, function_data->data);

	return FALSE;
}

void folder_func_to_all_folders(FolderItemFunc function, gpointer data)
{
	GList *list;
	Folder *folder;
	struct FuncToAllFoldersData function_data;
	
	function_data.function = function;
	function_data.data = data;

	for (list = folder_list; list != NULL; list = list->next) {
		folder = FOLDER(list->data);
		if (folder->node)
			g_node_traverse(folder->node, G_PRE_ORDER,
					G_TRAVERSE_ALL, -1,
					folder_func_to_all_folders_func,
					&function_data);
	}
}

static void folder_count_total_msgs_func(FolderItem *item, gpointer data)
{
	struct TotalMsgCount *count = (struct TotalMsgCount *)data;

	count->new_msgs += item->new_msgs;
	count->unread_msgs += item->unread_msgs;
	count->unreadmarked_msgs += item->unreadmarked_msgs;
	count->marked_msgs += item->marked_msgs;
	count->total_msgs += item->total_msgs;
}

struct TotalMsgStatus
{
        guint new;
        guint unread;
	guint total;
	GString *str;
};

static gboolean folder_get_status_full_all_func(GNode *node, gpointer data)
{
	FolderItem *item;
	struct TotalMsgStatus *status = (struct TotalMsgStatus *)data;
	gchar *id;
 
 	g_return_val_if_fail(node->data != NULL, FALSE);
 
 	item = FOLDER_ITEM(node->data);

	if (!item->path) return FALSE;

	status->new += item->new_msgs;
	status->unread += item->unread_msgs;
	status->total += item->total_msgs;

	if (status->str) {
		id = folder_item_get_identifier(item);
		g_string_append_printf(status->str, "%5d %5d %5d %s\n",
				  item->new_msgs, item->unread_msgs,
				  item->total_msgs, id);
		g_free(id);
	}
 
 	return FALSE;
 }
 
static void folder_get_status_full_all(GString *str, guint *new, guint *unread,
				       guint *total)
{
 	GList *list;
 	Folder *folder;
	struct TotalMsgStatus status;
 
	status.new = status.unread = status.total = 0;
	status.str = str;
 
	debug_print("Counting total number of messages...\n");
 
 	for (list = folder_list; list != NULL; list = list->next) {
 		folder = FOLDER(list->data);
 		if (folder->node)
 			g_node_traverse(folder->node, G_PRE_ORDER,
 					G_TRAVERSE_ALL, -1,
					folder_get_status_full_all_func,
					&status);
 	}
 
	*new = status.new;
	*unread = status.unread;
	*total = status.total;
}

gchar *folder_get_status(GPtrArray *folders, gboolean full)
{
	guint new, unread, total;
	GString *str;
	gint i;
	gchar *ret;

	new = unread = total = 0;

	str = g_string_new(NULL);

	if (folders) {
		for (i = 0; i < folders->len; i++) {
			FolderItem *item;

			item = g_ptr_array_index(folders, i);
			new += item->new_msgs;
			unread += item->unread_msgs;
			total += item->total_msgs;

			if (full) {
				gchar *id;

				id = folder_item_get_identifier(item);
				g_string_append_printf(str, "%5d %5d %5d %s\n",
						  item->new_msgs, item->unread_msgs,
						  item->total_msgs, id);
				g_free(id);
			}
		}
	} else {
		folder_get_status_full_all(full ? str : NULL,
					   &new, &unread, &total);
	}

	if (full)
		g_string_append_printf(str, "%5d %5d %5d\n", new, unread, total);
	else
		g_string_append_printf(str, "%d %d %d\n", new, unread, total);

	ret = str->str;
	g_string_free(str, FALSE);
 
	return ret;
}

void folder_count_total_msgs(guint *new_msgs, guint *unread_msgs, 
			     guint *unreadmarked_msgs, guint *marked_msgs,
			     guint *total_msgs)
{
	struct TotalMsgCount count;

	count.new_msgs = count.unread_msgs = count.unreadmarked_msgs = count.total_msgs = 0;

	debug_print("Counting total number of messages...\n");

	folder_func_to_all_folders(folder_count_total_msgs_func, &count);

	*new_msgs = count.new_msgs;
	*unread_msgs = count.unread_msgs;
	*unreadmarked_msgs = count.unreadmarked_msgs;
	*marked_msgs = count.marked_msgs;
	*total_msgs = count.total_msgs;
}

Folder *folder_find_from_path(const gchar *path)
{
	GList *list;
	Folder *folder;

	for (list = folder_list; list != NULL; list = list->next) {
		folder = list->data;
		if ((FOLDER_TYPE(folder) == F_MH || 
		     FOLDER_TYPE(folder) == F_MBOX) &&
		    !path_cmp(LOCAL_FOLDER(folder)->rootpath, path))
			return folder;
	}

	return NULL;
}

Folder *folder_find_from_name(const gchar *name, FolderClass *klass)
{
	GList *list;
	Folder *folder;

	for (list = folder_list; list != NULL; list = list->next) {
		folder = list->data;
		if (folder->klass == klass && 
		    strcmp2(name, folder->name) == 0)
			return folder;
	}

	return NULL;
}

static gboolean folder_item_find_func(GNode *node, gpointer data)
{
	FolderItem *item = node->data;
	gpointer *d = data;
	const gchar *path = d[0];

	if (path_cmp(path, item->path) != 0)
		return FALSE;

	d[1] = item;

	return TRUE;
}

FolderItem *folder_find_item_from_path(const gchar *path)
{
	Folder *folder;
	gpointer d[2];

	folder = folder_get_default_folder();
	g_return_val_if_fail(folder != NULL, NULL);

	d[0] = (gpointer)path;
	d[1] = NULL;
	g_node_traverse(folder->node, G_PRE_ORDER, G_TRAVERSE_ALL, -1,
			folder_item_find_func, d);
	return d[1];
}

FolderItem *folder_find_child_item_by_name(FolderItem *item, const gchar *name)
{
	GNode *node;
	FolderItem *child;

	for (node = item->node->children; node != NULL; node = node->next) {
		gchar *base;
		child = FOLDER_ITEM(node->data);
		base = g_path_get_basename(child->path);
		if (strcmp2(base, name) == 0) {
			g_free(base);
			return child;
		}
		g_free(base);
	}

	return NULL;
}

FolderClass *folder_get_class_from_string(const gchar *str)
{
	GSList *classlist;

	classlist = folder_get_class_list();
	for (; classlist != NULL; classlist = g_slist_next(classlist)) {
		FolderClass *class = (FolderClass *) classlist->data;
		if (g_ascii_strcasecmp(class->idstr, str) == 0)
			return class;
	}

	return NULL;
}

gchar *folder_get_identifier(Folder *folder)
{
	gchar *type_str;

	g_return_val_if_fail(folder != NULL, NULL);

	type_str = folder->klass->idstr;
	return g_strconcat("#", type_str, "/", folder->name, NULL);
}

gchar *folder_item_get_identifier(FolderItem *item)
{
	gchar *id = NULL;
	gchar *folder_id = NULL;

	g_return_val_if_fail(item != NULL, NULL);

	if (item->path == NULL)
		return NULL;

	folder_id = folder_get_identifier(item->folder);
	id = g_strconcat(folder_id, "/", item->path, NULL);
	g_free(folder_id);

	return id;
}

FolderItem *folder_find_item_from_identifier(const gchar *identifier)
{
	Folder *folder;
	gpointer d[2];
	gchar *str;
	gchar *p;
	gchar *name;
	gchar *path;
	FolderClass *class;

	g_return_val_if_fail(identifier != NULL, NULL);

	if (*identifier != '#')
		return folder_find_item_from_path(identifier);

	Xstrdup_a(str, identifier, return NULL);

	p = strchr(str, '/');
	if (!p)
		return folder_find_item_from_path(identifier);
	*p = '\0';
	p++;
	class = folder_get_class_from_string(&str[1]);
	if (class == NULL)
		return folder_find_item_from_path(identifier);

	name = p;
	p = strchr(p, '/');
	if (!p)
		return folder_find_item_from_path(identifier);
	*p = '\0';
	p++;

	folder = folder_find_from_name(name, class);
	if (!folder)
		return folder_find_item_from_path(identifier);

	path = p;

	d[0] = (gpointer)path;
	d[1] = NULL;
	g_node_traverse(folder->node, G_PRE_ORDER, G_TRAVERSE_ALL, -1,
			folder_item_find_func, d);
	return d[1];
}

/**
 * Get a displayable name for a FolderItem
 *
 * \param item FolderItem for that a name should be created
 * \return Displayable name for item, returned string has to
 *         be freed
 */
gchar *folder_item_get_name(FolderItem *item)
{
	gchar *name = NULL;

	g_return_val_if_fail(item != NULL, g_strdup(""));

	switch (item->stype) {
	case F_INBOX:
		name = g_strdup(!strcmp2(item->name, INBOX_DIR) ? _("Inbox") :
				item->name);
		break;
	case F_OUTBOX:
		name = g_strdup(!strcmp2(item->name, OUTBOX_DIR) ? _("Sent") :
				item->name);
		break;
	case F_QUEUE:
		name = g_strdup(!strcmp2(item->name, QUEUE_DIR) ? _("Queue") :
				item->name);
		break;
	case F_TRASH:
		name = g_strdup(!strcmp2(item->name, TRASH_DIR) ? _("Trash") :
				item->name);
		break;
	case F_DRAFT:
		name = g_strdup(!strcmp2(item->name, DRAFT_DIR) ? _("Drafts") :
				item->name);
		break;
	default:
		break;
	}

	if (name == NULL) {
		/*
		 * should probably be done by a virtual function,
		 * the folder knows the ui string and how to abbrev
		*/
		if (folder_item_parent(item) == NULL) {
			name = g_strconcat(item->name, " (", item->folder->klass->uistr, ")", NULL);
		} else {
			if (FOLDER_CLASS(item->folder) == news_get_class() &&
			    item->path && !strcmp2(item->name, item->path))
				name = get_abbrev_newsgroup_name
					(item->path,
					 prefs_common.ng_abbrev_len);
			else
				name = g_strdup(item->name);
		}
	}

	if (name == NULL)
		name = g_strdup("");

	return name;
}

Folder *folder_get_default_folder(void)
{
	return folder_list ? FOLDER(folder_list->data) : NULL;
}

gboolean folder_have_mailbox (void)
{
	GList *cur;
	for (cur = folder_list; cur != NULL; cur = g_list_next(cur)) {
		Folder *folder = FOLDER(cur->data);
		if (folder->inbox && folder->outbox)
			return TRUE;
	}
	return FALSE;
}

FolderItem *folder_get_default_inbox(void)
{
	GList *flist;

	for (flist = folder_list; flist != NULL; flist = g_list_next(flist)) {
		Folder * folder = FOLDER(flist->data);

		if (folder == NULL)
			continue;
		if (folder->inbox == NULL)
			continue;
		if (folder->klass->type == F_UNKNOWN)
			continue;

		return folder->inbox;
	}

	return NULL;
}

FolderItem *folder_get_default_outbox(void)
{
	GList *flist;

	for (flist = folder_list; flist != NULL; flist = g_list_next(flist)) {
		Folder * folder = FOLDER(flist->data);

		if (folder == NULL)
			continue;
		if (folder->outbox == NULL)
			continue;
		if (folder->klass->type == F_UNKNOWN)
			continue;

		return folder->outbox;
	}

	return NULL;
}

FolderItem *folder_get_default_draft(void)
{
	GList *flist;

	for (flist = folder_list; flist != NULL; flist = g_list_next(flist)) {
		Folder * folder = FOLDER(flist->data);

		if (folder == NULL)
			continue;
		if (folder->draft == NULL)
			continue;
		if (folder->klass->type == F_UNKNOWN)
			continue;

		return folder->draft;
	}

	return NULL;
}

FolderItem *folder_get_default_queue(void)
{
	GList *flist;

	for (flist = folder_list; flist != NULL; flist = g_list_next(flist)) {
		Folder * folder = FOLDER(flist->data);

		if (folder == NULL)
			continue;
		if (folder->queue == NULL)
			continue;
		if (folder->klass->type == F_UNKNOWN)
			continue;

		return folder->queue;
	}

	return NULL;
}

FolderItem *folder_get_default_trash(void)
{
	GList *flist;

	for (flist = folder_list; flist != NULL; flist = g_list_next(flist)) {
		Folder * folder = FOLDER(flist->data);

		if (folder == NULL)
			continue;
		if (folder->trash == NULL)
			continue;
		if (folder->klass->type == F_UNKNOWN)
			continue;

		return folder->trash;
	}

	return NULL;
}

#define CREATE_FOLDER_IF_NOT_EXIST(member, dir, type)		\
{								\
	if (!folder->member) {					\
		item = folder_item_new(folder, dir, dir);	\
		item->stype = type;				\
		folder_item_append(rootitem, item);		\
		folder->member = item;				\
	}							\
}

void folder_set_missing_folders(void)
{
	Folder *folder;
	FolderItem *rootitem;
	FolderItem *item;
	GList *list;

	for (list = folder_list; list != NULL; list = list->next) {
		folder = list->data;
		if (FOLDER_TYPE(folder) != F_MH) continue;
		rootitem = FOLDER_ITEM(folder->node->data);
		g_return_if_fail(rootitem != NULL);

		if (folder->inbox && folder->outbox && folder->draft &&
		    folder->queue && folder->trash)
			continue;

		if (folder->klass->create_tree(folder) < 0) {
			g_warning("%s: can't create the folder tree.\n",
				  LOCAL_FOLDER(folder)->rootpath);
			continue;
		}

		CREATE_FOLDER_IF_NOT_EXIST(inbox,  INBOX_DIR,  F_INBOX);
		CREATE_FOLDER_IF_NOT_EXIST(outbox, OUTBOX_DIR, F_OUTBOX);
		CREATE_FOLDER_IF_NOT_EXIST(draft,  DRAFT_DIR,  F_DRAFT);
		CREATE_FOLDER_IF_NOT_EXIST(queue,  QUEUE_DIR,  F_QUEUE);
		CREATE_FOLDER_IF_NOT_EXIST(trash,  TRASH_DIR,  F_TRASH);
	}
}

static gboolean folder_unref_account_func(GNode *node, gpointer data)
{
	FolderItem *item = node->data;
	PrefsAccount *account = data;

	if (item->account == account)
		item->account = NULL;

	return FALSE;
}

void folder_unref_account_all(PrefsAccount *account)
{
	Folder *folder;
	GList *list;

	if (!account) return;

	for (list = folder_list; list != NULL; list = list->next) {
		folder = list->data;
		if (folder->account == account)
			folder->account = NULL;
		g_node_traverse(folder->node, G_PRE_ORDER, G_TRAVERSE_ALL, -1,
				folder_unref_account_func, account);
	}
}

#undef CREATE_FOLDER_IF_NOT_EXIST

gchar *folder_item_get_path(FolderItem *item)
{
	Folder *folder;

	g_return_val_if_fail(item != NULL, NULL);
	folder = item->folder;
	g_return_val_if_fail(folder != NULL, NULL);

	return folder->klass->item_get_path(folder, item);
}

void folder_item_set_default_flags(FolderItem *dest, MsgFlags *flags)
{
	if (!(folder_has_parent_of_type(dest, F_OUTBOX) ||
	      folder_has_parent_of_type(dest, F_QUEUE)  ||
	      folder_has_parent_of_type(dest, F_DRAFT) ||
	      folder_has_parent_of_type(dest, F_TRASH))) {
		flags->perm_flags = MSG_NEW|MSG_UNREAD;
	} else {
		flags->perm_flags = 0;
	}
	if (FOLDER_TYPE(dest->folder) == F_MH) {
		if (folder_has_parent_of_type(dest, F_QUEUE)) {
			MSG_SET_TMP_FLAGS(*flags, MSG_QUEUED);
		} else if (folder_has_parent_of_type(dest, F_DRAFT)) {
			MSG_SET_TMP_FLAGS(*flags, MSG_DRAFT);
		}
	}
}

static gint folder_sort_cache_list_by_msgnum(gconstpointer a, gconstpointer b)
{
	MsgInfo *msginfo_a = (MsgInfo *) a;
	MsgInfo *msginfo_b = (MsgInfo *) b;

	return (msginfo_a->msgnum - msginfo_b->msgnum);
}

static gint folder_sort_folder_list(gconstpointer a, gconstpointer b)
{
	guint gint_a = GPOINTER_TO_INT(a);
	guint gint_b = GPOINTER_TO_INT(b);
	
	return (gint_a - gint_b);
}

void folder_item_process_open (FolderItem *item,
				 void (*before_proc_func)(gpointer data),
				 void (*after_proc_func)(gpointer data),
				 gpointer data)
{
	gchar *buf;
	if (item == NULL)
		return;
	if((item->folder->klass->scan_required != NULL) &&
	   (item->folder->klass->scan_required(item->folder, item))) {
		folder_item_scan_full(item, TRUE);
	} else {
		folder_item_syncronize_flags(item);
	}
	
	/* Processing */
	buf = g_strdup_printf(_("Processing (%s)...\n"), 
			      item->path ? item->path : item->name);
	g_free(buf);

	if (before_proc_func)
		before_proc_func(data);

	folder_item_apply_processing(item);

	if (after_proc_func)
		after_proc_func(data);

	item->processing_pending = FALSE;
	return;	
}

gint folder_item_open(FolderItem *item)
{
	START_TIMING(""); 
	if (item->no_select)
		return -1;

	if (item->scanning) {
		debug_print("%s is scanning... \n", item->path ? item->path : item->name);
		return -2;
	}

	item->processing_pending = TRUE;
	folder_item_process_open (item, NULL, NULL, NULL);
	
	item->opened = TRUE;
	END_TIMING();
	return 0;
}

gint folder_item_close(FolderItem *item)
{
	GSList *mlist, *cur;
	Folder *folder;
	
	g_return_val_if_fail(item != NULL, -1);

	if (item->no_select)
		return -1;

	if (item->new_msgs) {
		folder_item_update_freeze();
		mlist = folder_item_get_msg_list(item);
		for (cur = mlist ; cur != NULL ; cur = cur->next) {
			MsgInfo * msginfo;

			msginfo = (MsgInfo *) cur->data;
			if (MSG_IS_NEW(msginfo->flags))
				procmsg_msginfo_unset_flags(msginfo, MSG_NEW, 0);
			procmsg_msginfo_free(msginfo);
		}
		g_slist_free(mlist);
		folder_item_update_thaw();
	}		

	folder_item_write_cache(item);
	
	folder_item_update(item, F_ITEM_UPDATE_MSGCNT);

	item->opened = FALSE;
	folder = item->folder;

	if (folder->klass->close == NULL)
		return 0;

	return folder->klass->close(folder, item);
}

static MsgInfoList *get_msginfos(FolderItem *item, MsgNumberList *numlist)
{
	MsgInfoList *msglist = NULL;
	Folder *folder = item->folder;
	if (item->no_select)
		return NULL;
	
	if (folder->klass->get_msginfos != NULL)
		msglist = folder->klass->get_msginfos(folder, item, numlist);
	else {
		MsgNumberList *elem;

		for (elem = numlist; elem != NULL; elem = g_slist_next(elem)) {
			MsgInfo *msginfo;
			guint num;

			num = GPOINTER_TO_INT(elem->data);
			msginfo = folder->klass->get_msginfo(folder, item, num);
			if (msginfo != NULL)
				msglist = g_slist_prepend(msglist, msginfo);
		}		
	}

	return msglist;
}

static MsgInfo *get_msginfo(FolderItem *item, guint num)
{
	MsgNumberList numlist;
	MsgInfoList *msglist;
	MsgInfo *msginfo = NULL;

	numlist.data = GINT_TO_POINTER(num);
	numlist.next = NULL;
	msglist = get_msginfos(item, &numlist);
	if (msglist != NULL)
		msginfo = procmsg_msginfo_new_ref(msglist->data);
	procmsg_msg_list_free(msglist);

	return msginfo;
}

static gint syncronize_flags(FolderItem *item, MsgInfoList *msglist)
{
	GRelation *relation;
	gint ret = 0;
	GSList *cur;

	if(msglist == NULL)
		return 0;
	if(item->folder->klass->get_flags == NULL)
		return 0;
	if (item->no_select)
		return 0;

	relation = g_relation_new(2);
	g_relation_index(relation, 0, g_direct_hash, g_direct_equal);
	folder_item_set_batch(item, TRUE);
	if ((ret = item->folder->klass->get_flags(
	    item->folder, item, msglist, relation)) == 0) {
		GTuples *tuples;
		MsgInfo *msginfo;
		MsgPermFlags permflags = 0;
		gboolean skip;

		for (cur = msglist; cur != NULL; cur = g_slist_next(cur)) {
			msginfo = (MsgInfo *) cur->data;
		
			tuples = g_relation_select(relation, msginfo, 0);
			skip = tuples->len < 1;
			if (!skip)
				permflags = GPOINTER_TO_INT(g_tuples_index(tuples, 0, 1));
			g_tuples_destroy(tuples);
			if (skip)
				continue;
			
			if (msginfo->flags.perm_flags != permflags) {
				procmsg_msginfo_change_flags(msginfo,
					permflags & ~msginfo->flags.perm_flags, 0,
					~permflags & msginfo->flags.perm_flags, 0);
			}
		}
	}
	folder_item_set_batch(item, FALSE);
	g_relation_destroy(relation);	

	return ret;
}

gint folder_item_scan_full(FolderItem *item, gboolean filtering)
{
	Folder *folder;
	GSList *folder_list = NULL, *cache_list = NULL;
	GSList *folder_list_cur, *cache_list_cur, *new_list = NULL;
	GSList *exists_list = NULL, *elem;
	GSList *newmsg_list = NULL;
	guint newcnt = 0, unreadcnt = 0, totalcnt = 0;
	guint markedcnt = 0, unreadmarkedcnt = 0;
	guint cache_max_num, folder_max_num, cache_cur_num, folder_cur_num;
	gboolean update_flags = 0, old_uids_valid = FALSE;
	GHashTable *subject_table = NULL;
	
	g_return_val_if_fail(item != NULL, -1);
	if (item->path == NULL) return -1;

	folder = item->folder;

	g_return_val_if_fail(folder != NULL, -1);
	g_return_val_if_fail(folder->klass->get_num_list != NULL, -1);

	item->scanning = TRUE;

	debug_print("Scanning folder %s for cache changes.\n", item->path ? item->path : "(null)");
	
	/* Get list of messages for folder and cache */
	if (folder->klass->get_num_list(item->folder, item, &folder_list, &old_uids_valid) < 0) {
		debug_print("Error fetching list of message numbers\n");
		item->scanning = FALSE;
		return(-1);
	}

	if(prefs_common.thread_by_subject) {
		subject_table = g_hash_table_new(g_str_hash, g_str_equal);
	}
	
	if (old_uids_valid) {
		if (!item->cache)
			folder_item_read_cache(item);
		cache_list = msgcache_get_msg_list(item->cache);
	} else {
		if (item->cache)
			msgcache_destroy(item->cache);
		item->cache = msgcache_new();
		cache_list = NULL;
	}

	/* Sort both lists */
    	cache_list = g_slist_sort(cache_list, folder_sort_cache_list_by_msgnum);
	folder_list = g_slist_sort(folder_list, folder_sort_folder_list);

	cache_list_cur = cache_list;
	folder_list_cur = folder_list;

	if (cache_list_cur != NULL) {
		GSList *cache_list_last;
	
		cache_cur_num = ((MsgInfo *)cache_list_cur->data)->msgnum;
		cache_list_last = g_slist_last(cache_list);
		cache_max_num = ((MsgInfo *)cache_list_last->data)->msgnum;
	} else {
		cache_cur_num = G_MAXINT;
		cache_max_num = 0;
	}

	if (folder_list_cur != NULL) {
		GSList *folder_list_last;
	
		folder_cur_num = GPOINTER_TO_INT(folder_list_cur->data);
		folder_list_last = g_slist_last(folder_list);
		folder_max_num = GPOINTER_TO_INT(folder_list_last->data);
	} else {
		folder_cur_num = G_MAXINT;
		folder_max_num = 0;
	}

	while ((cache_cur_num != G_MAXINT) || (folder_cur_num != G_MAXINT)) {
		/*
		 *  Message only exists in the folder
		 *  Remember message for fetching
		 */
		if (folder_cur_num < cache_cur_num) {
			gboolean add = FALSE;

			switch(FOLDER_TYPE(folder)) {
				case F_NEWS:
					if (folder_cur_num < cache_max_num)
						break;
					
					if (folder->account->max_articles == 0) {
						add = TRUE;
					}

					if (folder_max_num <= folder->account->max_articles) {
						add = TRUE;
					} else if (folder_cur_num > (folder_max_num - folder->account->max_articles)) {
						add = TRUE;
					}
					break;
				default:
					add = TRUE;
					break;
			}
			
			if (add) {
				new_list = g_slist_prepend(new_list, GINT_TO_POINTER(folder_cur_num));
				debug_print("Remembered message %d for fetching\n", folder_cur_num);
			}

			/* Move to next folder number */
			folder_list_cur = folder_list_cur->next;

			if (folder_list_cur != NULL)
				folder_cur_num = GPOINTER_TO_INT(folder_list_cur->data);
			else
				folder_cur_num = G_MAXINT;

			continue;
		}

		/*
		 *  Message only exists in the cache
		 *  Remove the message from the cache
		 */
		if (cache_cur_num < folder_cur_num) {
			msgcache_remove_msg(item->cache, cache_cur_num);
			debug_print("Removed message %d from cache.\n", cache_cur_num);

			/* Move to next cache number */
			cache_list_cur = cache_list_cur->next;

			if (cache_list_cur != NULL)
				cache_cur_num = ((MsgInfo *)cache_list_cur->data)->msgnum;
			else
				cache_cur_num = G_MAXINT;

			update_flags |= F_ITEM_UPDATE_MSGCNT | F_ITEM_UPDATE_CONTENT;

			continue;
		}

		/*
		 *  Message number exists in folder and cache!
		 *  Check if the message has been modified
		 */
		if (cache_cur_num == folder_cur_num) {
			MsgInfo *msginfo;

			msginfo = msgcache_get_msg(item->cache, folder_cur_num);
			if (folder->klass->is_msg_changed && folder->klass->is_msg_changed(folder, item, msginfo)) {
				msgcache_remove_msg(item->cache, msginfo->msgnum);
				new_list = g_slist_prepend(new_list, GINT_TO_POINTER(msginfo->msgnum));
				procmsg_msginfo_free(msginfo);

				debug_print("Remembering message %d to update...\n", folder_cur_num);
			} else {
				exists_list = g_slist_prepend(exists_list, msginfo);

				if(prefs_common.thread_by_subject &&
					MSG_IS_IGNORE_THREAD(msginfo->flags) &&
					!subject_table_lookup(subject_table, msginfo->subject)) {
					subject_table_insert(subject_table, msginfo->subject, msginfo);
				}
			}
			
			/* Move to next folder and cache number */
			cache_list_cur = cache_list_cur->next;
			folder_list_cur = folder_list_cur->next;

			if (cache_list_cur != NULL)
				cache_cur_num = ((MsgInfo *)cache_list_cur->data)->msgnum;
			else
				cache_cur_num = G_MAXINT;

			if (folder_list_cur != NULL)
				folder_cur_num = GPOINTER_TO_INT(folder_list_cur->data);
			else
				folder_cur_num = G_MAXINT;

			continue;
		}
	}
	
	for(cache_list_cur = cache_list; cache_list_cur != NULL; cache_list_cur = g_slist_next(cache_list_cur))
		procmsg_msginfo_free((MsgInfo *) cache_list_cur->data);

	g_slist_free(cache_list);
	g_slist_free(folder_list);

	if (new_list != NULL) {
		GSList *tmp_list = NULL;
		newmsg_list = get_msginfos(item, new_list);
		g_slist_free(new_list);
		tmp_list = g_slist_concat(g_slist_copy(exists_list), g_slist_copy(newmsg_list));
		syncronize_flags(item, tmp_list);
		g_slist_free(tmp_list);
	} else {
		syncronize_flags(item, exists_list);
	}

	folder_item_update_freeze();
	if (newmsg_list != NULL) {
		GSList *elem, *to_filter = NULL;
		gboolean do_filter = (filtering == TRUE) &&
		    			(item->stype == F_INBOX) &&
		   			(item->folder->account != NULL) && 
		   			(item->folder->account->filter_on_recv);
		
		for (elem = newmsg_list; elem != NULL; elem = g_slist_next(elem)) {
			MsgInfo *msginfo = (MsgInfo *) elem->data;

			msgcache_add_msg(item->cache, msginfo);
			if (!do_filter) {
				exists_list = g_slist_prepend(exists_list, msginfo);

				if(prefs_common.thread_by_subject &&
					MSG_IS_IGNORE_THREAD(msginfo->flags) &&
					!subject_table_lookup(subject_table, msginfo->subject)) {
					subject_table_insert(subject_table, msginfo->subject, msginfo);
				}			
			}
		}

		if (do_filter) {
			GSList *unfiltered;
			procmsg_msglist_filter(newmsg_list, item->folder->account, 
					&to_filter, &unfiltered, 
					TRUE);
			
			if (to_filter != NULL) {
				filtering_move_and_copy_msgs(to_filter);
				for (elem = to_filter; elem; elem = g_slist_next(elem)) {
					MsgInfo *msginfo = (MsgInfo *)elem->data;
					procmsg_msginfo_free(msginfo);
				}
				g_slist_free(to_filter);
			}
			if (unfiltered != NULL) {
				for (elem = unfiltered; elem; elem = g_slist_next(elem)) {
					MsgInfo *msginfo = (MsgInfo *)elem->data;
					exists_list = g_slist_prepend(exists_list, msginfo);

					if(prefs_common.thread_by_subject &&
						MSG_IS_IGNORE_THREAD(msginfo->flags) &&
						!subject_table_lookup(subject_table, msginfo->subject)) {
						subject_table_insert(subject_table, msginfo->subject, msginfo);
					}
				}
				g_slist_free(unfiltered);
			}
		} 

		g_slist_free(newmsg_list);

		update_flags |= F_ITEM_UPDATE_MSGCNT | F_ITEM_UPDATE_CONTENT;
	}

	folder_item_set_batch(item, TRUE);
	for (elem = exists_list; elem != NULL; elem = g_slist_next(elem)) {
		MsgInfo *msginfo, *parent_msginfo;

		msginfo = elem->data;
		if (MSG_IS_IGNORE_THREAD(msginfo->flags) && (MSG_IS_NEW(msginfo->flags) || MSG_IS_UNREAD(msginfo->flags)))
			procmsg_msginfo_unset_flags(msginfo, MSG_NEW | MSG_UNREAD, 0);
		if (!MSG_IS_IGNORE_THREAD(msginfo->flags) && procmsg_msg_has_flagged_parent(msginfo, MSG_IGNORE_THREAD)) {
			procmsg_msginfo_change_flags(msginfo, MSG_IGNORE_THREAD, 0, MSG_NEW | MSG_UNREAD, 0);
		}
		if(prefs_common.thread_by_subject && !msginfo->inreplyto &&
			!msginfo->references && !MSG_IS_IGNORE_THREAD(msginfo->flags) &&
			(parent_msginfo = subject_table_lookup(subject_table, msginfo->subject)))
		{
			if(MSG_IS_IGNORE_THREAD(parent_msginfo->flags)) {
				procmsg_msginfo_change_flags(msginfo, MSG_IGNORE_THREAD, 0,
						MSG_NEW | MSG_UNREAD, 0);
			}
		}
		if ((folder_has_parent_of_type(item, F_OUTBOX) ||
		     folder_has_parent_of_type(item, F_QUEUE)  ||
		     folder_has_parent_of_type(item, F_DRAFT)  ||
		     folder_has_parent_of_type(item, F_TRASH)) &&
		    (MSG_IS_NEW(msginfo->flags) || MSG_IS_UNREAD(msginfo->flags)))
			procmsg_msginfo_unset_flags(msginfo, MSG_NEW | MSG_UNREAD, 0);
		if (MSG_IS_NEW(msginfo->flags))
			newcnt++;
		if (MSG_IS_UNREAD(msginfo->flags))
			unreadcnt++;
		if (MSG_IS_UNREAD(msginfo->flags) && procmsg_msg_has_marked_parent(msginfo))
			unreadmarkedcnt++;
		if (MSG_IS_MARKED(msginfo->flags))
			markedcnt++;

		totalcnt++;

		procmsg_msginfo_free(msginfo);
	}
	folder_item_set_batch(item, FALSE);
	g_slist_free(exists_list);
	
	if(prefs_common.thread_by_subject) {
		g_hash_table_destroy(subject_table);
	}
	
	if (item->new_msgs != newcnt || item->unread_msgs != unreadcnt
	||  item->total_msgs != totalcnt || item->marked_msgs != markedcnt
	||  item->unreadmarked_msgs != unreadmarkedcnt) {
		update_flags |= F_ITEM_UPDATE_CONTENT;
	}

	item->new_msgs = newcnt;
	item->unread_msgs = unreadcnt;
	item->total_msgs = totalcnt;
	item->unreadmarked_msgs = unreadmarkedcnt;
	item->marked_msgs = markedcnt;

	update_flags |= F_ITEM_UPDATE_MSGCNT;

	folder_item_update(item, update_flags);
	folder_item_update_thaw();
	
	item->scanning = FALSE;

	return 0;
}

gint folder_item_scan(FolderItem *item)
{
	return folder_item_scan_full(item, TRUE);
}

static gboolean folder_scan_all_items_func(GNode *node, gpointer data)
{
	FolderItem *item = node->data;

	folder_item_scan(item);

	return FALSE;
}

void folder_scan_all_items(Folder * folder)
{
	g_node_traverse(folder->node, G_PRE_ORDER,
			G_TRAVERSE_ALL, -1, folder_scan_all_items_func, NULL);
}

static void folder_item_scan_foreach_func(gpointer key, gpointer val,
					  gpointer data)
{
	folder_item_scan(FOLDER_ITEM(key));
}

void folder_item_scan_foreach(GHashTable *table)
{
	g_hash_table_foreach(table, folder_item_scan_foreach_func, NULL);
}

void folder_count_total_cache_memusage(FolderItem *item, gpointer data)
{
	gint *memusage = (gint *)data;

	if (item->cache == NULL)
		return;
	
	*memusage += msgcache_get_memory_usage(item->cache);
}

gint folder_item_syncronize_flags(FolderItem *item)
{
	MsgInfoList *msglist = NULL;
	GSList *cur;
	gint ret = 0;
	
	g_return_val_if_fail(item != NULL, -1);
	g_return_val_if_fail(item->folder != NULL, -1);
	g_return_val_if_fail(item->folder->klass != NULL, -1);
	if (item->no_select)
		return -1;

	item->scanning = TRUE;

	if (item->cache == NULL)
		folder_item_read_cache(item);
	
	msglist = msgcache_get_msg_list(item->cache);
	
	ret = syncronize_flags(item, msglist);

	for (cur = msglist; cur != NULL; cur = g_slist_next(cur))
		procmsg_msginfo_free((MsgInfo *) cur->data);
	
	g_slist_free(msglist);

	item->scanning = FALSE;

	return ret;
}

gint folder_cache_time_compare_func(gconstpointer a, gconstpointer b)
{
	FolderItem *fa = (FolderItem *)a;
	FolderItem *fb = (FolderItem *)b;
	
	return (gint) (msgcache_get_last_access_time(fa->cache) - msgcache_get_last_access_time(fb->cache));
}

void folder_find_expired_caches(FolderItem *item, gpointer data)
{
	GSList **folder_item_list = (GSList **)data;
	gint difftime, expiretime;
	
	if (item->cache == NULL)
		return;

	if (item->opened > 0)
		return;

	difftime = (gint) (time(NULL) - msgcache_get_last_access_time(item->cache));
	expiretime = prefs_common.cache_min_keep_time * 60;
	debug_print("Cache unused time: %d (Expire time: %d)\n", difftime, expiretime);

	if (difftime > expiretime && !item->opened && !item->processing_pending) {
		*folder_item_list = g_slist_insert_sorted(*folder_item_list, item, folder_cache_time_compare_func);
	}
}

gboolean folder_item_free_cache(FolderItem *item, gboolean force)
{
	g_return_val_if_fail(item != NULL, TRUE);
	
	if (item->cache == NULL)
		return TRUE;
	
	if (item->opened > 0 && !force)
		return FALSE;

	folder_item_write_cache(item);
	msgcache_destroy(item->cache);
	item->cache = NULL;
	return TRUE;
}

void folder_clean_cache_memory_force(void)
{
	int old_cache_max_mem_usage = prefs_common.cache_max_mem_usage;
	int old_cache_min_keep_time = prefs_common.cache_min_keep_time;

	prefs_common.cache_max_mem_usage = 0;
	prefs_common.cache_min_keep_time = 0;

	folder_clean_cache_memory(NULL);

	prefs_common.cache_max_mem_usage = old_cache_max_mem_usage;
	prefs_common.cache_min_keep_time = old_cache_min_keep_time;
}

void folder_clean_cache_memory(FolderItem *protected_item)
{
	gint memusage = 0;

	folder_func_to_all_folders(folder_count_total_cache_memusage, &memusage);	
	debug_print("Total cache memory usage: %d\n", memusage);
	
	if (memusage > (prefs_common.cache_max_mem_usage * 1024)) {
		GSList *folder_item_list = NULL, *listitem;
		
		debug_print("Trying to free cache memory\n");

		folder_func_to_all_folders(folder_find_expired_caches, &folder_item_list);	
		listitem = folder_item_list;
		while((listitem != NULL) && (memusage > (prefs_common.cache_max_mem_usage * 1024))) {
			FolderItem *item = (FolderItem *)(listitem->data);
			gint cache_size = 0;
			if (item == protected_item) {
				listitem = listitem->next;
				continue;
			}
			debug_print("Freeing cache memory for %s\n", item->path ? item->path : item->name);
			cache_size = msgcache_get_memory_usage(item->cache);
		        if (folder_item_free_cache(item, FALSE))
				memusage -= cache_size;

			listitem = listitem->next;
		}
		g_slist_free(folder_item_list);
	}
}

void folder_item_read_cache(FolderItem *item)
{
	gchar *cache_file, *mark_file;
	START_TIMING("");
	g_return_if_fail(item != NULL);

	if (item->path != NULL) {
	        cache_file = folder_item_get_cache_file(item);
		mark_file = folder_item_get_mark_file(item);
		item->cache = msgcache_read_cache(item, cache_file);
		if (!item->cache) {
			MsgInfoList *list, *cur;
			guint newcnt = 0, unreadcnt = 0;
			guint markedcnt = 0, unreadmarkedcnt = 0;
			MsgInfo *msginfo;

			item->cache = msgcache_new();
			folder_item_scan_full(item, TRUE);

			msgcache_read_mark(item->cache, mark_file);

			list = msgcache_get_msg_list(item->cache);
			for (cur = list; cur != NULL; cur = g_slist_next(cur)) {
				msginfo = cur->data;

				if (MSG_IS_NEW(msginfo->flags))
					newcnt++;
				if (MSG_IS_UNREAD(msginfo->flags))
					unreadcnt++;
				if (MSG_IS_UNREAD(msginfo->flags) && procmsg_msg_has_marked_parent(msginfo))
					unreadmarkedcnt++;
				if (MSG_IS_MARKED(msginfo->flags))
					markedcnt++;
			}
			item->new_msgs = newcnt;
		        item->unread_msgs = unreadcnt;
			item->unreadmarked_msgs = unreadmarkedcnt;
			item->marked_msgs = markedcnt;
			procmsg_msg_list_free(list);
		} else
			msgcache_read_mark(item->cache, mark_file);

		g_free(cache_file);
		g_free(mark_file);
	} else {
		item->cache = msgcache_new();
	}
	END_TIMING();
	folder_clean_cache_memory(item);
}

void folder_item_write_cache(FolderItem *item)
{
	gchar *cache_file, *mark_file;
	FolderItemPrefs *prefs;
	gint filemode = 0;
	gchar *id;
	time_t last_mtime = (time_t)0;
	gboolean need_scan = FALSE;
	
	if (!item || !item->path || !item->cache)
		return;

	if (FOLDER_TYPE(item->folder) == F_MH) {
		last_mtime = item->mtime;
		need_scan = item->folder->klass->scan_required(item->folder, item);
	}

	id = folder_item_get_identifier(item);
	debug_print("Save cache for folder %s\n", id);
	g_free(id);

	cache_file = folder_item_get_cache_file(item);
	mark_file = folder_item_get_mark_file(item);
	if (msgcache_write(cache_file, mark_file, item->cache) < 0) {
		prefs = item->prefs;
    		if (prefs && prefs->enable_folder_chmod && prefs->folder_chmod) {
			/* for cache file */
			filemode = prefs->folder_chmod;
			if (filemode & S_IRGRP) filemode |= S_IWGRP;
			if (filemode & S_IROTH) filemode |= S_IWOTH;
			chmod(cache_file, filemode);
		}
        }

	if (!need_scan && FOLDER_TYPE(item->folder) == F_MH) {
		if (item->mtime == last_mtime)
			item->mtime = time(NULL);
	}

	g_free(cache_file);
	g_free(mark_file);
}

MsgInfo *folder_item_get_msginfo(FolderItem *item, gint num)
{
	Folder *folder;
	MsgInfo *msginfo = NULL;
	
	g_return_val_if_fail(item != NULL, NULL);
	if (item->no_select)
		return NULL;
	folder = item->folder;
	if (!item->cache)
		folder_item_read_cache(item);
	
	if ((msginfo = msgcache_get_msg(item->cache, num)) != NULL)
		return msginfo;
	
	msginfo = get_msginfo(item, num);
	if (msginfo != NULL) {
		msgcache_add_msg(item->cache, msginfo);
		return msginfo;
	}
	
	return NULL;
}

MsgInfo *folder_item_get_msginfo_by_msgid(FolderItem *item, const gchar *msgid)
{
	Folder *folder;
	MsgInfo *msginfo;
	
	g_return_val_if_fail(item != NULL, NULL);
	g_return_val_if_fail(msgid != NULL, NULL);
	if (item->no_select)
		return FALSE;
	
	folder = item->folder;
	if (!item->cache)
		folder_item_read_cache(item);
	
	if ((msginfo = msgcache_get_msg_by_id(item->cache, msgid)) != NULL)
		return msginfo;

	return NULL;
}

GSList *folder_item_get_msg_list(FolderItem *item)
{
	g_return_val_if_fail(item != NULL, NULL);
	if (item->no_select)
		return FALSE;
	
	if (item->cache == 0)
		folder_item_read_cache(item);

	g_return_val_if_fail(item->cache != NULL, NULL);
	
	return msgcache_get_msg_list(item->cache);
}

void msginfo_set_mime_flags(GNode *node, gpointer data)
{
	MsgInfo *msginfo = data;
	MimeInfo *mimeinfo = node->data;
	
	if (mimeinfo->disposition == DISPOSITIONTYPE_ATTACHMENT
	 && (!mimeinfo->subtype ||  strcmp(mimeinfo->subtype, "pgp-signature"))) {
		procmsg_msginfo_set_flags(msginfo, 0, MSG_HAS_ATTACHMENT);
	} else if (mimeinfo->disposition == DISPOSITIONTYPE_UNKNOWN && 
		 mimeinfo->type != MIMETYPE_TEXT &&
		 mimeinfo->type != MIMETYPE_MULTIPART) {
		if (!mimeinfo->subtype 
		||  strcmp(mimeinfo->subtype, "pgp-signature"))
			procmsg_msginfo_set_flags(msginfo, 0, MSG_HAS_ATTACHMENT);
	} else if (mimeinfo->disposition == DISPOSITIONTYPE_INLINE &&
		strcmp(mimeinfo->subtype, "pgp-signature") && 
		(procmime_mimeinfo_get_parameter(mimeinfo, "name") != NULL ||
		 procmime_mimeinfo_get_parameter(mimeinfo, "filename") != NULL)) {
		procmsg_msginfo_set_flags(msginfo, 0, MSG_HAS_ATTACHMENT);
	} 

	/* don't descend below top level message for signed and encrypted info */
	if (mimeinfo->type == MIMETYPE_MESSAGE)
		return;

	if (privacy_mimeinfo_is_signed(mimeinfo)) {
		procmsg_msginfo_set_flags(msginfo, 0, MSG_SIGNED);
	}

	if (privacy_mimeinfo_is_encrypted(mimeinfo)) {
		procmsg_msginfo_set_flags(msginfo, 0, MSG_ENCRYPTED);
	} else {
		/* searching inside encrypted parts doesn't really make sense */
		g_node_children_foreach(mimeinfo->node, G_TRAVERSE_ALL, msginfo_set_mime_flags, msginfo);
	}
}

gchar *folder_item_fetch_msg(FolderItem *item, gint num)
{
	Folder *folder;
	gchar *msgfile;
	MsgInfo *msginfo;

	g_return_val_if_fail(item != NULL, NULL);

	folder = item->folder;

	g_return_val_if_fail(folder->klass->fetch_msg != NULL, NULL);
	if (item->no_select)
		return NULL;

	msgfile = folder->klass->fetch_msg(folder, item, num);

	if (msgfile != NULL) {
		msginfo = folder_item_get_msginfo(item, num);
		if ((msginfo != NULL) && !MSG_IS_SCANNED(msginfo->flags)) {
			MimeInfo *mimeinfo;

			if (!folder_has_parent_of_type(msginfo->folder, F_QUEUE) && 
			    !folder_has_parent_of_type(msginfo->folder, F_DRAFT))
				mimeinfo = procmime_scan_file(msgfile);
			else
				mimeinfo = procmime_scan_queue_file(msgfile);
			/* check for attachments */
			if (mimeinfo != NULL) {	
				g_node_children_foreach(mimeinfo->node, G_TRAVERSE_ALL, msginfo_set_mime_flags, msginfo);
				procmime_mimeinfo_free_all(mimeinfo);

				procmsg_msginfo_set_flags(msginfo, 0, MSG_SCANNED);
			}
		}
		procmsg_msginfo_free(msginfo);
	}

	return msgfile;
}

gchar *folder_item_fetch_msg_full(FolderItem *item, gint num, gboolean headers,
				  gboolean body)
{
	Folder *folder;
	gchar *msgfile;
	MsgInfo *msginfo;

	g_return_val_if_fail(item != NULL, NULL);
	if (item->no_select)
		return NULL;
	
	folder = item->folder;

	if (folder->klass->fetch_msg_full == NULL)
		return folder_item_fetch_msg(item, num);

	msgfile = folder->klass->fetch_msg_full(folder, item, num, 
						headers, body);

	if (msgfile != NULL) {
		msginfo = folder_item_get_msginfo(item, num);
		if ((msginfo != NULL) && !MSG_IS_SCANNED(msginfo->flags)) {
			MimeInfo *mimeinfo;

			if (!folder_has_parent_of_type(msginfo->folder, F_QUEUE) &&
			    !folder_has_parent_of_type(msginfo->folder, F_DRAFT))
				mimeinfo = procmime_scan_file(msgfile);
			else
				mimeinfo = procmime_scan_queue_file(msgfile);
			/* check for attachments */
			if (mimeinfo != NULL) {	
				g_node_children_foreach(mimeinfo->node, G_TRAVERSE_ALL, msginfo_set_mime_flags, msginfo);
				procmime_mimeinfo_free_all(mimeinfo);

				procmsg_msginfo_set_flags(msginfo, 0, MSG_SCANNED);
			}
		}
		procmsg_msginfo_free(msginfo);
	}

	return msgfile;
}


gint folder_item_fetch_all_msg(FolderItem *item)
{
	Folder *folder;
	GSList *mlist;
	GSList *cur;
	gint num = 0;
	gint ret = 0;
	gint total = 0;

	g_return_val_if_fail(item != NULL, -1);
	if (item->no_select)
		return -1;

	debug_print("fetching all messages in %s ...\n", item->path ? item->path : "(null)");
	statusbar_print_all(_("Fetching all messages in %s ...\n"), item->path ? item->path : "(null)");

	folder = item->folder;

	if (folder->ui_func)
		folder->ui_func(folder, item, folder->ui_func_data ?
				folder->ui_func_data : GINT_TO_POINTER(num));

	mlist = folder_item_get_msg_list(item);

	total = g_slist_length(mlist);

	for (cur = mlist; cur != NULL; cur = cur->next) {
		MsgInfo *msginfo = (MsgInfo *)cur->data;
		gchar *msg;

		statusbar_progress_all(num++,total, 10);

		if (folder->ui_func)
			folder->ui_func(folder, item,
					folder->ui_func_data ?
					folder->ui_func_data :
					GINT_TO_POINTER(num));

		msg = folder_item_fetch_msg(item, msginfo->msgnum);
		if (!msg) {
			g_warning("Can't fetch message %d. Aborting.\n",
				  msginfo->msgnum);
			ret = -1;
			break;
		}
		g_free(msg);
	}
	
	statusbar_progress_all(0,0,0);
	statusbar_pop_all();
	procmsg_msg_list_free(mlist);

	return ret;
}

static gint folder_item_get_msg_num_by_file(FolderItem *dest, const gchar *file)
{
	static HeaderEntry hentry[] = {{"Message-ID:",  NULL, TRUE},
				       {NULL,		NULL, FALSE}};
	FILE *fp;
	MsgInfo *msginfo;
	gint msgnum = 0;
	gchar buf[BUFFSIZE];

	if ((fp = g_fopen(file, "rb")) == NULL)
		return 0;

	if ((folder_has_parent_of_type(dest, F_QUEUE)) || 
	    (folder_has_parent_of_type(dest, F_DRAFT)))
		while (fgets(buf, sizeof(buf), fp) != NULL)
			if (buf[0] == '\r' || buf[0] == '\n') break;

	procheader_get_header_fields(fp, hentry);
	if (hentry[0].body) {
    		extract_parenthesis(hentry[0].body, '<', '>');
		remove_space(hentry[0].body);
		if ((msginfo = msgcache_get_msg_by_id(dest->cache, hentry[0].body)) != NULL) {
			msgnum = msginfo->msgnum;
			procmsg_msginfo_free(msginfo);

			debug_print("found message as uid %d\n", msgnum);
		}
	}
	
	g_free(hentry[0].body);
	hentry[0].body = NULL;
	fclose(fp);

	return msgnum;
}

static void copy_msginfo_flags(MsgInfo *source, MsgInfo *dest)
{
	MsgPermFlags perm_flags = 0;
	MsgTmpFlags tmp_flags = 0;

	/* create new flags */
	if (source != NULL) {
		/* copy original flags */
		perm_flags = source->flags.perm_flags;
		tmp_flags = source->flags.tmp_flags;
	} else {
		perm_flags = dest->flags.perm_flags;
		tmp_flags = dest->flags.tmp_flags;
	}

	/* remove new, unread and deleted in special folders */
	if (folder_has_parent_of_type(dest->folder, F_OUTBOX) || 
	    folder_has_parent_of_type(dest->folder, F_QUEUE) || 
	    folder_has_parent_of_type(dest->folder, F_DRAFT) || 
	    folder_has_parent_of_type(dest->folder, F_TRASH))
		perm_flags &= ~(MSG_NEW | MSG_UNREAD | MSG_DELETED);

	/* set ignore flag of ignored parent exists */
	if (procmsg_msg_has_flagged_parent(dest, MSG_IGNORE_THREAD))
		perm_flags |= MSG_IGNORE_THREAD;

	/* Unset tmp flags that should not be copied */
	tmp_flags &= ~(MSG_MOVE | MSG_COPY | MSG_MOVE_DONE);

	/* unset flags that are set but should not */
	/* and set new flags */
	procmsg_msginfo_change_flags(dest,
				  ~dest->flags.perm_flags & perm_flags,
				  ~dest->flags.tmp_flags  & tmp_flags,
				   dest->flags.perm_flags & ~perm_flags,
				   dest->flags.tmp_flags  & ~tmp_flags);
}

static void add_msginfo_to_cache(FolderItem *item, MsgInfo *newmsginfo, MsgInfo *flagsource)
{
	/* update folder stats */
	if (MSG_IS_NEW(newmsginfo->flags))
		item->new_msgs++;
	if (MSG_IS_UNREAD(newmsginfo->flags))
		item->unread_msgs++;
	if (MSG_IS_UNREAD(newmsginfo->flags) && procmsg_msg_has_marked_parent(newmsginfo))
		item->unreadmarked_msgs++;
	if (MSG_IS_MARKED(newmsginfo->flags))
		item->marked_msgs++;
	item->total_msgs++;

	folder_item_update_freeze();

	if (!item->cache)
		folder_item_read_cache(item);

	msgcache_add_msg(item->cache, newmsginfo);
	copy_msginfo_flags(flagsource, newmsginfo);
	folder_item_update_with_msg(item,  F_ITEM_UPDATE_MSGCNT | F_ITEM_UPDATE_CONTENT | F_ITEM_UPDATE_ADDMSG, newmsginfo);
	folder_item_update_thaw();
}

static void remove_msginfo_from_cache(FolderItem *item, MsgInfo *msginfo)
{
	MsgInfoUpdate msginfo_update;

	if (!item->cache)
		folder_item_read_cache(item);

	if (MSG_IS_NEW(msginfo->flags) && !MSG_IS_IGNORE_THREAD(msginfo->flags))
		msginfo->folder->new_msgs--;
	if (MSG_IS_UNREAD(msginfo->flags) && !MSG_IS_IGNORE_THREAD(msginfo->flags))
		msginfo->folder->unread_msgs--;
	if (MSG_IS_UNREAD(msginfo->flags) && procmsg_msg_has_marked_parent(msginfo))
		msginfo->folder->unreadmarked_msgs--;
	if (MSG_IS_MARKED(msginfo->flags))
		item->marked_msgs--;

	msginfo->folder->total_msgs--;

	msginfo_update.msginfo = msginfo;
	msginfo_update.flags = MSGINFO_UPDATE_DELETED;
	hooks_invoke(MSGINFO_UPDATE_HOOKLIST, &msginfo_update);

	msgcache_remove_msg(item->cache, msginfo->msgnum);
	folder_item_update_with_msg(msginfo->folder, F_ITEM_UPDATE_MSGCNT | F_ITEM_UPDATE_CONTENT | F_ITEM_UPDATE_REMOVEMSG, msginfo);
}

gint folder_item_add_msg(FolderItem *dest, const gchar *file,
			 MsgFlags *flags, gboolean remove_source)
{
        GSList file_list;
        MsgFileInfo fileinfo;

	g_return_val_if_fail(dest != NULL, -1);
	g_return_val_if_fail(file != NULL, -1);
 
	fileinfo.msginfo = NULL;
        fileinfo.file = (gchar *)file;
        fileinfo.flags = flags;
        file_list.data = &fileinfo;
        file_list.next = NULL;

	return folder_item_add_msgs(dest, &file_list, remove_source);
}

gint folder_item_add_msgs(FolderItem *dest, GSList *file_list,
                          gboolean remove_source)
{
        Folder *folder;
        gint ret, num, lastnum = -1;
	GSList *file_cur;
	GRelation *relation;
	MsgFileInfo *fileinfo = NULL;
	gboolean folderscan = FALSE;

        g_return_val_if_fail(dest != NULL, -1);
        g_return_val_if_fail(file_list != NULL, -1);
        g_return_val_if_fail(dest->folder != NULL, -1);
	if (dest->no_select)
		return -1;

        folder = dest->folder;

	relation = g_relation_new(2);
	g_relation_index(relation, 0, g_direct_hash, g_direct_equal);

	if (folder->klass->add_msgs != NULL) {
    		ret = folder->klass->add_msgs(folder, dest, file_list, relation);
		if (ret < 0) {
			g_relation_destroy(relation);
			return ret;
		}
	} else {
		for (file_cur = file_list; file_cur != NULL; file_cur = g_slist_next(file_cur)) {
			fileinfo = (MsgFileInfo *) file_cur->data;

    			ret = folder->klass->add_msg(folder, dest, fileinfo->file, fileinfo->flags);
			if (ret < 0) {
				g_relation_destroy(relation);
				return ret;
			}
			g_relation_insert(relation, fileinfo, GINT_TO_POINTER(ret));
		}
	}

	for (file_cur = file_list; file_cur != NULL; file_cur = g_slist_next(file_cur)) {
		GTuples *tuples;

		fileinfo = (MsgFileInfo *) file_cur->data;
		tuples = g_relation_select(relation, fileinfo, 0);
		num = GPOINTER_TO_INT(g_tuples_index(tuples, 0, 1));
		g_tuples_destroy(tuples);

		if (num >= 0) {
			MsgInfo *newmsginfo;

			if (num == 0) {
				if (!folderscan) {
					folder_item_scan_full(dest, FALSE);
					folderscan = TRUE;
				}
				num = folder_item_get_msg_num_by_file(dest, fileinfo->file);
			}

			if (num > lastnum)
				lastnum = num;

			if (num >= 0 && remove_source) {
				if (g_unlink(fileinfo->file) < 0)
					FILE_OP_ERROR(fileinfo->file, "unlink");
			}

			if (num == 0)
				continue;

			if (!folderscan && 
			    ((newmsginfo = get_msginfo(dest, num)) != NULL)) {
				add_msginfo_to_cache(dest, newmsginfo, NULL);
				procmsg_msginfo_free(newmsginfo);
			} else if ((newmsginfo = msgcache_get_msg(dest->cache, num)) != NULL) {
				/* TODO: set default flags */
				procmsg_msginfo_free(newmsginfo);
			}
		}
	}

	g_relation_destroy(relation);

        return lastnum;
}
		
FolderItem *folder_item_move_recursive(FolderItem *src, FolderItem *dest) 
{
	GSList *mlist;
	FolderItem *new_item;
	FolderItem *next_item;
	GNode *srcnode;
	gchar *old_id, *new_id;

	/* move messages */
	debug_print("Moving %s to %s\n", src->path, dest->path);
	new_item = folder_create_folder(dest, src->name);
	if (new_item == NULL) {
		printf("Can't create folder\n");
		return NULL;
	}
	
	if (new_item->folder == NULL)
		new_item->folder = dest->folder;

	/* move messages */
	log_message(_("Moving %s to %s...\n"), 
			src->name, new_item->path);

	mlist = folder_item_get_msg_list(src);
	
	if (mlist != NULL) {
		folder_item_move_msgs(new_item, mlist);
		procmsg_msg_list_free(mlist);
	}
	
	/*copy prefs*/
	folder_item_prefs_copy_prefs(src, new_item);
	new_item->collapsed = src->collapsed;
	new_item->thread_collapsed = src->thread_collapsed;
	new_item->threaded  = src->threaded;
	new_item->ret_rcpt  = src->ret_rcpt;
	new_item->hide_read_msgs = src->hide_read_msgs;
	new_item->sort_key  = src->sort_key;
	new_item->sort_type = src->sort_type;

	prefs_matcher_write_config();
	
	/* recurse */
	srcnode = src->folder->node;	
	srcnode = g_node_find(srcnode, G_PRE_ORDER, G_TRAVERSE_ALL, src);
	srcnode = srcnode->children;
	while (srcnode != NULL) {
		if (srcnode && srcnode->data) {
			next_item = (FolderItem*) srcnode->data;
			srcnode = srcnode->next;
			if (folder_item_move_recursive(next_item, new_item) == NULL) {
				return NULL;
			}
		}
	}
	old_id = folder_item_get_identifier(src);
	new_id = folder_item_get_identifier(new_item);
	debug_print("updating rules : %s => %s\n", old_id, new_id);

	/* if src supports removing, otherwise only copy folder */
	if (src->folder->klass->remove_folder != NULL)	
		src->folder->klass->remove_folder(src->folder, src);
	folder_write_list();

	if (old_id != NULL && new_id != NULL) {
		prefs_filtering_rename_path(old_id, new_id);
		account_rename_path(old_id, new_id);
	}
	g_free(old_id);
	g_free(new_id);
	
	return new_item;
}

gint folder_item_move_to(FolderItem *src, FolderItem *dest, FolderItem **new_item)
{
	FolderItem *tmp = folder_item_parent(dest);
	gchar * src_identifier, * dst_identifier;
	gchar * phys_srcpath, * phys_dstpath, *tmppath;
	
	while (tmp) {
		if (tmp == src) {
			return F_MOVE_FAILED_DEST_IS_CHILD;
		}
		tmp = folder_item_parent(tmp);
	}
	
	tmp = folder_item_parent(src);
	
	src_identifier = folder_item_get_identifier(src);
	dst_identifier = folder_item_get_identifier(dest);
	
	if(dst_identifier == NULL && dest->folder && folder_item_parent(dest) == NULL) {
		/* dest can be a root folder */
		dst_identifier = folder_get_identifier(dest->folder);
	}
	if (src_identifier == NULL || dst_identifier == NULL) {
		debug_print("Can't get identifiers\n");
		return F_MOVE_FAILED;
	}

	if (src->folder != dest->folder) {
		return F_MOVE_FAILED_DEST_OUTSIDE_MAILBOX;
	}

	phys_srcpath = folder_item_get_path(src);
	tmppath = folder_item_get_path(dest);
	phys_dstpath = g_strconcat(tmppath,
		       G_DIR_SEPARATOR_S,
		       g_path_get_basename(phys_srcpath),
		       NULL);
	g_free(tmppath);

	if (folder_item_parent(src) == dest || src == dest) {
		g_free(src_identifier);
		g_free(dst_identifier);
		g_free(phys_srcpath);
		g_free(phys_dstpath);
		return F_MOVE_FAILED_DEST_IS_PARENT;
	}
	debug_print("moving \"%s\" to \"%s\"\n", phys_srcpath, phys_dstpath);
	if ((tmp = folder_item_move_recursive(src, dest)) == NULL) {
		return F_MOVE_FAILED;
	}
	
	g_free(src_identifier);
	g_free(dst_identifier);
	g_free(phys_srcpath);
	g_free(phys_dstpath);

	*new_item = tmp;

	return F_MOVE_OK;
}

/**
 * Copy a list of message to a new folder and remove
 * source messages if wanted
 */
static gint do_copy_msgs(FolderItem *dest, GSList *msglist, gboolean remove_source)
{
	Folder *folder;
	GSList *l;
	gint num, lastnum = -1;
	gboolean folderscan = FALSE;
	GRelation *relation;
	GSList *not_moved = NULL;
	gint total = 0, curmsg = 0;
	MsgInfo *msginfo = NULL;

	g_return_val_if_fail(dest != NULL, -1);
	g_return_val_if_fail(msglist != NULL, -1);

	folder = dest->folder;

	g_return_val_if_fail(folder->klass->copy_msg != NULL, -1);
	if (dest->no_select)
		return -1;

	msginfo = (MsgInfo *)msglist->data;
	
	if (!msginfo)
		return -1;
	
	if (!MSG_IS_QUEUED(msginfo->flags) && 
	    MSG_IS_DRAFT(msginfo->flags) && 
	    folder_has_parent_of_type(dest, F_QUEUE)) {
		GSList *cur = msglist;
		gboolean queue_err = FALSE;
		for (; cur; cur = cur->next) {
			Compose *compose = NULL;
			FolderItem *queue = dest;
			int val = 0;
			
			msginfo = (MsgInfo *)cur->data;
			compose = compose_reedit(msginfo, TRUE);
			if (compose == NULL) {
				queue_err = TRUE;
				continue;
			}
			val = compose_queue(compose, NULL, &queue, NULL,
					FALSE);
			if (val < 0) {
				queue_err = TRUE;
			} else if (remove_source) {
				folder_item_remove_msg(msginfo->folder, msginfo->msgnum);
			}
			if (val == 0)
				compose_close(compose);
		}
		return queue_err ? -1:0;
	}

	relation = g_relation_new(2);
	g_relation_index(relation, 0, g_direct_hash, g_direct_equal);
	g_relation_index(relation, 1, g_direct_hash, g_direct_equal);

	for (l = msglist ; l != NULL ; l = g_slist_next(l)) {
		MsgInfo * msginfo = (MsgInfo *) l->data;

		if (msginfo->planned_download != 0) {
			int old_planned = msginfo->planned_download;
			partial_unmark(msginfo);
			/* little hack to reenable after */
			msginfo->planned_download = old_planned;
		}
	}

	/* 
	 * Copy messages to destination folder and 
	 * store new message numbers in newmsgnums
	 */
	if (folder->klass->copy_msgs != NULL) {
		if (folder->klass->copy_msgs(folder, dest, msglist, relation) < 0) {
			g_relation_destroy(relation);
			return -1;
		}
	} else {
		MsgInfo * msginfo;
		l = msglist;

		/* immediately stop if src and dest folders are identical */
		if (l != NULL) {
			msginfo = (MsgInfo *) l->data;
			if (msginfo != NULL && msginfo->folder == dest) {
				g_relation_destroy(relation);
				return -1;
			}
		}

		for (; l != NULL ; l = g_slist_next(l)) {
			msginfo = (MsgInfo *) l->data;

			num = folder->klass->copy_msg(folder, dest, msginfo);
			if (num > 0)
				g_relation_insert(relation, msginfo, GINT_TO_POINTER(num));
			else
				not_moved = g_slist_prepend(not_moved, msginfo);
		}
	}

	if (remove_source) {
		MsgInfo *msginfo = (MsgInfo *) msglist->data;
		FolderItem *item = msginfo->folder;
		/*
		 * Remove source messages from their folders if
		 * copying was successfull and update folder
		 * message counts
		 */
		if (not_moved == NULL && item->folder->klass->remove_msgs) {
			item->folder->klass->remove_msgs(item->folder,
					    		        msginfo->folder,
						    		msglist,
								relation);
		}
		for (l = msglist; l != NULL; l = g_slist_next(l)) {
            	        GTuples *tuples;
			msginfo = (MsgInfo *) l->data;
			item = msginfo->folder;

            		tuples = g_relation_select(relation, msginfo, 0);
            	        num = GPOINTER_TO_INT(g_tuples_index(tuples, 0, 1));
            		g_tuples_destroy(tuples);

			if (g_slist_find(not_moved, msginfo))
				continue;

			if ((num >= 0) && (item->folder->klass->remove_msg != NULL)) {
				if (!item->folder->klass->remove_msgs)
					item->folder->klass->remove_msg(item->folder,
					    		        msginfo->folder,
						    		msginfo->msgnum);
				remove_msginfo_from_cache(item, msginfo);
			}
		}
	}

	/* Read cache for dest folder */
	if (!dest->cache) folder_item_read_cache(dest);

	/* 
	 * Fetch new MsgInfos for new messages in dest folder,
	 * add them to the msgcache and update folder message counts
	 */
	if (g_relation_count(relation, GINT_TO_POINTER(0), 1) > 0) {
		folder_item_scan_full(dest, FALSE);
		folderscan = TRUE;
	}

	statusbar_print_all(_("Updating cache for %s..."), dest->path ? dest->path : "(null)");
	total = g_slist_length(msglist);
	for (l = msglist; l != NULL; l = g_slist_next(l)) {
		MsgInfo *msginfo = (MsgInfo *) l->data;
                GTuples *tuples;

                tuples = g_relation_select(relation, msginfo, 0);
                num = GPOINTER_TO_INT(g_tuples_index(tuples, 0, 1));
                g_tuples_destroy(tuples);

		statusbar_progress_all(curmsg++,total, 100);
		if (curmsg % 100 == 0)
			GTK_EVENTS_FLUSH();

		if (num >= 0) {
			MsgInfo *newmsginfo = NULL;

			if (folderscan) {
				if (msginfo->msgid != NULL) {
					newmsginfo = folder_item_get_msginfo_by_msgid(dest, msginfo->msgid);
					if (newmsginfo != NULL) {
						copy_msginfo_flags(msginfo, newmsginfo);
						num = newmsginfo->msgnum;
					}
				}
			} else {
				newmsginfo = get_msginfo(dest, num);
				if (newmsginfo != NULL) {
					add_msginfo_to_cache(dest, newmsginfo, msginfo);
				}
			}

			if (msginfo->planned_download 
			    == POP3_PARTIAL_DLOAD_DELE) {
				partial_mark_for_delete(newmsginfo);
			}
			if (msginfo->planned_download 
			    == POP3_PARTIAL_DLOAD_DLOAD) {
				partial_mark_for_download(newmsginfo);
			}
			if (!MSG_IS_POSTFILTERED (msginfo->flags)) {
			    procmsg_msginfo_set_flags (   msginfo, MSG_POSTFILTERED, 0);
			    procmsg_msginfo_set_flags (newmsginfo, MSG_POSTFILTERED, 0);
			    hooks_invoke (MAIL_POSTFILTERING_HOOKLIST, newmsginfo);
			}
			procmsg_msginfo_free(newmsginfo);


			if (num > lastnum)
				lastnum = num;
		}
	}
	statusbar_progress_all(0,0,0);
	statusbar_pop_all();

	g_relation_destroy(relation);
	if (not_moved != NULL) {
		g_slist_free(not_moved);
		return -1;
	} else
		return lastnum;
}

/**
 * Move a message to a new folder.
 *
 * \param dest Destination folder
 * \param msginfo The message
 */
gint folder_item_move_msg(FolderItem *dest, MsgInfo *msginfo)
{
	GSList list;

	g_return_val_if_fail(dest != NULL, -1);
	g_return_val_if_fail(msginfo != NULL, -1);

	list.data = msginfo;
	list.next = NULL;

	return do_copy_msgs(dest, &list, TRUE);
}

/**
 * Move a list of messages to a new folder.
 *
 * \param dest Destination folder
 * \param msglist List of messages
 */
gint folder_item_move_msgs(FolderItem *dest, GSList *msglist)
{
	gint result = -1;
	g_return_val_if_fail(dest != NULL, -1);
	g_return_val_if_fail(msglist != NULL, -1);
	inc_lock();
	result = do_copy_msgs(dest, msglist, TRUE);
	inc_unlock();
	return result;
}

/**
 * Copy a message to a new folder.
 *
 * \param dest Destination folder
 * \param msginfo The message
 */
gint folder_item_copy_msg(FolderItem *dest, MsgInfo *msginfo)
{
	GSList list;

	g_return_val_if_fail(dest != NULL, -1);
	g_return_val_if_fail(msginfo != NULL, -1);
    
	list.data = msginfo;
	list.next = NULL;

	return do_copy_msgs(dest, &list, FALSE);
}

/**
 * Copy a list of messages to a new folder.
 *
 * \param dest Destination folder
 * \param msglist List of messages
 */
gint folder_item_copy_msgs(FolderItem *dest, GSList *msglist)
{
	gint result;
	g_return_val_if_fail(dest != NULL, -1);
	g_return_val_if_fail(msglist != NULL, -1);

	inc_lock();
	result = do_copy_msgs(dest, msglist, FALSE);
	inc_unlock();
	
	return result;
}

gint folder_item_remove_msg(FolderItem *item, gint num)
{
	Folder *folder;
	gint ret;
	MsgInfo *msginfo;

	g_return_val_if_fail(item != NULL, -1);
	folder = item->folder;
	g_return_val_if_fail(folder->klass->remove_msg != NULL, -1);
	if (item->no_select)
		return -1;

	if (!item->cache) folder_item_read_cache(item);

	ret = folder->klass->remove_msg(folder, item, num);

	msginfo = msgcache_get_msg(item->cache, num);
	if (msginfo != NULL) {
		remove_msginfo_from_cache(item, msginfo);
		procmsg_msginfo_free(msginfo);
	}

	return ret;
}

gint folder_item_remove_msgs(FolderItem *item, GSList *msglist)
{
	Folder *folder;
	gint ret = 0;

	g_return_val_if_fail(item != NULL, -1);
	folder = item->folder;
	g_return_val_if_fail(folder != NULL, -1);
	if (item->no_select)
		return -1;
	inc_lock();
	if (!item->cache) folder_item_read_cache(item);

	folder_item_update_freeze();
	if (item->folder->klass->remove_msgs) {
		ret = item->folder->klass->remove_msgs(item->folder,
					    		item,
						    	msglist,
							NULL);
	}
	while (ret == 0 && msglist != NULL) {
		MsgInfo *msginfo = (MsgInfo *)msglist->data;
		if (MSG_IS_LOCKED(msginfo->flags)) {
			msglist = msglist->next;
			continue;
		}
		if (!item->folder->klass->remove_msgs)
			ret = folder_item_remove_msg(item, msginfo->msgnum);
		if (ret != 0) break;
		msgcache_remove_msg(item->cache, msginfo->msgnum);
		msglist = msglist->next;
	}
	folder_item_scan_full(item, FALSE);
	folder_item_update_thaw();
	inc_unlock();
	return ret;
}

gint folder_item_remove_all_msg(FolderItem *item)
{
	Folder *folder;
	gint result;

	g_return_val_if_fail(item != NULL, -1);
	if (item->no_select)
		return -1;

	folder = item->folder;

	inc_lock();
	if (folder->klass->remove_all_msg != NULL) {
		result = folder->klass->remove_all_msg(folder, item);

		if (result == 0) {
			folder_item_free_cache(item, TRUE);
			item->cache = msgcache_new();
		}
	} else {
		MsgInfoList *msglist;

		msglist = folder_item_get_msg_list(item);
		result = folder_item_remove_msgs(item, msglist);
		procmsg_msg_list_free(msglist);
	}

	if (result == 0) {
		item->new_msgs = 0;
		item->unread_msgs = 0;
		item->unreadmarked_msgs = 0;
		item->marked_msgs = 0;
		item->total_msgs = 0;
		folder_item_update(item, F_ITEM_UPDATE_MSGCNT | F_ITEM_UPDATE_CONTENT);
	}

	inc_unlock();
	return result;
}

void folder_item_change_msg_flags(FolderItem *item, MsgInfo *msginfo, MsgPermFlags newflags)
{
	g_return_if_fail(item != NULL);
	g_return_if_fail(msginfo != NULL);
	if (item->no_select)
		return;
	
	if (item->folder->klass->change_flags != NULL) {
		item->folder->klass->change_flags(item->folder, item, msginfo, newflags);
	} else {
		msginfo->flags.perm_flags = newflags;
	}
}

gboolean folder_item_is_msg_changed(FolderItem *item, MsgInfo *msginfo)
{
	Folder *folder;

	g_return_val_if_fail(item != NULL, FALSE);
	if (item->no_select)
		return FALSE;

	folder = item->folder;

	g_return_val_if_fail(folder->klass->is_msg_changed != NULL, -1);

	return folder->klass->is_msg_changed(folder, item, msginfo);
}

gchar *folder_item_get_cache_file(FolderItem *item)
{
	gchar *path;
	gchar *file;

	g_return_val_if_fail(item != NULL, NULL);
	g_return_val_if_fail(item->path != NULL, NULL);

	path = folder_item_get_path(item);
	g_return_val_if_fail(path != NULL, NULL);
	if (!is_dir_exist(path))
		make_dir_hier(path);
	file = g_strconcat(path, G_DIR_SEPARATOR_S, CACHE_FILE, NULL);
	g_free(path);

	return file;
}

gchar *folder_item_get_mark_file(FolderItem *item)
{
	gchar *path;
	gchar *file;

	g_return_val_if_fail(item != NULL, NULL);
	g_return_val_if_fail(item->path != NULL, NULL);

	path = folder_item_get_path(item);
	g_return_val_if_fail(path != NULL, NULL);
	if (!is_dir_exist(path))
		make_dir_hier(path);
	file = g_strconcat(path, G_DIR_SEPARATOR_S, MARK_FILE, NULL);
	g_free(path);

	return file;
}

static gpointer xml_to_folder_item(gpointer nodedata, gpointer data)
{
	XMLNode *xmlnode = (XMLNode *) nodedata;
	Folder *folder = (Folder *) data;
	FolderItem *item;

	g_return_val_if_fail(xmlnode != NULL, NULL);
	g_return_val_if_fail(folder != NULL, NULL);

	if (strcmp2(xmlnode->tag->tag, "folderitem") != 0) {
		g_warning("tag name != \"folderitem\"\n");
		return NULL;
	}

	item = folder_item_new(folder, "", "");
	if (folder->klass->item_set_xml != NULL)
		folder->klass->item_set_xml(folder, item, xmlnode->tag);
	else
		folder_item_set_xml(folder, item, xmlnode->tag);

	item->folder = folder;

	switch (item->stype) {
	case F_INBOX:  folder->inbox  = item; break;
	case F_OUTBOX: folder->outbox = item; break;
	case F_DRAFT:  folder->draft  = item; break;
	case F_QUEUE:  folder->queue  = item; break;
	case F_TRASH:  folder->trash  = item; break;
	default:       break;
	}
	folder_item_prefs_read_config(item);

	return item;
}

static gboolean folder_item_set_node(GNode *node, gpointer data)
{
	FolderItem *item = (FolderItem *) node->data;
	item->node = node;

	return FALSE;
}

static Folder *folder_get_from_xml(GNode *node)
{
	Folder *folder;
	XMLNode *xmlnode;
	GList *list;
	FolderClass *klass = NULL;
	GNode *cur;

	g_return_val_if_fail(node->data != NULL, NULL);

	xmlnode = node->data;
	if (strcmp2(xmlnode->tag->tag, "folder") != 0) {
		g_warning("tag name != \"folder\"\n");
		return NULL;
	}
	list = xmlnode->tag->attr;
	for (; list != NULL; list = list->next) {
		XMLAttr *attr = list->data;

		if (!attr || !attr->name || !attr->value) continue;
		if (!strcmp(attr->name, "type"))
			klass = folder_get_class_from_string(attr->value);
	}
	if (klass == NULL)
		return NULL;

	folder = folder_new(klass, "", "");
	g_return_val_if_fail(folder != NULL, NULL);

	if (klass->set_xml)
		klass->set_xml(folder, xmlnode->tag);
	else
		folder_set_xml(folder, xmlnode->tag);

	cur = node->children;
	while (cur != NULL) {
		GNode *itemnode;

		itemnode = g_node_map(cur, xml_to_folder_item, (gpointer) folder);
		g_node_append(folder->node, itemnode);
		cur = cur->next;
	}
	g_node_traverse(folder->node, G_IN_ORDER, G_TRAVERSE_ALL, -1, folder_item_set_node, NULL);
	
	return folder;
}

static gchar *folder_get_list_path(void)
{
	static gchar *filename = NULL;

	if (!filename)
		filename =  g_strconcat(get_rc_dir(), G_DIR_SEPARATOR_S,
					FOLDER_LIST, NULL);

	return filename;
}

#define PUT_ESCAPE_STR(fp, attr, str)			\
{							\
	fputs(" " attr "=\"", fp);			\
	xml_file_put_escape_str(fp, str);		\
	fputs("\"", fp);				\
}

static gpointer folder_item_to_xml(gpointer nodedata, gpointer data)
{
	FolderItem *item = (FolderItem *) nodedata;
	XMLTag *tag;

	g_return_val_if_fail(item != NULL, NULL);

	if (item->folder->klass->item_get_xml != NULL)
		tag = item->folder->klass->item_get_xml(item->folder, item);
	else
		tag = folder_item_get_xml(item->folder, item);

	return xml_node_new(tag, NULL);
}

static GNode *folder_get_xml_node(Folder *folder)
{
	GNode *node;
	XMLNode *xmlnode;
	XMLTag *tag;

	g_return_val_if_fail(folder != NULL, NULL);

	if (folder->klass->get_xml != NULL)
		tag = folder->klass->get_xml(folder);
	else
		tag = folder_get_xml(folder);

	xml_tag_add_attr(tag, xml_attr_new("type", folder->klass->idstr));

	xmlnode = xml_node_new(tag, NULL);

	node = g_node_new(xmlnode);
	
	g_return_val_if_fail (folder->node != NULL, NULL);
	
	if (folder->node->children) {
		GNode *cur;

		cur = folder->node->children;
		while (cur) {
			GNode *xmlnode;

			xmlnode = g_node_map(cur, folder_item_to_xml, (gpointer) folder);
			g_node_append(node, xmlnode);
			cur = cur->next;
		}
	}

	return node;
}

static void folder_update_op_count_rec(GNode *node)
{
	FolderItem *fitem = FOLDER_ITEM(node->data);

	if (g_node_depth(node) > 0) {
		if (fitem->op_count > 0) {
			fitem->op_count = 0;
			folder_item_update(fitem, F_ITEM_UPDATE_MSGCNT);
		}
		if (node->children) {
			GNode *child;

			child = node->children;
			while (child) {
				GNode *cur;

				cur = child;
				child = cur->next;
				folder_update_op_count_rec(cur);
			}
		}
	}
}

void folder_update_op_count(void) 
{
	GList *cur;
	Folder *folder;

	for (cur = folder_list; cur != NULL; cur = cur->next) {
		folder = cur->data;
		folder_update_op_count_rec(folder->node);
	}
}

typedef struct _type_str {
	gchar * str;
	gint type;
} type_str;


/*
static gchar * folder_item_get_tree_identifier(FolderItem * item)
{
	if (item->parent != NULL) {
		gchar * path;
		gchar * id;

		path = folder_item_get_tree_identifier(item->parent);
		if (path == NULL)
			return NULL;

		id = g_strconcat(path, "/", item->name, NULL);
		g_free(path);

		return id;
	}
	else {
		return g_strconcat("/", item->name, NULL);
	}
}
*/

/* CLAWS: temporary local folder for filtering */
#define TEMP_FOLDER "TEMP_FOLDER"
#define PROCESSING_FOLDER_ITEM "processing"	

static FolderItem *processing_folder_item;

static void folder_create_processing_folder(void)
{
	Folder *processing_folder;
	gchar      *tmpname;

	if ((processing_folder = folder_find_from_name(TEMP_FOLDER, mh_get_class())) == NULL) {
		gchar *tmppath;

		tmppath =
		    g_strconcat(get_rc_dir(), G_DIR_SEPARATOR_S,
				"tempfolder", NULL);
		processing_folder =
		    folder_new(mh_get_class(), TEMP_FOLDER, tmppath);
		g_free(tmppath);
	}
	g_assert(processing_folder != NULL);

	debug_print("tmpparentroot %s\n", LOCAL_FOLDER(processing_folder)->rootpath);
        /* FIXME: [W32] The code below does not correctly merge
           relative filenames; there should be a function to handle
           this.  */
	if (!is_relative_filename(LOCAL_FOLDER(processing_folder)->rootpath))
		tmpname = g_strconcat(LOCAL_FOLDER(processing_folder)->rootpath,
				      G_DIR_SEPARATOR_S, PROCESSING_FOLDER_ITEM,
				      NULL);
	else
		tmpname = g_strconcat(get_home_dir(), G_DIR_SEPARATOR_S,
				      LOCAL_FOLDER(processing_folder)->rootpath,
				      G_DIR_SEPARATOR_S, PROCESSING_FOLDER_ITEM,
				      NULL);

	if (!is_dir_exist(tmpname)) {
		debug_print("*TMP* creating %s\n", tmpname);
		processing_folder_item = processing_folder->klass->create_folder(processing_folder,
								   	         processing_folder->node->data,
										 PROCESSING_FOLDER_ITEM);
	} else {
		debug_print("*TMP* already created\n");
		processing_folder_item = folder_item_new(processing_folder, PROCESSING_FOLDER_ITEM, PROCESSING_FOLDER_ITEM);
		g_assert(processing_folder_item);
		folder_item_append(processing_folder->node->data, processing_folder_item);
	}
	g_assert(processing_folder_item != NULL);
	g_free(tmpname);
}

FolderItem *folder_get_default_processing(void)
{
	if (!processing_folder_item) {
		folder_create_processing_folder();
	}
	return processing_folder_item;
}

/* folder_persist_prefs_new() - return hash table with persistent
 * settings (and folder name as key). 
 * (note that in claws other options are in the folder_item_prefs_RC
 * file, so those don't need to be included in PersistPref yet) 
 */
GHashTable *folder_persist_prefs_new(Folder *folder)
{
	GHashTable *pptable;

	g_return_val_if_fail(folder, NULL);
	pptable = g_hash_table_new(g_str_hash, g_str_equal);
	folder_get_persist_prefs_recursive(folder->node, pptable);
	return pptable;
}

void folder_persist_prefs_free(GHashTable *pptable)
{
	g_return_if_fail(pptable);
	g_hash_table_foreach_remove(pptable, persist_prefs_free, NULL);
	g_hash_table_destroy(pptable);
}

const PersistPrefs *folder_get_persist_prefs(GHashTable *pptable, const char *name)
{
	if (pptable == NULL || name == NULL) return NULL;
	return g_hash_table_lookup(pptable, name);
}

void folder_item_restore_persist_prefs(FolderItem *item, GHashTable *pptable)
{
	const PersistPrefs *pp;
	gchar *id = folder_item_get_identifier(item);

	pp = folder_get_persist_prefs(pptable, id); 
	g_free(id);

	if (!pp) return;

	/* CLAWS: since not all folder properties have been migrated to 
	 * folderlist.xml, we need to call the old stuff first before
	 * setting things that apply both to Main and Claws. */
	folder_item_prefs_read_config(item); 

	item->collapsed = pp->collapsed;
	item->thread_collapsed = pp->thread_collapsed;
	item->threaded  = pp->threaded;
	item->ret_rcpt  = pp->ret_rcpt;
	item->hide_read_msgs = pp->hide_read_msgs;
	item->sort_key  = pp->sort_key;
	item->sort_type = pp->sort_type;
}

static void folder_get_persist_prefs_recursive(GNode *node, GHashTable *pptable)
{
	FolderItem *item = FOLDER_ITEM(node->data);
	PersistPrefs *pp;
	GNode *child, *cur;
	gchar *id;

	g_return_if_fail(node != NULL);
	g_return_if_fail(item != NULL);

	/* NOTE: item->path == NULL means top level folder; not interesting
	 * to store preferences of that one.  */
	if (item->path) {
		id = folder_item_get_identifier(item);
		pp = g_new0(PersistPrefs, 1);
		g_return_if_fail(pp != NULL);
		pp->collapsed = item->collapsed;
		pp->thread_collapsed = item->thread_collapsed;
		pp->threaded  = item->threaded;
		pp->ret_rcpt  = item->ret_rcpt;	
		pp->hide_read_msgs = item->hide_read_msgs;
		pp->sort_key  = item->sort_key;
		pp->sort_type = item->sort_type;
		g_hash_table_insert(pptable, id, pp);
	}

	if (node->children) {
		child = node->children;
		while (child) {
			cur = child;
			child = cur->next;
			folder_get_persist_prefs_recursive(cur, pptable);
		}
	}	
}

static gboolean persist_prefs_free(gpointer key, gpointer val, gpointer data)
{
	g_free(key);
	g_free(val);
	return TRUE;	
}

void folder_item_apply_processing(FolderItem *item)
{
	GSList *processing_list;
	GSList *mlist, *cur;
	guint total = 0, curmsg = 0;
	gint last_apply_per_account;

	g_return_if_fail(item != NULL);

	if (item->no_select)
	       return;

	processing_list = item->prefs->processing;

	if (!pre_global_processing && !processing_list
	&&  !post_global_processing)
		return;

	debug_print("processing %s\n", item->name);
	folder_item_update_freeze();

	mlist = folder_item_get_msg_list(item);
	total = g_slist_length(mlist);
	statusbar_print_all(_("Processing messages..."));

	last_apply_per_account = prefs_common.apply_per_account_filtering_rules;
	prefs_common.apply_per_account_filtering_rules = FILTERING_ACCOUNT_RULES_SKIP;

	for (cur = mlist ; cur != NULL ; cur = cur->next) {
		MsgInfo * msginfo;

		msginfo = (MsgInfo *) cur->data;
                
                /* reset parameters that can be modified by processing */
                msginfo->hidden = 0;
                msginfo->score = 0;

		statusbar_progress_all(curmsg++,total, 10);

                /* apply pre global rules */
		filter_message_by_msginfo(pre_global_processing, msginfo, NULL);
		
                /* apply rules of the folder */
		filter_message_by_msginfo(processing_list, msginfo, NULL);

                /* apply post global rules */
		filter_message_by_msginfo(post_global_processing, msginfo, NULL);
                
	}
	prefs_common.apply_per_account_filtering_rules = last_apply_per_account;

	if (pre_global_processing || processing_list
	    || post_global_processing)
		filtering_move_and_copy_msgs(mlist);
	for (cur = mlist ; cur != NULL ; cur = cur->next) {
		MsgInfo * msginfo = (MsgInfo *)cur->data;
		procmsg_msginfo_free(msginfo);
	}
	g_slist_free(mlist);
	
	statusbar_progress_all(0,0,0);
	statusbar_pop_all();

	folder_item_update_thaw();
}

/*
 *  functions for handling FolderItem content changes
 */
static gint folder_item_update_freeze_cnt = 0;

static void folder_item_update_with_msg(FolderItem *item, FolderItemUpdateFlags update_flags, MsgInfo *msg)
{
	if (folder_item_update_freeze_cnt == 0 /* || (msg != NULL && item->opened) */) {
		FolderItemUpdateData source;
	
		source.item = item;
		source.update_flags = update_flags;
		source.msg = msg;
    		hooks_invoke(FOLDER_ITEM_UPDATE_HOOKLIST, &source);
	} else {
		item->update_flags |= update_flags & ~(F_ITEM_UPDATE_ADDMSG | F_ITEM_UPDATE_REMOVEMSG);
	}
}

/**
 * Notify the folder system about changes to a folder. If the
 * update system is not frozen the FOLDER_ITEM_UPDATE_HOOKLIST will
 * be invoked, otherwise the changes will be remebered until
 * the folder system is thawed.
 *
 * \param item The FolderItem that was changed
 * \param update_flags Type of changed that was made
 */
void folder_item_update(FolderItem *item, FolderItemUpdateFlags update_flags)
{
	folder_item_update_with_msg(item, update_flags, NULL);
}

void folder_item_update_recursive(FolderItem *item, FolderItemUpdateFlags update_flags)
{
	GNode *node = item->folder->node;	

	node = g_node_find(node, G_PRE_ORDER, G_TRAVERSE_ALL, item);
	node = node->children;

	folder_item_update(item, update_flags);
	while (node != NULL) {
		if (node && node->data) {
			FolderItem *next_item = (FolderItem*) node->data;

			folder_item_update(next_item, update_flags);
		}
		node = node->next;
	}
}

void folder_item_update_freeze(void)
{
	folder_item_update_freeze_cnt++;
}

static void folder_item_update_func(FolderItem *item, gpointer data)
{
	FolderItemUpdateData source;
    
	if (item->update_flags) {
		source.item = item;
		source.update_flags = item->update_flags;
		source.msg = NULL;
		hooks_invoke(FOLDER_ITEM_UPDATE_HOOKLIST, &source);				
		item->update_flags = 0;
	}
}

void folder_item_update_thaw(void)
{
	if (folder_item_update_freeze_cnt > 0)
		folder_item_update_freeze_cnt--;
	if (folder_item_update_freeze_cnt == 0) {
		/* Update all folders */
		folder_func_to_all_folders(folder_item_update_func, NULL);
	}
}

static void folder_item_synchronise_func(FolderItem *item, gpointer data)
{
	Folder *folder = (Folder *)data;
	if (folder == NULL || item->folder == folder) {
		if(item->prefs->offlinesync && item->folder->klass->synchronise)
			item->folder->klass->synchronise(item);
	}
}

void folder_synchronise(Folder *folder)
{
	folder_func_to_all_folders(folder_item_synchronise_func, folder);
}

typedef struct _WantSyncData {
	Folder *folder;
	gboolean want_sync;
} WantSyncData;

static void folder_item_want_synchronise_func(FolderItem *item, gpointer data)
{
	WantSyncData *want_sync_data = (WantSyncData *)data;
	
	if (want_sync_data->folder == NULL || item->folder == want_sync_data->folder) {
		if (item->prefs->offlinesync && item->folder->klass->synchronise)
			want_sync_data->want_sync |= TRUE;
	}
}

gboolean folder_want_synchronise(Folder *folder)
{
	WantSyncData *want_sync_data = g_new0(WantSyncData, 1);
	gboolean result;
	want_sync_data->folder = folder;
	want_sync_data->want_sync = FALSE;
	
	folder_func_to_all_folders(folder_item_want_synchronise_func, want_sync_data);
	result = want_sync_data->want_sync;
	g_free(want_sync_data);
	debug_print("Folder %s wants sync: %d\n", folder->name, result);
	return result;
}

void folder_item_set_batch (FolderItem *item, gboolean batch)
{
	if (item->folder->klass->set_batch) {
		item->folder->klass->set_batch(item->folder, item, batch);
	}
}

gboolean folder_has_parent_of_type(FolderItem *item, 
					  SpecialFolderItemType type) 
{
	FolderItem *cur = item;

	/* if we already know it, make it short */
	if (item->parent_stype != -1) {
		return (item->parent_stype == type);
	}
	
	/* if we don't, find the type from the first possible parent,
	 * and set our parent type to be faster next time */
	while (cur) {
		if (cur->stype == type || cur->parent_stype == type) {
			item->parent_stype = type;
			return TRUE;
		}
		cur = folder_item_parent(cur);
	}
	
	/* if we didn't match what was asked, we didn't return. If our
	 * parent type is unknown, we may as well find it now to be faster
	 * later. */
	if (item->parent_stype == -1) {
		cur = item;
		while (cur) {
			/* here's an exception: Inbox subfolders are normal. */
			if (item->parent_stype == -1 && cur->stype == F_INBOX 
			&& item != cur) {
				item->parent_stype = F_NORMAL;
				break;
			}
			/* ah, we know this parent's parent's type, we may as 
			 * well copy it instead of going up the full way */
			if (cur->parent_stype != -1) {
				item->parent_stype = cur->parent_stype;
				break;
			}
			/* we found a parent that has a special type. That's 
			 * our parent type. */
			if (cur->stype != F_NORMAL) {
				cur->parent_stype = cur->stype;
				item->parent_stype = cur->stype;
				break;
			}
			/* if we didn't find anything, go up once more */
			cur = folder_item_parent(cur);
		}
		/* as we still didn't find anything, our parents must all be 
		 * normal. */
		if (item->parent_stype == -1) {
			item->parent_stype = F_NORMAL;
		}
	}
	return FALSE;
}

gboolean folder_subscribe (const gchar *uri)
{
	GList *cur;
	for (cur = folder_get_list(); cur != NULL; cur = g_list_next(cur)) {
		Folder *folder = (Folder *) cur->data;

		if (folder->klass->subscribe
		&&  folder->klass->subscribe(folder, uri)) {
			return TRUE;
		}
	}
	return FALSE;

}
#undef PUT_ESCAPE_STR
