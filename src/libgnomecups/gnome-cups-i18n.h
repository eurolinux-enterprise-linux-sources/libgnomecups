#ifndef GNOME_CUPS_I18N_H
#define GNOME_CUPS_I18N_H

#ifdef ENABLE_NLS

#include <glib.h>

G_CONST_RETURN char *_libgnomecups_gettext (const char *str) G_GNUC_FORMAT(1);
#include <libintl.h>
#define _(String) _libgnomecups_gettext(String)
#ifdef gettext_noop
#define N_(String) gettext_noop(String)
#else
#define N_(String) (String)
#endif

#else /* NLS is disabled */

#define _(String) (String)
#define N_(String) (String)
#define textdomain(String) (String)
#define gettext(String) (String)
#define dgettext(Domain,String) (String)
#define dcgettext(Domain,String,Type) (String)
#define bindtextdomain(Domain,Directory) (Domain)

#endif

#endif
