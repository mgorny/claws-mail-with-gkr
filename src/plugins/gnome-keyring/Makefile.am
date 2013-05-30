plugindir = $(pkglibdir)/plugins

if BUILD_GNOME_KEYRING_PLUGIN
plugin_LTLIBRARIES = gnome-keyring.la
endif

gnome_keyring_la_SOURCES = \
	gnome_keyring_plugin.c \
	gkr-if.c gkr-if.h

gnome_keyring_la_LDFLAGS = \
	-avoid-version -module

if CYGWIN
cygwin_export_lib = -L$(top_builddir)/src -lclaws-mail
else
cygwin_export_lib = 
endif

gnome_keyring_la_LIBADD = $(cygwin_export_lib) \
	$(GNOME_KEYRING_LIBS)


INCLUDES = \
	-I$(top_srcdir)/src \
	-I$(top_builddir)/src/common \
	-I$(top_srcdir)/src/common

AM_CPPFLAGS = \
	$(GLIB_CFLAGS) \
	$(GNOME_KEYRING_CFLAGS)
