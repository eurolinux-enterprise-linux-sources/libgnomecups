/* vim: set sw=8: -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/* FIXME: gone printers that come back to life */

#include <config.h>

#include "gnome-cups-queue.h"
#include "gnome-cups-printer.h"

#include <cups/cups.h>

#include "util.h"
#include "gnome-cups-request.h"
#include "gnome-cups-util.h"
#include "gnome-cups-i18n.h"

#define UPDATE_TIMEOUT 3000

struct _GnomeCupsQueueDetails {
	char *queue_name;
	GList *jobs;
	gboolean is_gone;

	guint get_jobs_request_id;
};

enum {
	JOBS_ADDED,
	JOBS_CHANGED,
	JOBS_REMOVED,
	GONE,
	LAST_SIGNAL
};

static GHashTable *queues = NULL;
static guint signals[LAST_SIGNAL];


static const char *job_state_strings[] = 
{
	N_("Unknown"),
	N_("Unknown"),
	N_("Unknown"),
	N_("Pending"),
	N_("Paused"),
	N_("Printing"),
	N_("Stopped"),
	N_("Canceled"),
	N_("Aborted"),
	N_("Completed")
};

static void
compare_queues (GList *old_jobs,
		GList *new_jobs,
		GList **added,
		GList **removed,
		GList **changed)
{
	GHashTable *new_hash;
	GHashTable *old_hash;
	GList *l;
	*added = *removed = *changed = NULL;
	
	new_hash = g_hash_table_new (g_direct_hash, g_direct_equal);
	for (l = new_jobs; l != NULL; l = l->next) {
		GnomeCupsJob *new_job = l->data;
		g_hash_table_insert (new_hash, 
				     GINT_TO_POINTER (new_job->id), new_job);
	}
	old_hash = g_hash_table_new (g_direct_hash, g_direct_equal);
	for (l = old_jobs; l != NULL; l = l->next) {
		GnomeCupsJob *old_job = l->data;
		g_hash_table_insert (old_hash, 
				     GINT_TO_POINTER (old_job->id), old_job);
	}

	for (l = old_jobs; l != NULL; l = l->next) {
		GnomeCupsJob *old_job = l->data;
		GnomeCupsJob *new_job = g_hash_table_lookup (new_hash, 
							     GINT_TO_POINTER (old_job->id));
		if (!new_job) {
			*removed = g_list_prepend (*removed, old_job);
		} else if (!gnome_cups_jobs_equal (old_job, new_job)) {
			*changed = g_list_prepend (*changed, new_job);
		}
	}

	for (l = new_jobs; l != NULL; l = l->next) {
		GnomeCupsJob *new_job = l->data;
		GnomeCupsJob *old_job = g_hash_table_lookup (old_hash, 
							     GINT_TO_POINTER (new_job->id));
		if (!old_job) {
			*added = g_list_prepend (*added, new_job);
		}
	}

	g_hash_table_destroy (old_hash);
	g_hash_table_destroy (new_hash);
}

static int 
strcmp_with_null (const char *a, const char *b)
{
	if (!a && !b) {
		return 0;
	}
	if (a && !b) {
		return -1;
	}
	if (b && !a) {
		return -1;
	}

	return strcmp (a, b);
}

gboolean
gnome_cups_jobs_equal (GnomeCupsJob *a, GnomeCupsJob *b)
{
	if (a->state != b->state) { return FALSE; }
	if (a->size != b->size) { return FALSE; }
	if (strcmp_with_null (a->name, b->name)) { return FALSE; }
	if (strcmp_with_null (a->owner, b->owner)) { return FALSE; }
	if (strcmp_with_null (a->state_reason, b->state_reason)) { return FALSE; }
	if (a->id != b->id) { return FALSE; }
	
	return TRUE;
}


void
gnome_cups_job_free (GnomeCupsJob *job)
{
	g_free (job->state_str);
	g_free (job->state_reason);
	g_free (job->full_state);
	
	g_free (job->name);
	g_free (job->owner);
	g_free (job);
}

GnomeCupsJob *
gnome_cups_job_dup (GnomeCupsJob *job)
{
	GnomeCupsJob *ret = g_new0 (GnomeCupsJob, 1);

	ret->id = job->id;
	ret->name = g_strdup (job->name);
	ret->owner = g_strdup (job->owner);
	ret->state = job->state;
	ret->state_str = g_strdup (job->state_str);
	ret->state_reason  = g_strdup (job->state_reason);
	ret->full_state = g_strdup (job->full_state);
	ret->size = job->size;
	ret->pages = job->pages;
	ret->pages_complete = job->pages_complete;
	ret->creation_time = job->creation_time;
	ret->processing_time = job->processing_time;
	ret->completed_time = job->completed_time;

	return ret;
}

void
gnome_cups_job_list_free (GList *jobs)
{
	GList *l;
	for (l = jobs; l != NULL; l = l->next) {
		gnome_cups_job_free (l->data);
	}
	g_list_free (jobs);
}

static void
finish_job (GnomeCupsJob *job)
{
	const char *str;

	if (!job->name[0]) {
		g_free (job->name);
		job->name = g_strdup (_("Unknown"));
	}
	
	str = (job->state <= IPP_JOB_COMPLETED) ? _(job_state_strings[job->state]) : _("Unknown");
	
	job->state_str = g_strdup (str);
	
	if (job->state_reason && 
	    job->state_reason[0] && 
	    strcmp (job->state_str, job->state_reason)) {
		job->full_state = g_strdup_printf ("%s: %s", 
						   job->state_str,
						   job->state_reason);
	} else {
		job->full_state = g_strdup (job->state_str);
	}

	job->size = job->size * 1024;
}

#define MAP_STR(dest, src) { if (!g_ascii_strcasecmp (attr->name, (src))) { if ((dest) != NULL) g_free (dest); (dest) = g_strdup (attr->values[0].string.text);}}
#define MAP_INT(dest, src) { if (!g_ascii_strcasecmp (attr->name, (src))) { (dest) = attr->values[0].integer; } }

static void
get_jobs_cb (guint id,
	     const char *path,
	     ipp_t *response,
	     GError **error,
	     gpointer cb_data)
{
	GnomeCupsQueue *queue;
	GList *jobs;
	GnomeCupsJob *job;
	ipp_attribute_t *attr;
	GList *old_jobs;
	GList *added_jobs;
	GList *removed_jobs;
	GList *changed_jobs;

	if (error) {
		ippDelete (response);
		g_clear_error (error);
		return;
	}

	queue = GNOME_CUPS_QUEUE (cb_data);

	old_jobs = queue->details->jobs;
	jobs = NULL;
	
	if (response) {
		job = g_new0 (GnomeCupsJob, 1);
		for (attr = response->attrs; attr != NULL; attr = attr->next) {
			if (attr->name == NULL) {
				if (job->name) {
					finish_job (job);
					jobs = g_list_prepend (jobs, job);	
				} else {
					gnome_cups_job_free (job);
				}
				
				job = g_new0 (GnomeCupsJob, 1);
				continue;
			}
			
			if (!g_ascii_strcasecmp (attr->name, "attributes-charset") || !g_ascii_strcasecmp (attr->name, "attributes-charset")) {
				continue;
				
			}
			MAP_STR (job->name, "job-name");
			MAP_INT (job->id, "job-id");
			MAP_STR (job->owner, "job-originating-user-name");
			MAP_INT (job->size, "job-k-octets");
			MAP_INT (job->state, "job-state");
			MAP_STR (job->state_reason, "job-state-reasons");
			MAP_INT (job->pages, "job-sheets");
			MAP_INT (job->pages_complete, "job-media-sheets-completed");
			MAP_INT (job->creation_time, "time-at-creation");
			MAP_INT (job->processing_time, "time-at-processing");
			MAP_INT (job->completed_time, "time-at-completed");
		}
		
		if (job->name) {
			finish_job (job);
			jobs = g_list_prepend (jobs, job);
		} else {
			gnome_cups_job_free (job);
		}
		
		queue->details->jobs = g_list_reverse (jobs);
	
		ippDelete (response);
	}
	
	compare_queues (old_jobs, queue->details->jobs, 
			&added_jobs, &removed_jobs, &changed_jobs);

	if (added_jobs) {
		g_signal_emit (queue, signals[JOBS_ADDED], 0, added_jobs);
		g_list_free (added_jobs);
	} 
	if (changed_jobs) {
		g_signal_emit (queue, signals[JOBS_CHANGED], 0, changed_jobs);
		g_list_free (changed_jobs);
	} 
	if (removed_jobs) {
		g_signal_emit (queue, signals[JOBS_REMOVED], 0, removed_jobs);
		g_list_free (removed_jobs);
	} 

	gnome_cups_job_list_free (old_jobs);

	queue->details->get_jobs_request_id = 0;
}

static void
get_jobs_on_server (GnomeCupsQueue *queue, const char *server)
{
	ipp_t *request;
	const char *printer_name;
	GnomeCupsPrinter *printer;

	if (queue->details->get_jobs_request_id > 0)
		return;

	printer_name = queue->details->queue_name;
	printer = gnome_cups_printer_get_existing (printer_name);
	g_return_if_fail (printer != NULL);

	request = gnome_cups_request_new_for_printer (IPP_GET_JOBS,
						      printer);
	g_object_unref (printer);

	queue->details->get_jobs_request_id =
		gnome_cups_request_execute_async (request, server, "/",
						  get_jobs_cb,
						  g_object_ref (queue),
						  (GDestroyNotify) g_object_unref);
}

static GnomeCupsJob *
gnome_cups_queue_get_job_nocache (GnomeCupsQueue *queue,
				  int             job_id)
{
	GError *error = NULL;
	ipp_t *request;
	ipp_t *response;	
	ipp_attribute_t *attr;
	GnomeCupsPrinter *printer;
	GnomeCupsJob *job;
	char *server;

	printer = gnome_cups_printer_get (queue->details->queue_name);
	
	if (!printer)
		return NULL;

	server = _gnome_cups_printer_get_host (printer);

	g_object_unref (G_OBJECT (printer));
	
	request = gnome_cups_request_new_for_job (IPP_GET_JOB_ATTRIBUTES, 
						  job_id);

	response = gnome_cups_request_execute (request, server, "/", &error);

	if (error) {
		ippDelete (response);
		g_error_free (error);
		return NULL;
	}
	
	job = NULL;

	if (response) {
		job = g_new0 (GnomeCupsJob, 1);
		for (attr = response->attrs; attr != NULL; attr = attr->next) {
			if (attr->name == NULL) {
				if (job->name) {
					finish_job (job);
				} else {
					gnome_cups_job_free (job);
					job = NULL;
				}
				break;
			}
			
			if (!g_ascii_strcasecmp (attr->name, "attributes-charset") || !g_ascii_strcasecmp (attr->name, "attributes-charset")) {
				continue;
			}
			
			MAP_STR (job->name, "job-name");
			MAP_INT (job->id, "job-id");
			MAP_STR (job->owner, "job-originating-user-name");
			MAP_INT (job->size, "job-k-octets");
			MAP_INT (job->state, "job-state");
			MAP_STR (job->state_reason, "job-state-reasons");
			MAP_INT (job->pages, "job-sheets");
			MAP_INT (job->pages_complete, "job-media-sheets-complete");
			MAP_INT (job->creation_time, "time-at-creation");
			MAP_INT (job->processing_time, "time-at-processing");
			MAP_INT (job->completed_time, "time-at-completed");
		}
		
		if (job->name) {
			finish_job (job);
		} else {
			gnome_cups_job_free (job);
			job = NULL;
		}

		ippDelete (response);
	}

	return job;
}

#undef MAP_STR
#undef MAP_INT

static int
find_job_by_id (GnomeCupsJob *job, gpointer job_id_pointer)
{
	int jobid = GPOINTER_TO_INT (job_id_pointer);
	return job->id == jobid ? 0 : 1;
}

GnomeCupsJob *
gnome_cups_queue_get_job (GnomeCupsQueue *queue,
			  int             job_id,
			  gboolean        cache_ok)
{
	if (cache_ok) {
		GList *link = g_list_find_custom (queue->details->jobs,
						  (GCompareFunc) find_job_by_id,
						  GINT_TO_POINTER (job_id));
		if (link)
			return gnome_cups_job_dup (link->data);
	}
	return gnome_cups_queue_get_job_nocache (queue, job_id);
}

static void
update_queue (GnomeCupsQueue *queue)
{
	gchar *printer_host = NULL;
	GnomeCupsPrinter *printer = gnome_cups_printer_get_existing (
		queue->details->queue_name);
	
	if (printer != NULL) {
		printer_host = _gnome_cups_printer_get_host (printer);
		g_object_unref (printer);
	} else
		return;

	get_jobs_on_server (queue,
		(gnome_cups_printer_get_is_local (printer) || printer_host == NULL)
		? NULL : printer_host);
	g_free (printer_host);
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

static void
queue_removed (const char *name)
{
	GnomeCupsQueue *queue;
	
	queue = gnome_cups_queue_get_existing (name);
	if (queue) {
		queue->details->is_gone = TRUE;
		g_signal_emit (queue, signals[GONE], 0);
		g_object_unref (queue);
	}
}

static gboolean
update_queues_timeout (gpointer user_data)
{
	static GList *printer_names;
	GList *old_printer_names;
	GList *l;
	
	/* To avoid unneccessary calls during authentication, check
	 * if a request is currently executing */
	if (_gnome_cups_outstanding_request_count () > 0) {
		return TRUE;
	}

	/* Update the state */

	old_printer_names = printer_names;
	printer_names = gnome_cups_get_printers ();

	for (l = printer_names; l != NULL; l = l->next) {
		GnomeCupsQueue *queue;
		char *name = l->data;
		
		queue = gnome_cups_queue_get_existing (name);
		if (queue) {
			update_queue (queue);
			g_object_unref (queue);
		}
	}

	/* Check for removals */
	for (l = old_printer_names; l != NULL; l = l->next) {
		char *old_name = l->data;
		if (!name_in_list (old_name, printer_names)) {
			queue_removed (old_name);
		}
	}

	gnome_cups_printer_list_free (old_printer_names);

	return TRUE;
}

static void
set_timeout (void)
{
	static guint update_timeout_id = 0;
	gboolean should_timeout;
	
	if (g_hash_table_size (queues) > 0) {
		should_timeout = TRUE;
	} else {
		should_timeout = FALSE;
	}	

	if (should_timeout && !update_timeout_id) {
		update_timeout_id = g_timeout_add (UPDATE_TIMEOUT, 
						   update_queues_timeout,
						   NULL);
	} else if (!should_timeout && update_timeout_id) {
		g_source_remove (update_timeout_id);
		update_timeout_id = 0;
	} 
}

static void
remove_from_queues (gpointer user_data, GObject *object)
{
	char *queue_name = user_data;
	
	g_hash_table_remove (queues, queue_name);
	
	set_timeout ();
}

GnomeCupsQueue *
gnome_cups_queue_get_existing (const char *queue_name)
{
	GnomeCupsQueue *queue;

	if (!queues) {
		queues = g_hash_table_new_full (g_str_hash, 
						g_str_equal,
						g_free,
						NULL);
	} else {
		queue = g_hash_table_lookup (queues, queue_name);
		if (queue) {
			g_object_ref (queue);
			return queue;
		}
	}

	return NULL;
}

static gboolean 
check_exists (const char *name)
{
	gboolean exists;
	GnomeCupsPrinter *printer;

	printer = gnome_cups_printer_get (name);
	exists = printer != NULL;
	g_object_unref (printer);

	return exists;
}

GnomeCupsQueue *
gnome_cups_queue_get (const char *queue_name)
{
	GnomeCupsQueue *queue;
	char *key;

	g_return_val_if_fail (queue_name, NULL);

	queue = gnome_cups_queue_get_existing (queue_name);
	if (queue) {
		return queue;
	}

	if (!check_exists (queue_name)) {
		return NULL;
	}

	queue = g_object_new (GNOME_CUPS_TYPE_QUEUE, NULL);
	queue->details->queue_name = g_strdup (queue_name);
	key = g_strdup (queue_name);
	g_hash_table_insert (queues, key, queue);
	g_object_weak_ref (G_OBJECT (queue), remove_from_queues, key);
	
	update_queue (queue);

	set_timeout ();

	return queue;
}

const char *
gnome_cups_queue_get_name (GnomeCupsQueue *queue)
{
	g_return_val_if_fail (GNOME_CUPS_IS_QUEUE (queue), NULL);
	
	return queue->details->queue_name;
}

gboolean
gnome_cups_queue_is_gone (GnomeCupsQueue *queue)
{
	g_return_val_if_fail (GNOME_CUPS_IS_QUEUE (queue), FALSE);
	
	return queue->details->is_gone;
}

int
gnome_cups_queue_get_job_count (GnomeCupsQueue *queue)
{
	g_return_val_if_fail (GNOME_CUPS_IS_QUEUE (queue), 0);

	return g_list_length (queue->details->jobs);
}

const GList *
gnome_cups_queue_get_jobs (GnomeCupsQueue *queue)
{
	g_return_val_if_fail (GNOME_CUPS_IS_QUEUE (queue), NULL);

	return queue->details->jobs;
}

void               
gnome_cups_queue_pause_job (GnomeCupsQueue *queue, 
			    int job_id,
			    GError **error)
{
	ipp_t *request;
	ipp_t *response;

	g_return_if_fail (GNOME_CUPS_IS_QUEUE (queue));

	request = gnome_cups_request_new_for_job (IPP_HOLD_JOB, job_id);
	response = gnome_cups_request_execute (request, NULL, "/jobs", error);
	ippDelete (response);

	update_queue (queue);
}

void               
gnome_cups_queue_resume_job (GnomeCupsQueue *queue, 
			     int job_id,
			     GError **error)
{
	ipp_t *request;
	ipp_t *response;

	g_return_if_fail (GNOME_CUPS_IS_QUEUE (queue));

	request = gnome_cups_request_new_for_job (IPP_RELEASE_JOB, job_id);
	response = gnome_cups_request_execute (request, NULL, "/jobs", error);
	ippDelete (response);	

	update_queue (queue);
}

void
gnome_cups_queue_cancel_job (GnomeCupsQueue *queue, 
			       int job_id,
			       GError **error)
{
	ipp_t *request;
	ipp_t *response;

	g_return_if_fail (GNOME_CUPS_IS_QUEUE (queue));

	request = gnome_cups_request_new_for_job (IPP_CANCEL_JOB, job_id);
	response = gnome_cups_request_execute (request, NULL, "/jobs", error);
	ippDelete (response);

	update_queue (queue);
}

static void
gnome_cups_queue_finalize (GObject *object)
{
	GnomeCupsQueue *queue = GNOME_CUPS_QUEUE (object);

	if (queue->details->get_jobs_request_id > 0)
		gnome_cups_request_cancel (queue->details->get_jobs_request_id > 0);
	
	if (queue->details->jobs) {
		gnome_cups_job_list_free (queue->details->jobs);
	}

	g_free (queue->details->queue_name);
	g_free (queue->details);
}

static void
gnome_cups_queue_instance_init (GnomeCupsQueue *queue)
{
	queue->details = g_new0 (GnomeCupsQueueDetails, 1);
}

static void
gnome_cups_queue_class_init (GnomeCupsQueueClass *class)
{
	G_OBJECT_CLASS (class)->finalize = gnome_cups_queue_finalize;

	signals[JOBS_ADDED] =
		g_signal_new ("jobs_added",
		              G_TYPE_FROM_CLASS (class),
		              G_SIGNAL_RUN_LAST,
		              G_STRUCT_OFFSET (GnomeCupsQueueClass, jobs_added),
		              NULL, NULL,
		              g_cclosure_marshal_VOID__POINTER,
		              G_TYPE_NONE, 1, G_TYPE_POINTER);
	signals[JOBS_CHANGED] =
		g_signal_new ("jobs_changed",
		              G_TYPE_FROM_CLASS (class),
		              G_SIGNAL_RUN_LAST,
		              G_STRUCT_OFFSET (GnomeCupsQueueClass, jobs_changed),
		              NULL, NULL,
		              g_cclosure_marshal_VOID__POINTER,
		              G_TYPE_NONE, 1, G_TYPE_POINTER);
	signals[JOBS_REMOVED] =
		g_signal_new ("jobs_removed",
		              G_TYPE_FROM_CLASS (class),
		              G_SIGNAL_RUN_LAST,
		              G_STRUCT_OFFSET (GnomeCupsQueueClass, jobs_removed),
		              NULL, NULL,
		              g_cclosure_marshal_VOID__POINTER,
		              G_TYPE_NONE, 1, G_TYPE_POINTER);
	signals[GONE] =
		g_signal_new ("gone",
		              G_TYPE_FROM_CLASS (class),
		              G_SIGNAL_RUN_LAST,
		              G_STRUCT_OFFSET (GnomeCupsQueueClass, gone),
		              NULL, NULL,
		              g_cclosure_marshal_VOID__VOID,
		              G_TYPE_NONE, 0);
}

GType
gnome_cups_queue_get_type (void)
{
	static GType type = 0;

	if (!type) {
		static const GTypeInfo info = {
			sizeof (GnomeCupsQueueClass),
			NULL,
			NULL,
			(GClassInitFunc)gnome_cups_queue_class_init,
			NULL,
			NULL,
			sizeof (GnomeCupsQueue),
			0,
			(GInstanceInitFunc)gnome_cups_queue_instance_init,
		};
		
		type = g_type_register_static (G_TYPE_OBJECT, 
					       "GnomeCupsQueue",
					       &info, 0);
	}

	return type;
}


