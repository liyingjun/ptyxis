/* Bug 5 reproduction: mirrors the user's reported trigger.
 *
 *   1. open a tab (single primary pane).
 *   2. "close pane" on the last pane -> PtyxisWindow falls through to
 *      page.close -> ptyxis_window_close_page_cb -> ptyxis_parking_lot_push
 *      (tab is parked; the 5s parking timeout would later force_quit it).
 *   3. immediately destroy the window -> PtyxisWindow.dispose ->
 *      clear-parking-lot -> parking_lot_remove(force_quit=TRUE) ->
 *      ptyxis_tab_force_quit arms the SIGKILL safety-net (pending_kill_source,
 *      holding a ref on the tab) -> then PtyxisTab.dispose runs while that
 *      source is live.
 *
 * Pre-fix: cancelling pending_kill_source inside the 1st dispose fired the
 * source's destroy_notify (g_object_unref) which re-entered PtyxisTab.dispose
 * before self->disposed was set -> 2nd full dispose + finalize freed the
 * instance -> 1st call resumed on freed memory ->
 *   GLib-GObject-CRITICAL: invalid unclassed pointer in cast to 'GObject'
 *
 * Post-fix: PtyxisTab.dispose sets self->disposed=TRUE at the very top, so the
 * re-entrant dispatch returns at the early-return guard. No CRITICAL.
 */
#include <gtk/gtk.h>
#include "ptyxis-application.h"
#include "ptyxis-window.h"
#include "ptyxis-tab.h"
#include "ptyxis-profile.h"

/* main.c defines this; we exclude main.c.o (it brings its own main()),
 * so provide a minimal stub here. */
gint64
ptyxis_application_get_default_rlimit_nofile (void)
{
  return 0;
}

static gboolean
do_close_pane (gpointer user_data)
{
  GtkApplication *app = user_data;
  GList *windows = gtk_application_get_windows (app);
  PtyxisWindow *win = windows ? PTYXIS_WINDOW (windows->data) : NULL;
  PtyxisTab *tab = win ? ptyxis_window_get_active_tab (win) : NULL;

  g_print ("[test_bug5] close-pane: win=%p tab=%p n_panes=%u\n",
           (void*)win, (void*)tab,
           tab ? ptyxis_tab_get_n_panes (tab) : 0);

  if (win != NULL)
    {
      /* win.close-pane: on a single-pane tab this falls through to page.close,
       * which parks the tab in the parking lot — exactly the user's trigger. */
      gtk_widget_activate_action (GTK_WIDGET (win), "win.close-pane", NULL);
    }
  return G_SOURCE_REMOVE;
}

static gboolean
do_destroy_window (gpointer user_data)
{
  GtkApplication *app = user_data;
  GList *windows = gtk_application_get_windows (app);
  PtyxisWindow *win = windows ? PTYXIS_WINDOW (windows->data) : NULL;
  PtyxisTab *tab = win ? ptyxis_window_get_active_tab (win) : NULL;

  /* Pre-arm the SIGKILL safety-net (exactly what the close-dialog confirm
   * path does via ptyxis_close_dialog_confirm -> ptyxis_tab_force_quit)
   * so that pending_kill_source is LIVE when PtyxisTab.dispose runs its
   * cancel-pending-kill-source step. This is the trigger from the user's
   * log (source id 683 was live at dispose time). */
  if (tab != NULL)
    {
      g_print ("[test_bug5] pre-arm force_quit on tab=%p\n", (void*)tab);
      ptyxis_tab_force_quit (tab);
    }

  g_print ("[test_bug5] destroy window=%p (kill source live)\n", (void*)win);

  if (win != NULL)
    gtk_window_destroy (GTK_WINDOW (win));

  return G_SOURCE_REMOVE;
}

static gboolean
do_quit (gpointer user_data)
{
  GtkApplication *app = user_data;
  g_print ("[test_bug5] quitting app\n");
  g_application_quit (G_APPLICATION (app));
  return G_SOURCE_REMOVE;
}

static void
on_activate (GtkApplication *app, gpointer user_data)
{
  PtyxisApplication *ptyxis_app = PTYXIS_APPLICATION (app);
  PtyxisProfile *profile = ptyxis_application_dup_default_profile (ptyxis_app);
  PtyxisWindow *win = ptyxis_window_new_empty ();
  PtyxisTab *tab;

  gtk_application_add_window (app, GTK_WINDOW (win));
  gtk_window_set_default_size (GTK_WINDOW (win), 800, 600);

  tab = ptyxis_tab_new (profile);
  ptyxis_window_add_tab (win, tab);
  g_object_unref (tab);

  gtk_window_present (GTK_WINDOW (win));

  /* t=1s: close-pane on the single (last) pane -> tab parked.
   * t=2s: destroy window -> PtyxisWindow.dispose -> parking_lot_remove(force_quit)
   *       -> PtyxisTab.dispose with pending_kill_source live.
   * t=3s: quit. */
  g_timeout_add_seconds (1, do_close_pane, app);
  g_timeout_add_seconds (2, do_destroy_window, app);
  g_timeout_add_seconds (3, do_quit, app);
}

int main (int argc, char **argv)
{
  PtyxisApplication *app;
  int status;

  app = ptyxis_application_new ("org.gnome.Ptyxis.test.bug5", 0);
  g_signal_connect (app, "activate", G_CALLBACK (on_activate), NULL);
  status = g_application_run (G_APPLICATION (app), argc, argv);
  g_object_unref (app);
  return status;
}
