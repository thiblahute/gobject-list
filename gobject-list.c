/*
 * gobject-list: a LD_PRELOAD library for tracking the lifetime of GObjects
 *
 * Copyright (C) 2011, 2014  Collabora Ltd.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301
 * USA
 *
 * Authors:
 *     Danielle Madeley  <danielle.madeley@collabora.co.uk>
 *     Philip Withnall  <philip.withnall@collabora.co.uk>
 */
#include <glib-object.h>
#include <gst/gst.h>

#include <dlfcn.h>
#include <signal.h>
#include <string.h>
#include <stdlib.h>

#ifdef HAVE_LIBUNWIND
#define UNW_LOCAL_ONLY
#include <libunwind.h>
#endif

typedef enum
{
  DISPLAY_FLAG_NONE = 0,
  DISPLAY_FLAG_CREATE = 1,
  DISPLAY_FLAG_REFS = 1 << 2,
  DISPLAY_FLAG_BACKTRACE = 1 << 3,
  DISPLAY_FLAG_ALL =
      DISPLAY_FLAG_CREATE | DISPLAY_FLAG_REFS | DISPLAY_FLAG_BACKTRACE,
  DISPLAY_FLAG_DEFAULT = DISPLAY_FLAG_CREATE,
} DisplayFlags;

typedef struct
{
  const gchar *name;
  DisplayFlags flag;
} DisplayFlagsMapItem;

DisplayFlagsMapItem display_flags_map[] =
{
  { "none", DISPLAY_FLAG_NONE },
  { "create", DISPLAY_FLAG_CREATE },
  { "refs", DISPLAY_FLAG_REFS },
  { "backtrace", DISPLAY_FLAG_BACKTRACE },
  { "all", DISPLAY_FLAG_ALL },
};

typedef struct {
  GHashTable *objects;  /* owned */

  /* Those 2 hash tables contains the objects which have been added/removed
   * since the last time we catched the USR2 signal (check point). */
  GHashTable *added;  /* owned */
  /* GObject -> (gchar *) type
   *
   * We keep the string representing the type of the object as we won't be able
   * to get it when displaying later as the object would have been destroyed. */
  GHashTable *removed;  /* owned */
} ObjectData;

/* Global static state, which must be accessed with the @gobject_list mutex
 * held. */
static volatile ObjectData gobject_list_state = { NULL, };

/* Global lock protecting access to @gobject_list_state, since GObject methods
 * may be called from multiple threads concurrently. */
G_LOCK_DEFINE_STATIC (gobject_list);

/* Global output mutex. We don't want multiple threads outputting their
 * backtraces at the same time, otherwise the output becomes impossible to
 * read */
static GMutex output_mutex;


static gboolean
display_filter (DisplayFlags flags)
{
  static DisplayFlags display_flags = DISPLAY_FLAG_DEFAULT;
  static gboolean parsed = FALSE;

  if (!parsed)
    {
      const gchar *display = g_getenv ("GOBJECT_LIST_DISPLAY");

      if (display != NULL)
        {
          gchar **tokens = g_strsplit (display, ",", 0);
          guint len = g_strv_length (tokens);
          guint i = 0;

          /* If there really are items to parse, clear the default flags */
          if (len > 0)
            display_flags = 0;

          for (; i < len; ++i)
            {
              gchar *token = tokens[i];
              guint j = 0;

              for (; j < G_N_ELEMENTS (display_flags_map); ++j)
                {
                  if (!g_ascii_strcasecmp (token, display_flags_map[j].name))
                    {
                      display_flags |= display_flags_map[j].flag;
                      break;
                    }
                }
            }

          g_strfreev (tokens);
        }
      parsed = TRUE;
    }

  return (display_flags & flags) ? TRUE : FALSE;
}

static gboolean
object_filter (const char *obj_name)
{
  const char *filter = g_getenv ("GOBJECT_LIST_FILTER");

	  return FALSE;
  if (filter == NULL)
    return TRUE;
  else
    return (strncmp (filter, obj_name, strlen (filter)) == 0);
}

static void
print_trace (void)
{
#ifdef HAVE_LIBUNWIND
  unw_context_t uc;
  unw_cursor_t cursor;
  guint stack_num = 0;

  if (!display_filter (DISPLAY_FLAG_BACKTRACE))
    return;

  unw_getcontext (&uc);
  unw_init_local (&cursor, &uc);

  while (unw_step (&cursor) > 0)
    {
      gchar name[129];
      unw_word_t off;
      int result;

      result = unw_get_proc_name (&cursor, name, sizeof (name), &off);
      if (result < 0 && result != -UNW_ENOMEM)
        {
          g_print ("Error getting frame: %s (%d)\n",
                   unw_strerror (result), -result);
          break;
        }

      g_print ("#%d  %s + [0x%08x]\n", stack_num++, name, (unsigned int)off);
    }
#endif
}

static void
_dump_object_list (GHashTable *hash)
{
  GHashTableIter iter;
  GObject *obj;

  g_hash_table_iter_init (&iter, hash);
  while (g_hash_table_iter_next (&iter, (gpointer) &obj, NULL))
    {
      /* FIXME: Not really sure how we get to this state. */
      if (obj == NULL || obj->ref_count == 0)
        continue;

      GST_ERROR (" - %" GST_PTR_FORMAT " (%p) : %u refs", obj, obj,
              obj->ref_count);
    }
  g_print ("%u objects\n", g_hash_table_size (hash));
}

static void
_sig_usr1_handler (G_GNUC_UNUSED int signal)
{
  g_print ("Living Objects:\n");

  G_LOCK (gobject_list);
  _dump_object_list (gobject_list_state.objects);
  G_UNLOCK (gobject_list);
}

static void
_sig_usr2_handler (G_GNUC_UNUSED int signal)
{
  GHashTableIter iter;
  gpointer obj, type;

  G_LOCK (gobject_list);

  g_print ("Added Objects:\n");
  _dump_object_list (gobject_list_state.added);

  g_print ("\nRemoved Objects:\n");
  g_hash_table_iter_init (&iter, gobject_list_state.removed);
  while (g_hash_table_iter_next (&iter, &obj, &type))
    {
      GST_ERROR (" - %" GST_PTR_FORMAT "(%p)", obj, obj);
    }
  g_print ("%u objects\n", g_hash_table_size (gobject_list_state.removed));

  g_hash_table_remove_all (gobject_list_state.added);
  g_hash_table_remove_all (gobject_list_state.removed);
  g_print ("\nSaved new check point\n");

  G_UNLOCK (gobject_list);
}

static void
print_still_alive (void)
{
  g_print ("\nStill Alive in %s:\n", g_get_prgname());

  G_LOCK (gobject_list);
  _dump_object_list (gobject_list_state.objects);
  G_UNLOCK (gobject_list);
}

static void
_exiting (void)
{
  print_still_alive ();
}

/* Handle signals which terminate the process. We’re technically not allowed to
 * call printf() from this signal handler, but we do anyway as it’s only a
 * best-effort debugging tool. */
static void
_sig_bad_handler (int sig_num)
{
  signal (sig_num, SIG_DFL);
  print_still_alive ();
  raise (sig_num);
}

static void *
get_func (const char *func_name)
{
  static void *handle = NULL;
  void *func;
  char *error;

  G_LOCK (gobject_list);

  if (G_UNLIKELY (g_once_init_enter (&handle)))
    {
      void *_handle;

      _handle = dlopen("libgobject-2.0.so.0", RTLD_LAZY);

      if (_handle == NULL)
        g_error ("Failed to open libgobject-2.0.so.0: %s", dlerror ());

      /* set up signal handlers */
      signal (SIGUSR1, _sig_usr1_handler);
      signal (SIGUSR2, _sig_usr2_handler);
      signal (SIGINT, _sig_bad_handler);
      signal (SIGTERM, _sig_bad_handler);
      signal (SIGABRT, _sig_bad_handler);
      signal (SIGSEGV, _sig_bad_handler);

      /* set up objects map */
      gobject_list_state.objects = g_hash_table_new (NULL, NULL);
      gobject_list_state.added = g_hash_table_new (NULL, NULL);
      gobject_list_state.removed = g_hash_table_new_full (NULL, NULL, NULL, g_free);

      /* Set up exit handler */
      atexit (_exiting);

      /* Prevent propagation to child processes. */
      if (g_getenv ("GOBJECT_PROPAGATE_LD_PRELOAD") == NULL)
        {
          g_unsetenv ("LD_PRELOAD");
        }

      g_once_init_leave (&handle, _handle);
    }

  func = dlsym (handle, func_name);

  if ((error = dlerror ()) != NULL)
    g_error ("Failed to find symbol: %s", error);

  G_UNLOCK (gobject_list);

  return func;
}

static void
_object_finalized (G_GNUC_UNUSED gpointer data,
    gpointer obj)
{
  G_LOCK (gobject_list);

  if (display_filter (DISPLAY_FLAG_CREATE))
    {
      g_mutex_lock(&output_mutex);


      GST_ERROR (" -- Finalized %" GST_PTR_FORMAT "(%p)", obj, obj);
      print_trace();

      g_mutex_unlock(&output_mutex);

      /* Only care about the object which were already existing during last
       * check point. */
      if (g_hash_table_lookup (gobject_list_state.added, obj) == NULL)
        g_hash_table_insert (gobject_list_state.removed, obj,
            g_strdup (G_OBJECT_TYPE_NAME (obj)));
    }

  g_hash_table_remove (gobject_list_state.objects, obj);
  g_hash_table_remove (gobject_list_state.added, obj);

  G_UNLOCK (gobject_list);
}

gpointer
g_object_new (GType type,
    const char *first,
    ...)
{
  gpointer (* real_g_object_new_valist) (GType, const char *, va_list);
  va_list var_args;
  GObject *obj;
  const char *obj_name;

  real_g_object_new_valist = get_func ("g_object_new_valist");

  va_start (var_args, first);
  obj = real_g_object_new_valist (type, first, var_args);
  va_end (var_args);

  obj_name = G_OBJECT_TYPE_NAME (obj);

  G_LOCK (gobject_list);

  if (g_hash_table_lookup (gobject_list_state.objects, obj) == NULL &&
      object_filter (obj_name))
    {
      if (display_filter (DISPLAY_FLAG_CREATE))
        {
          g_mutex_lock(&output_mutex);

          GST_ERROR (" ++ Created object %" GST_PTR_FORMAT "(%p)", obj, obj);
          print_trace();

          g_mutex_unlock(&output_mutex);
        }

      /* FIXME: For thread safety, GWeakRef should be used here, except it
       * won’t give us notify callbacks. Perhaps an opportunistic combination
       * of GWeakRef and g_object_weak_ref() — the former for safety, the latter
       * for notifications (with the knowledge that due to races, some
       * notifications may get omitted)?
       *
       * Alternatively, we could abuse GToggleRef. Inadvisable because other
       * code could be using it.
       *
       * Alternatively, we could switch to a garbage-collection style of
       * working, where gobject-list runs in its own thread and uses GWeakRefs
       * to keep track of objects. Periodically, it would check the hash table
       * and notify of which references have been nullified. */
      g_object_weak_ref (obj, (GWeakNotify)_object_finalized, NULL);

      g_hash_table_insert (gobject_list_state.objects, obj,
          GUINT_TO_POINTER (TRUE));
      g_hash_table_insert (gobject_list_state.added, obj,
          GUINT_TO_POINTER (TRUE));
    }

  G_UNLOCK (gobject_list);

  return obj;
}

gpointer
g_object_ref (gpointer object)
{
  gpointer (* real_g_object_ref) (gpointer);
  GObject *obj = G_OBJECT (object);
  const char *obj_name;
  guint ref_count;
  GObject *ret;

  real_g_object_ref = get_func ("g_object_ref");

  obj_name = G_OBJECT_TYPE_NAME (obj);

  ref_count = obj->ref_count;
  ret = real_g_object_ref (object);

  if (object_filter (obj_name) && display_filter (DISPLAY_FLAG_REFS))
    {
      g_mutex_lock(&output_mutex);

      GST_ERROR (" +  Reffed object %" GST_PTR_FORMAT "(%p); ref_count: %d -> %d",
          obj, obj, ref_count, ref_count + 1);
      print_trace();

      g_mutex_unlock(&output_mutex);
    }

  return ret;
}

void
g_object_unref (gpointer object)
{
  void (* real_g_object_unref) (gpointer);
  GObject *obj = G_OBJECT (object);
  gint ref_count;
  const char *obj_name;

  real_g_object_unref = get_func ("g_object_unref");

  obj_name = G_OBJECT_TYPE_NAME (obj);
  ref_count = obj->ref_count;

  if (object_filter (obj_name) && display_filter (DISPLAY_FLAG_REFS))
    {
      g_mutex_lock(&output_mutex);

      GST_ERROR (" -  Unreffed object %" GST_PTR_FORMAT "(%p); ref_count: %d -> %d\n",
          obj, obj, ref_count, ref_count - 1);
      print_trace();

      g_mutex_unlock(&output_mutex);
    }

  real_g_object_unref (object);

}

static void *
get_gst_func (const char *func_name)
{
  static void *handle = NULL;
  void *func;
  char *error;

  if (G_UNLIKELY (g_once_init_enter (&handle)))
    {
      void *_handle;

      _handle = dlopen("libgstreamer-1.0.so.0", RTLD_LAZY);

      if (_handle == NULL)
        g_error ("Failed to open libgstreamer-1.0.so.0: %s", dlerror ());

      g_once_init_leave (&handle, _handle);
    }

  func = dlsym (handle, func_name);

  if ((error = dlerror ()) != NULL)
    g_error ("Failed to find symbol: %s", error);

  return func;
}

static gpointer
new_mini_object(GstMiniObject *mini_object)
{
  G_LOCK (gobject_list);
  if (display_filter(DISPLAY_FLAG_CREATE) && object_filter(g_type_name(GST_MINI_OBJECT_TYPE(mini_object)))) {
    GST_ERROR("Created %s(%p)", g_type_name (GST_MINI_OBJECT_TYPE (mini_object)), mini_object);
    print_trace();
  }
  gst_mini_object_weak_ref (mini_object, (GstMiniObjectNotify)_object_finalized, NULL);

  g_hash_table_insert (gobject_list_state.objects, mini_object, GUINT_TO_POINTER (TRUE));
  g_hash_table_insert (gobject_list_state.added, mini_object, GUINT_TO_POINTER (TRUE));
  G_UNLOCK (gobject_list);

  return (gpointer) mini_object;
}

GstBuffer *
gst_buffer_new (void)
{
    GstBuffer * (* real_gst_buffer_new) (void);

    real_gst_buffer_new = get_gst_func("gst_buffer_new");

    return new_mini_object(GST_MINI_OBJECT(real_gst_buffer_new()));
}

GstBuffer *
gst_buffer_new_allocate (GstAllocator * allocator, gsize size,
    GstAllocationParams * params)
{
    GstBuffer * (*real_gst_buffer_new_allocate) (GstAllocator * allocator, gsize size, GstAllocationParams * params);
    real_gst_buffer_new_allocate = get_gst_func("gst_buffer_new_allocate");

    return new_mini_object(GST_MINI_OBJECT(real_gst_buffer_new_allocate (allocator, size, params)));
}

GstBuffer *
gst_buffer_new_wrapped_full (GstMemoryFlags flags, gpointer data,
    gsize maxsize, gsize offset, gsize size, gpointer user_data,
    GDestroyNotify notify)
{
    GstBuffer * (*real_gst_buffer_new_wrapped_full) (GstMemoryFlags flags, gpointer data,
    gsize maxsize, gsize offset, gsize size, gpointer user_data,
    GDestroyNotify notify);

    real_gst_buffer_new_wrapped_full = get_gst_func("gst_buffer_new_wrapped_full");

    return new_mini_object(GST_MINI_OBJECT(real_gst_buffer_new_wrapped_full (flags, data, maxsize, offset, size, user_data, notify)));
}

/* FIXME!!! Why doesn't it override the real function! */
void
gst_mini_object_init (GstMiniObject * mini_object, guint flags, GType type,
    GstMiniObjectCopyFunction copy_func,
    GstMiniObjectDisposeFunction dispose_func,
    GstMiniObjectFreeFunction free_func)
{
  void (*real_gst_mini_object_init)(GstMiniObject * mini_object, guint flags, GType type, GstMiniObjectCopyFunction copy_func, GstMiniObjectDisposeFunction dispose_func, GstMiniObjectFreeFunction free_func);

  GST_ERROR("What ze fucking fuck");
  real_gst_mini_object_init = get_gst_func("gst_mini_object_init");

  if (display_filter(DISPLAY_FLAG_CREATE) && object_filter(g_type_name(GST_MINI_OBJECT_TYPE(mini_object)))) {
      GST_ERROR (" -  create %" GST_PTR_FORMAT " (%p)", mini_object, mini_object);
      print_trace();
      gst_mini_object_weak_ref (mini_object, (GstMiniObjectNotify)_object_finalized, NULL);

      g_hash_table_insert (gobject_list_state.objects, mini_object, GUINT_TO_POINTER (TRUE));
      g_hash_table_insert (gobject_list_state.added, mini_object, GUINT_TO_POINTER (TRUE));
  }

  real_gst_mini_object_init(mini_object, flags, type, copy_func, dispose_func, free_func);
}

void
gst_mini_object_unref (GstMiniObject * mini_object)
{
  void (* real_gst_mini_object_unref) (GstMiniObject * mini_object);

  real_gst_mini_object_unref = get_gst_func("gst_mini_object_unref");

  if (object_filter (g_type_name(GST_MINI_OBJECT_TYPE (mini_object)))) {
      if (display_filter (DISPLAY_FLAG_REFS)) {
        GST_ERROR (" -  Unrefed %p %" GST_PTR_FORMAT "; ref_count: %d -> %d",
                mini_object, mini_object, mini_object->refcount, mini_object->refcount + 1);
        print_trace();
      }
  }

  real_gst_mini_object_unref (mini_object);
}

GstMiniObject *
gst_mini_object_ref (GstMiniObject * mini_object)
{
  GstMiniObject * (* real_gst_mini_object_ref) (GstMiniObject * mini_object);

  real_gst_mini_object_ref = get_gst_func ("gst_mini_object_ref");

  if (object_filter (g_type_name(GST_MINI_OBJECT_TYPE (mini_object)))) {
      if (display_filter(DISPLAY_FLAG_REFS)) {
          GST_ERROR(" -  REF %p %" GST_PTR_FORMAT "; ref_count: %d -> %d",
              mini_object, mini_object, mini_object->refcount, mini_object->refcount + 1);
          print_trace();
      }
  }

  return real_gst_mini_object_ref (mini_object);
}
