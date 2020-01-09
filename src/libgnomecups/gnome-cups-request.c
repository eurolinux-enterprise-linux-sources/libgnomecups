/* vim: set sw=8: -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
#include <config.h>
#include "gnome-cups-request.h"
#include <glib.h>

#include <stdlib.h>
#include <stdio.h>
#include <cups/cups.h>
#include <cups/language.h>
#include <cups/http.h>
#include <cups/ipp.h>

#include "gnome-cups-util.h"
#include "gnome-cups-i18n.h"
#include "gnome-cups-printer.h"

/* Arbitrary. */
#define MAX_REQUEST_THREADS 10
#define STOP_UNUSED_THREADS_TIMEOUT 60
#define CLOSE_UNUSED_CONNECTIONS_TIMEOUT 30

typedef struct
{
	GMutex *mutex;
	gint refcount;
	char *server;
	GTimeVal use_time;
	http_t *http;
} GnomeCupsConnection;

typedef struct
{
	gboolean canceled;
	gboolean direct_callback;
	guint id;
	GnomeCupsConnection *connection;
	
	ipp_t *response;
	GError **error; 
	GnomeCupsAsyncRequestCallback callback;
	gpointer cb_data;
	GDestroyNotify destroy_notify;

	ipp_t *request;
	char *path;
	int output_fd;
} GnomeCupsRequest;

static void request_thread_main (GnomeCupsRequest *request, gpointer unused);
static ipp_t * gnome_cups_request_execute_sync_internal (ipp_t *request,
							 const char *server,
							 const char *path,
							 int output_fd,
							 GError **error);
static guint gnome_cups_request_execute_async_internal (ipp_t      *request, 
							const char *server, 
							const char *path, 
							int output_fd,
							gboolean direct_callback,
							GnomeCupsAsyncRequestCallback callback,
							gpointer cb_data,
							GDestroyNotify destroy_notify);
static void gnome_cups_request_connection_destroy (GnomeCupsConnection *conn);
static gboolean idle_stop_unused_threads (gpointer unused);
static gboolean idle_close_unused_connections (gpointer unused);

/* Should be per thread with push/pop/user_data etc. (clearly) */
static GnomeCupsAuthFunction global_auth = NULL;
static gboolean _gnome_cups_debug = FALSE;

static const char *
cups_password_cb (const char *prompt)
{
	static char *hazard = NULL;

	g_free (hazard);
	hazard = NULL;

	if (global_auth) {
		char *password = NULL;
		char *username = g_strdup (g_get_user_name ());

		if (global_auth (prompt, &username, &password, NULL)) {

			if (username) {
				cupsSetUser (username);
			} else {
				cupsSetUser (g_get_user_name ());
			}
			hazard = password;
		}
		g_free (username);

	} else {
		g_warning ("Cannot prompt for password: '%s'", prompt);
	}

	return hazard;
}

GStaticMutex request_mutex = G_STATIC_MUTEX_INIT;
static guint request_serial_number = 0;
static guint request_system_refcount = 0;
static guint idle_stop_unused_threads_id = 0;
static guint idle_close_unused_connections_id = 0;
static GThreadPool *request_thread_pool;
static GHashTable *request_map = NULL;
static GHashTable *connection_cache_map = NULL;

void
_gnome_cups_request_init (GnomeCupsAuthFunction auth_fn)
{
	GError *error = NULL;
	global_auth = auth_fn;
	cupsSetPasswordCB (cups_password_cb);

	if (!g_thread_supported ())
		g_thread_init (NULL);
	
	g_static_mutex_lock (&request_mutex);
	if (request_system_refcount == 0) {
		request_map = g_hash_table_new (NULL, NULL);
		connection_cache_map = g_hash_table_new_full (g_str_hash, g_str_equal,
							      (GDestroyNotify) g_free,
							      (GDestroyNotify) gnome_cups_request_connection_destroy);
		request_thread_pool = g_thread_pool_new ((GFunc) request_thread_main,
							 NULL,
							 MAX_REQUEST_THREADS,
							 FALSE,
							 &error);
		idle_stop_unused_threads_id = g_timeout_add (STOP_UNUSED_THREADS_TIMEOUT * 1000, (GSourceFunc) idle_stop_unused_threads, NULL);
		idle_close_unused_connections_id = g_timeout_add (CLOSE_UNUSED_CONNECTIONS_TIMEOUT * 1000, (GSourceFunc) idle_close_unused_connections, NULL);
	}
	request_system_refcount++;
	g_static_mutex_unlock (&request_mutex);

	if (error != NULL) {
		g_critical ("Error creating thread pool: %s", error->message);
		_gnome_cups_request_shutdown ();
	}
}

void
_gnome_cups_request_shutdown (void)
{
	g_static_mutex_lock (&request_mutex);
	request_system_refcount--;
	if (request_system_refcount == 0) {
		g_hash_table_destroy (request_map);
		g_hash_table_destroy (connection_cache_map);
		g_source_remove (idle_stop_unused_threads_id);
		g_source_remove (idle_close_unused_connections_id);
		g_thread_pool_free (request_thread_pool, TRUE, TRUE);
	}
	g_static_mutex_unlock (&request_mutex);
}

static gboolean
idle_stop_unused_threads (gpointer unused)
{
	g_static_mutex_lock (&request_mutex);

	if (request_system_refcount == 0) {
		g_static_mutex_unlock (&request_mutex);
		return FALSE;
	}
	g_static_mutex_unlock (&request_mutex);

	g_thread_pool_stop_unused_threads ();

	return TRUE;
}
 
static gboolean
close_unused_connection (const char *server,
			 GnomeCupsConnection *connection,
			 GTimeVal *current_time)
{
	gboolean ret;

	if (!g_mutex_trylock (connection->mutex))
		return FALSE;
	ret = (g_atomic_int_get (&connection->refcount) == 0
	       && (current_time->tv_sec - connection->use_time.tv_sec > 30));
	g_mutex_unlock (connection->mutex);
	return ret;
}

static gboolean
idle_close_unused_connections (gpointer unused)
{
	GTimeVal current_time;

	g_static_mutex_lock (&request_mutex);

	if (request_system_refcount == 0) {
		g_static_mutex_unlock (&request_mutex);
		return FALSE;
	}

	g_get_current_time (&current_time);
	g_hash_table_foreach_remove (connection_cache_map,
				     (GHRFunc) close_unused_connection,
				     &current_time);

	g_static_mutex_unlock (&request_mutex);
	return TRUE;
}

static void
gnome_cups_request_struct_free (GnomeCupsRequest *request)
{
	/* "request->connection" is destroyed indepenently -
	 *   it can be shared between multiple requests.
	 * "request->response" should be freed by the client.
	 * "request->request" has already been freed by the
	 *   CUPS libraries.
	 */
	g_free (request->path);
	g_free (request);
}	

static void
gnome_cups_request_connection_destroy (GnomeCupsConnection *connection)
{
	g_mutex_lock (connection->mutex);
	/* FIXME - we shouldn't do this in the main thread
	 * probably need a cleanup thread to do this
	 */
	if (connection->http)
		httpClose (connection->http);
	g_free (connection->server);
	g_mutex_unlock (connection->mutex);
	g_mutex_free (connection->mutex);
	g_free (connection);
}

static gboolean
idle_signal_request_complete (GnomeCupsRequest *request)
{
	if (!request->canceled && request->callback) {
		request->callback (request->id,
				   request->path,
				   request->response,
				   request->error,
				   request->cb_data);
	} else {
		if (request->response)
			ippDelete (request->response);
	}

	g_static_mutex_lock (&request_mutex);
	g_assert (g_hash_table_remove (request_map, GUINT_TO_POINTER (request->id)));
	g_static_mutex_unlock (&request_mutex);

	if (request->destroy_notify)
		request->destroy_notify (request->cb_data);

	gnome_cups_request_struct_free (request);

	return FALSE;
}

static void
do_signal_complete (GnomeCupsRequest *request)
{
	if (request->direct_callback)
		idle_signal_request_complete (request);
	else
		g_idle_add ((GSourceFunc) idle_signal_request_complete, request);
}

static void
dump_request (ipp_t const *req)
{
	unsigned i;
	ipp_attribute_t *attr;

	for (attr = req->attrs; attr != NULL; attr = attr->next) {
		g_print ("%s", attr->name);
		for (i = 0 ; i < attr->num_values ; i++) {
			g_print ("\t[%d] = ", i);
			switch (attr->value_tag & ~IPP_TAG_COPY) {
			case IPP_TAG_INTEGER:
			case IPP_TAG_ENUM:
				g_print ("%d\n", attr->values[i].integer);
				break;

			case IPP_TAG_STRING:
			case IPP_TAG_TEXT:
			case IPP_TAG_NAME:
			case IPP_TAG_KEYWORD:
			case IPP_TAG_URI:
			case IPP_TAG_CHARSET:
			case IPP_TAG_LANGUAGE:
			case IPP_TAG_MIMETYPE:
				g_print ("'%s'\n", attr->values[i].string.text);
				break;

			case IPP_TAG_BOOLEAN:
				g_print ("%s\n", (int)attr->values[i].boolean ? "true" : "false");
				break;

			default:
				g_print ("unprintable\n");
			}
		}
	}
}

static void
request_thread_main (GnomeCupsRequest *request,
		     G_GNUC_UNUSED gpointer unused)
{
	g_return_if_fail (request != NULL);

	if (request->canceled) {
		do_signal_complete (request);
		return;
	}

	if (_gnome_cups_debug) {
		g_print ("---->>>  locking %p\n", request->connection);
	}
	g_mutex_lock (request->connection->mutex);
	
	if (_gnome_cups_debug && request->request != NULL) {
		g_print ("request = \n");
		dump_request (request->request);
	}

	g_get_current_time (&request->connection->use_time);

	/* This is a deferred open for the first connection */
	if (!request->connection->http)
		request->connection->http = httpConnectEncrypt (request->connection->server, ippPort(), cupsEncryption());

	if (request->request) {		/* IPP request */
		ipp_status_t status;

		request->response = cupsDoRequest (request->connection->http,
			request->request, request->path);

		/* FIXME - not currently threadsafe, but cups returns NULL on
		 * any error.  Thus we just set the status to an internal error
		 * for now.
		 */
		status = cupsLastError ();
		if (request->response == NULL)
			status = IPP_INTERNAL_ERROR;

		if (status > IPP_OK_CONFLICT) {
			g_warning ("IPP request failed with status %d", status);
			if (request->error != NULL)
				*(request->error) = g_error_new (GNOME_CUPS_ERROR, 
								 status, ippErrorString (status));
		} else if (request->response && _gnome_cups_debug) {
			g_print ("response = \n");
			dump_request (request->response);
		}
	} else if (request->output_fd >= 0) {	/* File request */
		http_status_t status = cupsGetFd (request->connection->http,
			request->path, request->output_fd);
		request->response = NULL;
		if (HTTP_OK != status && request->error != NULL)
			*(request->error) = g_error_new (GNOME_CUPS_ERROR, 
							 status, httpStatus (status));
	} else {
		g_warning ("Neither request nor output_fd set");
		if (request->error != NULL)
			*(request->error) = g_error_new (GNOME_CUPS_ERROR, 0,
				"CORRUPT request that lacked both an ipp-request and an output_fd");
	}

	g_atomic_int_dec_and_test (&request->connection->refcount);
	g_mutex_unlock (request->connection->mutex);
	if (_gnome_cups_debug) {
		g_print ("<<<<----- unlocking %p\n", request->connection);
	}

	do_signal_complete (request);
}

guint
_gnome_cups_outstanding_request_count (void)
{
	guint ret;

	g_static_mutex_lock (&request_mutex);
	ret = g_hash_table_size (request_map);
	g_static_mutex_unlock (&request_mutex);

	return ret;
}

ipp_t *
gnome_cups_request_new (int operation_id)
{
	cups_lang_t *language;
	ipp_t *request;
	
	request = ippNewRequest (operation_id);
	
	language = cupsLangDefault ();
	ippAddString (request, IPP_TAG_OPERATION, IPP_TAG_LANGUAGE,
		      "attributes-natural-language", 
		      NULL, language->language);
	cupsLangFree (language);
	
	return request;
}

ipp_t *
gnome_cups_request_new_for_printer (int operation_id, 
				    GnomeCupsPrinter *printer)
{
	ipp_t *request;
	char *printer_uri;

	g_return_val_if_fail (gnome_cups_printer_get_attributes_initialized (printer), NULL);

	printer_uri = g_strdup (gnome_cups_printer_get_uri (printer));
	if (!printer_uri)
		printer_uri = g_strdup_printf ("ipp://localhost/printers/%s",
					       gnome_cups_printer_get_name (printer));
	request = gnome_cups_request_new (operation_id);

	ippAddString (request, IPP_TAG_OPERATION, IPP_TAG_URI,
		      "printer-uri", NULL, printer_uri);
	g_free (printer_uri);

	return request;
}

ipp_t *
gnome_cups_request_new_for_job (int operation_id, int job)
{
	ipp_t *request;
	char *job_uri;
	
	request = gnome_cups_request_new (operation_id);

	job_uri = g_strdup_printf ("ipp://localhost/jobs/%d", job);

	ippAddString (request, IPP_TAG_OPERATION, IPP_TAG_URI,
		      "job-uri", NULL, gnome_cups_strdup (job_uri));

	/* FIXME: need a way to act as another user.  I guess. */
	ippAddString (request, 
		      IPP_TAG_OPERATION,
		      IPP_TAG_NAME, 
		      "requesting-user-name", NULL, 
		      gnome_cups_strdup (g_get_user_name ()));

	g_free (job_uri);

	return request;
}

void
gnome_cups_request_add_requested_attributes (ipp_t *request, 
					     ipp_tag_t group,
					     int n_attributes,
					     char **attributes)
{
	ippAddStrings (request, group,
		       IPP_TAG_NAME | IPP_TAG_COPY,
		       "requested-attributes",
		       n_attributes, NULL, attributes);
}

typedef struct
{
	GMutex *mutex;
	GCond *cond;
	gboolean done;
	ipp_t *response;
	GError **error;
} GnomeCupsAsyncWrapperData;

static void
async_wrapper_cb (guint id, const char *path,
		  ipp_t *response, GError **error,
		  gpointer user_data)
{
	GnomeCupsAsyncWrapperData *data = user_data;
	g_mutex_lock (data->mutex);
	data->done = TRUE;
	data->response = response;
	if (data->error && error && *error)
		g_propagate_error (data->error, *error);
	g_cond_signal (data->cond);
	g_mutex_unlock (data->mutex);
}

ipp_t *
gnome_cups_request_execute (ipp_t *request, const char *server, const char *path, GError **err)
{
	return gnome_cups_request_execute_sync_internal (request, server, path, -1, err);
}

/**
 * gnome_cups_request_file:
 * @server: The hostname of the IPP server to connect to
 * @path: The URI of the file to retrieve 
 * @output_fd: The file descriptor to which to write the retrieved file
 * @err: A #GError for storing errors
 *
 * Retrieve the file named by @path via IPP, writing * the data to @output_fd.
 **/
void
gnome_cups_request_file (const char *server, const char *path,
			 int output_fd,
			 GError **err)
{
	(void) gnome_cups_request_execute_sync_internal (NULL, server, path, output_fd, err);
}

static ipp_t *
gnome_cups_request_execute_sync_internal (ipp_t *request,
					  const char *server,
					  const char *path,
					  int output_fd,
					  GError **err)
{
	guint id;
	GnomeCupsAsyncWrapperData data;

	data.mutex = g_mutex_new ();
	data.cond = g_cond_new ();
	data.done = FALSE;
	data.response = NULL;	
	data.error = err;

	id = gnome_cups_request_execute_async_internal (request, server, path,
							output_fd, TRUE,
							async_wrapper_cb,
							&data,
							NULL);
	if (id > 0) {
		g_mutex_lock (data.mutex);
		while (!data.done)
			g_cond_wait (data.cond, data.mutex);
		g_mutex_unlock (data.mutex);
	}

	g_mutex_free (data.mutex);
	g_cond_free (data.cond);

	return data.response;
}

/**
 * gnome_cups_request_execute_async:
 * @request: An IPP request, allocated via gnome_cups_request_new
 * @server: The hostname of the IPP server to connect to
 * @path: The URI path to execute from
 * @callback: A #GnomeCupsAsyncRequestCallback.
 * @cb_data: Data for the callback
 * @destroy_notify: A function to free the callback data
 * @returns: an operation ID, suitable for passing to gnome_cups_request_cancel
 *
 * Creates a new asynchronous IPP operation, which will invoke @cb_data when
 * complete.
 **/
guint
gnome_cups_request_execute_async (ipp_t      *request, 
				  const char *server, 
				  const char *path, 
				  GnomeCupsAsyncRequestCallback callback,
				  gpointer cb_data,
				  GDestroyNotify destroy_notify)
{
	return gnome_cups_request_execute_async_internal (request, server,
							  path, -1,
							  FALSE, callback,
							  cb_data, destroy_notify);
}

/**
 * gnome_cups_request_file_async:
 * @server: The hostname of the IPP server to connect to
 * @path: The URI of the file to retrieve 
 * @output_fd: The file descriptor to which to write the retrieved file
 * @callback: A #GnomeCupsAsyncRequestCallback.
 * @cb_data: Data for the callback
 * @destroy_notify: A function to free the callback data
 * @returns: an operation ID, suitable for passing to gnome_cups_request_cancel
 *
 * Creates a new asynchronous IPP request to retrieve the specified file, which will
 * invoke @cb_data when complete.  Note that the "response" parameter of the callback
 * will always be #NULL.
 **/
guint
gnome_cups_request_file_async (const char *server, 
			       const char *path, 
			       int output_fd,
			       GnomeCupsAsyncRequestCallback callback,
			       gpointer cb_data,
			       GDestroyNotify destroy_notify)
{
	return gnome_cups_request_execute_async_internal (NULL, server,
							  path, output_fd,
							  FALSE, callback,
							  cb_data, destroy_notify);
}

static guint
gnome_cups_request_execute_async_internal (ipp_t      *request, 
					   const char *server, 
					   const char *path, 
					   int output_fd, 
					   gboolean direct_callback,
					   GnomeCupsAsyncRequestCallback callback,
					   gpointer cb_data,
					   GDestroyNotify destroy_notify)
{
	GnomeCupsConnection *connection;
	GnomeCupsRequest *req;
	gint retval;

	if (!server)
		server = cupsServer();
	if (!path)
		path = "/";

	g_static_mutex_lock (&request_mutex);

	/* Connections are shared between multiple threads; actual
	 * usage of the connection is protected by connection->mutex.
	 */
	if ((connection = g_hash_table_lookup (connection_cache_map, server)) == NULL) {
		connection = g_new0 (GnomeCupsConnection, 1);
		connection->mutex = g_mutex_new ();
		connection->server = g_strdup (server);
		/* Let the thread actually make the HTTP connection */
		connection->http = NULL;
		connection->refcount = 0;
		g_hash_table_insert (connection_cache_map, g_strdup (server),
				     connection);
	}
	g_atomic_int_add (&connection->refcount, 1);
	
	req = g_new0 (GnomeCupsRequest, 1);
	req->connection = connection;
	req->canceled = FALSE;
	req->request = request;
	req->callback = callback;
	req->cb_data = cb_data;
	req->destroy_notify = destroy_notify;
	req->path = g_strdup (path);
	req->output_fd = output_fd;
	req->direct_callback = direct_callback;
	req->error = NULL;

	req->id = ++request_serial_number;

	g_thread_pool_push (request_thread_pool, req, NULL);

	g_hash_table_insert (request_map, GUINT_TO_POINTER (req->id), req);
	retval=req->id;
	g_static_mutex_unlock (&request_mutex);

	return retval;
}

void
gnome_cups_request_cancel (guint request_id)
{
	GnomeCupsRequest *request;
	
	g_static_mutex_lock (&request_mutex);
	if ((request = g_hash_table_lookup (request_map, GUINT_TO_POINTER (request_id))) != NULL) {
		request->canceled = TRUE;
	}
	g_static_mutex_unlock (&request_mutex);
}
