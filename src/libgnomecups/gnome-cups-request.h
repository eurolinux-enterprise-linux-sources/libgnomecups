#ifndef GNOME_CUPS_REQUEST_H
#define GNOME_CUPS_REQUEST_H

#include <cups/ipp.h>
#include <glib.h>
#include "gnome-cups-init.h"
#include "gnome-cups-printer.h"

/**
 * GnomeCupsAsyncRequestCallback:
 * @id: The operation identifier
 * @path: The path used on the server
 * @response: The IPP response from the server, must be freed by the callback
 * @error: A GError, which if set must be freed by the callback
 * @cb_data: user data, which will be automatically freed by
 *           the @destroy_notify passed to #gnome_cups_request_execute_async,
 *           if one was specified.
 *
 * Callback informing the user of the result of an operation.  If an
 * error occurred, @error will be set, and @response will be NULL.
 **/
typedef void (*GnomeCupsAsyncRequestCallback) (guint id,
					       const char *path,
					       ipp_t *response,
					       GError **error,
					       gpointer cb_data);

ipp_t *gnome_cups_request_new                      (int          operation_id);
ipp_t *gnome_cups_request_new_for_printer          (int          operation_id,
						    GnomeCupsPrinter *printer);
ipp_t *gnome_cups_request_new_for_job              (int          operation_id,
						    int          job_id);
void   gnome_cups_request_add_requested_attributes (ipp_t       *request,
						    ipp_tag_t    group,
						    int          n_attributes,
						    char       **attributes);
ipp_t *gnome_cups_request_execute                  (ipp_t       *request,
						    const char  *server,
						    const char  *path,
						    GError     **err);
void  gnome_cups_request_file			   (const char  *server,
						    const char  *path,
						    int fd,
						    GError     **err);
guint gnome_cups_request_execute_async             (ipp_t       *request,
						    const char  *server,
						    const char  *path,
						    GnomeCupsAsyncRequestCallback callback,
						    gpointer cb_data,
						    GDestroyNotify destroy_notify);
guint gnome_cups_request_file_async                (const char  *server,
						    const char  *path,
						    int outfile_fd,
						    GnomeCupsAsyncRequestCallback callback,
						    gpointer cb_data,
						    GDestroyNotify destroy_notify);

void gnome_cups_request_cancel (guint request_id);

/* private */
guint _gnome_cups_outstanding_request_count (void);
void  _gnome_cups_request_init	(GnomeCupsAuthFunction authfn);
void  _gnome_cups_request_shutdown (void);

#endif /* GNOME_CUPS_REQUEST_H */
