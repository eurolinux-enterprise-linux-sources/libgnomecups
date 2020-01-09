#include <config.h>
#include "util.h"

static void
collect_key_func (gpointer key, gpointer value, gpointer user_data)
{
	GList **l = user_data;
	
	*l = g_list_prepend (*l, key);
}

GList *
gnome_cups_hash_table_keys (GHashTable *hash)
{
	GList *ret = NULL;
	
	g_hash_table_foreach (hash, collect_key_func, &ret);
	
	return ret;
}

static void
collect_value_func (gpointer key, gpointer value, gpointer user_data)
{
	GList **l = user_data;
	
	*l = g_list_prepend (*l, value);
}

GList *
gnome_cups_hash_table_values (GHashTable *hash)
{
	GList *ret = NULL;
	
	g_hash_table_foreach (hash, collect_value_func, &ret);
	
	return ret;
}
