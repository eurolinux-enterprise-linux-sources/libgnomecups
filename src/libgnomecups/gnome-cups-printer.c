/* vim: set sw=8: -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/* 
 * gnome-cups-printer.c - 
 * Copyright (C) 2002, Ximian, Inc.
 *
 * Authors:
 *   Dave Camp <campd@ximian.com>
 *
 * This file is free software; you can redistribute it and/or modify
 * it under the terms of version 2 of the GNU Library General Public
 * License as published by the Free Software Foundation.
 *
 * This file is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this file; if not, write to
 * the Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 **/

#include <config.h>

#include "gnome-cups-printer.h"

#include <cups/cups.h>
#include <time.h>
#include <stdlib.h>
#include <ctype.h>

#include "util.h"
#include "gnome-cups-request.h"
#include "gnome-cups-util.h"
#include "gnome-cups-i18n.h"

#define UPDATE_TIMEOUT 5000

struct _GnomeCupsPPDFile {
	char name[1];
};

struct _GnomeCupsPrinterDetails {
	char *printer_name;

	guint attributes_set : 1;
	guint is_default : 1;
	guint is_gone : 1;
	guint is_local : 1;

	guint attributes_request_id;

	/* Option management */
	guint options_invalid : 1;
	GHashTable *ppd_options;
	GHashTable *dest_options;

	/* cups attributes */
	char *description;
	char *location;
	char *info;
	char *make_and_model;
	char *device_uri;
	char *printer_uri;
	char *state_message;
	ipp_pstate_t state;
	int job_count;

	/* Fields derived from the cups attributes */
	char *full_state;
	GList *state_reasons;
};

typedef struct {
	guint id;
	GnomeCupsPrinterAddedCallback func;
	GnomeCupsOnlyOnceCallback once;
	gpointer user_data;
} AddNotify;

typedef struct {
	guint id;
	GnomeCupsPrinterRemovedCallback func;
	gpointer user_data;
} RemovedNotify;

static const char *printer_state_strings[] = {
	N_("Ready"),
	N_("Printing"),
	N_("Paused")
};

enum {
	IS_DEFAULT_CHANGED,
	ATTRIBUTES_CHANGED,
	GONE,
	LAST_SIGNAL
};

static void update_printers (void);
static void set_timeout (void);

static GList *printer_names = NULL;
static char *default_printer = NULL;
static GHashTable *printers = NULL;
static GList *add_notifies = NULL;
static GList *removed_notifies = NULL;
static guint signals[LAST_SIGNAL];

GList *
gnome_cups_get_printers (void)
{
	GList *l;
	GList *ret = NULL;
	static time_t last_update = 0;

	if ((time (NULL) - last_update) > (UPDATE_TIMEOUT / 1000)) {
		update_printers ();
		last_update = time (NULL);
	}
	
	for (l = printer_names; l != NULL; l = l->next) {
		ret = g_list_prepend (ret, g_strdup (l->data));
	}
	
	return ret;
}

void
gnome_cups_printer_list_free (GList *printers)
{
	GList *l, *n;
	
	l = printers;
	while (l != NULL) {
		n = l->next;
		g_free (l->data);
		g_list_free_1 (l);
		l = n;
	}
}

char *
gnome_cups_get_default (void)
{
	return g_strdup (default_printer);
}

static gint
reason_severity_compare_fn (gconstpointer  a,
			    gconstpointer  b)
{
	const GnomeCupsPrinterReason *ar = a;
	const GnomeCupsPrinterReason *br = b;

	if (ar->reason < br->reason)
		return 1;
	else if (ar->reason > br->reason)
		return -1;
	else
		return 0;
}

static void
map_reasons (GnomeCupsPrinter *printer,
	     ipp_attribute_t  *attr)
{
	int i;
	GnomeCupsPrinterReason *reason;

	gnome_cups_printer_free_reasons (printer->details->state_reasons);
	printer->details->state_reasons = NULL;

	/* cf. RFC2911 4.4.12 */
	for (i = 0; i < attr->num_values; i++) {
		const char *p;
		const char *keyword = attr->values [i].string.text;

		reason = g_new (GnomeCupsPrinterReason, 1);

		if ((p = g_strrstr (keyword, "-report"))) {
			reason->reason = GNOME_CUPS_PRINTER_REASON_REPORT;

		} else if ((p = g_strrstr (keyword, "-warning"))) {
			reason->reason = GNOME_CUPS_PRINTER_REASON_WARNING;

		} else {
		  p = g_strrstr (keyword, "-error");
		  reason->reason = GNOME_CUPS_PRINTER_REASON_ERROR;
		}

		reason->keyword = g_strndup (keyword, p ? p - keyword : strlen (keyword));

		printer->details->state_reasons = g_list_insert_sorted
			(printer->details->state_reasons, reason,
			reason_severity_compare_fn);
	}
	if (printer->details->state_reasons) {
		reason = g_new (GnomeCupsPrinterReason, 1);
		reason->keyword = g_strdup ("none");
		reason->reason  = GNOME_CUPS_PRINTER_REASON_REPORT;

		printer->details->state_reasons = g_list_prepend 
			(printer->details->state_reasons, reason);
	}
}

const char *
gnome_cups_printer_get_make_and_model (GnomeCupsPrinter *printer)
{
	g_return_val_if_fail (GNOME_CUPS_IS_PRINTER (printer), NULL);
	g_return_val_if_fail (printer->details->make_and_model != NULL, "");
	
	return printer->details->make_and_model;
}

const char *
gnome_cups_printer_get_info (GnomeCupsPrinter *printer)
{
	g_return_val_if_fail (GNOME_CUPS_IS_PRINTER (printer), NULL);
	g_return_val_if_fail (printer->details->info != NULL, "");
	
	return printer->details->info;
}

#define MAP_INT(v,a) {if (!g_ascii_strcasecmp (attr->name, (a))) { if ((v) != attr->values[0].integer) { changed = TRUE; } (v) = attr->values[0].integer; }}
#define MAP_STRING(v,a) {if (!g_ascii_strcasecmp (attr->name, (a))) { if (!v || strcmp (v, attr->values[0].string.text)) { g_free (v); changed = TRUE; (v) = g_strdup (attr->values[0].string.text); }}}

static void
attributes_update_cb (guint id,
		      const char *path,
		      ipp_t *response,
		      GError **error,
		      gpointer cb_data)
{
	GnomeCupsPrinter *printer;
	ipp_attribute_t *attr;
	gboolean changed;

	printer = GNOME_CUPS_PRINTER (cb_data);

	changed = FALSE;

	if (!error && response) {
		for (attr = response->attrs; attr != NULL; attr = attr->next) {
			if (!attr->name) {
				continue;
			} 
			if (!g_ascii_strcasecmp (attr->name, "attributes-charset") || !strcmp (attr->name, "attributes-charset")) {
				continue;
			}
			if (!g_ascii_strcasecmp (attr->name, "printer-state-reasons")) {
				map_reasons (printer, attr);
			}
			MAP_INT (printer->details->state, "printer-state");
			MAP_INT (printer->details->job_count, "queued-job-count");
			MAP_STRING (printer->details->description, "printer-info");
			MAP_STRING (printer->details->location, "printer-location");
			MAP_STRING (printer->details->device_uri, "device-uri");
			MAP_STRING (printer->details->state_message, "printer-state-message");
			MAP_STRING (printer->details->info, "printer-info");
			MAP_STRING (printer->details->make_and_model, "printer-make-and-model");
			MAP_STRING (printer->details->printer_uri, "printer-uri-supported");
		}
	}
	ippDelete (response);
	g_clear_error (error);

	printer->details->attributes_set = 1;

	if (changed) {
		g_free (printer->details->full_state);
		printer->details->full_state = NULL;
		g_signal_emit (printer, signals[ATTRIBUTES_CHANGED], 0);
	}

	printer->details->attributes_request_id = 0;
}

#undef MAP_INT
#undef MAP_STRING

/* JEG 
 * Disable this for now.  The default SuSE network config does not
 * produce a resolvable hostname for the localhost and cups has a bug
 * that will produce an infinite loop when presented with an invalid
 * hostname.  This will also produce a hang if the remote printer is
 * unavailable and we do a syncronous lookup. */
static gboolean go_directly_to_printer_when_possible = FALSE;

static void
update_attributes (GnomeCupsPrinter *printer)
{
	ipp_t *request;
	char *host;
	static const char *attributes[] = {
	  "printer-name",
	  "printer-state",		"queued-job-count",
	  "printer-location",		"printer-info",
	  "printer-state-message",	"device-uri",
	  "printer-state-reasons",	"printer-info",
	  "printer-make-and-model",	"printer-uri-supported"
	};

	if (printer->details->attributes_request_id > 0) {
		return;
	}
	
	if (go_directly_to_printer_when_possible &&
	    printer->details->attributes_set) {
		request = gnome_cups_request_new_for_printer (IPP_GET_PRINTER_ATTRIBUTES,
							      printer);
		gnome_cups_request_add_requested_attributes (request, 
							     IPP_TAG_OPERATION,
							     G_N_ELEMENTS (attributes),
							     (char**)attributes);
		host = _gnome_cups_printer_get_host (printer);
	} else {
		char *printer_uri;
		request = gnome_cups_request_new (IPP_GET_PRINTER_ATTRIBUTES);
		printer_uri = g_strdup_printf ("ipp://localhost/printers/%s",
					       printer->details->printer_name);
		ippAddString (request, IPP_TAG_OPERATION, IPP_TAG_URI,
			      "printer-uri", NULL, printer_uri);
		gnome_cups_request_add_requested_attributes (request, 
							     IPP_TAG_OPERATION,
							     G_N_ELEMENTS (attributes),
							     (char**)attributes);
		g_free (printer_uri);
		host = NULL;
	}
		
	printer->details->attributes_request_id = 
		gnome_cups_request_execute_async (request, host, "/",
						  attributes_update_cb,
						  g_object_ref (printer),
						  g_object_unref);
	g_free (host);
}

/*
 * cups_get_dest() is copied from the cups libraries, file cups/dest.c,
 * which is 
 *   Copyright 1997-2004 by Easy Software Products.
 */
static int				/* O - Number of destinations */
cups_get_dests(const char  *filename,	/* I - File to read from */
               int         num_dests,	/* I - Number of destinations */
               cups_dest_t **dests)	/* IO - Destinations */
{
  int		i;			/* Looping var */
  cups_dest_t	*dest;			/* Current destination */
  FILE		*fp;			/* File pointer */
  char		line[8192],		/* Line from file */
		*lineptr,		/* Pointer into line */
		*name,			/* Name of destination/option */
		*instance;		/* Instance of destination */
  const char	*printer;		/* PRINTER or LPDEST */


 /*
  * Check environment variables...
  */

  if ((printer = getenv("LPDEST")) == NULL)
    if ((printer = getenv("PRINTER")) != NULL)
      if (strcmp(printer, "lp") == 0)
        printer = NULL;

 /*
  * Try to open the file...
  */

  if ((fp = fopen(filename, "r")) == NULL)
    return (num_dests);

 /*
  * Read each printer; each line looks like:
  *
  *    Dest name[/instance] options
  *    Default name[/instance] options
  */

  while (fgets(line, sizeof(line), fp) != NULL)
  {
   /*
    * See what type of line it is...
    */

    if (strncasecmp(line, "dest", 4) == 0 && isspace(line[4] & 255))
      lineptr = line + 4;
    else if (strncasecmp(line, "default", 7) == 0 && isspace(line[7] & 255))
      lineptr = line + 7;
    else
      continue;

   /*
    * Skip leading whitespace...
    */

    while (isspace(*lineptr & 255))
      lineptr ++;

    if (!*lineptr)
      continue;

    name = lineptr;

   /*
    * Search for an instance...
    */

    while (!isspace(*lineptr & 255) && *lineptr && *lineptr != '/')
      lineptr ++;

    if (!*lineptr)
      continue;

    if (*lineptr == '/')
    {
     /*
      * Found an instance...
      */

      *lineptr++ = '\0';
      instance = lineptr;

     /*
      * Search for an instance...
      */

      while (!isspace(*lineptr & 255) && *lineptr)
	lineptr ++;
    }
    else
      instance = NULL;

    *lineptr++ = '\0';

   /*
    * See if the primary instance of the destination exists; if not,
    * ignore this entry and move on...
    */
#if 0
    /* Don't do this here, since we call this with an empty dests array */
    if (cupsGetDest(name, NULL, num_dests, *dests) == NULL)
      continue;
#endif

   /*
    * Add the destination...
    */

    num_dests = cupsAddDest(name, instance, num_dests, dests);

    if ((dest = cupsGetDest(name, instance, num_dests, *dests)) == NULL)
    {
     /*
      * Out of memory!
      */

      fclose(fp);
      return (num_dests);
    }

   /*
    * Add options until we hit the end of the line...
    */

    dest->num_options = cupsParseOptions(lineptr, dest->num_options,
                                         &(dest->options));

   /*
    * Set this as default if needed...
    */

    if (strncasecmp(line, "default", 7) == 0 && printer == NULL)
    {
      for (i = 0; i < num_dests; i ++)
        (*dests)[i].is_default = 0;

      dest->is_default = 1;
    }
  }

 /*
  * Close the file and return...
  */

  fclose(fp);      

  return (num_dests);
}

static int
parse_lpoptions (cups_dest_t **dests)
{
	gchar *filename;
	const gchar *home;
	int num_dests;

	num_dests = 0;
	*dests = NULL;

	home = g_getenv("CUPS_SERVERROOT");
	if (!home)
		home = "/etc/cups";
	filename = g_build_filename (home, "lpoptions", NULL);
	num_dests = cups_get_dests (filename, num_dests, dests);
					  
	g_free (filename);
	
	filename = g_build_filename (g_get_home_dir (), ".lpoptions", NULL);
	num_dests = cups_get_dests (filename, num_dests, dests);
	g_free (filename);

	return num_dests;
}

static char *
get_default (void)
{
	const char *env;
	char *name;
	ipp_t *request;
	ipp_t *response;
	ipp_attribute_t *attr;
	GError *error = NULL;
	int num_dests;
	cups_dest_t *dests = NULL;
	cups_dest_t *default_dest;

	/* First look in the environment */

	env = g_getenv ("LPDEST");
	if (env) {
		return g_strdup (env);
	}
	
	env = g_getenv ("PRINTER");
	if (env && strcmp (env, "lp")) {
		return g_strdup (env);
	}
	
	/* Then look in .lpoptions */	
	num_dests = parse_lpoptions (&dests);
	default_dest = cupsGetDest (NULL, NULL, num_dests, dests);
	if (default_dest) {
		name = g_strdup (default_dest->name);
		cupsFreeDests (num_dests, dests);

		return name;
	}
	cupsFreeDests (num_dests, dests);

	request = gnome_cups_request_new (CUPS_GET_DEFAULT);
	response = gnome_cups_request_execute (request, NULL, "/", &error);

	if (error) {
		ippDelete (response);
		g_error_free (error);

		return g_strdup ("");
	}
	
	attr = ippFindAttribute (response, "printer-name", IPP_TAG_NAME);
	if (attr) {
		name = g_strdup (attr->values[0].string.text);
	} else {
		name = NULL;
	}
	
	ippDelete (response);
	
	return name;
}

static void
update_default (void)
{
	GnomeCupsPrinter *printer;
	char *old_default;
	
	old_default = default_printer;
	
	default_printer = get_default ();

	if (!default_printer) {
		default_printer = g_strdup ("");
	}

	if (!old_default) {
		old_default = g_strdup ("");
	}
	
	if (!strcmp (old_default, default_printer)) {
		g_free (old_default);
		return;
	}

	printer = gnome_cups_printer_get_existing (old_default);
	if (printer) {
		printer->details->is_default = FALSE;
		g_signal_emit (printer, signals[IS_DEFAULT_CHANGED], 0);
		g_object_unref (printer);
	}
	
	printer = gnome_cups_printer_get_existing (default_printer);
	if (printer) {
		printer->details->is_default = TRUE;
		g_signal_emit (printer, signals[IS_DEFAULT_CHANGED], 0);
		g_object_unref (printer);
	}

	g_free (old_default);
}

static void
remove_from_printers (gpointer user_data, GObject *object)
{
	char *printer_name = user_data;

	g_hash_table_remove (printers, printer_name);
	set_timeout ();
}

static void
printer_added (const char *name)
{
	GList *l;

	for (l = add_notifies; l != NULL; l = l->next) {
		AddNotify *notify = l->data;
		notify->func (name, notify->user_data);
	}
}

static void
printer_removed (const char *name)
{
	GnomeCupsPrinter *printer;
	GList *l;
	
	printer = gnome_cups_printer_get_existing (name);
	
	if (printer) {
		printer->details->is_gone = TRUE;
		g_signal_emit (printer, signals[GONE], 0);
		g_object_unref (printer);
	}

	for (l = removed_notifies; l != NULL; l = l->next) {
		RemovedNotify *notify = l->data;
		notify->func (name, notify->user_data);
	}
}

static gboolean
name_in_list (const char *name, GList *list)
{
	for (; list != NULL; list = list->next) {
		if (!strcmp (name, list->data)) {
			return TRUE;
		}
	}
	return FALSE;
}

static GList *
get_printer_names (void)
{
	GError *error = NULL;
	GList *ret;
	ipp_t *request;
	ipp_t *response;
	ipp_attribute_t *attr;
	
	request = gnome_cups_request_new (CUPS_GET_PRINTERS);
	
	response = gnome_cups_request_execute (request, NULL, "/", &error);

	if (error) {
		ippDelete (response);
		g_error_free (error);
		return NULL;
	}

	if (!response) {
		return NULL;
	}

	ret = NULL;
	attr = ippFindAttribute (response, "printer-name", IPP_TAG_NAME);
	while (attr) {
		ret = g_list_prepend (ret, 
				      g_strdup (attr->values[0].string.text));
		
		attr = ippFindNextAttribute (response, 
					     "printer-name", 
					     IPP_TAG_NAME);
	}

	ret = g_list_reverse (ret);

	ippDelete (response);

	return ret;
}

static void
update_printers (void)
{
	GList *old_printer_names;
	GList *l;

	/* Update the state */
	update_default ();
	
	old_printer_names = printer_names;
	printer_names = get_printer_names ();

	for (l = printer_names; l != NULL; l = l->next) {
		GnomeCupsPrinter *printer;
		char *name = l->data;
		
		printer = gnome_cups_printer_get_existing (name);
		if (printer) {
			update_attributes (printer);
			g_object_unref (printer);
		}
	}

	/* Check for removals */
	for (l = old_printer_names; l != NULL; l = l->next) {
		char *old_name = l->data;
		if (!name_in_list (old_name, printer_names)) {
			printer_removed (old_name);
		}
	}
	/* Check for additions */
	for (l = printer_names; l != NULL; l = l->next) {
		char *new_name = l->data;
		if (!name_in_list (new_name, old_printer_names)) {
			printer_added (new_name);
		}
	}

	gnome_cups_printer_list_free (old_printer_names);
}

static gboolean
update_printers_timeout (void)
{
	AddNotify *notify;
	GList *l, *n;
	
	/* To avoid unneccessary calls during authentication, check
	 * if a request is currently executing */
	if (_gnome_cups_outstanding_request_count () == 0) {
		update_printers ();
		
		/* call all our only_once callbacks and remove them from the notify list */
		l = add_notifies;
		while (l != NULL) {
			n = l->next;
			
			notify = l->data;
			if (notify->once) {
				notify->once (notify->user_data);
				add_notifies = g_list_remove_link (add_notifies, l);
				g_list_free_1 (l);
				g_free (notify);
			}
			
			l = n;
		}
		
		set_timeout ();
	}
	
	return TRUE;
}

static void
set_timeout (void)
{
	static guint update_timeout_id = 0;
	gboolean should_timeout =
		add_notifies || (printers != NULL && g_hash_table_size (printers) > 0);

	if (should_timeout && !update_timeout_id) {
		update_timeout_id = g_timeout_add (UPDATE_TIMEOUT, 
						   update_printers_timeout,
						   NULL);
	} else if (!should_timeout && update_timeout_id) {
		g_source_remove (update_timeout_id);
		update_timeout_id = 0;
	}
}

guint
gnome_cups_printer_new_printer_notify_add (GnomeCupsPrinterAddedCallback cb,
					   gpointer user_data)
{
	guint id = 0;
	AddNotify *notify;

	g_return_val_if_fail (cb != NULL, 0);

	notify = g_new0 (AddNotify, 1);
	
	notify->id = ++id;
	notify->func = cb;
	notify->user_data = user_data;
	
	add_notifies = g_list_append (add_notifies, notify);

	set_timeout ();

	return notify->id;
}

guint
gnome_cups_printer_new_printer_notify_add_only_once (GnomeCupsPrinterAddedCallback cb,
						     GnomeCupsOnlyOnceCallback once,
						     gpointer user_data)
{
	guint id = 0;
	AddNotify *notify;
	
	g_return_val_if_fail (cb != NULL, 0);
	
	notify = g_new0 (AddNotify, 1);
	
	notify->id = ++id;
	notify->func = cb;
	notify->once = once;
	notify->user_data = user_data;
	
	add_notifies = g_list_append (add_notifies, notify);
	
	set_timeout ();
	
	return notify->id;
}

void
gnome_cups_printer_new_printer_notify_remove (guint id)
{
	GList *l;
	for (l = add_notifies; l != NULL; l = l->next) {
		AddNotify *notify = l->data;
		if (notify->id == id) {
			g_free (notify);
			add_notifies = g_list_remove_link (add_notifies, l);
			g_list_free_1 (l);
			break;
		}
	}

	set_timeout ();
}

guint
gnome_cups_printer_printer_removed_notify_add (GnomeCupsPrinterRemovedCallback cb,
					       gpointer user_data)
{
	guint id = 0;
	RemovedNotify *notify;

	g_return_val_if_fail (cb != NULL, 0);

	notify = g_new0 (RemovedNotify, 1);
	
	notify->id = ++id;
	notify->func = cb;
	notify->user_data = user_data;
	
	removed_notifies = g_list_append (removed_notifies, notify);

	set_timeout ();

	return notify->id;
}

void
gnome_cups_printer_printer_removed_notify_remove (guint id)
{
	GList *l;
	for (l = removed_notifies; l != NULL; l = l->next) {
		RemovedNotify *notify = l->data;
		if (notify->id == id) {
			g_free (notify);
			removed_notifies = g_list_remove_link (removed_notifies, l);
			g_list_free_1 (l);
			break;
		}
	}

	set_timeout ();
}

GnomeCupsPrinter *
gnome_cups_printer_get_existing (const char *printer_name)
{
	GnomeCupsPrinter *printer;

	if (!default_printer) {
		default_printer = g_strdup (cupsGetDefault ());
	}

	if (!printer_name) {
		printer_name = default_printer;
	}

	if (!printers) {
		printers = g_hash_table_new_full (g_str_hash, 
						  g_str_equal,
						  g_free,
						  NULL);
	} else {
		printer = g_hash_table_lookup (printers, printer_name);
		if (printer) {
			return g_object_ref (printer);
		}
	}

	return NULL;
}

GnomeCupsPrinter *
gnome_cups_printer_get (const char *printer_name)
{
	GnomeCupsPrinter *printer;
	char *key;

	printer = gnome_cups_printer_get_existing (printer_name);
	if (printer) {
		return printer;
	}
	
	if (!printer_name) {
		printer_name = default_printer;
	}

	if (!name_in_list (printer_name, printer_names)) {
		return NULL;
	}
	
	printer = g_object_new (GNOME_CUPS_TYPE_PRINTER, NULL);
	printer->details->printer_name = g_strdup (printer_name);
	key = g_strdup (printer_name);
	g_hash_table_insert (printers, key, printer);
	g_object_weak_ref (G_OBJECT (printer), remove_from_printers, key);
	set_timeout ();
	
	if (default_printer && !strcmp (printer_name, default_printer)) {
		printer->details->is_default = TRUE;
	}

	update_attributes (printer);

	return printer;
}

void
gnome_cups_printer_unref (GnomeCupsPrinter *printer)
{
	if (printer != NULL)
		g_object_unref (printer);
}

const char *
gnome_cups_printer_get_name (GnomeCupsPrinter *printer)
{
	g_return_val_if_fail (GNOME_CUPS_IS_PRINTER (printer), "");

	return printer->details->printer_name;
}

#if 0
CUPS does not support this
void
gnome_cups_printer_set_name (GnomeCupsPrinter *printer,
			     const char *name,
			     GError **error)
{
	ipp_t *request;
	ipp_t *response;

	g_return_if_fail (GNOME_CUPS_IS_PRINTER (printer));
	g_return_if_fail (name != NULL);

	if (!strcmp (name, printer->details->printer_name))
		return;

	request = gnome_cups_request_new_for_printer (IPP_SET_PRINTER_ATTRIBUTES, printer);
	ippAddString (request,	IPP_TAG_PRINTER, IPP_TAG_NAME,
		"printer-name", NULL, gnome_cups_strdup (name));
	response = gnome_cups_request_execute (request, NULL, "/admin/", error);
	ippDelete (response);

#if 0
	g_free (printer->details->printer_name);
	printer->details->printer_name = g_strdup (name);
#endif
	update_attributes (printer);
}
#endif

gboolean
gnome_cups_printer_is_gone (GnomeCupsPrinter *printer)
{
	g_return_val_if_fail (GNOME_CUPS_IS_PRINTER (printer), FALSE);

	return printer->details->is_gone;
}

gboolean
gnome_cups_printer_get_attributes_initialized (GnomeCupsPrinter *printer)
{
	g_return_val_if_fail (GNOME_CUPS_IS_PRINTER (printer), FALSE);

	return printer->details->attributes_set;
}

gboolean
gnome_cups_printer_get_is_local (GnomeCupsPrinter *printer)
{
	g_return_val_if_fail (GNOME_CUPS_IS_PRINTER (printer), FALSE);

	return (printer->details->device_uri != NULL) && 
		(strcmp (printer->details->device_uri, "") != 0);
}

void
gnome_cups_printer_get_icon (GnomeCupsPrinter *printer,
			     char **name,
			     GList **emblems)
{
	g_return_if_fail (GNOME_CUPS_IS_PRINTER (printer));
	g_return_if_fail (name != NULL);

	*name = gnome_cups_printer_get_is_local (printer) ? g_strdup ("gnome-dev-printer") : g_strdup ("gnome-dev-printer-network");
	if (emblems) {
		*emblems = NULL;
		if (gnome_cups_printer_get_state (printer) == IPP_PRINTER_STOPPED) {
			*emblems = g_list_append (*emblems, 
						  g_strdup ("emblem-paused"));
		}
		
		if (gnome_cups_printer_get_is_default (printer)) {
                        *emblems = g_list_append (*emblems, 
						  g_strdup ("emblem-default"));
                }
	}
}

ipp_pstate_t
gnome_cups_printer_get_state (GnomeCupsPrinter *printer)
{
	g_return_val_if_fail (GNOME_CUPS_IS_PRINTER (printer), IPP_PRINTER_IDLE);
	g_return_val_if_fail (printer->details->state >= IPP_PRINTER_IDLE && printer->details->state <= IPP_PRINTER_STOPPED, IPP_PRINTER_IDLE);

	return printer->details->state;
}

const char *
gnome_cups_printer_get_state_name (GnomeCupsPrinter *printer)
{
	const char *state_str;
	ipp_pstate_t state;

	g_return_val_if_fail (GNOME_CUPS_IS_PRINTER (printer), NULL);


	g_return_val_if_fail (printer->details->state >= IPP_PRINTER_IDLE && printer->details->state <= IPP_PRINTER_STOPPED, _("Unknown"));

	state = printer->details->state;

	return state_str =  _(printer_state_strings[state - IPP_PRINTER_IDLE]);
}

const char *
gnome_cups_printer_get_full_state (GnomeCupsPrinter *printer)
{
	const char *state_name;

	g_return_val_if_fail (GNOME_CUPS_IS_PRINTER (printer), _("Unknown"));
	
	state_name = gnome_cups_printer_get_state_name (printer);
	
	if (!printer->details->full_state) {
		if (printer->details->state_message && strcmp (printer->details->state_message, state_name)) {
			printer->details->full_state = 
				g_strdup_printf (_("%s: %s"),
						 state_name,
						 printer->details->state_message);
		} else {
			printer->details->full_state = g_strdup (state_name);
		}
	}		

	return printer->details->full_state;
}

static char *
get_ppd_uri_path (GnomeCupsPrinter *printer)
{
	return g_strdup_printf ("/printers/%s.ppd", printer->details->printer_name);
}

static int
get_tmp_ppd_file (GnomeCupsPrinter *printer,
		  char **filename,
		  GError **error)
{
	int fd;
	char *tmpname;
	char *tmp_filename;

	tmpname = gnome_cups_util_escape_uri_string (printer->details->printer_name, GNOME_CUPS_UNSAFE_ALL);
	tmp_filename = g_strdup_printf ("%s-printer-ppd-%s-XXXXXX",
					g_get_user_name (),
					tmpname);
	g_free (tmpname);
	fd = g_file_open_tmp (tmp_filename, filename, error);
	g_free (tmp_filename);
	return fd;
}

static GHashTable *get_ppd_options (GnomeCupsPrinter *printer, ppd_file_t *ppd);

ppd_file_t *
gnome_cups_printer_get_ppd (GnomeCupsPrinter *printer)
{
	GError *error = NULL;
	int fd;
	char *host;
	char *ppdpath;
	char *filename;
	ppd_file_t *ppd;

	g_return_val_if_fail (GNOME_CUPS_IS_PRINTER (printer), NULL);

	fd = get_tmp_ppd_file (printer, &filename, &error);
	if (error != NULL) {
		g_warning ("Couldn't create temporary file: %s",
			   error->message);
		g_error_free (error);
		return NULL;
	}

	host = _gnome_cups_printer_get_host (printer);
	ppdpath = get_ppd_uri_path (printer);

	gnome_cups_request_file (host, ppdpath, fd, &error);
	if (error != NULL) {
		g_warning ("Couldn't retrieve PPD for %s: %s",
			   printer->details->printer_name,
			   error->message);
		g_error_free (error);
		return NULL;
	}

	close (fd);
	ppd = ppdOpenFile (filename);

	/* This is loaded in to memory now, so we can free it. */
	unlink (filename);
	g_free (filename);

	if (!printer->details->ppd_options)
		printer->details->ppd_options = get_ppd_options (printer, ppd);

	return ppd;
}

typedef struct
{
	GnomeCupsPrinter *printer;
	GnomeCupsPrinterGetPPDCallback cb;	
	gpointer user_data;
	GDestroyNotify destroy_notify;
	char *filename;
	int fd;
} GnomeCupsPrinterGetPPDWrapData;

static void
free_get_ppd_wrapper_data (gpointer p)
{
	GnomeCupsPrinterGetPPDWrapData *data = p;
	g_object_unref (data->printer);
	unlink (data->filename);
	g_free (data->filename);
	if (data->destroy_notify)
		data->destroy_notify (data->user_data);
	close (data->fd);
	g_free (data);
}

static void
wrap_ppd_request_cb (guint id,
		     const char *path,
		     ipp_t *response,
		     GError **error,
		     gpointer cb_data)
{
	GnomeCupsPrinterGetPPDWrapData *data = cb_data;
	ppd_file_t *ppd;

	if (error && *error != NULL) {
		data->cb (id, NULL, error, data->user_data);
		g_clear_error (error);
	} else {
		ppd = ppdOpenFile (data->filename);
		if (!data->printer->details->ppd_options)
			data->printer->details->ppd_options = 
				get_ppd_options (data->printer, ppd);

		data->cb (id, ppd, NULL, data->user_data);
	}
}

guint
gnome_cups_printer_get_ppd_async (GnomeCupsPrinter *printer,
				  GnomeCupsPrinterGetPPDCallback cb,
				  gpointer user_data,
				  GDestroyNotify destroy_notify)
{
	GError *error = NULL;
	guint opid;
	int fd;
	char *host;
	char *ppdpath;
	char *filename;
	GnomeCupsPrinterGetPPDWrapData *data;

	g_return_val_if_fail (GNOME_CUPS_IS_PRINTER (printer), 0);

	fd = get_tmp_ppd_file (printer, &filename, &error);
	if (error != NULL) {
		g_warning ("Couldn't create temporary file: %s",
			   error->message);
		g_error_free (error);
		return 0;
	}

	host = _gnome_cups_printer_get_host (printer);
	ppdpath = get_ppd_uri_path (printer);

	data = g_new0 (GnomeCupsPrinterGetPPDWrapData, 1);
	data->printer = g_object_ref (printer);
	data->filename = filename;
	data->fd = fd;
	data->cb = cb;
	data->user_data = user_data;
	data->destroy_notify = destroy_notify;

	opid = gnome_cups_request_file_async (host, ppdpath, fd,
					      wrap_ppd_request_cb,
					      data,
					      free_get_ppd_wrapper_data);
	g_free (ppdpath);
	return opid;
}

const char *
gnome_cups_printer_get_device_uri (GnomeCupsPrinter const *printer)
{
	g_return_val_if_fail (GNOME_CUPS_IS_PRINTER (printer), "");
	g_return_val_if_fail (printer->details->device_uri, "");
	return printer->details->device_uri;
}
const char *
gnome_cups_printer_get_uri (GnomeCupsPrinter const *printer)
{
	g_return_val_if_fail (GNOME_CUPS_IS_PRINTER (printer), "");
	g_return_val_if_fail (printer->details->printer_uri, "");
	return printer->details->printer_uri;
}

const char *       
gnome_cups_printer_get_description (GnomeCupsPrinter *printer)
{
	g_return_val_if_fail (GNOME_CUPS_IS_PRINTER (printer), "");
	g_return_val_if_fail (printer->details->description, "");

	return printer->details->description;
	
}

void
gnome_cups_printer_set_description (GnomeCupsPrinter *printer,
				    const char *description,
				    GError **error)
{
	ipp_t *request;
	ipp_t *response;

	g_return_if_fail (GNOME_CUPS_IS_PRINTER (printer));
	g_return_if_fail (description != NULL);

	if (!strcmp (description, printer->details->description)) {
		return;
	}

	request = gnome_cups_request_new_for_printer (IPP_SET_PRINTER_ATTRIBUTES,
						      printer);
	ippAddString (request, IPP_TAG_PRINTER, IPP_TAG_TEXT,
		      "printer-info", NULL, description);
	response = gnome_cups_request_execute (request, NULL, "/admin/", error);
	ippDelete (response);
	update_attributes (printer);
}

const char *       
gnome_cups_printer_get_location (GnomeCupsPrinter *printer)
{
	g_return_val_if_fail (GNOME_CUPS_IS_PRINTER (printer), NULL);
	
	return printer->details->location;
}

void
gnome_cups_printer_set_location (GnomeCupsPrinter *printer,
				 const char *location,
				 GError **error)
{
	ipp_t *request;
	ipp_t *response;
	
	g_return_if_fail (GNOME_CUPS_IS_PRINTER (printer));
	g_return_if_fail (location != NULL);

	if (!strcmp (location, printer->details->location)) {
		return;
	}

	request = gnome_cups_request_new_for_printer (
		IPP_SET_PRINTER_ATTRIBUTES, printer);
	ippAddString (request, IPP_TAG_PRINTER, IPP_TAG_TEXT,
		"printer-location", NULL, location);
	response = gnome_cups_request_execute (request, NULL, "/admin/", error);
	ippDelete (response);
	update_attributes (printer);
}

void
gnome_cups_printer_pause (GnomeCupsPrinter *printer,
			  GError **error)
{
	ipp_t *request;
	ipp_t *response;

	g_return_if_fail (GNOME_CUPS_IS_PRINTER (printer));
	
	request = gnome_cups_request_new_for_printer (IPP_PAUSE_PRINTER, printer);
	response = gnome_cups_request_execute (request, NULL, "/admin/", error);
	ippDelete (response);
	update_attributes (printer);
}

void
gnome_cups_printer_resume (GnomeCupsPrinter *printer,
			   GError **error)
{
	ipp_t *request;
	ipp_t *response;

	g_return_if_fail (GNOME_CUPS_IS_PRINTER (printer));
	
	request = gnome_cups_request_new_for_printer (IPP_RESUME_PRINTER, printer);
	response = gnome_cups_request_execute (request, NULL, "/admin/", error);
	ippDelete (response);
	update_attributes (printer);	
}

void
gnome_cups_printer_delete (GnomeCupsPrinter *printer,
			   GError **error)
{
	ipp_t *request;
	ipp_t *response;

	g_return_if_fail (GNOME_CUPS_IS_PRINTER (printer));

	request = gnome_cups_request_new_for_printer (CUPS_DELETE_PRINTER,
						      printer);
	response = gnome_cups_request_execute (request, NULL, "/admin/", error);
	ippDelete (response);
}

gboolean
gnome_cups_printer_get_is_default (GnomeCupsPrinter *printer)
{
	g_return_val_if_fail (GNOME_CUPS_IS_PRINTER (printer), FALSE);

	return printer->details->is_default;
}

void
gnome_cups_printer_set_default (GnomeCupsPrinter *printer,
				GError **error)
{
	int num_dests;
	cups_dest_t *dests;
	cups_dest_t *old_default_dest = NULL;
	cups_dest_t *new_default_dest = NULL;

	g_return_if_fail (GNOME_CUPS_IS_PRINTER (printer));
	
	num_dests = cupsGetDests (&dests);

	old_default_dest = cupsGetDest (NULL, NULL, num_dests, dests);
	new_default_dest = cupsGetDest (printer->details->printer_name, NULL, 
					num_dests, dests);

	if (old_default_dest) {
		old_default_dest->is_default = 0;
	}
	if (new_default_dest) {
		new_default_dest->is_default = 1;
		cupsSetDests (num_dests, dests);
	}
	
	cupsFreeDests (num_dests, dests);

	update_default ();
}

int
gnome_cups_printer_get_job_count (GnomeCupsPrinter *printer)
{
	g_return_val_if_fail (GNOME_CUPS_IS_PRINTER (printer), 0);

	return printer->details->job_count;
}

GnomeCupsQueue *
gnome_cups_printer_get_queue (GnomeCupsPrinter *printer)
{
	g_return_val_if_fail (GNOME_CUPS_IS_PRINTER (printer), NULL);

	return gnome_cups_queue_get (printer->details->printer_name);
}

static GnomeCupsPrinterOption *
get_option (ppd_option_t *option)
{
	GnomeCupsPrinterOption *ret;
	int i;
	char *defchoice;
	char *p;
	
	ret = g_new0 (GnomeCupsPrinterOption, 1);
	ret->id = g_strdup (option->keyword);
	ret->text = g_strdup (option->text);
	ret->type = option->ui;
	ret->n_choices = option->num_choices;
	ret->choices = g_new0 (GnomeCupsPrinterOptionChoice, ret->n_choices);
	for (i = 0; i < option->num_choices; i++) {
		ret->choices[i].value = g_strdup (option->choices[i].choice);
		ret->choices[i].text = g_strdup (option->choices[i].text);
	}

	defchoice = g_strdup (option->defchoice);
	
	/* Strip trailing spaces and tabs since CUPS doesn't does this
	 */
	p = defchoice + strlen (defchoice);
	while (p > defchoice &&
	       (*(p - 1) == ' ' || *(p - 1) == '\t')) {
		*(p - 1) = '\0';
		p--;
	}
	ret->value = defchoice;

	return ret;
}

static GHashTable *
get_ppd_options (GnomeCupsPrinter *printer,
		 ppd_file_t       *ppd)
{
	int i;
	GHashTable *options;
	
	options = g_hash_table_new_full (g_str_hash, g_str_equal, 
					 NULL,
					 (GDestroyNotify)gnome_cups_printer_option_free);
	
	if (!ppd)
		return options;
	
	for (i = 0; i < ppd->num_groups; i++) {
		ppd_group_t *group = &ppd->groups[i];
		int j;
		for (j = 0; j < group->num_options; j++) {
			GnomeCupsPrinterOption *option;

			option = get_option (&group->options[j]);

			g_hash_table_insert (options, option->id, option);
		}
	}

	return options;
}

static GHashTable *
get_dest_options (GnomeCupsPrinter *printer)
{
	int num_dests, i;
	cups_dest_t *dest;
	cups_dest_t *dests = NULL;
	GHashTable *dest_options;

	/* just parse lpoptions */
	num_dests = parse_lpoptions (&dests);

	dest = cupsGetDest (printer->details->printer_name, NULL, num_dests, dests);	

	dest_options = g_hash_table_new_full (g_str_hash, g_str_equal,
					      g_free, g_free);
	
	if (dest) {
		for (i = 0; i < dest->num_options; i++) {
			g_hash_table_insert (dest_options, 
					     g_strdup (dest->options[i].name),
					     g_strdup (dest->options[i].value));
		}
	}
	printer->details->options_invalid = FALSE;

	cupsFreeDests (num_dests, dests);

	return dest_options;
}

static void
update_options (GnomeCupsPrinter *printer)
{ 
	if (!printer->details->ppd_options) {
		ppd_file_t *ppd;
		
		ppd = gnome_cups_printer_get_ppd (printer);
		
		if (ppd)
			ppdClose (ppd);
	}
	
	if (printer->details->options_invalid) {
		g_hash_table_destroy (printer->details->dest_options);
		printer->details->dest_options = NULL;
	}
		
	if (!printer->details->dest_options) {
		printer->details->dest_options = get_dest_options (printer);
	}
}

static GnomeCupsPrinterOption *
printer_option_copy (GnomeCupsPrinterOption *option)
{
	GnomeCupsPrinterOption *ret;
	int i;
	
	ret = g_new0 (GnomeCupsPrinterOption, 1);
	ret->id = g_strdup (option->id);
	ret->text = g_strdup (option->text);
	ret->value = g_strdup (option->value);
	ret->type = option->type;
	ret->n_choices = option->n_choices;
	ret->choices = g_new0 (GnomeCupsPrinterOptionChoice, ret->n_choices);
	for (i = 0; i < ret->n_choices; i++) {
		ret->choices[i].value = g_strdup (option->choices[i].value);
		ret->choices[i].text = g_strdup (option->choices[i].text);
	}

	return ret;
}

static void
collect_ppds_foreach_cb (gpointer key,
			 gpointer value,
			 gpointer user_data)
{
	GList **options = user_data;
	
	*options = g_list_prepend (*options, value);
}

GList *
gnome_cups_printer_get_options (GnomeCupsPrinter *printer)
{
	GList *options;
	GList *ret;
	GList *l;

	update_options (printer);

	options = NULL;
	g_hash_table_foreach (printer->details->ppd_options,
			      collect_ppds_foreach_cb,
			      &options);

	ret = NULL;
	for (l = options; l != NULL; l = l->next) {
		GnomeCupsPrinterOption *option;		
		char *dest_option;
		option = printer_option_copy (l->data);
		dest_option = g_hash_table_lookup
			(printer->details->dest_options, option->id);

		if (dest_option) {
			g_free (option->value);
			option->value = g_strdup (dest_option);
		}
		
		ret = g_list_prepend (ret, option);
	}
	
	return ret;
}

extern int	cups_get_dests(const char *filename, int num_dests,
		               cups_dest_t **dests);


GnomeCupsPrinterOption *
gnome_cups_printer_get_option (GnomeCupsPrinter *printer,
			       const char *id)
{
	GnomeCupsPrinterOption *ppd_option;
	
	g_return_val_if_fail (GNOME_CUPS_IS_PRINTER (printer), NULL);
	g_return_val_if_fail (id != NULL, NULL);

	update_options (printer);

	ppd_option = g_hash_table_lookup (printer->details->ppd_options,
					  id);
	if (ppd_option) {
		GnomeCupsPrinterOption *ret;
		char *dest_option;

		ret = printer_option_copy (ppd_option);
		dest_option = g_hash_table_lookup
			(printer->details->dest_options, id);
		
		if (dest_option) {
			g_free (ret->value);
			ret->value = g_strdup (dest_option);
		}
		
		return ret;
	}
	
	return NULL;
}

char *
gnome_cups_printer_get_option_value (GnomeCupsPrinter *printer,
				     const char *id)
{
	GnomeCupsPrinterOption *option;
	char *value = NULL;

	g_return_val_if_fail (GNOME_CUPS_IS_PRINTER (printer), NULL);
	g_return_val_if_fail (id != NULL, NULL);

	update_options (printer);

	option = g_hash_table_lookup (printer->details->ppd_options,
				      id);
	if (option) {
		value = g_hash_table_lookup
			(printer->details->dest_options, id);
		if (value == NULL) {
			value = option->value;
		}
	}
	
	return g_strdup (value);
}

void
gnome_cups_printer_set_option_value (GnomeCupsPrinter *printer,
				     const char *id,
				     const char *value)
{
	int num_dests;
	cups_dest_t *dests;
	cups_dest_t *dest;
	
	g_return_if_fail (GNOME_CUPS_IS_PRINTER (printer));
	g_return_if_fail (id != NULL);
	g_return_if_fail (value != NULL);

	num_dests = cupsGetDests (&dests);

	dest = cupsGetDest (printer->details->printer_name, NULL, 
			    num_dests, dests);

	if (dest) {
		dest->num_options = cupsAddOption 
			(id, value, dest->num_options, &dest->options);
		cupsSetDests (num_dests, dests);
	}
	cupsFreeDests (num_dests, dests);

	printer->details->options_invalid = TRUE;
}

void
gnome_cups_printer_option_free (GnomeCupsPrinterOption *option)
{
	int i;

	g_return_if_fail (option != NULL);

	g_free (option->id);
	g_free (option->text);
	g_free (option->value);
	for (i = 0; i < option->n_choices; i++) {
		g_free (option->choices[i].value);
		g_free (option->choices[i].text);
	}
	g_free (option->choices);

	g_free (option);
}

void
gnome_cups_printer_option_list_free (GList *options)
{
	GList *l, *n;
	
	l = options;
	while (l != NULL) {
		n = l->next;
		gnome_cups_printer_option_free (l->data);
		g_list_free_1 (l);
		l = n;
	}
}

static void
gnome_cups_printer_finalize (GObject *object)
{
	GnomeCupsPrinter *printer = GNOME_CUPS_PRINTER (object);

	if (printer->details->attributes_request_id > 0)
		gnome_cups_request_cancel (printer->details->attributes_request_id > 0);
	
	if (printer->details->ppd_options) {
		g_hash_table_destroy (printer->details->ppd_options);
	}

	if (printer->details->dest_options) {
		g_hash_table_destroy (printer->details->dest_options);
	}

	gnome_cups_printer_free_reasons (printer->details->state_reasons);
	printer->details->state_reasons = NULL;
	
	g_free (printer->details->printer_name);
	g_free (printer->details->full_state);
	g_free (printer->details->description);
	g_free (printer->details->location);
	g_free (printer->details->device_uri);
	g_free (printer->details->state_message);
	g_free (printer->details->info);
	g_free (printer->details->make_and_model);
	g_free (printer->details->printer_uri);
	g_free (printer->details);
}

static void
gnome_cups_printer_instance_init (GnomeCupsPrinter *printer)
{
	printer->details = g_new0 (GnomeCupsPrinterDetails, 1);

	printer->details->description = g_strdup ("");
	printer->details->location = g_strdup ("");
	printer->details->info = g_strdup ("");
	printer->details->make_and_model = g_strdup ("");
	printer->details->device_uri = g_strdup ("");
	printer->details->state_message = g_strdup ("");

	printer->details->state = IPP_PRINTER_IDLE;
}

static void
gnome_cups_printer_class_init (GnomeCupsPrinterClass *class)
{
	G_OBJECT_CLASS (class)->finalize = gnome_cups_printer_finalize;

	signals[IS_DEFAULT_CHANGED] =
		g_signal_new ("is_default_changed",
		              G_TYPE_FROM_CLASS (class),
		              G_SIGNAL_RUN_LAST,
		              G_STRUCT_OFFSET (GnomeCupsPrinterClass, is_default_changed),
		              NULL, NULL,
		              g_cclosure_marshal_VOID__VOID,
		              G_TYPE_NONE, 0);
	signals[ATTRIBUTES_CHANGED] =
		g_signal_new ("attributes_changed",
		              G_TYPE_FROM_CLASS (class),
		              G_SIGNAL_RUN_LAST,
		              G_STRUCT_OFFSET (GnomeCupsPrinterClass, attributes_changed),
		              NULL, NULL,
		              g_cclosure_marshal_VOID__VOID,
		              G_TYPE_NONE, 0);
	signals[GONE] =
		g_signal_new ("gone",
		              G_TYPE_FROM_CLASS (class),
		              G_SIGNAL_RUN_LAST,
		              G_STRUCT_OFFSET (GnomeCupsPrinterClass, gone),
		              NULL, NULL,
		              g_cclosure_marshal_VOID__VOID,
		              G_TYPE_NONE, 0);
}

GType
gnome_cups_printer_get_type (void)
{
	static GType type = 0;

	if (!type) {
		static const GTypeInfo info = {
			sizeof (GnomeCupsPrinterClass),
			NULL,
			NULL,
			(GClassInitFunc)gnome_cups_printer_class_init,
			NULL,
			NULL,
			sizeof (GnomeCupsPrinter),
			0,
			(GInstanceInitFunc)gnome_cups_printer_instance_init,
		};
		
		type = g_type_register_static (G_TYPE_OBJECT, 
					       "GnomeCupsPrinter",
					       &info, 0);
	}

	return type;
}

void
_gnome_cups_printer_init (void)
{
	static gboolean initialized = FALSE;
	if (initialized) {
		return;
	}
	initialized = TRUE;

	update_printers ();
}

GList *
gnome_cups_printer_get_state_reasons (GnomeCupsPrinter *printer)
{
	GList *l, *result = NULL;
	
	g_return_val_if_fail (GNOME_CUPS_IS_PRINTER (printer), NULL);

	for (l = printer->details->state_reasons; l; l = l->next) {
		const GnomeCupsPrinterReason *src = l->data;
		GnomeCupsPrinterReason       *dest = g_new (GnomeCupsPrinterReason, 1);
		dest->keyword = g_strdup (src->keyword);
		dest->reason  = src->reason;
		result = g_list_prepend (result, dest);
	}

	return g_list_reverse (result);
}

void
gnome_cups_printer_free_reasons (GList *reasons)
{
	GList *l;

	for (l = reasons; l; l = l->next) {
		GnomeCupsPrinterReason *reason = l->data;

		g_free (reason->keyword);
		g_free (reason);
	}
	g_list_free (reasons);
}

gboolean
gnome_cups_printer_print_file (GnomeCupsPrinter *printer,
			       const char       *filename,
			       const char       *job_name,
			       GList            *options,
			       GError          **error)
{
	int num_options;
	int i;
	gboolean success;
	cups_option_t *cups_options;
	GList *l;

	g_return_val_if_fail (printer != NULL, FALSE);
	g_return_val_if_fail (GNOME_CUPS_IS_PRINTER (printer), FALSE);
	g_return_val_if_fail (printer->details != NULL, FALSE);
	g_return_val_if_fail (printer->details->printer_name != NULL, FALSE);

	num_options = g_list_length (options);

	if (num_options > 0) {
		cups_options = g_new0 (cups_option_t, num_options);
		for (l = options, i = 0; l != NULL; l = l->next, i++) {
			GnomeCupsPrinterOption *option = l->data;
			
			cups_options[i].name = option->id;
			cups_options[i].value = option->value;
		}
	} else {
		cups_options = NULL;
	}

	success = cupsPrintFile (printer->details->printer_name,
				 filename,
				 job_name,
				 num_options,
				 cups_options);
	if (!success && error) {
		*error = g_error_new (GNOME_CUPS_ERROR,
				      cupsLastError(),
				      "Print to '%s' failed",
				      printer->details->printer_name);
	}

	g_free (cups_options);

	return success;
}

GnomeCupsPPDFile *
gnome_cups_printer_get_ppd_file (GnomeCupsPrinter *printer)
{
	const char *ppd_name;
	GnomeCupsPPDFile *ppd_file;

	g_return_val_if_fail (GNOME_CUPS_IS_PRINTER (printer), NULL);

	/* NB. Classically broken thread-unsafe API */
	ppd_name = cupsGetPPD (printer->details->printer_name);
	if (ppd_name) {
		ppd_file = (GnomeCupsPPDFile *) g_strdup (ppd_name);
	} else {
		ppd_file = NULL;
	}

	return ppd_file;
}

char *
gnome_cups_ppd_file_get_name (GnomeCupsPPDFile *ppd_file)
{
	g_return_val_if_fail (ppd_file != NULL, NULL);
	return g_strdup (ppd_file->name);
}

void
gnome_cups_ppd_file_release (GnomeCupsPPDFile *ppd_file)
{
	if (ppd_file) {
		unlink (ppd_file->name);
		g_free (ppd_file);
	}
}

void
gnome_cups_printer_force_refresh (GnomeCupsPrinter        *printer,
				  GnomeCupsPrinterRefresh  type)
{
	g_return_if_fail (GNOME_CUPS_IS_PRINTER (printer));

	if (type & GNOME_CUPS_PRINTER_REFRESH_PPD &&
	    printer->details->ppd_options) {
		g_hash_table_destroy (printer->details->ppd_options);
		printer->details->ppd_options = NULL;
	}

	if (type & GNOME_CUPS_PRINTER_REFRESH_OPTIONS &&
	    printer->details->dest_options) {
		printer->details->options_invalid = TRUE;
	}
}

gchar *
_gnome_cups_printer_get_host (GnomeCupsPrinter *printer)
{
	gchar *host = NULL;

#warning this is broken for smb://user:pass@host/printer urls
	if (go_directly_to_printer_when_possible &&
	    printer->details->printer_uri) {
		gchar *x, *y;

		x = strstr (printer->details->printer_uri, "://");

		if (x) {
			x += 3;
			y = strpbrk (x, ":/");
			if (y)
				host = g_strndup (x, y - x);
			else 
				host = g_strdup (x);
		}
	}
	
	return host;
}
