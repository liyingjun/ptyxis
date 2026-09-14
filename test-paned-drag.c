/* test-paned-drag.c — after session restore, the divider must be
 * user-draggable again. We simulate a drag by calling
 * gtk_paned_set_position() on the restored root paned and pumping the
 * loop; the position must stick (not be snapped back to the saved value
 * by a stuck position guard). */
#include <gtk/gtk.h>
#include <adwaita.h>
#include "config.h"
#include "ptyxis-application.h"
#include "ptyxis-window.h"
#include "ptyxis-tab.h"

static GtkWidget *
find_first_paned (GtkWidget *w)
{
  if (GTK_IS_PANED (w))
    return w;
  for (GtkWidget *c = gtk_widget_get_first_child (w); c != NULL;
       c = gtk_widget_get_next_sibling (c))
    {
      GtkWidget *r = find_first_paned (c);
      if (r != NULL)
        return r;
    }
  return NULL;
}

static void
pump (int n)
{
  for (int i = 0; i < n; i++)
    {
      while (g_main_context_iteration (NULL, FALSE))
        ;
      g_usleep (20000);
    }
}

static void
on_activate (GtkApplication *app, gpointer user_data)
{
  PtyxisApplication *pa = PTYXIS_APPLICATION (app);
  g_autoptr(PtyxisProfile) profile = ptyxis_application_dup_default_profile (pa);
  int *exit_code = user_data;

  /* === SOURCE: split-V at 250 then 350 === */
  /* NOTE: we use a plain GtkApplicationWindow rather than PtyxisWindow so
   * the test exercises only the paned position logic; tearing down a real
   * PtyxisWindow with a live multi-pane tab drives the parking-lot /
   * force-quit path (Bug 5 territory), which is covered elsewhere. The
   * ptyxis_window_is_animating CRITICALs seen during the run are a harmless
   * artifact of the tab casting its plain GtkWindow to PtyxisWindow. */
  PtyxisTab *src = ptyxis_tab_new (profile);
  GtkWidget *swin = gtk_application_window_new (app);
  gtk_window_set_child (GTK_WINDOW (swin), GTK_WIDGET (src));
  gtk_window_set_default_size (GTK_WINDOW (swin), 800, 600);
  ptyxis_tab_split_full (src, GTK_ORIENTATION_VERTICAL, 250, NULL, NULL);
  ptyxis_tab_split_full (src, GTK_ORIENTATION_VERTICAL, 350, NULL, NULL);
  gtk_window_present (GTK_WINDOW (swin));
  pump (20);
  GVariant *saved = ptyxis_tab_dup_panes_state (src);
  gtk_window_destroy (GTK_WINDOW (swin));
  pump (5);
  g_object_unref (src);

  /* === DEST: restore === */
  PtyxisTab *dst = ptyxis_tab_new (profile);
  GtkWidget *dwin = gtk_application_window_new (app);
  gtk_window_set_child (GTK_WINDOW (dwin), GTK_WIDGET (dst));
  gtk_window_set_default_size (GTK_WINDOW (dwin), 800, 600);
  gtk_window_present (GTK_WINDOW (dwin));
  pump (20);
  ptyxis_tab_restore_panes_state (dst, saved);
  pump (30);  /* let layout settle after restore */

  GtkWidget *paned = find_first_paned (GTK_WIDGET (dst));
  g_assert_nonnull (paned);

  int restored = gtk_paned_get_position (GTK_PANED (paned));
  g_print ("[drag] restored root paned position = %d (saved 250)\n", restored);

  /* Simulate a user drag to a new value. */
  int target = restored + 123;
  if (target > 760)
    target = 760;
  gtk_paned_set_position (GTK_PANED (paned), target);
  pump (20);
  int after = gtk_paned_get_position (GTK_PANED (paned));
  g_print ("[drag] after set_position(%d) + pump: position = %d\n", target, after);

  if (after == target)
    {
      g_print ("[drag] PASS: divider is user-movable after restore\n");
      *exit_code = 0;
    }
  else
    {
      g_print ("[drag] FAIL: divider snapped back to %d (guard still armed)\n", after);
      *exit_code = 1;
    }

  gtk_window_destroy (GTK_WINDOW (dwin));
  pump (5);
  g_object_unref (dst);
  g_variant_unref (saved);
  g_application_quit (G_APPLICATION (app));
}

#include <glib.h>
gint64 ptyxis_application_get_default_rlimit_nofile (void) { return 0; }

int
main (int argc, char **argv)
{
  PtyxisApplication *app = ptyxis_application_new ("test.paned.drag",
                                                   G_APPLICATION_NON_UNIQUE);
  int exit_code = 99;
  g_signal_connect (app, "activate", G_CALLBACK (on_activate), &exit_code);
  g_application_run (G_APPLICATION (app), argc, argv);
  g_object_unref (app);
  return exit_code;
}
