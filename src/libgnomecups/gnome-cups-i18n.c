#include <config.h>
#include <glib/gmacros.h>
#include "gnome-cups-i18n.h"

#ifdef ENABLE_NLS
#include <libintl.h>

G_CONST_RETURN char *
_libgnomecups_gettext (const char *str)
{
	static gboolean initialized = FALSE;
	
	if (!initialized) {
		bindtextdomain (GETTEXT_PACKAGE, GNOMELOCALEDIR);
#ifdef HAVE_BIND_TEXTDOMAIN_CODESET
		bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
#endif
		initialized = TRUE;
	}
	
	return dgettext (GETTEXT_PACKAGE, str);
}

#endif
