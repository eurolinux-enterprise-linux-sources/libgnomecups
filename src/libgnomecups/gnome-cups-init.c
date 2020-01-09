#include "config.h"
#include <cups/cups.h>
#include "gnome-cups-init.h"
#include "gnome-cups-printer.h"
#include "gnome-cups-request.h"

/**
 * gnome_cups_init:
 * @opt_auth_fn: an authorization callback
 *
 * Initialize libgnomecups.  @opt_auth_fn is optional, but without it password
 * protected printers cannot be accessed.
 */
void
gnome_cups_init (GnomeCupsAuthFunction opt_auth_fn)
{
	g_type_init ();

	_gnome_cups_request_init (opt_auth_fn);
	_gnome_cups_printer_init ();
}

void
gnome_cups_shutdown (void)
{
	_gnome_cups_request_shutdown ();
}
