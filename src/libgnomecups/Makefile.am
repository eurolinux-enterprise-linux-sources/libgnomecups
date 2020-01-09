INCLUDES = 						\
	-I$(top_srcdir) 				\
	-I$(top_builddir) 				\
	$(WARN_CFLAGS)                                  \
	$(CUPS_CFLAGS)                                  \
	$(LIBGNOMECUPS_CFLAGS)                          \
	-DDATADIR=\""$(datadir)"\"

lib_LTLIBRARIES=libgnomecups-1.0.la
libgnomecups_1_0_la_LIBADD = \
	$(LIBGNOMECUPS_LIBS) \
	$(CUPS_LIBS) 
libgnomecups_1_0_la_LDFLAGS = $(LT_VERSION_INFO)

libgnomecups_1_0_la_SOURCES = \
	gnome-cups-i18n.c \
	gnome-cups-i18n.h \
	gnome-cups-init.c \
	gnome-cups-printer.c \
	gnome-cups-queue.c \
	gnome-cups-request.c \
	gnome-cups-util.c \
	util.c \
	util.h

libgnomecupsincludedir=$(includedir)/libgnomecups-1/libgnomecups

libgnomecupsinclude_HEADERS = \
	gnome-cups-init.h \
	gnome-cups-printer.h \
	gnome-cups-queue.h \
	gnome-cups-request.h \
	gnome-cups-util.h

#noinst_PROGRAMS=test-cups
#test_cups_SOURCES=test-cups.c
#test_cups_LDFLAGS=libgnomecupsui-1.0.la

EXTRA_DIST=$(libgnomecupsinclude_HEADERS)

