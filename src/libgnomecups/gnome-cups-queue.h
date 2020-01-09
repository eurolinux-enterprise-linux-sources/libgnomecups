#ifndef GNOME_CUPS_QUEUE_H
#define GNOME_CUPS_QUEUE_H

#include <glib.h>
#include <glib-object.h>
#include <cups/ipp.h>

G_BEGIN_DECLS

#define GNOME_CUPS_TYPE_QUEUE            (gnome_cups_queue_get_type())
#define GNOME_CUPS_QUEUE(obj)            (G_TYPE_CHECK_INSTANCE_CAST ((obj), GNOME_CUPS_TYPE_QUEUE, GnomeCupsQueue))
#define GNOME_CUPS_QUEUE_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST ((klass), GNOME_CUPS_TYPE_QUEUE, GnomeCupsQueueClass))
#define GNOME_CUPS_IS_QUEUE(obj)         (G_TYPE_CHECK_INSTANCE_TYPE ((obj), GNOME_CUPS_TYPE_QUEUE))
#define GNOME_CUPS_IS_QUEUE_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((obj), GNOME_CUPS_TYPE_QUEUE))
#define GNOME_CUPS_QUEUE_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS((obj), GNOME_CUPS_TYPE_QUEUE, GnomeCupsQueueClass))

typedef struct _GnomeCupsQueue        GnomeCupsQueue;
typedef struct _GnomeCupsQueueClass   GnomeCupsQueueClass;
typedef struct _GnomeCupsQueueDetails GnomeCupsQueueDetails;
typedef struct _GnomeCupsJob          GnomeCupsJob;

struct _GnomeCupsQueue {
	GObject parent;
	
	GnomeCupsQueueDetails *details;
};

struct _GnomeCupsQueueClass {
	GObjectClass parent_class;

	void (*jobs_added) (GnomeCupsQueue *queue,
			    GList *jobs);
	void (*jobs_removed) (GnomeCupsQueue *queue,
			      GList *jobs);
	void (*jobs_changed) (GnomeCupsQueue *queue,
			      GList *jobs);
	void (*gone) (GnomeCupsQueue *queue);
};

struct _GnomeCupsJob {
	int id;
	char *name;
	char *owner;
	ipp_jstate_t state;
	char *state_str;
	char *state_reason;
	char *full_state;
	unsigned long size;
	int pages;
	int pages_complete;
        time_t creation_time;
        time_t processing_time;
        time_t completed_time;
};

gboolean           gnome_cups_jobs_equal          (GnomeCupsJob    *a,
						   GnomeCupsJob    *b);
void               gnome_cups_job_free            (GnomeCupsJob    *job);
GnomeCupsJob *     gnome_cups_job_dup             (GnomeCupsJob    *job);
void               gnome_cups_job_list_free       (GList           *jobs);


/* GnomeCupsQueue */
GType              gnome_cups_queue_get_type      (void);
GnomeCupsQueue    *gnome_cups_queue_get           (const char      *name);
GnomeCupsQueue    *gnome_cups_queue_get_existing  (const char      *name);

const char        *gnome_cups_queue_get_name      (GnomeCupsQueue  *queue);
gboolean           gnome_cups_queue_is_gone       (GnomeCupsQueue  *queue);
const GList       *gnome_cups_queue_get_jobs      (GnomeCupsQueue  *queue);
int                gnome_cups_queue_get_job_count (GnomeCupsQueue  *queue);
GnomeCupsJob      *gnome_cups_queue_get_job       (GnomeCupsQueue  *queue,
						   int              job_id,
						   gboolean	    cache_ok);
void               gnome_cups_queue_pause_job     (GnomeCupsQueue  *queue,
						   int              job_id,
						   GError         **error);
void               gnome_cups_queue_resume_job    (GnomeCupsQueue  *queue,
						   int              job_id,
						   GError         **error);
void               gnome_cups_queue_cancel_job    (GnomeCupsQueue  *queue,
						   int              job_id,
						   GError         **error);

G_END_DECLS
	
#endif

