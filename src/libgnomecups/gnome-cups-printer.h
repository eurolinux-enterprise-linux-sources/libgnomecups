#ifndef GNOME_CUPS_PRINTER_H
#define GNOME_CUPS_PRINTER_H

#include <glib.h>
#include <glib-object.h>
#include <cups/ipp.h>
#include <cups/ppd.h>

#include "gnome-cups-queue.h"

G_BEGIN_DECLS

#define GNOME_CUPS_TYPE_PRINTER            (gnome_cups_printer_get_type())
#define GNOME_CUPS_PRINTER(obj)            (G_TYPE_CHECK_INSTANCE_CAST ((obj), GNOME_CUPS_TYPE_PRINTER, GnomeCupsPrinter))
#define GNOME_CUPS_PRINTER_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST ((klass), GNOME_CUPS_TYPE_PRINTER, GnomeCupsPrinterClass))
#define GNOME_CUPS_IS_PRINTER(obj)         (G_TYPE_CHECK_INSTANCE_TYPE ((obj), GNOME_CUPS_TYPE_PRINTER))
#define GNOME_CUPS_IS_PRINTER_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((obj), GNOME_CUPS_TYPE_PRINTER))
#define GNOME_CUPS_PRINTER_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS((obj), GNOME_CUPS_TYPE_PRINTER, GnomeCupsPrinterClass))

typedef struct _GnomeCupsPrinter        GnomeCupsPrinter;
typedef struct _GnomeCupsPPDFile        GnomeCupsPPDFile;
typedef struct _GnomeCupsPrinterClass   GnomeCupsPrinterClass;
typedef struct _GnomeCupsPrinterDetails GnomeCupsPrinterDetails;
typedef struct _GnomeCupsPrinterReason  GnomeCupsPrinterReason;
typedef struct _GnomeCupsPrinterOption  GnomeCupsPrinterOption;
typedef struct _GnomeCupsPrinterOptionChoice GnomeCupsPrinterOptionChoice;

typedef void (*GnomeCupsPrinterAddedCallback) (const char *name, 
					       gpointer user_data);
typedef void (*GnomeCupsPrinterRemovedCallback) (const char *name, 
						 gpointer user_data);
typedef void (*GnomeCupsPrinterGetPPDCallback) (guint id, ppd_file_t *ppd,
						GError **error,
						gpointer user_data);
typedef void (*GnomeCupsOnlyOnceCallback) (gpointer user_data);

struct _GnomeCupsPrinter {
	GObject parent;
	
	GnomeCupsPrinterDetails *details;
};

struct _GnomeCupsPrinterClass {
	GObjectClass parent_class;

	void (*is_default_changed) (GnomeCupsPrinter *printer);
	void (*attributes_changed) (GnomeCupsPrinter *printer);
	void (*gone) (GnomeCupsPrinter *printer);
};

struct _GnomeCupsPrinterOptionChoice {
	char *value;
	char *text;
};

struct _GnomeCupsPrinterReason {
	char *keyword;
	enum {
	  GNOME_CUPS_PRINTER_REASON_ERROR,
	  GNOME_CUPS_PRINTER_REASON_WARNING,
	  GNOME_CUPS_PRINTER_REASON_REPORT
	} reason;
};


struct _GnomeCupsPrinterOption {
	char *id;
	char *text;
	char *value;
	enum {
		GNOME_CUPS_PRINTER_OPTION_BOOLEAN,
		GNOME_CUPS_PRINTER_OPTION_PICK_ONE,
		GNOME_CUPS_PRINTER_OPTION_PICK_MANY
	} type;
	int n_choices;
	GnomeCupsPrinterOptionChoice *choices;
};

typedef enum {
	GNOME_CUPS_PRINTER_REFRESH_PPD     = 0x1,
	GNOME_CUPS_PRINTER_REFRESH_OPTIONS = 0x2,
	/* ... */
	GNOME_CUPS_PRINTER_REFRESH_ALL     = 0xffff
} GnomeCupsPrinterRefresh;

/* All of the printer names */
GList *gnome_cups_get_printers      (void);
void   gnome_cups_printer_list_free (GList *printers);
char  *gnome_cups_get_default       (void);

/* Notifications of printer additions (removals are handled with the
 * gone signal of the GnomeCupsPrinter class */
guint             gnome_cups_printer_new_printer_notify_add    (GnomeCupsPrinterAddedCallback,
								gpointer user_data);
guint             gnome_cups_printer_new_printer_notify_add_only_once (GnomeCupsPrinterAddedCallback,
								       GnomeCupsOnlyOnceCallback,
								       gpointer user_data);
void              gnome_cups_printer_new_printer_notify_remove (guint id);

guint             gnome_cups_printer_printer_removed_notify_add    (GnomeCupsPrinterRemovedCallback,
								    gpointer user_data);
void              gnome_cups_printer_printer_removed_notify_remove (guint id);

/* GnomeCupsPrinter */

GType                   gnome_cups_printer_get_type          (void);
GnomeCupsPrinter  *     gnome_cups_printer_get               (const char              *name);
GnomeCupsPrinter  *     gnome_cups_printer_get_existing      (const char              *name);
void                    gnome_cups_printer_unref             (GnomeCupsPrinter        *printer);
const char *            gnome_cups_printer_get_name          (GnomeCupsPrinter        *printer);
gboolean                gnome_cups_printer_is_gone           (GnomeCupsPrinter        *printer);

gboolean		gnome_cups_printer_get_attributes_initialized (GnomeCupsPrinter *printer);

/* Status and attributes */

const char *            gnome_cups_printer_get_uri           (GnomeCupsPrinter const *printer);
const char *		gnome_cups_printer_get_device_uri    (GnomeCupsPrinter const *printer);

ipp_pstate_t            gnome_cups_printer_get_state         (GnomeCupsPrinter        *printer);
const char        *     gnome_cups_printer_get_state_name    (GnomeCupsPrinter        *printer);
const char        *     gnome_cups_printer_get_full_state    (GnomeCupsPrinter        *printer);
GList             *     gnome_cups_printer_get_state_reasons (GnomeCupsPrinter        *printer);
void                    gnome_cups_printer_free_reasons      (GList                   *reasons);
const char *            gnome_cups_printer_get_description   (GnomeCupsPrinter        *printer);
void                    gnome_cups_printer_set_description   (GnomeCupsPrinter        *printer,
							      const char              *description,
							      GError                 **error);
const char *            gnome_cups_printer_get_location      (GnomeCupsPrinter        *printer);
void                    gnome_cups_printer_set_location      (GnomeCupsPrinter        *printer,
							      const char              *location,
							      GError                 **error);
const char *            gnome_cups_printer_get_make_and_model(GnomeCupsPrinter        *printer);
const char *            gnome_cups_printer_get_info          (GnomeCupsPrinter        *printer);
void                    gnome_cups_printer_get_icon          (GnomeCupsPrinter        *printer,
							      char                   **name,
							      GList                  **emblems);

/* Operations */
void                    gnome_cups_printer_pause             (GnomeCupsPrinter        *printer,
							      GError                 **error);
void                    gnome_cups_printer_resume            (GnomeCupsPrinter        *printer,
							      GError                 **error);
void                    gnome_cups_printer_delete            (GnomeCupsPrinter        *printer,
							      GError                 **error);

/* Default Printers */
gboolean                gnome_cups_printer_get_is_default    (GnomeCupsPrinter        *printer);
void                    gnome_cups_printer_set_default       (GnomeCupsPrinter        *printer,
							      GError                 **error);

gboolean                gnome_cups_printer_get_is_local      (GnomeCupsPrinter        *printer);

/* Queue */
int                     gnome_cups_printer_get_job_count     (GnomeCupsPrinter        *printer);
GnomeCupsQueue    *     gnome_cups_printer_get_queue         (GnomeCupsPrinter        *printer);

/* Configuration */
ppd_file_t        *     gnome_cups_printer_get_ppd           (GnomeCupsPrinter        *printer);
guint			gnome_cups_printer_get_ppd_async     (GnomeCupsPrinter        *printer,
							      GnomeCupsPrinterGetPPDCallback cb,
							      gpointer user_data,
							      GDestroyNotify destroy_notify);
char              *     gnome_cups_printer_get_option_value  (GnomeCupsPrinter        *printer,
							      const char              *id);
void                    gnome_cups_printer_set_option_value  (GnomeCupsPrinter        *printer,
							      const char              *id,
							      const char              *value);
GList                  *gnome_cups_printer_get_options       (GnomeCupsPrinter        *printer);
GnomeCupsPrinterOption *gnome_cups_printer_get_option        (GnomeCupsPrinter        *printer,
							      const char              *id);
void                    gnome_cups_printer_option_free       (GnomeCupsPrinterOption  *option);
void                    gnome_cups_printer_option_list_free  (GList                   *options);

void                    gnome_cups_printer_force_refresh     (GnomeCupsPrinter        *printer,
							      GnomeCupsPrinterRefresh  type);

gboolean                gnome_cups_printer_print_file        (GnomeCupsPrinter        *printer,
							      const char              *filename,
							      const char              *job_name,
							      GList                   *options,
							      GError                 **error);

GnomeCupsPPDFile       *gnome_cups_printer_get_ppd_file      (GnomeCupsPrinter        *printer);
char                   *gnome_cups_ppd_file_get_name         (GnomeCupsPPDFile        *ppd_file);
void                    gnome_cups_ppd_file_release          (GnomeCupsPPDFile        *ppd_file);

/* Private */
void _gnome_cups_printer_init (void);
gchar *_gnome_cups_printer_get_host (GnomeCupsPrinter *printer);

G_END_DECLS
	
#endif
