#ifndef GNOME_CUPS_INIT
#define GNOME_CUPS_INIT

#include <glib/gtypes.h>
#include <glib/gmacros.h>

G_BEGIN_DECLS

typedef struct _GnomeCupsAuthContext GnomeCupsAuthContext;
typedef gboolean (*GnomeCupsAuthFunction) (const char           *prompt,
					   char                **username,
					   char                **password,
					   GnomeCupsAuthContext *ctxt);

void gnome_cups_init (GnomeCupsAuthFunction opt_auth_fn);
void gnome_cups_shutdown (void);

G_END_DECLS

#endif
