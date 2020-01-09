#ifndef GNOME_CUPS_UTIL_H
#define GNOME_CUPS_UTIL_H

#include <glib.h>

#define gnome_cups_strdup(s) strdup (s)
#define gnome_cups_free(s)   free(s)

#define GNOME_CUPS_ERROR gnome_cups_error_quark ()
GQuark   gnome_cups_error_quark     (void);

gboolean gnome_cups_check_daemon    (void);
char *   gnome_cups_get_printer_uri (const char *printer_name);

typedef enum {
  GNOME_CUPS_UNSAFE_ALL        = 0x1,  /* Escape all unsafe characters   */
  GNOME_CUPS_UNSAFE_ALLOW_PLUS = 0x2,  /* Allows '+'  */
  GNOME_CUPS_UNSAFE_PATH       = 0x8,  /* Allows '/', '&', '=', ':', '@', '+', '$' and ',' */
  GNOME_CUPS_UNSAFE_HOST       = 0x10, /* Allows '/' and ':' and '@' */
  GNOME_CUPS_UNSAFE_SLASHES    = 0x20  /* Allows all characters except for '/' and '%' */
} GnomeCupsUnsafeCharacterSet;
gchar *  gnome_cups_util_escape_uri_string   (const gchar *string, GnomeCupsUnsafeCharacterSet mask);
gchar *  gnome_cups_util_unescape_uri_string (const gchar *escaped);

#endif
