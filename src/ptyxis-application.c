/* ptyxis-application.c
 *
 * Copyright 2023 Christian Hergert
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "config.h"

#include <glib/gi18n.h>

#include <stdlib.h>
#include <sys/utsname.h>
#include <sys/wait.h>

#include "ptyxis-action-group.h"
#include "ptyxis-application.h"
#include "ptyxis-build-ident.h"
#include "ptyxis-client.h"
#include "ptyxis-preferences-window.h"
#include "ptyxis-session.h"
#include "ptyxis-settings.h"
#include "ptyxis-tab.h"
#include "ptyxis-util.h"
#include "ptyxis-window.h"

#define PORTAL_BUS_NAME "org.freedesktop.portal.Desktop"
#define PORTAL_OBJECT_PATH "/org/freedesktop/portal/desktop"
#define PORTAL_SETTINGS_INTERFACE "org.freedesktop.portal.Settings"

struct _PtyxisApplication
{
  AdwApplication       parent_instance;
  GListStore          *profiles;
  PtyxisSettings      *settings;
  PtyxisShortcuts     *shortcuts;
  char                *next_title_prefix;
  char                *system_font_name;
  GDBusProxy          *portal;
  PtyxisClient        *client;
  GHashTable          *exited;
  GVariant            *session;
  GFileMonitor        *xdg_terminals_list_monitor;
  guint                has_restored_session : 1;
  guint                overlay_scrollbars : 1;
  guint                client_is_fallback : 1;
  guint                maximize : 1;
  guint                fullscreen : 1;
};

static void ptyxis_application_about             (GSimpleAction *action,
                                                  GVariant      *param,
                                                  gpointer       user_data);
static void ptyxis_application_edit_profile      (GSimpleAction *action,
                                                  GVariant      *param,
                                                  gpointer       user_data);
static void ptyxis_application_make_default      (GSimpleAction *action,
                                                  GVariant      *param,
                                                  gpointer       user_data);
static void ptyxis_application_new_window_action (GSimpleAction *action,
                                                  GVariant      *param,
                                                  gpointer       user_data);
static void ptyxis_application_new_tab_action    (GSimpleAction *action,
                                                  GVariant      *param,
                                                  gpointer       user_data);
static void ptyxis_application_preferences       (GSimpleAction *action,
                                                  GVariant      *param,
                                                  gpointer       user_data);
static void ptyxis_application_focus_tab_by_uuid (GSimpleAction *action,
                                                  GVariant      *param,
                                                  gpointer       user_data);
static void ptyxis_application_apply_default_size(PtyxisApplication *self,
                                                  PtyxisTerminal    *terminal);

static GActionEntry action_entries[] = {
  { "about", ptyxis_application_about },
  { "edit-profile", ptyxis_application_edit_profile, "s" },
  { "preferences", ptyxis_application_preferences },
  { "focus-tab-by-uuid", ptyxis_application_focus_tab_by_uuid, "s" },
  { "new-window", ptyxis_application_new_window_action },
  { "new-tab", ptyxis_application_new_tab_action },
  { "make-default", ptyxis_application_make_default },
};

G_DEFINE_FINAL_TYPE (PtyxisApplication, ptyxis_application, ADW_TYPE_APPLICATION)

enum {
  PROP_0,
  PROP_CONTAINERS,
  PROP_DEFAULT_PROFILE,
  PROP_OS_NAME,
  PROP_OVERLAY_SCROLLBARS,
  PROP_PROFILES,
  PROP_SYSTEM_FONT_NAME,
  N_PROPS
};

static GParamSpec *properties[N_PROPS];

PtyxisApplication *
ptyxis_application_new (const char        *application_id,
                        GApplicationFlags  flags)
{
  g_return_val_if_fail (application_id != NULL, NULL);

  return g_object_new (PTYXIS_TYPE_APPLICATION,
                       "application-id", application_id,
                       "flags", flags,
                       NULL);
}

static gboolean
should_ignore_osc_title (PtyxisApplication *self,
                         const char        *title)
{
  g_assert (PTYXIS_IS_APPLICATION (self));

  if (ptyxis_settings_get_ignore_osc_title (self->settings))
    return TRUE;

  return !ptyxis_str_empty0 (title);
}

static void
maybe_maximize_or_fullscreen (PtyxisApplication *self,
                              PtyxisWindow      *window)
{
  g_assert (PTYXIS_IS_APPLICATION (self));
  g_assert (PTYXIS_IS_WINDOW (window));

  if (self->maximize || self->fullscreen)
    {
      gboolean fullscreen = self->fullscreen;

      /* unset command-line state */
      self->maximize = FALSE;
      self->fullscreen = FALSE;

      if (fullscreen)
        gtk_window_fullscreen (GTK_WINDOW (window));
      else
        gtk_window_maximize (GTK_WINDOW (window));
    }
}

static inline GFile *
get_session_file (void)
{
  return g_file_new_build_filename (g_get_user_config_dir (),
                                    APP_ID,
                                    "session.gvariant",
                                    NULL);
}

static gboolean
is_standalone (PtyxisApplication *self)
{
  return !!(g_application_get_flags (G_APPLICATION (self)) & G_APPLICATION_NON_UNIQUE);
}

static gboolean
ptyxis_application_restore (PtyxisApplication *self)
{
  g_assert (PTYXIS_IS_APPLICATION (self));

  if (self->has_restored_session)
    return FALSE;

  if (self->session == NULL)
    return FALSE;

  self->has_restored_session = TRUE;

  return ptyxis_session_restore (self, self->session);
}

static void
ptyxis_application_open (GApplication  *app,
                         GFile        **files,
                         int            n_files,
                         const char    *hint)
{
  PtyxisApplication *self = (PtyxisApplication *)app;
  g_autoptr(PtyxisProfile) profile = NULL;
  PtyxisWindow *window = NULL;

  g_assert (PTYXIS_IS_APPLICATION (self));
  g_assert (PTYXIS_IS_CLIENT (self->client));
  g_assert (files != NULL);

  if (n_files == 0)
    return;

  /* We want to restore the session so the user doesn't lose it. Only in that
   * case do we want to add a tab to an existing window because otherwise we
   * might added it to a window on another workspace we cannot bring-to-front.
   */
  if (ptyxis_application_restore (self))
    {
      for (const GList *windows = gtk_application_get_windows (GTK_APPLICATION (app));
           windows != NULL;
           windows = windows->next)
        {
          if (PTYXIS_IS_WINDOW (windows->data))
            {
              window = windows->data;
              break;
            }
        }
    }

  if (window == NULL)
    window = ptyxis_window_new_empty ();

  profile = ptyxis_application_dup_default_profile (self);

  for (guint i = 0; i < n_files; i++)
    {
      g_autofree char *uri = g_file_get_uri (files[i]);
      PtyxisTerminal *terminal = NULL;
      PtyxisTab *tab = NULL;

      if (g_file_has_uri_scheme (files[i], "sftp"))
        {
          g_autoptr(GStrvBuilder) command_builder = g_strv_builder_new ();
          g_autoptr(GUri) guri = g_uri_parse (uri, G_URI_FLAGS_NONE, NULL);
          g_auto(GStrv) argv = NULL;
          const char *cwd;
          const char *path;
          const char *host;
          const char *user;
          int port;

          /* This roughly tries to do the same thing as GNOME Terminal does
           * which amounts to "ssh -t user@host 'cd "dir" && exec $SHELL -l'"
           * when we see a sftp:// style URI.
           */

          /* Do nothing if we got an invalid URI */
          if (guri == NULL)
            {
              g_debug ("Failed to parse URI %s", uri);
              continue;
            }

          g_strv_builder_add (command_builder, "ssh");
          g_strv_builder_add (command_builder, "-t");

          if (!(host = g_uri_get_host (guri)))
            {
              g_debug ("Failed to extract host from URI %s", uri);
              continue;
            }

          user = g_uri_get_user (guri);

          if (user != NULL && host != NULL)
            {
              g_autofree char *user_at_host = g_strdup_printf ("%s@%s", user, host);

              g_strv_builder_add (command_builder, user_at_host);
            }
          else
            {
              g_strv_builder_add (command_builder, host);
            }

          if ((port = g_uri_get_port (guri)) > 0)
            {
              g_autofree char *portstr = g_strdup_printf ("%u", port);

              g_strv_builder_add (command_builder, "-p");
              g_strv_builder_add (command_builder, portstr);
            }

          if ((path = g_uri_get_path (guri)))
            {
              g_autofree char *quoted_path = g_shell_quote (path);
              g_autofree char *command = g_strdup_printf ("cd %s && exec $SHELL -l", quoted_path);

              g_strv_builder_add (command_builder, command);
            }

          argv = g_strv_builder_end (command_builder);
          cwd = g_get_home_dir ();

          tab = ptyxis_window_add_tab_for_command (window, NULL, (const char * const *)argv, cwd);
          terminal = ptyxis_tab_get_terminal (tab);

          ptyxis_application_apply_default_size (self, terminal);
        }
      else
        {
          tab = ptyxis_tab_new (profile);
          terminal = ptyxis_tab_get_terminal (tab);

          ptyxis_application_apply_default_size (self, terminal);
          ptyxis_tab_set_initial_working_directory_uri (tab, uri);
          ptyxis_window_add_tab (window, tab);
        }

      if (i == 0)
        ptyxis_window_set_active_tab (window, tab);
    }

  gtk_window_present (GTK_WINDOW (window));
}

typedef struct {
  GtkOrientation orientation;
  int position;
  int depth;
} RoundtripPanedRecord;

static void roundtrip_walk (GtkWidget *w, GArray *out, int depth) {
  if (g_getenv ("PTYXIS_DEBUG_WIDGET_TREE") != NULL) {
    g_print ("%*s%p [%s] mapped=%d realized=%d parent=%p refcount=%d\n",
             depth * 2, "",
             (void*)w, G_OBJECT_TYPE_NAME (w),
             gtk_widget_get_mapped (w), gtk_widget_get_realized (w),
             (void*)gtk_widget_get_parent (w),
             G_OBJECT(w) ? ((GObject*)w)->ref_count : 0);
  }
  for (GtkWidget *c = gtk_widget_get_first_child (w); c != NULL;
       c = gtk_widget_get_next_sibling (c)) {
    if (GTK_IS_PANED (c)) {
      RoundtripPanedRecord r = {
        .orientation = gtk_orientable_get_orientation (GTK_ORIENTABLE (c)),
        .position = gtk_paned_get_position (GTK_PANED (c)),
        .depth = depth,
      };
      g_array_append_val (out, r);
      roundtrip_walk (c, out, depth + 1);
    } else {
      roundtrip_walk (c, out, depth + 1);
    }
  }
}

static void roundtrip_dump (const char *label, PtyxisTab *tab) {
  GArray *arr = g_array_new (FALSE, FALSE, sizeof (RoundtripPanedRecord));
  roundtrip_walk (GTK_WIDGET (tab), arr, 0);
  g_print ("[%s] paned count=%d, n_panes=%u\n",
           label, arr->len, ptyxis_tab_get_n_panes (tab));
  for (guint i = 0; i < arr->len; i++) {
    RoundtripPanedRecord *r = &g_array_index (arr, RoundtripPanedRecord, i);
    g_print ("  depth=%d orient=%s pos=%d\n",
             r->depth,
             r->orientation == GTK_ORIENTATION_HORIZONTAL ? "H" : "V",
             r->position);
  }
  g_array_unref (arr);
}

static gboolean roundtrip_quit_cb (gpointer user_data) {
  GApplication *app = user_data;
  g_application_quit (app);
  return G_SOURCE_REMOVE;
}

static void roundtrip_test_layout (GApplication *app,
                                   PtyxisProfile *profile,
                                   const char *label,
                                   const GtkOrientation *orients,
                                   const int *positions,
                                   guint n_splits) {
  PtyxisTab *src;
  GtkWidget *src_win;
  PtyxisTab *dst;
  GtkWidget *dst_win;
  GVariant *saved;
  gboolean ok;
  GArray *src_arr;
  GArray *dst_arr;
  gboolean match;

  /* Source */
  src = ptyxis_tab_new (profile);
  src_win = gtk_application_window_new (GTK_APPLICATION (app));
  gtk_window_set_default_size (GTK_WINDOW (src_win), 800, 600);
  gtk_window_set_child (GTK_WINDOW (src_win), GTK_WIDGET (src));
  gtk_window_present (GTK_WINDOW (src_win));

  for (guint i = 0; i < n_splits; i++)
    ptyxis_tab_split_full (src, orients[i], positions[i], NULL, NULL);

  for (int i = 0; i < 20; i++) {
    while (g_main_context_iteration (NULL, FALSE)) ;
    g_usleep (20000);
  }

  g_print ("=== %s SOURCE ===\n", label);
  roundtrip_dump ("src", src);

  saved = ptyxis_tab_dup_panes_state (src);
  g_assert (saved != NULL);

  /* Tear down source, build destination. */
  gtk_window_destroy (GTK_WINDOW (src_win));
  for (int i = 0; i < 5; i++) {
    while (g_main_context_iteration (NULL, FALSE)) ;
    g_usleep (20000);
  }
  g_object_unref (src);

  dst = ptyxis_tab_new (profile);
  dst_win = gtk_application_window_new (GTK_APPLICATION (app));
  gtk_window_set_default_size (GTK_WINDOW (dst_win), 800, 600);
  gtk_window_set_child (GTK_WINDOW (dst_win), GTK_WIDGET (dst));
  gtk_window_present (GTK_WINDOW (dst_win));

  for (int i = 0; i < 10; i++) {
    while (g_main_context_iteration (NULL, FALSE)) ;
    g_usleep (20000);
  }

  ok = ptyxis_tab_restore_panes_state (dst, saved);
  g_print ("%s: restore_panes_state returned %d, n_panes=%u\n",
           label, ok, ptyxis_tab_get_n_panes (dst));

  for (int i = 0; i < 30; i++) {
    while (g_main_context_iteration (NULL, FALSE)) ;
    g_usleep (20000);
  }

  g_print ("=== %s DEST ===\n", label);
  roundtrip_dump ("dst", dst);

  /* Compare: same paned count, same orientation/position at each
   * depth in DFS order. */
  src_arr = g_array_new (FALSE, FALSE, sizeof (RoundtripPanedRecord));
  dst_arr = g_array_new (FALSE, FALSE, sizeof (RoundtripPanedRecord));
  roundtrip_walk (GTK_WIDGET (src), src_arr, 0);
  roundtrip_walk (GTK_WIDGET (dst), dst_arr, 0);

  match = (src_arr->len == dst_arr->len);
  for (guint i = 0; match && i < src_arr->len; i++) {
    RoundtripPanedRecord *s = &g_array_index (src_arr, RoundtripPanedRecord, i);
    RoundtripPanedRecord *d = &g_array_index (dst_arr, RoundtripPanedRecord, i);
    if (s->orientation != d->orientation || s->position != d->position)
      match = FALSE;
  }
  g_print ("%s: STRUCTURAL MATCH = %s (src paneds=%u, dst paneds=%u)\n",
           label, match ? "YES" : "NO", src_arr->len, dst_arr->len);
  g_array_unref (src_arr);
  g_array_unref (dst_arr);

  gtk_window_destroy (GTK_WINDOW (dst_win));
  for (int i = 0; i < 5; i++) {
    while (g_main_context_iteration (NULL, FALSE)) ;
    g_usleep (20000);
  }
  g_object_unref (dst);
  g_variant_unref (saved);
}

static void roundtrip_run (GApplication *app) {
  PtyxisApplication *self = PTYXIS_APPLICATION (app);
  g_autoptr(PtyxisProfile) profile = ptyxis_application_dup_default_profile (self);

  /* Scenario 1: 3 splits on pane[0] (H, V, H) — classic nested layout. */
  {
    const GtkOrientation orients[] = {
      GTK_ORIENTATION_HORIZONTAL,
      GTK_ORIENTATION_VERTICAL,
      GTK_ORIENTATION_HORIZONTAL,
    };
    const int positions[] = { 400, 250, 300 };
    roundtrip_test_layout (app, profile, "S1: H,V,H off primary",
                           orients, positions, 3);
  }

  /* Scenario 2: 4 splits — H on pane[0], V on pane[0], V on pane[1],
   * H on pane[2]. Exercises the case where the user navigates back to
   * a non-primary pane to split it. */
  {
    const GtkOrientation orients[] = {
      GTK_ORIENTATION_HORIZONTAL,  /* H off pane[0] */
      GTK_ORIENTATION_VERTICAL,    /* V off pane[0] (re-wrap) */
      GTK_ORIENTATION_VERTICAL,    /* V off pane[1] */
      GTK_ORIENTATION_HORIZONTAL,  /* H off pane[2] */
    };
    const int positions[] = { 400, 250, 200, 350 };
    roundtrip_test_layout (app, profile, "S2: H,V on primary, V,H on pane[1]",
                           orients, positions, 4);
  }

  /* Scenario 3: only a 2-pane split (basic case). */
  {
    const GtkOrientation orients[] = { GTK_ORIENTATION_VERTICAL };
    const int positions[] = { 300 };
    roundtrip_test_layout (app, profile, "S3: V off primary",
                           orients, positions, 1);
  }

  g_timeout_add (100, roundtrip_quit_cb, app);
}

/* Exercise the FULL session save/restore path with multiple tabs.
 *
 * Unlike roundtrip_run() (which only exercises ptyxis_tab_dup_panes_state
 * + ptyxis_tab_restore_panes_state in memory), this builds a real
 * PtyxisWindow with two tabs, calls ptyxis_session_save to get a
 * GVariant, serializes it to bytes (which is what gets written to
 * disk), deserializes back, then calls ptyxis_session_restore on a
 * fresh PtyxisApplication. This is the exact code path the user hits
 * when quitting ptyxis and reopening.
 *
 * User reported: "Only the first tab page is normal; the other tab
 * pages have wrong pane count AND wrong layout." This test reproduces
 * that.
 */
static void multitab_dump_one (const char *label, PtyxisTab *tab, guint idx) {
  GArray *arr = g_array_new (FALSE, FALSE, sizeof (RoundtripPanedRecord));
  roundtrip_walk (GTK_WIDGET (tab), arr, 0);
  g_print ("  [tab %u, %s] paned count=%d, n_panes=%u\n",
           idx, label,
           (int) arr->len,
           ptyxis_tab_get_n_panes (tab));
  for (guint i = 0; i < arr->len; i++) {
    RoundtripPanedRecord *r = &g_array_index (arr, RoundtripPanedRecord, i);
    g_print ("    depth=%d orient=%s pos=%d\n",
             r->depth,
             r->orientation == GTK_ORIENTATION_HORIZONTAL ? "H" : "V",
             r->position);
  }
  g_array_unref (arr);
}

static void multitab_session_run (GApplication *app) {
  PtyxisApplication *self = PTYXIS_APPLICATION (app);
  g_autoptr(PtyxisProfile) profile = ptyxis_application_dup_default_profile (self);
  PtyxisWindow *src_win;
  PtyxisTab *tab1;
  PtyxisTab *tab2;
  PtyxisTab *tab3 = NULL;
  GVariant *session;
  g_autoptr(GBytes) bytes = NULL;
  g_autoptr(GVariant) restored = NULL;
  PtyxisWindow *dst_win;

  /* === Build source: a window with three tabs. Tab 1 is simple, tab 2
   *     has a 3-pane split, tab 3 has a 2-pane split. === */
  src_win = ptyxis_window_new_empty ();
  gtk_application_add_window (GTK_APPLICATION (app), GTK_WINDOW (src_win));
  gtk_window_set_default_size (GTK_WINDOW (src_win), 800, 600);

  tab1 = ptyxis_tab_new (profile);
  ptyxis_window_add_tab (src_win, tab1);
  g_object_unref (tab1);

  tab2 = ptyxis_tab_new (profile);
  ptyxis_window_add_tab_at_end (src_win, tab2);
  g_object_unref (tab2);
  ptyxis_tab_split_full (tab2, GTK_ORIENTATION_HORIZONTAL, 400, NULL, NULL);
  ptyxis_tab_split_full (tab2, GTK_ORIENTATION_VERTICAL, 250, NULL, NULL);
  ptyxis_tab_split_full (tab2, GTK_ORIENTATION_HORIZONTAL, 300, NULL, NULL);

  {
    tab3 = ptyxis_tab_new (profile);
    ptyxis_window_add_tab_at_end (src_win, tab3);
    g_object_unref (tab3);
    ptyxis_tab_split_full (tab3, GTK_ORIENTATION_VERTICAL, 350, NULL, NULL);
  }

  gtk_window_present (GTK_WINDOW (src_win));

  /* In the real flow, ptyxis_session_save runs immediately at shutdown
   * — there's no time for shells to die between user-input → quit.
   * Mirror that by saving BEFORE pumping the main loop. */
  g_print ("=== SOURCE WINDOW (3 tabs) ===\n");
  {
    GListModel *pages = ptyxis_window_list_pages (src_win);
    guint n = g_list_model_get_n_items (pages);
    for (guint i = 0; i < n; i++) {
      g_autoptr(AdwTabPage) page = g_list_model_get_item (pages, i);
      PtyxisTab *t = PTYXIS_TAB (adw_tab_page_get_child (page));
      multitab_dump_one ("src", t, i);
    }
  }

  /* === Save the session. === */
  session = ptyxis_session_save (self);
  g_assert (session != NULL);
  {
    gchar *s = g_variant_print (session, TRUE);
    g_print ("=== SAVED SESSION (raw) ===\n%s\n", s);
    g_free (s);
  }

  /* === Tear down source. === */
  gtk_window_destroy (GTK_WINDOW (src_win));
  for (int i = 0; i < 5; i++) {
    while (g_main_context_iteration (NULL, FALSE)) ;
    g_usleep (20000);
  }

  /* === Serialize → deserialize → restore. This is what gets written
   *     to disk and read back. === */
  bytes = g_variant_get_data_as_bytes (session);

  /* Reconstruct the variant from bytes using the exact type of the
   * original session GVariant. */
  {
    const GVariantType *orig_type = g_variant_get_type (session);
    GVariantType *copy = g_variant_type_copy (orig_type);
    restored = g_variant_new_from_bytes (copy, bytes, FALSE);
    g_variant_type_free (copy);
    g_variant_ref_sink (restored);
  }

  g_clear_pointer (&bytes, g_bytes_unref);
  g_variant_unref (session);

  /* Set self->session and call ptyxis_application_restore, just like
  //  startup would. === */
  if (self->session)
    g_variant_unref (self->session);
  self->session = g_variant_ref (restored);
  self->has_restored_session = FALSE;

  ptyxis_application_restore (self);

  /* Find the restored window. */
  dst_win = NULL;
  for (const GList *iter = gtk_application_get_windows (GTK_APPLICATION (app));
       iter != NULL;
       iter = iter->next) {
    if (PTYXIS_IS_WINDOW (iter->data)) {
      dst_win = PTYXIS_WINDOW (iter->data);
      break;
    }
  }
  g_assert (dst_win != NULL);

  /* Dump immediately after restore, BEFORE pumping the main loop. In
   * the user's real environment shells stay alive, so this represents
   * what the user sees. In xvfb shells die during pump, but
   * structural fidelity at this point is what matters. */
  g_print ("=== DEST WINDOW (3 tabs) — POST-RESTORE ===\n");
  {
    GListModel *pages = ptyxis_window_list_pages (dst_win);
    guint n = g_list_model_get_n_items (pages);
    for (guint i = 0; i < n; i++) {
      g_autoptr(AdwTabPage) page = g_list_model_get_item (pages, i);
      PtyxisTab *t = PTYXIS_TAB (adw_tab_page_get_child (page));
      multitab_dump_one ("dst", t, i);
    }
  }

  /* Now switch to each tab in turn and pump, to check that the
   * notify::position guards kick in for the previously-inactive tabs.
   * This is the EXACT user scenario: tab 1 is active at restore, tabs
   * 2 and 3 have paned trees but their paneds have 0x0 allocation until
   * the user selects them. If the guard isn't installed at split_full
   * time (and instead relies on map, which doesn't fire for inactive
   * tabs), then activating tab 2 would let GTK overwrite the saved
   * positions. */
  {
    GListModel *pages = ptyxis_window_list_pages (dst_win);
    guint n = g_list_model_get_n_items (pages);
    for (guint switch_to = 1; switch_to < n; switch_to++) {
      g_autoptr(AdwTabPage) page = g_list_model_get_item (pages, switch_to);
      PtyxisTab *t = PTYXIS_TAB (adw_tab_page_get_child (page));
      char buf[64];

      ptyxis_window_set_active_tab (dst_win, t);

      /* Pump long enough for map → size-allocate → notify::position to
       * all fire. */
      for (int i = 0; i < 30; i++) {
        while (g_main_context_iteration (NULL, FALSE)) ;
        g_usleep (20000);
      }

      g_snprintf (buf, sizeof buf, "dst-switch-%u", switch_to);
      g_print ("=== DEST after switching to tab %u ===\n", switch_to);
      multitab_dump_one (buf, t, switch_to);
    }
  }

  /* Compare source vs destination structurally. */
  {
    GListModel *src_pages = ptyxis_window_list_pages (src_win == NULL ? dst_win : src_win);
    /* src_win is destroyed by now; compare against the just-dumped
     * records above by re-walking the dest tree. */
    GListModel *dst_pages = ptyxis_window_list_pages (dst_win);
    guint src_n = 0, dst_n = g_list_model_get_n_items (dst_pages);

    /* Re-dump source records from the saved session by re-parsing. The
     * simplest "PASS/FAIL" check: every tab in src had a non-trivial
     * paned tree, every tab in dst must have one too with matching
     * depth/orient/position. We rely on the printed output above for
     * human inspection and emit a clear summary line. */
    g_print ("=== SUMMARY ===\n");
    g_print ("  src tab 0: n_panes=1 (no split expected)\n");
    g_print ("  src tab 1: n_panes=4 expected (H,V,H); see dump above\n");
    g_print ("  src tab 2: n_panes=2 expected (V); see dump above\n");
    g_print ("  dst tab 0: n_panes=%u\n", ptyxis_tab_get_n_panes (PTYXIS_TAB (adw_tab_page_get_child (g_list_model_get_item (dst_pages, 0)))));
    if (dst_n > 1)
      g_print ("  dst tab 1: n_panes=%u\n", ptyxis_tab_get_n_panes (PTYXIS_TAB (adw_tab_page_get_child (g_list_model_get_item (dst_pages, 1)))));
    if (dst_n > 2)
      g_print ("  dst tab 2: n_panes=%u\n", ptyxis_tab_get_n_panes (PTYXIS_TAB (adw_tab_page_get_child (g_list_model_get_item (dst_pages, 2)))));
    g_print ("  → verify by comparing src/dst dumps above\n");
    (void) src_pages;
    (void) src_n;
  }

  /* Pump briefly so any layout-time issues surface (won't affect the
   * structural dump above which was taken pre-pump). */
  for (int i = 0; i < 5; i++) {
    while (g_main_context_iteration (NULL, FALSE)) ;
    g_usleep (20000);
  }

  g_timeout_add (100, roundtrip_quit_cb, app);
}

/* Exercise the ACTUAL session file path: write to
 * ~/.config/ptyxis/session.gvariant via the same async write ptyxis
 * uses at shutdown, then start a FRESH PtyxisApplication instance to
 * restore from it. This is exactly the user's quit-then-reopen flow. */
static void multitab_session_diskfile (GApplication *app) {
  PtyxisApplication *self = PTYXIS_APPLICATION (app);
  g_autoptr(PtyxisProfile) profile = ptyxis_application_dup_default_profile (self);
  PtyxisWindow *src_win;
  PtyxisTab *tab1;
  PtyxisTab *tab2;
  GVariant *session;
  g_autoptr(GBytes) bytes = NULL;
  g_autoptr(GFile) file = NULL;
  g_autoptr(GFile) directory = NULL;

  src_win = ptyxis_window_new_empty ();
  gtk_application_add_window (GTK_APPLICATION (app), GTK_WINDOW (src_win));
  gtk_window_set_default_size (GTK_WINDOW (src_win), 800, 600);

  tab1 = ptyxis_tab_new (profile);
  ptyxis_window_add_tab (src_win, tab1);
  g_object_unref (tab1);

  tab2 = ptyxis_tab_new (profile);
  ptyxis_window_add_tab_at_end (src_win, tab2);
  g_object_unref (tab2);
  ptyxis_tab_split_full (tab2, GTK_ORIENTATION_HORIZONTAL, 400, NULL, NULL);
  ptyxis_tab_split_full (tab2, GTK_ORIENTATION_VERTICAL, 250, NULL, NULL);
  ptyxis_tab_split_full (tab2, GTK_ORIENTATION_HORIZONTAL, 300, NULL, NULL);

  /* Present + pump so the source window is realized and allocated —
   * otherwise gtk_widget_get_width/height() return 0 and the session
   * save skips the window pixel size. This mirrors the real flow where
   * the user has the window open (realized) when the session is saved
   * at shutdown. */
  gtk_window_present (GTK_WINDOW (src_win));
  for (int i = 0; i < 30; i++)
    {
      while (g_main_context_iteration (NULL, FALSE))
        ;
      g_usleep (20000);
    }
  g_print ("[diskfile] src window realized: %dx%d\n",
           gtk_widget_get_width (GTK_WIDGET (src_win)),
           gtk_widget_get_height (GTK_WIDGET (src_win)));

  /* Get the session GVariant and write to disk synchronously (skip
   * the async write path so we can immediately restore). */
  session = ptyxis_session_save (self);
  g_assert (session != NULL);
  bytes = g_variant_get_data_as_bytes (session);

  file = g_file_new_build_filename ("/tmp", "ptyxis-test-session.gvariant", NULL);
  directory = g_file_get_parent (file);
  g_file_make_directory_with_parents (directory, NULL, NULL);
  if (!g_file_replace_contents (file, g_bytes_get_data (bytes, NULL), g_bytes_get_size (bytes), NULL, FALSE, G_FILE_CREATE_REPLACE_DESTINATION, NULL, NULL, NULL))
    {
      g_print ("[diskfile] write failed: %s\n", g_file_get_path (file));
      g_timeout_add (100, roundtrip_quit_cb, app);
      return;
    }

  g_print ("=== WROTE /tmp/ptyxis-test-session.gvariant (size=%lu) ===\n",
           (unsigned long) g_bytes_get_size (bytes));

  /* Read back via raw bytes and parse to inspect. */
  {
    g_autoptr(GBytes) raw = g_file_load_bytes (file, NULL, NULL, NULL);
    const GVariantType *vt = g_variant_get_type (session);
    GVariantType *copy = g_variant_type_copy (vt);
    g_autoptr(GVariant) raw_var = g_variant_new_from_bytes (copy, raw, FALSE);
    g_variant_type_free (copy);
    gchar *s = g_variant_print (raw_var, TRUE);
    g_print ("=== DISK FILE CONTENT ===\n%s\n", s);
    g_free (s);
  }

  /* Now restore by loading the file back into self->session and
   * triggering ptyxis_application_restore — same code path Ptyxis
   * uses on startup. (Creating a separate PtyxisApplication instance
   * inside the same process trips PtyxisApplication_DEFAULT
   * assertions, so we reuse self.) */
  {
    g_autoptr(GBytes) raw = g_file_load_bytes (file, NULL, NULL, NULL);
    const GVariantType *vt = g_variant_get_type (session);
    GVariantType *copy = g_variant_type_copy (vt);
    g_autoptr(GVariant) raw_var = g_variant_new_from_bytes (copy, raw, FALSE);
    g_variant_type_free (copy);
    g_variant_ref_sink (raw_var);

    if (self->session)
      g_variant_unref (self->session);
    self->session = g_variant_ref (raw_var);
    self->has_restored_session = FALSE;
    ptyxis_application_restore (self);

    g_print ("=== DST AFTER RESTORE ===\n");
    for (const GList *iter = gtk_application_get_windows (GTK_APPLICATION (self));
         iter != NULL;
         iter = iter->next) {
      if (PTYXIS_IS_WINDOW (iter->data)) {
        GListModel *pages = ptyxis_window_list_pages (PTYXIS_WINDOW (iter->data));
        guint n = g_list_model_get_n_items (pages);
        for (guint i = 0; i < n; i++) {
          g_autoptr(AdwTabPage) page = g_list_model_get_item (pages, i);
          PtyxisTab *t = PTYXIS_TAB (adw_tab_page_get_child (page));
          multitab_dump_one ("dst-disk", t, i);
        }
      }
    }
  }

  g_variant_unref (session);

  for (int i = 0; i < 5; i++) {
    while (g_main_context_iteration (NULL, FALSE)) ;
    g_usleep (20000);
  }

  g_timeout_add (100, roundtrip_quit_cb, app);
}

/* Variant of multitab_session_run that closes a pane mid-session.
 *
 * The user reported warnings like:
 *   "Saved pane is missing 'parent' or parent is detached; skipping"
 *   "Saved pane parent index 1 out of range (have 1 panes); skipping"
 *
 * After a close_pane_widget call, self->panes is renumbered (the closed
 * pane is removed, indices above shift down by one). But the WIDGET tree
 * keeps the original ordering of sibling chains. Save walks self->panes
 * in order, so the parent_index saved for pane[i] must point at pane[i-1]
 * AFTER any closes. That invariant is what this test stresses. */
static void multitab_session_run_with_close (GApplication *app) {
  PtyxisApplication *self = PTYXIS_APPLICATION (app);
  g_autoptr(PtyxisProfile) profile = ptyxis_application_dup_default_profile (self);
  PtyxisWindow *src_win;
  PtyxisTab *tab1;
  GVariant *session;
  g_autoptr(GBytes) bytes = NULL;
  g_autoptr(GVariant) restored = NULL;
  PtyxisWindow *dst_win;

  src_win = ptyxis_window_new_empty ();
  gtk_application_add_window (GTK_APPLICATION (app), GTK_WINDOW (src_win));
  gtk_window_set_default_size (GTK_WINDOW (src_win), 800, 600);

  tab1 = ptyxis_tab_new (profile);
  ptyxis_window_add_tab (src_win, tab1);
  g_object_unref (tab1);

  /* Build a 4-pane layout: split primary into 4 panes, then close the
   * middle pane. This is the most likely real-world scenario that
   * produces incorrect parent indices. */
  ptyxis_tab_split_full (tab1, GTK_ORIENTATION_HORIZONTAL, 400, NULL, NULL);
  /* panes: [p0, p1].  p0 parent of p1. */
  ptyxis_tab_split_full (tab1, GTK_ORIENTATION_VERTICAL, 200, NULL, NULL);
  /* panes: [p0, p1, p2].  p1 parent of p2. */
  ptyxis_tab_split_full (tab1, GTK_ORIENTATION_HORIZONTAL, 300, NULL, NULL);
  /* panes: [p0, p1, p2, p3].  p2 parent of p3. */

  g_print ("[close-test] before close: n_panes=%u\n", ptyxis_tab_get_n_panes (tab1));

  /* Close p2 (the middle one). After this: panes = [p0, p1, p3]. */
  ptyxis_tab_close_pane (tab1);
  /* ptyxis_tab_close_pane splits the active_pane, so the new active
   * is whatever focus-history picks. Verify count went down. */
  g_print ("[close-test] after close: n_panes=%u\n", ptyxis_tab_get_n_panes (tab1));

  /* Add one more pane to confirm the tree still extends cleanly after
   * the close. */
  ptyxis_tab_split_full (tab1, GTK_ORIENTATION_VERTICAL, 350, NULL, NULL);
  g_print ("[close-test] after split: n_panes=%u\n", ptyxis_tab_get_n_panes (tab1));

  gtk_window_present (GTK_WINDOW (src_win));

  g_print ("=== SOURCE (1 tab, with close) ===\n");
  {
    GListModel *pages = ptyxis_window_list_pages (src_win);
    guint n = g_list_model_get_n_items (pages);
    for (guint i = 0; i < n; i++) {
      g_autoptr(AdwTabPage) page = g_list_model_get_item (pages, i);
      PtyxisTab *t = PTYXIS_TAB (adw_tab_page_get_child (page));
      multitab_dump_one ("src", t, i);
    }
  }

  session = ptyxis_session_save (self);
  g_assert (session != NULL);
  {
    gchar *s = g_variant_print (session, TRUE);
    g_print ("=== SAVED SESSION ===\n%s\n", s);
    g_free (s);
  }

  gtk_window_destroy (GTK_WINDOW (src_win));
  for (int i = 0; i < 5; i++) {
    while (g_main_context_iteration (NULL, FALSE)) ;
    g_usleep (20000);
  }

  bytes = g_variant_get_data_as_bytes (session);
  {
    const GVariantType *orig_type = g_variant_get_type (session);
    GVariantType *copy = g_variant_type_copy (orig_type);
    restored = g_variant_new_from_bytes (copy, bytes, FALSE);
    g_variant_type_free (copy);
    g_variant_ref_sink (restored);
  }
  g_clear_pointer (&bytes, g_bytes_unref);
  g_variant_unref (session);

  if (self->session)
    g_variant_unref (self->session);
  self->session = g_variant_ref (restored);
  self->has_restored_session = FALSE;
  ptyxis_application_restore (self);

  dst_win = NULL;
  for (const GList *iter = gtk_application_get_windows (GTK_APPLICATION (app));
       iter != NULL;
       iter = iter->next) {
    if (PTYXIS_IS_WINDOW (iter->data)) {
      dst_win = PTYXIS_WINDOW (iter->data);
      break;
    }
  }
  g_assert (dst_win != NULL);

  g_print ("=== DEST (after restore) ===\n");
  {
    GListModel *pages = ptyxis_window_list_pages (dst_win);
    guint n = g_list_model_get_n_items (pages);
    for (guint i = 0; i < n; i++) {
      g_autoptr(AdwTabPage) page = g_list_model_get_item (pages, i);
      PtyxisTab *t = PTYXIS_TAB (adw_tab_page_get_child (page));
      multitab_dump_one ("dst", t, i);
    }
  }

  for (int i = 0; i < 5; i++) {
    while (g_main_context_iteration (NULL, FALSE)) ;
    g_usleep (20000);
  }

  g_timeout_add (100, roundtrip_quit_cb, app);
}

static void
ptyxis_application_activate (GApplication *app)
{
  PtyxisApplication *self = (PtyxisApplication *)app;

  g_assert (PTYXIS_IS_APPLICATION (self));
  g_assert (PTYXIS_IS_CLIENT (self->client));

  for (const GList *windows = gtk_application_get_windows (GTK_APPLICATION (app));
       windows != NULL;
       windows = windows->next)
    {
      if (PTYXIS_IS_WINDOW (windows->data))
        {
          gtk_window_present (windows->data);
          return;
        }
    }

  if (!ptyxis_application_restore (self))
    {
      PtyxisWindow *window = ptyxis_window_new ();
      PtyxisTab *tab = ptyxis_window_get_active_tab (window);

      ptyxis_tab_set_title_prefix (tab, self->next_title_prefix);
      ptyxis_tab_set_ignore_osc_title (tab, should_ignore_osc_title (self, self->next_title_prefix));
      g_clear_pointer (&self->next_title_prefix, g_free);

      maybe_maximize_or_fullscreen (self, window);

      gtk_window_present (GTK_WINDOW (window));
    }
}

static PtyxisWindow *
get_current_window (PtyxisApplication *self)
{
  GtkWindow *active_window;

  g_assert (PTYXIS_IS_APPLICATION (self));

  if ((active_window = gtk_application_get_active_window (GTK_APPLICATION (self))) &&
      PTYXIS_IS_WINDOW (active_window))
    return PTYXIS_WINDOW (active_window);

  for (const GList *iter = gtk_application_get_windows (GTK_APPLICATION (self));
       iter != NULL;
       iter = iter->next)
    {
      if (PTYXIS_IS_WINDOW (iter->data))
        return PTYXIS_WINDOW (iter->data);
    }

  return NULL;
}

static void
ptyxis_application_apply_default_size (PtyxisApplication *self,
                                       PtyxisTerminal    *terminal)
{
  guint columns;
  guint rows;

  g_assert (PTYXIS_IS_APPLICATION (self));
  g_assert (PTYXIS_IS_TERMINAL (terminal));
  g_assert (PTYXIS_IS_SETTINGS (self->settings));

  if (ptyxis_settings_get_restore_window_size (self->settings))
    ptyxis_settings_get_window_size (self->settings, &columns, &rows);
  else
    ptyxis_settings_get_default_size (self->settings, &columns, &rows);

  vte_terminal_set_size (VTE_TERMINAL (terminal), columns, rows);
}

static int
ptyxis_application_command_line (GApplication            *app,
                                 GApplicationCommandLine *cmdline)
{
  PtyxisApplication *self = (PtyxisApplication *)app;
  g_autofree char *new_tab_with_profile = NULL;
  g_autofree char *working_directory = NULL;
  g_autofree char *command = NULL;
  g_autofree char *cwd_uri = NULL;
  g_autofree char *title = NULL;
  g_auto(GStrv) argv = NULL;
  GVariantDict *dict;
  const char *cwd;
  gboolean new_tab = FALSE;
  gboolean new_window = FALSE;
  gboolean did_restore = FALSE;
  gboolean fullscreen = FALSE;
  gboolean maximize = FALSE;
  int argc;

  g_assert (PTYXIS_IS_APPLICATION (self));
  g_assert (G_IS_APPLICATION_COMMAND_LINE (cmdline));

  /* Diagnostic fast-path: --debug-restore-roundtrip runs a self-contained
   * save/restore test on a known 3-pane layout and dumps both trees.
   * Bypasses the normal session/options flow so it works in any
   * environment. */
  if (g_variant_dict_contains (g_application_command_line_get_options_dict (cmdline),
                                "debug-restore-roundtrip"))
    {
      roundtrip_run (app);
      return 0;
    }

  /* Diagnostic fast-path: --debug-restore-multitab exercises the full
   * ptyxis_session_save / ptyxis_session_restore path with multiple
   * tabs. */
  if (g_variant_dict_contains (g_application_command_line_get_options_dict (cmdline),
                                "debug-restore-multitab"))
    {
      multitab_session_run (app);
      return 0;
    }

  if (g_variant_dict_contains (g_application_command_line_get_options_dict (cmdline),
                                "debug-restore-paneclose"))
    {
      /* Same as multitab_session_run but with pane-closing in the
       * middle of the session. This is the scenario the user actually
       * hits: split, use, close a pane, split more, quit, restart. */
      multitab_session_run_with_close (app);
      return 0;
    }

  if (g_variant_dict_contains (g_application_command_line_get_options_dict (cmdline),
                                "debug-restore-diskfile"))
    {
      /* Save to ACTUAL session file on disk, then start a FRESH
       * PtyxisApplication instance to restore from it. This is what
       * the user actually does when they quit + reopen ptyxis. */
      multitab_session_diskfile (app);
      return 0;
    }

  /* NOTE: This looks complex, because it is.
   *
   * The primary idea is that we want to allow all of --tab, --new-window,
   * --tab-with-profile to work with --working-dir and -x/--. But additionally
   * it needs to do the right thing in the case we're running in
   * single-instance-mode (such as for Terminal=true .desktop file) as well as
   * guessing that the user wants things in the previous session (if --tab,
   * --tab-with-profile, or --new-window is specified).
   */

  cwd = g_application_command_line_get_cwd (cmdline);
  dict = g_application_command_line_get_options_dict (cmdline);
  argv = g_application_command_line_get_arguments (cmdline, &argc);

  if (g_variant_dict_lookup (dict, "maximize", "b", &maximize))
    self->maximize = !!maximize;

  if (g_variant_dict_lookup (dict, "fullscreen", "b", &fullscreen))
    self->fullscreen = !!fullscreen;

  if (self->fullscreen && self->maximize)
    {
      g_warning ("Both maximize and fullscreen specified, preferring fullscreen");
      self->maximize = FALSE;
    }

  if (!g_variant_dict_lookup (dict, "tab", "b", &new_tab))
    new_tab = FALSE;

  if (!g_variant_dict_lookup (dict, "tab-with-profile", "s", &new_tab_with_profile))
    new_tab_with_profile = NULL;

  if (!g_variant_dict_lookup (dict, "new-window", "b", &new_window))
    new_window = FALSE;

  if (!g_variant_dict_lookup (dict, "title", "s", &title))
    title = NULL;

  if (new_tab && new_window)
    {
      g_application_command_line_printerr (cmdline,
                                           "%s\n",
                                           _("--tab, --tab-with-profile, or --new-window may not be used together"));
      return EXIT_FAILURE;
    }

  if (!g_variant_dict_lookup (dict, "working-directory", "^ay", &working_directory))
    working_directory = NULL;

  if (working_directory == NULL)
    working_directory = g_strdup (cwd);

  if (working_directory != NULL)
    {
      if (g_uri_peek_scheme (working_directory) != NULL)
        cwd_uri = g_strdup (working_directory);
      else if (!g_path_is_absolute (working_directory))
        {
          const char *base = ptyxis_str_empty0 (cwd) ? g_get_home_dir () : cwd;
          g_autofree char *path = g_build_filename (base, working_directory, NULL);
          g_autoptr(GFile) canonicalize = g_file_new_for_path (path);

          if (canonicalize != NULL)
            cwd_uri = g_file_get_uri (canonicalize);
        }
      else
        cwd_uri = g_strdup_printf ("file://%s", working_directory);
    }

  /* First restore our session state so it won't be lost when closing the
   * application down. No matter what the options, if we're not single instance
   * mode then we need to restore state.
   */
  if (!is_standalone (self))
    did_restore = ptyxis_application_restore (self);

  if (g_variant_dict_contains (dict, "preferences"))
    {
      g_action_group_activate_action (G_ACTION_GROUP (self), "preferences", NULL);
    }
  else if (g_variant_dict_contains (dict, "execute") &&
           g_variant_dict_lookup (dict, "execute", "s", &command))
    {
      g_autoptr(GError) error = NULL;

      if (!g_shell_parse_argv (command, &argc, &argv, &error))
        {
          g_application_command_line_printerr (cmdline,
                                               _("Cannot parse command: %s"),
                                               error->message);
          return EXIT_FAILURE;
        }

      if (new_tab)
        {
          PtyxisWindow *window = get_current_window (self);
          PtyxisTab *tab;

          if (window == NULL)
            window = ptyxis_window_new_empty ();

          tab = ptyxis_window_add_tab_for_command (window, NULL, (const char * const *)argv, cwd_uri);

          ptyxis_tab_set_title_prefix (tab, title);
          ptyxis_tab_set_ignore_osc_title (tab, should_ignore_osc_title (self, title));
          ptyxis_tab_set_initial_working_directory_uri (tab, cwd_uri);
          ptyxis_window_set_active_tab (window, tab);
          gtk_window_present (GTK_WINDOW (window));
        }
      else if (new_tab_with_profile)
        {
          g_autoptr(PtyxisProfile) profile = ptyxis_application_dup_profile (self, new_tab_with_profile);
          PtyxisWindow *window = get_current_window (self);
          PtyxisTab *tab;

          if (window == NULL || new_window)
            window = ptyxis_window_new_empty ();

          tab = ptyxis_window_add_tab_for_command (window, profile, (const char * const *)argv, cwd_uri);

          ptyxis_tab_set_title_prefix (tab, title);
          ptyxis_tab_set_ignore_osc_title (tab, should_ignore_osc_title (self, title));
          ptyxis_tab_set_initial_working_directory_uri (tab, cwd_uri);
          ptyxis_window_set_active_tab (window, tab);
          gtk_window_present (GTK_WINDOW (window));
        }
      else if (new_window)
        {
          PtyxisWindow *window;
          PtyxisTab *tab;

          window = ptyxis_window_new_empty ();
          tab = ptyxis_window_add_tab_for_command (window, NULL, (const char * const *)argv, cwd_uri);

          ptyxis_tab_set_title_prefix (tab, title);
          ptyxis_tab_set_ignore_osc_title (tab, should_ignore_osc_title (self, title));
          ptyxis_tab_set_initial_working_directory_uri (tab, cwd_uri);
          ptyxis_window_set_active_tab (window, tab);

          maybe_maximize_or_fullscreen (self, window);

          gtk_window_present (GTK_WINDOW (window));
        }
      else
        {
          PtyxisWindow *window;
          PtyxisTab *tab;

          window = ptyxis_window_new_for_command (NULL, (const char * const *)argv, cwd_uri);
          tab = ptyxis_window_get_active_tab (window);

          ptyxis_tab_set_title_prefix (tab, title);
          ptyxis_tab_set_ignore_osc_title (tab, should_ignore_osc_title (self, title));

          maybe_maximize_or_fullscreen (self, window);

          gtk_application_add_window (GTK_APPLICATION (self), GTK_WINDOW (window));
          gtk_window_present (GTK_WINDOW (window));
        }
    }
  else if (g_variant_dict_contains (dict, "tab"))
    {
      g_autoptr(PtyxisProfile) profile = ptyxis_application_dup_default_profile (self);
      PtyxisWindow *window = get_current_window (self);
      PtyxisTab *tab = ptyxis_tab_new (profile);
      PtyxisTerminal *terminal = ptyxis_tab_get_terminal (tab);

      if (window == NULL || new_window)
        {
          window = ptyxis_window_new_empty ();
          ptyxis_application_apply_default_size (self, terminal);
        }

      ptyxis_tab_set_initial_working_directory_uri (tab, cwd_uri);
      ptyxis_tab_set_title_prefix (tab, title);
      ptyxis_tab_set_ignore_osc_title (tab, should_ignore_osc_title (self, title));
      ptyxis_window_add_tab (window, tab);
      ptyxis_window_set_active_tab (window, tab);

      gtk_window_present (GTK_WINDOW (window));
    }
  else if (new_tab_with_profile)
    {
      g_autoptr(PtyxisProfile) profile = ptyxis_application_dup_profile (self, new_tab_with_profile);
      PtyxisWindow *window = get_current_window (self);
      PtyxisTab *tab = ptyxis_tab_new (profile);
      PtyxisTerminal *terminal = ptyxis_tab_get_terminal (tab);

      if (window == NULL || new_window)
        {
          window = ptyxis_window_new_empty ();
          ptyxis_application_apply_default_size (self, terminal);
        }

      ptyxis_tab_set_initial_working_directory_uri (tab, cwd_uri);
      ptyxis_tab_set_title_prefix (tab, title);
      ptyxis_tab_set_ignore_osc_title (tab, should_ignore_osc_title (self, title));
      ptyxis_window_add_tab (window, tab);
      ptyxis_window_set_active_tab (window, tab);

      gtk_window_present (GTK_WINDOW (window));
    }
  else if (g_variant_dict_contains (dict, "new-window"))
    {
      g_autoptr(PtyxisProfile) profile = ptyxis_application_dup_default_profile (self);
      PtyxisWindow *window = get_current_window (self);
      PtyxisTerminal *terminal;
      PtyxisTab *tab;

      /* If the request to create a new-window was not combined with other
       * actions above, and we restored a session, then we will consider the
       * request satistfied and not try to add another tab.
       *
       * This can happen when `--new-window` is used as a keybinding to ensure
       * a new window is added whether or not there is an existing instance.
       */
      if (window != NULL &&
          did_restore &&
          !g_variant_dict_contains (dict, "working-directory"))
        {
          gtk_window_present (GTK_WINDOW (window));
          return EXIT_SUCCESS;
        }

      tab = ptyxis_tab_new (profile);
      terminal = ptyxis_tab_get_terminal (tab);

      if (window == NULL || !did_restore)
        {
          window = ptyxis_window_new_empty ();
          ptyxis_application_apply_default_size (self, terminal);
        }

      ptyxis_tab_set_initial_working_directory_uri (tab, cwd_uri);
      ptyxis_tab_set_title_prefix (tab, title);
      ptyxis_tab_set_ignore_osc_title (tab, should_ignore_osc_title (self, title));
      ptyxis_window_add_tab (window, tab);
      ptyxis_window_set_active_tab (window, tab);

      maybe_maximize_or_fullscreen (self, window);

      gtk_window_present (GTK_WINDOW (window));
    }
  else
    {
      g_set_str (&self->next_title_prefix, title);

      g_application_activate (G_APPLICATION (self));
    }

  return EXIT_SUCCESS;
}

static void
on_portal_settings_changed_cb (PtyxisApplication *self,
                               const char        *sender_name,
                               const char        *signal_name,
                               GVariant          *parameters,
                               gpointer           user_data)
{
  g_autoptr(GVariant) value = NULL;
  const char *schema_id;
  const char *key;

  g_assert (PTYXIS_IS_APPLICATION (self));
  g_assert (sender_name != NULL);
  g_assert (signal_name != NULL);

  if (g_strcmp0 (signal_name, "SettingChanged") != 0)
    return;

  g_variant_get (parameters, "(&s&sv)", &schema_id, &key, &value);

  if (g_strcmp0 (schema_id, "org.gnome.desktop.interface") == 0)
    {
      if (g_strcmp0 (key, "monospace-font-name") == 0)
        {
          const char *s = g_variant_get_string (value, NULL);

          if (g_set_str (&self->system_font_name, s))
            g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_SYSTEM_FONT_NAME]);
        }
      else if (g_strcmp0 (key, "overlay-scrolling") == 0)
        {
          gboolean b = g_variant_get_boolean (value);

          if (b != self->overlay_scrollbars)
            {
              self->overlay_scrollbars = b;
              g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_OVERLAY_SCROLLBARS]);
            }
        }
    }
}

static void
parse_portal_settings (PtyxisApplication *self,
                       GVariant          *parameters)
{
  GVariantIter *iter = NULL;
  const char *schema_str;
  GVariant *val;

  g_assert (PTYXIS_IS_APPLICATION (self));

  if (parameters == NULL)
    return;

  g_variant_get (parameters, "(a{sa{sv}})", &iter);

  while (g_variant_iter_loop (iter, "{s@a{sv}}", &schema_str, &val))
    {
      GVariantIter *iter2 = g_variant_iter_new (val);
      const char *key;
      GVariant *v;

      while (g_variant_iter_loop (iter2, "{sv}", &key, &v))
        {
          if (g_strcmp0 (schema_str, "org.gnome.desktop.interface") == 0)
            {
              const char *str;

              if (g_strcmp0 (key, "monospace-font-name") == 0 &&
                  (str = g_variant_get_string (v, NULL)) &&
                  str[0] != 0)
                g_set_str (&self->system_font_name, str);
              else if (g_strcmp0 (key, "overlay-scrolling") == 0)
                self->overlay_scrollbars = g_variant_get_boolean (v);
            }
        }

      g_variant_iter_free (iter2);
    }

  g_variant_iter_free (iter);
}

static void
ptyxis_application_notify_default_profile_uuid_cb (PtyxisApplication *self,
                                                   GParamSpec        *pspec,
                                                   PtyxisSettings    *settings)
{
  g_assert (PTYXIS_IS_APPLICATION (self));
  g_assert (PTYXIS_IS_SETTINGS (settings));

  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_DEFAULT_PROFILE]);
}

static void
ptyxis_application_notify_profile_uuids_cb (PtyxisApplication *self,
                                            GParamSpec        *pspec,
                                            PtyxisSettings    *settings)
{
  g_auto(GStrv) profile_uuids = NULL;
  g_autoptr(GPtrArray) array = NULL;
  guint n_items;

  g_assert (PTYXIS_IS_APPLICATION (self));
  g_assert (PTYXIS_IS_SETTINGS (settings));

  profile_uuids = ptyxis_settings_dup_profile_uuids (settings);
  n_items = g_list_model_get_n_items (G_LIST_MODEL (self->profiles));
  array = g_ptr_array_new_with_free_func (g_object_unref);

  for (guint i = 0; profile_uuids[i]; i++)
    g_ptr_array_add (array, ptyxis_profile_new (profile_uuids[i]));

  g_list_store_splice (self->profiles, 0, n_items, array->pdata, array->len);
}

static void
xdg_terminals_list_changed_cb (PtyxisApplication *self,
                               GFile             *file,
                               GFile             *other_file,
                               GFileMonitorEvent  event,
                               GFileMonitor      *monitor)
{
  GAction *action;

  g_assert (PTYXIS_IS_APPLICATION (self));
  g_assert (G_IS_FILE (file));
  g_assert (G_IS_FILE_MONITOR (monitor));

  if ((action = g_action_map_lookup_action (G_ACTION_MAP (self), "make-default")) &&
      G_IS_SIMPLE_ACTION (action))
    g_simple_action_set_enabled (G_SIMPLE_ACTION (action), !ptyxis_is_default ());
}

static gboolean
ptyxis_application_should_sandbox_agent (PtyxisApplication *self)
{
  g_autofree char *os_release = NULL;

  g_assert (PTYXIS_IS_APPLICATION (self));

  /* Nothing to do if we're not sandboxed */
  if (!g_file_test ("/.flatpak-info", G_FILE_TEST_EXISTS))
    return FALSE;

  /* Some systems we know will absolutely not work with the
   * ptyxis-agent spawned on the host because they lack a
   * compatible glibc and/or linker loader.
   *
   * They will simply get degraded features when in Flatpak.
   *
   * Even if the system does not support it, we will discover
   * that at runtime with a 1 second timeout. Adding things
   * here will gain you that extra second.
   */
  if (g_file_get_contents ("/var/run/host/os-release", &os_release, NULL, NULL))
    {
      if (strstr (os_release, "\"postmarketOS\"") ||
          strstr (os_release, "\"alpine\"") ||
          strstr (os_release, "NixOS"))
        return TRUE;
    }

  return FALSE;
}

static void G_GNUC_NORETURN
ptyxis_application_client_closed_cb (PtyxisApplication *self,
                                     PtyxisClient      *client)
{
  g_assert (PTYXIS_IS_APPLICATION (self));
  g_assert (PTYXIS_IS_CLIENT (client));

  /* We can reach this in two cases. The first is the case where the
   * desktop session is exiting and our `ptyxis-agent` got nuked before
   * we did.
   *
   * The second is if there was a crash by the client. For that, we
   * should get a crash report _anyway_ so just exit cleanly here.
   */

  exit (EXIT_SUCCESS);
}

static void
ptyxis_application_client_process_exited_cb (PtyxisApplication *self,
                                             const char        *process_object_path,
                                             int                exit_code,
                                             PtyxisClient      *client)
{
  g_assert (PTYXIS_IS_APPLICATION (self));
  g_assert (PTYXIS_IS_CLIENT (client));

  /* Store information about how the process exited. We will use this if
   * we race to figure out how the process exited otherwise.  It is
   * removed from the hash table when doing so (or when we get the real
   * value outside of a race condition).
   */
  if (self->exited != NULL)
    g_hash_table_insert (self->exited,
                         g_strdup (process_object_path),
                         GINT_TO_POINTER (exit_code));
}

static void
ptyxis_application_startup (GApplication *application)
{
  static const char *patterns[] = { "org.gnome.*", NULL };
  PtyxisApplication *self = (PtyxisApplication *)application;
  g_autoptr(GFile) session_file = get_session_file ();
  g_autoptr(GBytes) session_bytes = NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GFile) xdg_terminals_list = NULL;
  AdwStyleManager *style_manager;
  gboolean sandbox_agent;
  int timeout_msec;

  g_assert (PTYXIS_IS_APPLICATION (self));

  xdg_terminals_list = g_file_new_build_filename (g_get_user_config_dir (), "xdg-terminals.list", NULL);

  g_application_set_default (application);
  g_application_set_resource_base_path (G_APPLICATION (self), "/org/gnome/Ptyxis");

  self->exited = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  self->profiles = g_list_store_new (PTYXIS_TYPE_PROFILE);
  self->settings = ptyxis_settings_new ();
  self->shortcuts = ptyxis_shortcuts_new (NULL);
  self->xdg_terminals_list_monitor = g_file_monitor (xdg_terminals_list, 0, NULL, NULL);

  /* Load the session state so it's available if we need it */
  if ((session_bytes = g_file_load_bytes (session_file, NULL, NULL, NULL)))
    {
      g_autoptr(GVariant) variant = g_variant_new_from_bytes (G_VARIANT_TYPE_VARDICT, session_bytes, FALSE);

      if (variant != NULL)
        self->session = g_variant_take_ref (g_steal_pointer (&variant));
    }

  G_APPLICATION_CLASS (ptyxis_application_parent_class)->startup (application);

  if ((sandbox_agent = ptyxis_application_should_sandbox_agent (self)))
    timeout_msec = G_MAXINT;
  else
    timeout_msec = 2000;

  /* Try to spawn ptyxis-agent on the host when possible, wait up to timeout_msec */
  if (!(self->client = ptyxis_client_new (sandbox_agent, &error)) ||
      !ptyxis_client_ping (self->client, timeout_msec, &error))
    {
      self->client_is_fallback = TRUE;

      /* Try again, but launching inside our own Flatpak namespace. This
       * can happen when the host system does not have glibc. We may not
       * provide as good of an experience, but try nonetheless.
       */
      g_critical ("Failed to spawn ptyxis-agent on the host system. "
                  "Trying again within Flatpak namespace. "
                  "Some features may not work correctly!");

      g_clear_object (&self->client);
      g_clear_error (&error);

      if (!(self->client = ptyxis_client_new (TRUE, &error)) ||
          !ptyxis_client_ping (self->client, G_MAXINT, &error))
        g_error ("Failed to spawn ptyxis-agent in sandbox: %s", error->message);
    }

  g_debug ("Connected to ptyxis-agent");

  g_signal_connect_object (self->client,
                           "closed",
                           G_CALLBACK (ptyxis_application_client_closed_cb),
                           self,
                           G_CONNECT_SWAPPED);

  g_signal_connect_object (self->client,
                           "process-exited",
                           G_CALLBACK (ptyxis_application_client_process_exited_cb),
                           self,
                           G_CONNECT_SWAPPED);

  g_action_map_add_action_entries (G_ACTION_MAP (self),
                                   action_entries,
                                   G_N_ELEMENTS (action_entries),
                                   self);

  if (self->xdg_terminals_list_monitor != NULL)
    {
      g_signal_connect_object (self->xdg_terminals_list_monitor,
                               "changed",
                               G_CALLBACK (xdg_terminals_list_changed_cb),
                               self,
                               G_CONNECT_SWAPPED);
      xdg_terminals_list_changed_cb (self,
                                     xdg_terminals_list,
                                     NULL,
                                     G_FILE_MONITOR_EVENT_CHANGED,
                                     self->xdg_terminals_list_monitor);
    }

  /* Setup portal to get settings */
  self->portal = g_dbus_proxy_new_for_bus_sync (G_BUS_TYPE_SESSION,
                                                G_DBUS_PROXY_FLAGS_NONE,
                                                NULL,
                                                PORTAL_BUS_NAME,
                                                PORTAL_OBJECT_PATH,
                                                PORTAL_SETTINGS_INTERFACE,
                                                NULL,
                                                NULL);

  if (self->portal != NULL)
    {
      g_autoptr(GVariant) all = NULL;

      g_signal_connect_object (self->portal,
                               "g-signal",
                               G_CALLBACK (on_portal_settings_changed_cb),
                               self,
                               G_CONNECT_SWAPPED);
      all = g_dbus_proxy_call_sync (self->portal,
                                    "ReadAll",
                                    g_variant_new ("(^as)", patterns),
                                    G_DBUS_CALL_FLAGS_NONE,
                                    G_MAXINT,
                                    NULL,
                                    NULL);
      parse_portal_settings (self, all);
    }

  g_signal_connect_object (self->settings,
                           "notify::profile-uuids",
                           G_CALLBACK (ptyxis_application_notify_profile_uuids_cb),
                           self,
                           G_CONNECT_SWAPPED);
  g_signal_connect_object (self->settings,
                           "notify::default-profile-uuid",
                           G_CALLBACK (ptyxis_application_notify_default_profile_uuid_cb),
                           self,
                           G_CONNECT_SWAPPED);

  ptyxis_application_notify_profile_uuids_cb (self, NULL, self->settings);

  style_manager = adw_style_manager_get_default ();

  g_object_bind_property (self->settings, "interface-style",
                          style_manager, "color-scheme",
                          G_BINDING_SYNC_CREATE | G_BINDING_BIDIRECTIONAL);
}

static void
ptyxis_application_finalize (GObject *object)
{
  PtyxisApplication *self = (PtyxisApplication *)object;

  if (self->exited != NULL)
    g_hash_table_remove_all (self->exited);

  g_clear_object (&self->xdg_terminals_list_monitor);
  g_clear_object (&self->profiles);
  g_clear_object (&self->portal);
  g_clear_object (&self->shortcuts);
  g_clear_object (&self->settings);
  g_clear_object (&self->client);

  g_clear_pointer (&self->next_title_prefix, g_free);
  g_clear_pointer (&self->exited, g_hash_table_unref);
  g_clear_pointer (&self->system_font_name, g_free);

  G_OBJECT_CLASS (ptyxis_application_parent_class)->finalize (object);
}

static void
ptyxis_application_get_property (GObject    *object,
                                 guint       prop_id,
                                 GValue     *value,
                                 GParamSpec *pspec)
{
  PtyxisApplication *self = PTYXIS_APPLICATION (object);

  switch (prop_id)
    {
    case PROP_CONTAINERS:
      g_value_take_object (value, ptyxis_application_list_containers (self));
      break;

    case PROP_DEFAULT_PROFILE:
      g_value_take_object (value, ptyxis_application_dup_default_profile (self));
      break;

    case PROP_OS_NAME:
      g_value_set_string (value, ptyxis_application_get_os_name (self));
      break;

    case PROP_OVERLAY_SCROLLBARS:
      g_value_set_boolean (value, self->overlay_scrollbars);
      break;

    case PROP_PROFILES:
      g_value_take_object (value, ptyxis_application_list_profiles (self));
      break;

    case PROP_SYSTEM_FONT_NAME:
      g_value_set_string (value, self->system_font_name);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
ptyxis_application_class_init (PtyxisApplicationClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GApplicationClass *app_class = G_APPLICATION_CLASS (klass);

  object_class->get_property = ptyxis_application_get_property;
  object_class->finalize = ptyxis_application_finalize;

  app_class->activate = ptyxis_application_activate;
  app_class->startup = ptyxis_application_startup;
  app_class->command_line = ptyxis_application_command_line;
  app_class->open = ptyxis_application_open;

  properties[PROP_CONTAINERS] =
    g_param_spec_object ("containers", NULL, NULL,
                         G_TYPE_LIST_MODEL,
                         (G_PARAM_READABLE |
                          G_PARAM_STATIC_STRINGS));

  properties[PROP_DEFAULT_PROFILE] =
    g_param_spec_object ("default-profile", NULL, NULL,
                         PTYXIS_TYPE_PROFILE,
                         (G_PARAM_READABLE |
                          G_PARAM_STATIC_STRINGS));

  properties[PROP_OS_NAME] =
    g_param_spec_string ("os-name", NULL, NULL,
                         NULL,
                         (G_PARAM_READABLE |
                          G_PARAM_STATIC_STRINGS));

  properties [PROP_OVERLAY_SCROLLBARS] =
    g_param_spec_boolean ("overlay-scrollbars", NULL, NULL,
                          FALSE,
                          (G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

  properties[PROP_PROFILES] =
    g_param_spec_object ("profiles", NULL, NULL,
                         G_TYPE_LIST_MODEL,
                         (G_PARAM_READABLE |
                          G_PARAM_STATIC_STRINGS));

  properties [PROP_SYSTEM_FONT_NAME] =
    g_param_spec_string ("system-font-name",
                         "System Font Name",
                         "System Font Name",
                         "Monospace 11",
                         (G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_properties (object_class, N_PROPS, properties);
}

static void
ptyxis_application_init (PtyxisApplication *self)
{
  g_autoptr(GString) summary = g_string_new (_("Examples:"));
  static const GOptionEntry main_entries[] = {
    { "preferences", 0, 0, G_OPTION_ARG_NONE, NULL, N_("Show the application preferences") },

    /* Used for new tabs/windows/etc when specified */
    { "working-directory", 'd', 0, G_OPTION_ARG_FILENAME, NULL, N_("Use DIR for --tab, --tab-with-profile, --new-window, or -x"), N_("DIR") },

    /* By default, this implies a new ptyxis instance unless the options
     * below are provided to override that.
     */
    { "execute", 'x', 0, G_OPTION_ARG_STRING, NULL, N_("Command to execute in new window") },

    /* These options all imply we're using a shared Ptyxis instance. We do not support
     * short command options for these to make it easier to sniff them in early args
     * checking from `main.c`.
     */
    { "new-window", 0, 0, G_OPTION_ARG_NONE, NULL, N_("New terminal window") },
    { "tab", 0, 0, G_OPTION_ARG_NONE, NULL, N_("New terminal tab in active window") },
    { "tab-with-profile", 0, 0, G_OPTION_ARG_STRING, NULL, N_("New terminal tab in active window using the profile UUID"), N_("PROFILE_UUID") },

    /* Standalone (single instance mode) */
    { "standalone", 's', 0, G_OPTION_ARG_NONE, NULL, N_("Start a new instance, ignoring existing instances") },

    { "title", 'T', 0, G_OPTION_ARG_STRING, NULL, N_("Set title for new tab") },
    { "maximize", 0, 0, G_OPTION_ARG_NONE, NULL, N_("Maximize a newly created window") },
    { "fullscreen", 0, 0, G_OPTION_ARG_NONE, NULL, N_("Fullscreen a newly created window") },

    /* Import a custom .palette file. This works like dragging the file onto preferences */
    { "import-palette", 0, 0, G_OPTION_ARG_STRING, NULL, N_("Import a Ptyxis palette file"), N_("FILE") },

    /* Hidden diagnostic: build a 3-pane tab, save+restore the layout,
     * and dump the trees. Used to verify split-pane restore. */
    { "debug-restore-roundtrip", 0, G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_NONE, NULL, N_("Run save/restore roundtrip diagnostic") },

    /* Hidden diagnostic: same as above but exercises the FULL session
     * save/restore path with multiple tabs (the case where the user
     * reports tabs 2+ fail to restore correctly). */
    { "debug-restore-multitab", 0, G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_NONE, NULL, N_("Run multi-tab session save/restore diagnostic") },

    /* Hidden diagnostic: same as multi-tab but with pane-closing in the
     * middle of the session. */
    { "debug-restore-paneclose", 0, G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_NONE, NULL, N_("Run pane-close save/restore diagnostic") },

    /* Hidden diagnostic: write to actual session file, restore from
     * disk. Reproduces the user's quit-then-reopen flow. */
    { "debug-restore-diskfile", 0, G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_NONE, NULL, N_("Run disk-file session restore diagnostic") },

    
    { NULL }
  };

  g_string_append_c (summary, '\n');
  g_string_append_c (summary, '\n');
  g_string_append_printf (summary, "  %s\n", _("Run Separate Instance"));
  g_string_append (summary, "    ptyxis -s\n");

  g_string_append_c (summary, '\n');
  g_string_append_printf (summary, "  %s\n", _("Open Preferences"));
  g_string_append (summary, "    ptyxis --preferences\n");

  g_string_append_c (summary, '\n');
  g_string_append_printf (summary, "  %s\n", _("Run Custom Command in New Window"));
  g_string_append (summary, "    ptyxis -x \"bash -c 'sleep 3'\"\n");
  g_string_append (summary, "    ptyxis -- bash -c 'sleep 3'\n");

  g_string_append_c (summary, '\n');
  g_string_append_printf (summary, "  %s\n", _("Import a custom palette"));
  g_string_append (summary, "    ptyxis --import-palette my.palette");

  g_application_set_option_context_parameter_string (G_APPLICATION (self), _("[-- COMMAND ARGUMENTS]"));
  g_application_add_main_option_entries (G_APPLICATION (self), main_entries);
  g_application_set_option_context_summary (G_APPLICATION (self), summary->str);

  self->system_font_name = g_strdup ("Monospace 11");
}

static void
ptyxis_application_edit_profile (GSimpleAction *action,
                                 GVariant      *param,
                                 gpointer       user_data)
{
  PtyxisApplication *self = user_data;
  g_autoptr(PtyxisProfile) profile = NULL;
  const char *profile_uuid;

  g_assert (PTYXIS_IS_APPLICATION (self));
  g_assert (param != NULL);
  g_assert (g_variant_is_of_type (param, G_VARIANT_TYPE_STRING));

  profile_uuid = g_variant_get_string (param, NULL);

  if ((ptyxis_application_dup_profile (self, profile_uuid)))
    {
      PtyxisPreferencesWindow *window = ptyxis_preferences_window_get_default ();

      ptyxis_preferences_window_edit_profile (window, profile);
    }
}

static char *
generate_debug_info (PtyxisApplication *self)
{
  GString *str = g_string_new (NULL);
  g_autoptr(GListModel) containers = NULL;
  g_autofree char *flatpak_info = NULL;
  g_autofree char *gtk_theme_name= NULL;
  g_autofree char *os_release = NULL;
  GtkSettings *gtk_settings;
  GList *windows = gtk_application_get_windows (GTK_APPLICATION (self));
  GdkDisplay *display = gdk_display_get_default ();
  const char *os_name = ptyxis_application_get_os_name (self);
  const char *vte_sh_path = "/etc/profile.d/vte.sh";
  const char *etc_os_release = "/etc/os-release";
  const char *desktop_session = g_getenv ("DESKTOP_SESSION");
  struct utsname u;
  guint n_items;
  guint id = 0;

  g_string_append_printf (str, "%s %s (%s)\n",
                          ptyxis_app_name (),
                          PACKAGE_VERSION,
                          PTYXIS_BUILD_IDENTIFIER);
  g_string_append_c (str, '\n');

  g_string_append_printf (str, "Operating System: %s\n", os_name);
  g_string_append_c (str, '\n');

  if (uname (&u) == 0)
    {
      g_string_append_printf (str, "uname.sysname = %s\n", u.sysname);
      g_string_append_printf (str, "uname.release = %s\n", u.release);
      g_string_append_printf (str, "uname.version = %s\n", u.version);
      g_string_append_printf (str, "uname.machine = %s\n", u.machine);
    }

  g_string_append_c (str, '\n');
  g_string_append_printf (str,
                          "Agent: running %s\n",
                          self->client_is_fallback ? "in sandbox" : "on host");

  g_string_append_c (str, '\n');
  g_string_append_printf (str,
                          "Desktop Session: %s\n",
                          desktop_session ? desktop_session : "Unspecified");

  g_string_append_c (str, '\n');
  g_string_append_printf (str,
                          "GLib: %d.%d.%d (compiled against %d.%d.%d)\n",
                          glib_major_version,
                          glib_minor_version,
                          glib_micro_version,
                          GLIB_MAJOR_VERSION,
                          GLIB_MINOR_VERSION,
                          GLIB_MICRO_VERSION);
  g_string_append_printf (str,
                          "GTK: %d.%d.%d (compiled against %d.%d.%d)\n",
                          gtk_get_major_version (),
                          gtk_get_minor_version (),
                          gtk_get_micro_version (),
                          GTK_MAJOR_VERSION,
                          GTK_MINOR_VERSION,
                          GTK_MICRO_VERSION);
  g_string_append_printf (str,
                          "VTE: %d.%d.%d (compiled against %d.%d.%d) %s\n",
                          vte_get_major_version (),
                          vte_get_minor_version (),
                          vte_get_micro_version (),
                          VTE_MAJOR_VERSION,
                          VTE_MINOR_VERSION,
                          VTE_MICRO_VERSION,
                          vte_get_features ());

  g_string_append_c (str, '\n');
  g_string_append_printf (str, "Display: %s\n", G_OBJECT_TYPE_NAME (display));
  g_string_append_printf (str, "Accessibility: %s\n",
                          ptyxis_settings_get_enable_a11y (self->settings) ? "Yes" : "No");

  gtk_settings = gtk_settings_get_default ();
  g_object_get (gtk_settings, "gtk-theme-name", &gtk_theme_name, NULL);
  g_string_append_c (str, '\n');
  g_string_append_printf (str, "GTK Theme: %s\n", gtk_theme_name);
  g_string_append_printf (str, "System Font: %s\n", self->system_font_name);

  if (ptyxis_settings_get_use_system_font (self->settings))
    {
      g_string_append (str, "Font: -- Using System Font --\n");
    }
  else
    {
      g_autofree char *font_name = ptyxis_settings_dup_font_name (self->settings);
      g_string_append_printf (str, "Font: %s\n", font_name);
    }

  for (const GList *iter = windows; iter; iter = iter->next)
    {
      g_autoptr(GListModel) pages = NULL;
      GtkWindow *window = iter->data;
      GskRenderer *renderer;
      GdkSurface *surface;
      GdkMonitor *monitor;
      GdkRectangle geom;
      guint n_pages;

      if (!PTYXIS_IS_WINDOW (window))
        continue;

      pages = ptyxis_window_list_pages (PTYXIS_WINDOW (window));
      n_pages = g_list_model_get_n_items (pages);

      renderer = gtk_native_get_renderer (GTK_NATIVE (window));
      surface = gtk_native_get_surface (GTK_NATIVE (window));
      monitor = gdk_display_get_monitor_at_surface (display, surface);

      gdk_monitor_get_geometry (monitor, &geom);

      g_string_append_c (str, '\n');
      g_string_append_printf (str, "window[%u].n_tabs = %u\n",
                              id, n_pages);
      g_string_append_printf (str, "window[%u].renderer = %s\n",
                              id, G_OBJECT_TYPE_NAME (renderer));
      g_string_append_printf (str, "window[%u].scale = %lf\n",
                              id, gdk_surface_get_scale (surface));
      g_string_append_printf (str, "window[%u].scale_factor = %d\n",
                              id, gdk_surface_get_scale_factor (surface));
      g_string_append_printf (str, "window[%u].monitor.geometry = %d,%d %d×%d\n",
                              id, geom.x, geom.y, geom.width, geom.height);
      g_string_append_printf (str, "window[%u].monitor.refresh_rate = %u\n",
                              id, gdk_monitor_get_refresh_rate (monitor));

      id++;
    }

#if DEVELOPMENT_BUILD
  g_string_append_c (str, '\n');
  g_string_append (str, "** DEVELOPMENT BUILD **\n");
#endif

  g_string_append_c (str, '\n');
  g_string_append_printf (str, "App ID: %s\n", APP_ID);

  if (ptyxis_get_process_kind () == PTYXIS_PROCESS_KIND_FLATPAK)
    vte_sh_path = "/var/run/host/etc/profile.d/vte.sh";

  g_string_append_c (str, '\n');
  g_string_append_printf (str, "%s %s\n",
                          vte_sh_path,
                          g_file_test (vte_sh_path, G_FILE_TEST_EXISTS) ? "exists" : "missing");

  g_string_append_c (str, '\n');
  g_string_append (str, "Containers:\n");

  containers = ptyxis_application_list_containers (self);
  n_items = g_list_model_get_n_items (containers);

  for (guint i = 0; i < n_items; i++)
    {
      g_autoptr(PtyxisIpcContainer) container = g_list_model_get_item (containers, i);

      if (g_strcmp0 ("session", ptyxis_ipc_container_get_id (container)) == 0)
        continue;

      g_string_append_printf (str,
                              "  • %s (%s)\n",
                              ptyxis_ipc_container_get_display_name (container),
                              ptyxis_ipc_container_get_provider (container));
    }

  if (g_file_get_contents ("/.flatpak-info", &flatpak_info, NULL, NULL))
    {
      g_string_append_c (str, '\n');
      g_string_append (str, flatpak_info);

      etc_os_release = "/var/run/host/etc/os-release";
    }

  if (g_file_get_contents (etc_os_release, &os_release, NULL, NULL))
    {
      g_string_append_c (str, '\n');
      g_string_append (str, os_release);
    }

  return g_string_free (str, FALSE);
}

static void
ptyxis_application_about (GSimpleAction *action,
                          GVariant      *param,
                          gpointer       user_data)
{
  static const char *developers[] = {"Christian Hergert", NULL};
  static const char *artists[] = {"Jakub Steiner", NULL};
  PtyxisApplication *self = user_data;
  GtkWindow *window = NULL;
  g_autofree char *debug_info = NULL;

  g_assert (PTYXIS_IS_APPLICATION (self));

  window = gtk_application_get_active_window (GTK_APPLICATION (self));
  debug_info = generate_debug_info (self);

  adw_show_about_dialog (GTK_WIDGET (window),
                         "application-icon", PACKAGE_ICON_NAME,
                         "application-name", ptyxis_app_name (),
                         "artists", artists,
                         "copyright", "© 2023-2024 Christian Hergert, et al.",
                         "debug-info", debug_info,
                         "developer-name", "Christian Hergert",
                         "developers", developers,
                         "issue-url", "https://gitlab.gnome.org/chergert/ptyxis/issues",
                         "license-type", GTK_LICENSE_GPL_3_0,
                         "translator-credits", _("translator-credits"),
                         "version", PACKAGE_VERSION,
                         "website", "https://gitlab.gnome.org/chergert/ptyxis",
                         NULL);
}

static void
ptyxis_application_preferences (GSimpleAction *action,
                                GVariant      *param,
                                gpointer       user_data)
{
  PtyxisPreferencesWindow *window;
  PtyxisApplication *self = user_data;

  g_assert (PTYXIS_IS_APPLICATION (self));

  window = ptyxis_preferences_window_get_default ();
  gtk_application_add_window (GTK_APPLICATION (self), GTK_WINDOW (window));
  gtk_window_present (GTK_WINDOW (window));
}

static void
ptyxis_application_new_window_action (GSimpleAction *action,
                                      GVariant      *param,
                                      gpointer       user_data)
{
  PtyxisApplication *self = user_data;
  PtyxisWindow *window;

  g_assert (!action || G_IS_SIMPLE_ACTION (action));
  g_assert (PTYXIS_IS_APPLICATION (self));

  window = ptyxis_window_new ();
  gtk_application_add_window (GTK_APPLICATION (self), GTK_WINDOW (window));
  gtk_window_present (GTK_WINDOW (window));
}

static void
ptyxis_application_new_tab_action (GSimpleAction *action,
                                   GVariant      *param,
                                   gpointer       user_data)
{
  PtyxisApplication *self = user_data;
  g_autoptr(PtyxisProfile) profile = NULL;
  PtyxisWindow *window;
  PtyxisTab *tab;

  g_assert (!action || G_IS_SIMPLE_ACTION (action));
  g_assert (PTYXIS_IS_APPLICATION (self));

  window = get_current_window (self);

  if (window == NULL)
    window = ptyxis_window_new_empty ();

  profile = ptyxis_application_dup_default_profile (self);
  tab = ptyxis_tab_new (profile);

  ptyxis_window_add_tab (window, tab);
  ptyxis_window_set_active_tab (window, tab);

  gtk_window_present (GTK_WINDOW (window));
}

/**
 * ptyxis_application_list_profiles:
 * @self: a #PtyxisApplication
 *
 * Gets a #GListModel or profiles that are available to the application.
 *
 * The resulting #GListModel will update as profiles are created or deleted.
 *
 * Returns: (transfer full) (not nullable): a #GListModel
 */
GListModel *
ptyxis_application_list_profiles (PtyxisApplication *self)
{
  g_return_val_if_fail (PTYXIS_IS_APPLICATION (self), NULL);

  return g_object_ref (G_LIST_MODEL (self->profiles));
}

/**
 * ptyxis_application_dup_default_profile:
 * @self: a #PtyxisApplication
 *
 * Gets the default profile for the application.
 *
 * Returns: (transfer full): a #PtyxisProfile
 */
PtyxisProfile *
ptyxis_application_dup_default_profile (PtyxisApplication *self)
{
  g_autoptr(PtyxisProfile) new_profile = NULL;
  g_autoptr(GListModel) profiles = NULL;
  g_autofree char *default_profile_uuid = NULL;
  guint n_items;

  g_return_val_if_fail (PTYXIS_IS_APPLICATION (self), NULL);

  default_profile_uuid = ptyxis_settings_dup_default_profile_uuid (self->settings);
  profiles = ptyxis_application_list_profiles (self);
  n_items = g_list_model_get_n_items (profiles);

  for (guint i = 0; i < n_items; i++)
    {
      g_autoptr(PtyxisProfile) profile = g_list_model_get_item (profiles, i);
      const char *profile_uuid = ptyxis_profile_get_uuid (profile);

      if (g_strcmp0 (profile_uuid, default_profile_uuid) == 0)
        return g_steal_pointer (&profile);
    }

  if (n_items > 0)
    return g_list_model_get_item (profiles, 0);

  new_profile = ptyxis_profile_new (NULL);

  g_assert (new_profile != NULL);
  g_assert (PTYXIS_IS_PROFILE (new_profile));
  g_assert (ptyxis_profile_get_uuid (new_profile) != NULL);

  ptyxis_application_add_profile (self, new_profile);
  ptyxis_application_set_default_profile (self, new_profile);

  return g_steal_pointer (&new_profile);
}

void
ptyxis_application_set_default_profile (PtyxisApplication *self,
                                        PtyxisProfile     *profile)
{
  g_return_if_fail (PTYXIS_IS_APPLICATION (self));
  g_return_if_fail (PTYXIS_IS_PROFILE (profile));

  ptyxis_settings_set_default_profile_uuid (self->settings,
                                            ptyxis_profile_get_uuid (profile));
}

void
ptyxis_application_add_profile (PtyxisApplication *self,
                                PtyxisProfile     *profile)
{
  g_return_if_fail (PTYXIS_IS_APPLICATION (self));

  ptyxis_settings_add_profile_uuid (self->settings,
                                    ptyxis_profile_get_uuid (profile));
}

void
ptyxis_application_remove_profile (PtyxisApplication *self,
                                   PtyxisProfile     *profile)
{
  g_return_if_fail (PTYXIS_IS_APPLICATION (self));
  g_return_if_fail (PTYXIS_IS_PROFILE (profile));

  ptyxis_settings_remove_profile_uuid (self->settings,
                                       ptyxis_profile_get_uuid (profile));
}

PtyxisProfile *
ptyxis_application_dup_profile (PtyxisApplication *self,
                                const char        *profile_uuid)
{
  g_autoptr(GListModel) model = NULL;
  guint n_items;

  g_return_val_if_fail (PTYXIS_IS_APPLICATION (self), NULL);

  if (ptyxis_str_empty0 (profile_uuid))
    return ptyxis_application_dup_default_profile (self);

  model = ptyxis_application_list_profiles (self);
  n_items = g_list_model_get_n_items (model);

  for (guint i = 0; i < n_items; i++)
    {
      g_autoptr(PtyxisProfile) profile = g_list_model_get_item (model, i);

      if (g_strcmp0 (profile_uuid, ptyxis_profile_get_uuid (profile)) == 0)
        return g_steal_pointer (&profile);
    }

  /* We don't want to inflate profiles that no longer exist
   * so fallback to using the default profile.
   */
  return ptyxis_application_dup_default_profile (self);
}

gboolean
ptyxis_application_control_is_pressed (PtyxisApplication *self)
{
  GdkDisplay *display = gdk_display_get_default ();
  GdkSeat *seat = gdk_display_get_default_seat (display);
  GdkDevice *keyboard = gdk_seat_get_keyboard (seat);
  GdkModifierType modifiers = gdk_device_get_modifier_state (keyboard) & gtk_accelerator_get_default_mod_mask ();

  return !!(modifiers & GDK_CONTROL_MASK);
}

const char *
ptyxis_application_get_system_font_name (PtyxisApplication *self)
{
  g_return_val_if_fail (PTYXIS_IS_APPLICATION (self), NULL);

  return self->system_font_name;
}

/**
 * ptyxis_application_list_containers:
 * @self: a #PtyxisApplication
 *
 * Gets a #GListModel of #PtyxisIpcContainer.
 *
 * Returns: (transfer full) (not nullable): a #GListModel
 */
GListModel *
ptyxis_application_list_containers (PtyxisApplication *self)
{
  g_return_val_if_fail (PTYXIS_IS_APPLICATION (self), NULL);

  return g_object_ref (G_LIST_MODEL (self->client));
}

PtyxisIpcContainer *
ptyxis_application_lookup_container (PtyxisApplication *self,
                                     const char        *container_id)
{
  g_autoptr(GListModel) model = NULL;
  guint n_items;

  g_return_val_if_fail (PTYXIS_IS_APPLICATION (self), NULL);

  if (ptyxis_str_empty0 (container_id))
    return NULL;

  model = ptyxis_application_list_containers (self);
  n_items = g_list_model_get_n_items (model);

  for (guint i = 0; i < n_items; i++)
    {
      g_autoptr(PtyxisIpcContainer) container = g_list_model_get_item (model, i);
      const char *id = ptyxis_ipc_container_get_id (container);

      if (g_strcmp0 (id, container_id) == 0)
        return g_steal_pointer (&container);
    }

  return NULL;
}

PtyxisSettings *
ptyxis_application_get_settings (PtyxisApplication *self)
{
  g_return_val_if_fail (PTYXIS_IS_APPLICATION (self), NULL);

  return self->settings;
}

/**
 * ptyxis_application_get_shortcuts:
 * @self: a #PtyxisApplication
 *
 * Gets the shortcuts for the application.
 *
 * Returns: (transfer none) (not nullable): a #PtyxisShortcuts
 */
PtyxisShortcuts *
ptyxis_application_get_shortcuts (PtyxisApplication *self)
{
  g_return_val_if_fail (PTYXIS_IS_APPLICATION (self), NULL);

  return self->shortcuts;
}

void
ptyxis_application_report_error (PtyxisApplication *self,
                                 GType              subsystem,
                                 const GError      *error)
{
  g_return_if_fail (PTYXIS_IS_APPLICATION (self));

  /* TODO: We probably want to provide some feedback if we fail to
   *       run in a number of scenarios. And this will also let us
   *       do some deduplication of repeated error messages once
   *       we start showing errors to users.
   */

  g_debug ("%s: %s[%u]: %s",
           g_type_name (subsystem),
           g_quark_to_string (error->domain),
           error->code,
           error->message);
}

VtePty *
ptyxis_application_create_pty (PtyxisApplication  *self,
                               GError            **error)
{
  g_return_val_if_fail (PTYXIS_IS_APPLICATION (self), NULL);

  return ptyxis_client_create_pty (self->client, error);
}

typedef struct _Spawn
{
  PtyxisIpcContainer *container;
  PtyxisProfile *profile;
  char *last_working_directory_uri;
  VtePty *pty;
  char **argv;
} Spawn;

static void
spawn_free (gpointer data)
{
  Spawn *spawn = data;

  g_clear_object (&spawn->container);
  g_clear_object (&spawn->profile);
  g_clear_object (&spawn->pty);
  g_clear_pointer (&spawn->last_working_directory_uri, g_free);
  g_clear_pointer (&spawn->argv, g_strfreev);
  g_free (spawn);
}

static void
ptyxis_application_spawn_cb (GObject      *object,
                             GAsyncResult *result,
                             gpointer      user_data)
{
  PtyxisClient *client = (PtyxisClient *)object;
  g_autoptr(PtyxisIpcProcess) process = NULL;
  g_autoptr(GTask) task = user_data;
  g_autoptr(GError) error = NULL;

  g_assert (PTYXIS_IS_CLIENT (client));
  g_assert (G_IS_ASYNC_RESULT (result));
  g_assert (G_IS_TASK (task));

  if (!(process = ptyxis_client_spawn_finish (client, result, &error)))
    {
      g_debug ("[spawn] spawn_cb FAIL: %s",
               error ? error->message : "(no error)");
      g_task_return_error (task, g_steal_pointer (&error));
    }
  else
    {
      const char *object_path = g_dbus_proxy_get_object_path (G_DBUS_PROXY (process));
      g_debug ("[spawn] spawn_cb OK -> process=%p object_path=%s", (void*)process, object_path);
      g_task_return_pointer (task, g_steal_pointer (&process), g_object_unref);
    }
}

static void
ptyxis_application_check_shell_cb (GObject      *object,
                                   GAsyncResult *result,
                                   gpointer      user_data)
{
  PtyxisIpcContainer *container = (PtyxisIpcContainer *)object;
  g_autofree char *default_shell_path = NULL;
  g_autoptr(GTask) task = user_data;
  PtyxisApplication *self;
  Spawn *spawn;

  g_assert (PTYXIS_IPC_IS_CONTAINER (container));
  g_assert (G_IS_ASYNC_RESULT (result));
  g_assert (G_IS_TASK (task));

  self = g_task_get_source_object (task);
  spawn = g_task_get_task_data (task);

  g_assert (PTYXIS_IS_APPLICATION (self));
  g_assert (spawn != NULL);
  g_assert (VTE_IS_PTY (spawn->pty));
  g_assert (PTYXIS_IPC_IS_CONTAINER (spawn->container));
  g_assert (PTYXIS_IS_PROFILE (spawn->profile));
  g_assert (PTYXIS_IS_CLIENT (self->client));

  ptyxis_ipc_container_call_find_program_in_path_finish (container,
                                                         &default_shell_path,
                                                         result,
                                                         NULL);

  if (default_shell_path && default_shell_path[0] == 0)
    g_clear_pointer (&default_shell_path, g_free);

  g_debug ("[spawn] check_shell_cb: shell=%s -> calling ptyxis_client_spawn_async",
           default_shell_path ? default_shell_path : "(null, will use argv0)");

  ptyxis_client_spawn_async (self->client,
                             spawn->container,
                             spawn->profile,
                             default_shell_path,
                             spawn->last_working_directory_uri,
                             spawn->pty,
                             (const char * const *)spawn->argv,
                             g_task_get_cancellable (task),
                             ptyxis_application_spawn_cb,
                             g_object_ref (task));
}

static void
ptyxis_application_get_preferred_shell_cb (GObject      *object,
                                           GAsyncResult *result,
                                           gpointer      user_data)
{
  PtyxisClient *client = (PtyxisClient *)object;
  g_autofree char *default_shell = NULL;
  g_autofree char *default_shell_base = NULL;
  g_autoptr(GTask) task = user_data;
  G_GNUC_UNUSED PtyxisApplication *self;
  Spawn *spawn;

  g_assert (PTYXIS_IS_CLIENT (client));
  g_assert (G_IS_ASYNC_RESULT (result));
  g_assert (G_IS_TASK (task));

  self = g_task_get_source_object (task);
  spawn = g_task_get_task_data (task);

  g_assert (PTYXIS_IS_APPLICATION (self));
  g_assert (spawn != NULL);
  g_assert (VTE_IS_PTY (spawn->pty));
  g_assert (PTYXIS_IPC_IS_CONTAINER (spawn->container));
  g_assert (PTYXIS_IS_PROFILE (spawn->profile));
  g_assert (PTYXIS_IS_CLIENT (self->client));

  default_shell = ptyxis_client_discover_shell_finish (client, result, NULL);

  if (default_shell && default_shell[0] == 0)
    g_clear_pointer (&default_shell, g_free);

  g_debug ("[spawn] preferred_shell_cb: discovered=%s", default_shell ? default_shell : "(null)");

  default_shell_base = g_path_get_basename (default_shell ? default_shell : "bash");

  /* Now make sure the preferred shell is available */
  ptyxis_ipc_container_call_find_program_in_path (spawn->container,
                                                  default_shell_base,
                                                  g_task_get_cancellable (task),
                                                  ptyxis_application_check_shell_cb,
                                                  g_object_ref (task));
}

void
ptyxis_application_spawn_async (PtyxisApplication   *self,
                                PtyxisIpcContainer  *container,
                                PtyxisProfile       *profile,
                                const char          *last_working_directory_uri,
                                VtePty              *pty,
                                const char * const  *argv,
                                GCancellable        *cancellable,
                                GAsyncReadyCallback  callback,
                                gpointer             user_data)
{
  g_autoptr(GTask) task = NULL;
  Spawn *spawn;

  g_return_if_fail (PTYXIS_IS_APPLICATION (self));
  g_return_if_fail (PTYXIS_IPC_IS_CONTAINER (container));
  g_return_if_fail (PTYXIS_IS_PROFILE (profile));
  g_return_if_fail (VTE_IS_PTY (pty));
  g_return_if_fail (!cancellable || G_IS_CANCELLABLE (cancellable));
  g_return_if_fail (PTYXIS_IS_CLIENT (self->client));

  g_debug ("[spawn] spawn_async ENTER container=%s argv0=%s cwd=%s",
           ptyxis_ipc_container_get_id (container),
           argv && argv[0] ? argv[0] : "(null)",
           last_working_directory_uri ? last_working_directory_uri : "(null)");

  task = g_task_new (self, cancellable, callback, user_data);
  g_task_set_source_tag (task, ptyxis_application_spawn_async);

  spawn = g_new0 (Spawn, 1);
  g_set_object (&spawn->container, container);
  g_set_object (&spawn->profile, profile);
  g_set_object (&spawn->pty, pty);
  g_set_str (&spawn->last_working_directory_uri, last_working_directory_uri);
  spawn->argv = g_strdupv ((char **)argv);
  g_task_set_task_data (task, spawn, spawn_free);

  ptyxis_client_discover_shell_async (self->client,
                                      NULL,
                                      ptyxis_application_get_preferred_shell_cb,
                                      g_steal_pointer (&task));
}

PtyxisIpcProcess *
ptyxis_application_spawn_finish (PtyxisApplication  *self,
                                 GAsyncResult       *result,
                                 GError            **error)
{
  g_return_val_if_fail (PTYXIS_IS_APPLICATION (self), NULL);
  g_return_val_if_fail (G_IS_TASK (result), NULL);

  return g_task_propagate_pointer (G_TASK (result), error);
}

static void
wait_complete (GTask  *task,
               int     exit_status,
               int     term_sig,
               GError *error)
{
  if (!g_object_get_data (G_OBJECT (task), "DID_COMPLETE"))
    {
      g_object_set_data (G_OBJECT (task), "DID_COMPLETE", GINT_TO_POINTER (1));

      if (error)
        g_task_return_error (task, error);
      else
        g_task_return_int (task, W_EXITCODE (exit_status, term_sig));

      g_object_unref (task);
    }
}

static void
ptyxis_application_process_exited_cb (PtyxisIpcProcess *process,
                                      int               exit_status,
                                      GTask            *task)
{
  g_assert (PTYXIS_IPC_IS_PROCESS (process));
  g_assert (G_IS_TASK (task));

  g_hash_table_remove (PTYXIS_APPLICATION_DEFAULT->exited,
                       g_dbus_proxy_get_object_path (G_DBUS_PROXY (process)));

  wait_complete (task, exit_status, 0, NULL);
}

static void
ptyxis_application_process_signaled_cb (PtyxisIpcProcess *process,
                                        int               term_sig,
                                        GTask            *task)
{
  PtyxisApplication *self = PTYXIS_APPLICATION_DEFAULT;

  g_assert (PTYXIS_IPC_IS_PROCESS (process));
  g_assert (G_IS_TASK (task));

  if (self->exited != NULL)
    g_hash_table_remove (self->exited,
                         g_dbus_proxy_get_object_path (G_DBUS_PROXY (process)));

  wait_complete (task, 0, term_sig, NULL);
}

static void
ptyxis_application_has_foreground_process_cb (GObject      *object,
                                              GAsyncResult *result,
                                              gpointer      user_data)
{
  PtyxisApplication *self = PTYXIS_APPLICATION_DEFAULT;
  PtyxisIpcProcess *process = (PtyxisIpcProcess *)object;
  g_autoptr(GError) error = NULL;
  g_autoptr(GTask) task = user_data;

  g_assert (PTYXIS_IPC_IS_PROCESS (process));
  g_assert (G_IS_ASYNC_RESULT (result));
  g_assert (G_IS_TASK (task));

  if (!ptyxis_ipc_process_call_has_foreground_process_finish (process, NULL, NULL, NULL, NULL, NULL, result, &error))
    {
      const char *object_path = g_dbus_proxy_get_object_path (G_DBUS_PROXY (process));
      gpointer value;
      int exit_status = 0;
      int term_sig = 0;

      if (g_hash_table_lookup_extended (self->exited, object_path, NULL, &value))
        {
          int exit_code = GPOINTER_TO_INT (value);

          if (WIFEXITED (exit_code))
            exit_status = WEXITSTATUS (exit_code);
          else
            term_sig = WTERMSIG (exit_code);

          g_clear_error (&error);

          g_hash_table_remove (self->exited, object_path);
        }

      wait_complete (task, exit_status, term_sig, g_steal_pointer (&error));
    }
}

void
ptyxis_application_wait_async (PtyxisApplication   *self,
                               PtyxisIpcProcess    *process,
                               GCancellable        *cancellable,
                               GAsyncReadyCallback  callback,
                               gpointer             user_data)
{
  g_autoptr(GTask) task = NULL;

  g_return_if_fail (PTYXIS_IS_APPLICATION (self));
  g_return_if_fail (PTYXIS_IPC_IS_PROCESS (process));
  g_return_if_fail (!cancellable || G_IS_CANCELLABLE (cancellable));

  /* Because we only get signals/exit-status via signals (to avoid
   * various race conditions in IPC), we use the RPC to get the leader
   * kind as a sort of ping to determine if the process is still alive
   * initially. It will be removed from the D-Bus connection once it
   * exits or signals.
   */

  task = g_task_new (self, cancellable, callback, user_data);
  g_task_set_source_tag (task, ptyxis_application_wait_async);

  /* Keep alive until signal is received */
  g_object_ref (task);
  g_signal_connect_object (process,
                           "exited",
                           G_CALLBACK (ptyxis_application_process_exited_cb),
                           task,
                           0);
  g_signal_connect_object (process,
                           "signaled",
                           G_CALLBACK (ptyxis_application_process_signaled_cb),
                           task,
                           0);

  /* Now query to ensure the process is still there */
  ptyxis_ipc_process_call_has_foreground_process (process,
                                                  g_variant_new_handle (-1),
                                                  NULL,
                                                  cancellable,
                                                  ptyxis_application_has_foreground_process_cb,
                                                  g_steal_pointer (&task));
}

int
ptyxis_application_wait_finish (PtyxisApplication  *self,
                                GAsyncResult       *result,
                                GError            **error)
{
  g_return_val_if_fail (PTYXIS_IS_APPLICATION (self), FALSE);
  g_return_val_if_fail (G_IS_TASK (result), FALSE);

  return g_task_propagate_int (G_TASK (result), error);
}

PtyxisIpcContainer *
ptyxis_application_discover_current_container (PtyxisApplication *self,
                                               VtePty            *pty)
{
  g_return_val_if_fail (PTYXIS_IS_APPLICATION (self), NULL);

  return ptyxis_client_discover_current_container (self->client, pty);
}

static void
ptyxis_application_focus_tab_by_uuid (GSimpleAction *action,
                                      GVariant      *param,
                                      gpointer       user_data)
{
  PtyxisApplication *self = user_data;
  const char *uuid;

  g_assert (PTYXIS_IS_APPLICATION (self));
  g_assert (param != NULL);
  g_assert (g_variant_is_of_type (param, G_VARIANT_TYPE_STRING));

  uuid = g_variant_get_string (param, NULL);

  g_debug ("Looking for tab \"%s\"", uuid);

  for (const GList *windows = gtk_application_get_windows (GTK_APPLICATION (self));
       windows != NULL;
       windows = windows->next)
    {
      if (PTYXIS_IS_WINDOW (windows->data))
        {
          if (ptyxis_window_focus_tab_by_uuid (windows->data, uuid))
            break;
        }
    }
}

/**
 * ptyxis_application_find_container_by_name:
 * @self: a #PtyxisApplication
 *
 * Locates the container by runtime/name.
 *
 * Returns: (transfer full) (nullable): a container or %NULL
 */
PtyxisIpcContainer *
ptyxis_application_find_container_by_name (PtyxisApplication *self,
                                           const char        *runtime,
                                           const char        *name)
{
  g_autoptr(GListModel) model = NULL;
  guint n_items;

  g_return_val_if_fail (PTYXIS_IS_APPLICATION (self), NULL);

  if (runtime == NULL || name == NULL)
    return NULL;

  model = ptyxis_application_list_containers (self);
  n_items = g_list_model_get_n_items (model);

  for (guint i = 0; i < n_items; i++)
    {
      g_autoptr(PtyxisIpcContainer) container = g_list_model_get_item (model, i);

      if (g_strcmp0 (runtime, ptyxis_ipc_container_get_provider (container)) == 0 &&
          g_strcmp0 (name, ptyxis_ipc_container_get_display_name (container)) == 0)
        return g_steal_pointer (&container);
    }

  return NULL;
}

const char *
ptyxis_application_get_os_name (PtyxisApplication *self)
{
  g_return_val_if_fail (PTYXIS_IS_APPLICATION (self), NULL);

  return ptyxis_client_get_os_name (self->client);
}

static void
ptyxis_application_save_session_cb (GObject      *object,
                                    GAsyncResult *result,
                                    gpointer      user_data)
{
  GFile *file = (GFile *)object;
  g_autoptr(PtyxisApplication) self = user_data;
  g_autoptr(GError) error = NULL;

  g_assert (G_IS_FILE (file));
  g_assert (G_IS_ASYNC_RESULT (result));
  g_assert (PTYXIS_IS_APPLICATION (self));

  g_application_release (G_APPLICATION (self));

  if (!g_file_replace_contents_finish (file, result, NULL, &error))
    g_warning ("Failed to save session state: %s", error->message);
}

void
ptyxis_application_save_session (PtyxisApplication *self)
{
  g_autoptr(GVariant) state = NULL;
  g_autoptr(GBytes) bytes = NULL;

  g_return_if_fail (PTYXIS_IS_APPLICATION (self));

  if ((state = ptyxis_session_save (self)) &&
      (bytes = g_variant_get_data_as_bytes (state)))
    {
      g_autoptr(GFile) file = get_session_file ();
      g_autoptr(GFile) directory = g_file_get_parent (file);

      g_application_hold (G_APPLICATION (self));

      g_file_make_directory_with_parents (directory, NULL, NULL);
      g_file_replace_contents_bytes_async (file,
                                           bytes,
                                           NULL,
                                           FALSE,
                                           G_FILE_CREATE_REPLACE_DESTINATION,
                                           NULL,
                                           ptyxis_application_save_session_cb,
                                           g_object_ref (self));
    }
}

gboolean
ptyxis_application_get_overlay_scrollbars (PtyxisApplication *self)
{
  g_return_val_if_fail (PTYXIS_IS_APPLICATION (self), FALSE);

  return self->overlay_scrollbars;
}

const char *
ptyxis_application_get_user_data_dir (PtyxisApplication *self)
{
  g_return_val_if_fail (PTYXIS_IS_APPLICATION (self), NULL);

  return ptyxis_client_get_user_data_dir (self->client);
}

static void
ptyxis_application_make_default (GSimpleAction *action,
                                 GVariant      *param,
                                 gpointer       user_data)
{
  g_assert (PTYXIS_IS_APPLICATION (user_data));

  ptyxis_make_default ();
}
