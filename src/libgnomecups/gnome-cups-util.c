#include <config.h>

#include <cups/cups.h>
#include <cups/http.h>

#include "gnome-cups-util.h"
#include "gnome-cups-request.h"

gboolean
gnome_cups_check_daemon (void) 
{
	http_t *http;
	
	http = httpConnectEncrypt (cupsServer(), ippPort(), cupsEncryption());

	if (http) {
		httpClose (http);
		return TRUE;
	} else {
		return FALSE;
	}
}

GQuark
gnome_cups_error_quark (void)
{
	static GQuark quark = 0;
	
	if (quark == 0) {
		quark = g_quark_from_static_string ("gnome-cups-quark");
	}
	
	return quark;
}

char *
gnome_cups_get_printer_uri (const char *printer_name)
{
	GnomeCupsPrinter *printer;

	if (printer_name) {
		const char *uri;
		printer = gnome_cups_printer_get_existing (printer_name);
		uri = gnome_cups_printer_get_uri (printer);
		g_object_unref (printer);
		return g_strdup (uri);
	} else {
		return g_strdup_printf ("ipp://localhost/printers/");
	}
}

/*****************************************************************************/
/* Internal function lifted from Lifted from glib-2.5.0 */
typedef enum {
  UNSAFE_ALL        = 0x1,  /* Escape all unsafe characters   */
  UNSAFE_ALLOW_PLUS = 0x2,  /* Allows '+'  */
  UNSAFE_PATH       = 0x8,  /* Allows '/', '&', '=', ':', '@', '+', '$' and ',' */
  UNSAFE_HOST       = 0x10, /* Allows '/' and ':' and '@' */
  UNSAFE_SLASHES    = 0x20  /* Allows all characters except for '/' and '%' */
} UnsafeCharacterSet;

static const guchar acceptable[96] = {
  /* A table of the ASCII chars from space (32) to DEL (127) */
  /*      !    "    #    $    %    &    '    (    )    *    +    ,    -    .    / */ 
  0x00,0x3F,0x20,0x20,0x28,0x00,0x2C,0x3F,0x3F,0x3F,0x3F,0x2A,0x28,0x3F,0x3F,0x1C,
  /* 0    1    2    3    4    5    6    7    8    9    :    ;    <    =    >    ? */
  0x3F,0x3F,0x3F,0x3F,0x3F,0x3F,0x3F,0x3F,0x3F,0x3F,0x38,0x20,0x20,0x2C,0x20,0x20,
  /* @    A    B    C    D    E    F    G    H    I    J    K    L    M    N    O */
  0x38,0x3F,0x3F,0x3F,0x3F,0x3F,0x3F,0x3F,0x3F,0x3F,0x3F,0x3F,0x3F,0x3F,0x3F,0x3F,
  /* P    Q    R    S    T    U    V    W    X    Y    Z    [    \    ]    ^    _ */
  0x3F,0x3F,0x3F,0x3F,0x3F,0x3F,0x3F,0x3F,0x3F,0x3F,0x3F,0x20,0x20,0x20,0x20,0x3F,
  /* `    a    b    c    d    e    f    g    h    i    j    k    l    m    n    o */
  0x20,0x3F,0x3F,0x3F,0x3F,0x3F,0x3F,0x3F,0x3F,0x3F,0x3F,0x3F,0x3F,0x3F,0x3F,0x3F,
  /* p    q    r    s    t    u    v    w    x    y    z    {    |    }    ~  DEL */
  0x3F,0x3F,0x3F,0x3F,0x3F,0x3F,0x3F,0x3F,0x3F,0x3F,0x3F,0x20,0x20,0x20,0x3F,0x20
};

static const gchar hex[16] = "0123456789ABCDEF";

/* Note: This escape function works on file: URIs, but if you want to
 * escape something else, please read RFC-2396 */
gchar *
gnome_cups_util_escape_uri_string (const gchar *string, 
				   GnomeCupsUnsafeCharacterSet mask)
{
#define ACCEPTABLE(a) ((a)>=32 && (a)<128 && (acceptable[(a)-32] & use_mask))

  const gchar *p;
  gchar *q;
  gchar *result;
  int c;
  gint unacceptable;
  UnsafeCharacterSet use_mask;
  
  g_return_val_if_fail (mask == UNSAFE_ALL
			|| mask == UNSAFE_ALLOW_PLUS
			|| mask == UNSAFE_PATH
			|| mask == UNSAFE_HOST
			|| mask == UNSAFE_SLASHES, NULL);
  
  unacceptable = 0;
  use_mask = mask;
  for (p = string; *p != '\0'; p++)
    {
      c = (guchar) *p;
      if (!ACCEPTABLE (c)) 
	unacceptable++;
    }
  
  result = g_malloc (p - string + unacceptable * 2 + 1);
  
  use_mask = mask;
  for (q = result, p = string; *p != '\0'; p++)
    {
      c = (guchar) *p;
      
      if (!ACCEPTABLE (c))
	{
	  *q++ = '%'; /* means hex coming */
	  *q++ = hex[c >> 4];
	  *q++ = hex[c & 15];
	}
      else
	*q++ = *p;
    }
  
  *q = '\0';
  
  return result;
}

static int
unescape_character (const char *scanner)
{
  int first_digit;
  int second_digit;

  first_digit = g_ascii_xdigit_value (scanner[0]);
  if (first_digit < 0) 
    return -1;
  
  second_digit = g_ascii_xdigit_value (scanner[1]);
  if (second_digit < 0) 
    return -1;
  
  return (first_digit << 4) | second_digit;
}
static gchar *
g_unescape_uri_string (const char *escaped,
		       int         len,
		       const char *illegal_escaped_characters,
		       gboolean    ascii_must_not_be_escaped)
{
  const gchar *in, *in_end;
  gchar *out, *result;
  int c;
  
  if (escaped == NULL)
    return NULL;

  if (len < 0)
    len = strlen (escaped);

  result = g_malloc (len + 1);
  
  out = result;
  for (in = escaped, in_end = escaped + len; in < in_end; in++)
    {
      c = *in;

      if (c == '%')
	{
	  /* catch partial escape sequences past the end of the substring */
	  if (in + 3 > in_end)
	    break;

	  c = unescape_character (in + 1);

	  /* catch bad escape sequences and NUL characters */
	  if (c <= 0)
	    break;

	  /* catch escaped ASCII */
	  if (ascii_must_not_be_escaped && c <= 0x7F)
	    break;

	  /* catch other illegal escaped characters */
	  if (strchr (illegal_escaped_characters, c) != NULL)
	    break;

	  in += 2;
	}

      *out++ = c;
    }
  
  g_assert (out - result <= len);
  *out = '\0';

  if (in != in_end)
    {
      g_free (result);
      return NULL;
    }

  return result;
}

gchar *
gnome_cups_util_unescape_uri_string (const gchar *escaped)
{
	g_return_val_if_fail (escaped != NULL, NULL);

	return g_unescape_uri_string (escaped, strlen (escaped), "", FALSE);
}
