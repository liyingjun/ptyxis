/*
 * ptyxis-tab.c
 *
 * Copyright 2023 Christian Hergert <chergert@redhat.com>
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

#include <cairo.h>

#ifdef __linux__
# include <libportal/portal.h>
# include <libportal-gtk4/portal-gtk4.h>
#endif

#include "ptyxis-agent-ipc.h"
#include "ptyxis-application.h"
#include "ptyxis-enums.h"
#include "ptyxis-inspector.h"
#include "ptyxis-tab-monitor.h"
#include "ptyxis-tab-notify.h"
#include "ptyxis-tab-private.h"
#include "ptyxis-terminal.h"
#include "ptyxis-util.h"
#include "ptyxis-window.h"

typedef enum _PtyxisTabState
{
  PTYXIS_TAB_STATE_INITIAL,
  PTYXIS_TAB_STATE_SPAWNING,
  PTYXIS_TAB_STATE_RUNNING,
  PTYXIS_TAB_STATE_EXITED,
  PTYXIS_TAB_STATE_FAILED,
} PtyxisTabState;

typedef struct _PtyxisTabPane PtyxisTabPane;

struct _PtyxisTabPane
{
  PtyxisTab               *tab;
  GtkWidget               *box;
  AdwBanner               *banner;
  GtkScrolledWindow       *scrolled_window;
  PtyxisTerminal          *terminal;

  PtyxisIpcProcess        *process;
  GCancellable            *cancellable;
  char                    *command_line;
  char                    *program_name;
  char                    *initial_working_directory_uri;
  char                    *previous_working_directory_uri;
  PtyxisIpcContainer      *container_at_creation;
  char                   **command;

  PtyxisTabState           state;
  GPid                     pid;
  gint64                   respawn_time;

  PtyxisProcessLeaderKind  leader_kind : 3;
  guint                    has_foreground_process : 1;
  guint                    forced_exit : 1;
  guint                    is_primary : 1;
};

struct _PtyxisTab
{
  GtkWidget                parent_instance;

  char                    *initial_working_directory_uri;
  char                    *previous_working_directory_uri;
  PtyxisProfile           *profile;
  PtyxisIpcProcess        *process;
  char                    *title_prefix;
  PtyxisTabMonitor        *monitor;
  char                    *uuid;
  PtyxisIpcContainer      *container_at_creation;
  char                   **command;
  char                    *initial_title;
  GdkTexture              *cached_texture;
  GtkBox                  *content_box;
  AdwBanner               *banner;
  GtkScrolledWindow       *scrolled_window;
  PtyxisTerminal          *terminal;
  char                    *command_line;
  char                    *program_name;
  PtyxisTabNotify          notify;
  GSignalGroup            *profile_signals;

  GPtrArray               *panes;
  PtyxisTabPane           *active_pane;

  /* Ordered list of panes that have gained focus, most recent first.
   * Updated on every focus transition; consulted when closing a pane
   * to decide where focus should land (the user expects focus to
   * return to the pane that was focused just before the closing one,
   * not to whatever GTK picks after the widget tree is mutated).
   * Entries are not ref-held: panes are owned by self->panes and
   * close_pane_widget looks the candidate up in self->panes before
   * dereferencing, so stale entries are harmless.
   */
  GPtrArray               *focus_history;

  PtyxisTabState           state;
  GPid                     pid;

  gint64                   respawn_time;

  PtyxisZoomLevel          zoom : 5;
  PtyxisProcessLeaderKind  leader_kind : 3;
  guint                    has_foreground_process : 1;
  guint                    forced_exit : 1;
  guint                    ignore_osc_title : 1;
  guint                    ignore_snapshot : 1;

  guint                    inhibit_cookie;

  /* Deferred focus-grab source.
   *
   * When the user (or a notification such as ::selected-page) asks us
   * to focus the active pane's terminal, the target may not yet be
   * mapped or realized — for example when switching to a tab 2+ in an
   * AdwTabView whose paned children only get real allocations after
   * the page becomes the selected one. Calling gtk_widget_grab_focus()
   * in that window silently no-ops, leaving focus on the previous tab
   * (or nowhere) and the first split pane becomes unresponsive to
   * keystrokes. We schedule a low-priority idle that retries until
   * the grab sticks, coalescing repeated calls via this single source
   * id. */
  guint                    pending_focus_source;

  /* Source id of the 50 ms SIGKILL safety-net timer scheduled by
   * ptyxis_tab_force_quit(). The shell often exits cleanly within a
   * few ms of SIGHUP (especially when the spawned process sees its
   * stdin close) and PtyxisTab.dispose() runs almost immediately
   * afterwards — long before the 50 ms SIGKILL timer fires. Without
   * cancellation the timer callback would dereference an already-
   * finalized PtyxisTab and trip the PTYXIS_IS_TAB assertion in
   * ptyxis_tab_force_quit_in_idle(). Tracked here so dispose() can
   * g_source_remove() it. */
  guint                    pending_kill_source;
};

enum {
  PROP_0,
  PROP_COMMAND_LINE,
  PROP_ICON,
  PROP_IGNORE_OSC_TITLE,
  PROP_INDICATOR_ICON,
  PROP_PROCESS_LEADER_KIND,
  PROP_PROFILE,
  PROP_PROGRESS,
  PROP_PROGRESS_FRACTION,
  PROP_READ_ONLY,
  PROP_SUBTITLE,
  PROP_TITLE,
  PROP_TITLE_PREFIX,
  PROP_UUID,
  PROP_ZOOM,
  PROP_ZOOM_LABEL,
  PROP_N_PANES,
  N_PROPS
};

enum {
  BELL,
  COMMIT,
  N_SIGNALS
};

static void ptyxis_tab_respawn (PtyxisTab *self);
static void ptyxis_tab_map_cb (GtkWidget *widget, gpointer user_data);
static void ptyxis_tab_install_position_guard (GtkWidget *widget);
static void ptyxis_tab_schedule_focus_grab (PtyxisTab *self);
static void ptyxis_tab_profile_signals_bind_cb (PtyxisTab     *self,
                                                PtyxisProfile *profile,
                                                GSignalGroup  *group);
static void ptyxis_tab_pane_free (gpointer data);
static void ptyxis_tab_sync_active_pane (PtyxisTab     *self,
                                         PtyxisTabPane *pane);
static void ptyxis_tab_pane_respawn (PtyxisTabPane *pane);
static void ptyxis_tab_bind_terminal_settings (PtyxisTab      *self,
                                               PtyxisTerminal *terminal);
static void ptyxis_tab_apply_zoom_to_terminal (PtyxisTab      *self,
                                               PtyxisTerminal *terminal);
static gboolean ptyxis_tab_close_pane_widget (PtyxisTab     *self,
                                              PtyxisTabPane *pane);
static void ptyxis_tab_pane_apply_scrollbar_policy (PtyxisTabPane *pane);
static void ptyxis_tab_pane_send_signal (PtyxisTabPane *pane,
                                         int            signum);
static void ptyxis_tab_pane_focus_enter_cb (PtyxisTabPane           *pane,
                                           GParamSpec              *pspec,
                                           GtkEventControllerFocus *focus);

G_DEFINE_FINAL_TYPE (PtyxisTab, ptyxis_tab, GTK_TYPE_WIDGET)

#ifdef __linux__
static XdpPortal *portal;
#endif

static GParamSpec *properties[N_PROPS];
static guint signals[N_SIGNALS];
static double zoom_font_scales[] = {
  0,

  /* MINUS_14 through MINUS_1: each step is 1.2^(1/2) ≈ 1.095445 */
  1.0 / (1.2 * 1.2 * 1.2 * 1.2 * 1.2 * 1.2 * 1.2),                     /* MINUS_14: 1.2^(-7) */
  1.0 / (1.2 * 1.2 * 1.2 * 1.2 * 1.2 * 1.2 * 1.2) * 1.095445115010332, /* MINUS_13: 1.2^(-6.5) */
  1.0 / (1.2 * 1.2 * 1.2 * 1.2 * 1.2 * 1.2),                           /* MINUS_12: 1.2^(-6) */
  1.0 / (1.2 * 1.2 * 1.2 * 1.2 * 1.2 * 1.2) * 1.095445115010332,       /* MINUS_11: 1.2^(-5.5) */
  1.0 / (1.2 * 1.2 * 1.2 * 1.2 * 1.2),                                 /* MINUS_10: 1.2^(-5) */
  1.0 / (1.2 * 1.2 * 1.2 * 1.2 * 1.2) * 1.095445115010332,             /* MINUS_9: 1.2^(-4.5) */
  1.0 / (1.2 * 1.2 * 1.2 * 1.2),                                       /* MINUS_8: 1.2^(-4) */
  1.0 / (1.2 * 1.2 * 1.2 * 1.2) * 1.095445115010332,                   /* MINUS_7: 1.2^(-3.5) */
  1.0 / (1.2 * 1.2 * 1.2),                                             /* MINUS_6: 1.2^(-3) */
  1.0 / (1.2 * 1.2 * 1.2) * 1.095445115010332,                         /* MINUS_5: 1.2^(-2.5) */
  1.0 / (1.2 * 1.2),                                                   /* MINUS_4: 1.2^(-2) */
  1.0 / (1.2 * 1.2) * 1.095445115010332,                               /* MINUS_3: 1.2^(-1.5) */
  1.0 / (1.2),                                                         /* MINUS_2: 1.2^(-1) */
  1.0 / (1.2) * 1.095445115010332,                                     /* MINUS_1: 1.2^(-0.5) */
  1.0,                                                                 /* DEFAULT: 1.2^0 */

  /* PLUS_1 through PLUS_14: each step is 1.2^(1/2) ≈ 1.095445 */
  1.0 * 1.095445115010332,                                             /* PLUS_1: 1.2^0.5 */
  1.0 * 1.2,                                                           /* PLUS_2: 1.2^1 */
  1.0 * 1.2 * 1.095445115010332,                                       /* PLUS_3: 1.2^1.5 */
  1.0 * 1.2 * 1.2,                                                     /* PLUS_4: 1.2^2 */
  1.0 * 1.2 * 1.2 * 1.095445115010332,                                 /* PLUS_5: 1.2^2.5 */
  1.0 * 1.2 * 1.2 * 1.2,                                               /* PLUS_6: 1.2^3 */
  1.0 * 1.2 * 1.2 * 1.2 * 1.095445115010332,                           /* PLUS_7: 1.2^3.5 */
  1.0 * 1.2 * 1.2 * 1.2 * 1.2,                                         /* PLUS_8: 1.2^4 */
  1.0 * 1.2 * 1.2 * 1.2 * 1.2 * 1.095445115010332,                     /* PLUS_9: 1.2^4.5 */
  1.0 * 1.2 * 1.2 * 1.2 * 1.2 * 1.2,                                   /* PLUS_10: 1.2^5 */
  1.0 * 1.2 * 1.2 * 1.2 * 1.2 * 1.2 * 1.095445115010332,               /* PLUS_11: 1.2^5.5 */
  1.0 * 1.2 * 1.2 * 1.2 * 1.2 * 1.2 * 1.2,                             /* PLUS_12: 1.2^6 */
  1.0 * 1.2 * 1.2 * 1.2 * 1.2 * 1.2 * 1.2 * 1.095445115010332,         /* PLUS_13: 1.2^6.5 */
  1.0 * 1.2 * 1.2 * 1.2 * 1.2 * 1.2 * 1.2 * 1.2,                       /* PLUS_14: 1.2^7 */
};

static gboolean
on_scroll_scrolled_cb (GtkEventControllerScroll *scroll,
                       double                    dx,
                       double                    dy,
                       PtyxisTab                *self)
{
  GdkModifierType mods;

  g_assert (GTK_IS_EVENT_CONTROLLER_SCROLL (scroll));
  g_assert (PTYXIS_IS_TAB (self));

  mods = gtk_event_controller_get_current_event_state (GTK_EVENT_CONTROLLER (scroll));

  if ((mods & GDK_CONTROL_MASK) != 0)
    {
      PtyxisSettings *settings = ptyxis_application_get_settings (PTYXIS_APPLICATION_DEFAULT);

      if (ptyxis_settings_get_enable_zoom_scroll_ctrl(settings))
        {
          if (dy < 0)
            ptyxis_tab_zoom_in (self);
          else if (dy > 0)
            ptyxis_tab_zoom_out (self);
	}

      return TRUE;
    }

  return FALSE;
}

static void
on_scroll_begin_cb (GtkEventControllerScroll *scroll,
                    PtyxisTab                *self)
{
  GdkModifierType state;

  g_assert (GTK_IS_EVENT_CONTROLLER_SCROLL (scroll));
  g_assert (PTYXIS_IS_TAB (self));

  state = gtk_event_controller_get_current_event_state (GTK_EVENT_CONTROLLER (scroll));

  if ((state & GDK_CONTROL_MASK) != 0)
    gtk_event_controller_scroll_set_flags (scroll,
                                           GTK_EVENT_CONTROLLER_SCROLL_VERTICAL |
                                           GTK_EVENT_CONTROLLER_SCROLL_DISCRETE);
}

static void
on_scroll_end_cb (GtkEventControllerScroll *scroll,
                  PtyxisTab                *self)
{
  g_assert (GTK_IS_EVENT_CONTROLLER_SCROLL (scroll));
  g_assert (PTYXIS_IS_TAB (self));

  gtk_event_controller_scroll_set_flags (scroll, GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
}

static void
ptyxis_tab_send_signal (PtyxisTab *self,
                        int        signum)
{
  g_autofree char *title = NULL;

  g_assert (PTYXIS_IS_TAB (self));

  if (self->process == NULL)
    {
      g_debug ("Cannot send signal %d to tab, process is gone.", signum);
      return;
    }

  title = ptyxis_tab_dup_title (self);
  g_debug ("Sending signal %d to tab \"%s\"", signum, title);

  ptyxis_ipc_process_call_send_signal (self->process, signum, NULL, NULL, NULL);
}

static gboolean
ptyxis_tab_is_active (PtyxisTab *self)
{
  GtkWidget *window;

  g_assert (PTYXIS_IS_TAB (self));

  if ((window = gtk_widget_get_ancestor (GTK_WIDGET (self), PTYXIS_TYPE_WINDOW)))
    return ptyxis_window_get_active_tab (PTYXIS_WINDOW (window)) == self;

  return FALSE;
}

static void
ptyxis_tab_update_scrollback_lines (PtyxisTab *self)
{
  long scrollback_lines = -1;

  g_assert (PTYXIS_IS_TAB (self));

  if (ptyxis_profile_get_limit_scrollback (self->profile))
    scrollback_lines = ptyxis_profile_get_scrollback_lines (self->profile);

  if (self->panes != NULL)
    {
      for (guint i = 0; i < self->panes->len; i++)
        {
          PtyxisTabPane *pane = g_ptr_array_index (self->panes, i);

          if (pane->terminal != NULL)
            vte_terminal_set_scrollback_lines (VTE_TERMINAL (pane->terminal), scrollback_lines);
        }
    }
  else if (self->terminal != NULL)
    {
      vte_terminal_set_scrollback_lines (VTE_TERMINAL (self->terminal), scrollback_lines);
    }
}

static void
ptyxis_tab_update_cell_height_scale (PtyxisTab *self)
{
  double cell_height_scale = 1.0;

  g_assert (PTYXIS_IS_TAB (self));

  if (ptyxis_profile_get_cell_height_scale (self->profile))
    cell_height_scale = ptyxis_profile_get_cell_height_scale (self->profile);

  if (self->panes != NULL)
    {
      for (guint i = 0; i < self->panes->len; i++)
        {
          PtyxisTabPane *pane = g_ptr_array_index (self->panes, i);

          if (pane->terminal != NULL)
            vte_terminal_set_cell_height_scale (VTE_TERMINAL (pane->terminal), cell_height_scale);
        }
    }
  else if (self->terminal != NULL)
    {
      vte_terminal_set_cell_height_scale (VTE_TERMINAL (self->terminal), cell_height_scale);
    }
}

static void
ptyxis_tab_update_cell_width_scale (PtyxisTab *self)
{
  double cell_width_scale = 1.0;

  g_assert (PTYXIS_IS_TAB (self));

  if (ptyxis_profile_get_cell_width_scale (self->profile))
    cell_width_scale = ptyxis_profile_get_cell_width_scale (self->profile);

  if (self->panes != NULL)
    {
      for (guint i = 0; i < self->panes->len; i++)
        {
          PtyxisTabPane *pane = g_ptr_array_index (self->panes, i);

          if (pane->terminal != NULL)
            vte_terminal_set_cell_width_scale (VTE_TERMINAL (pane->terminal), cell_width_scale);
        }
    }
  else if (self->terminal != NULL)
    {
      vte_terminal_set_cell_width_scale (VTE_TERMINAL (self->terminal), cell_width_scale);
    }
}

static void
ptyxis_tab_update_custom_links (PtyxisTab *self)
{
  g_autoptr(GListModel) custom_links_list = NULL;

  g_assert (PTYXIS_IS_TAB (self));

  custom_links_list = ptyxis_profile_list_custom_links(self->profile);

  if (self->panes != NULL)
    {
      for (guint i = 0; i < self->panes->len; i++)
        {
          PtyxisTabPane *pane = g_ptr_array_index (self->panes, i);

          if (pane->terminal != NULL)
            ptyxis_terminal_update_custom_links_list(pane->terminal, custom_links_list);
        }
    }
  else if (self->terminal != NULL)
    {
      ptyxis_terminal_update_custom_links_list(self->terminal, custom_links_list);
    }
}

static void
ptyxis_tab_update_inhibit (PtyxisTab *self)
{
  PtyxisSettings *settings;
  gboolean inhibit = FALSE;
  GtkWidget *window;

  g_assert (PTYXIS_IS_TAB (self));

  settings = ptyxis_application_get_settings (PTYXIS_APPLICATION_DEFAULT);

  /* Clear if the user has disabled logout inhibition */
  if (!ptyxis_settings_get_inhibit_logout (settings))
    {
      if (self->inhibit_cookie)
        {
          gtk_application_uninhibit (GTK_APPLICATION (PTYXIS_APPLICATION_DEFAULT),
                                     self->inhibit_cookie);
          self->inhibit_cookie = 0;
        }

      return;
    }

  /* Only inhibit if there's a foreground process running and it's not a shell */
  if (self->has_foreground_process &&
      self->program_name != NULL &&
      !ptyxis_is_shell (self->program_name))
    inhibit = TRUE;

  /* Check if we need to change the inhibit state */
  if ((inhibit && self->inhibit_cookie != 0) ||
      (!inhibit && self->inhibit_cookie == 0))
    return;

  /* Get the window to use for the inhibit call */
  window = gtk_widget_get_ancestor (GTK_WIDGET (self), GTK_TYPE_WINDOW);

  if (inhibit)
    {
      /* Only inhibit if we have a valid window reference */
      if (window != NULL)
        {
          self->inhibit_cookie =
            gtk_application_inhibit (GTK_APPLICATION (PTYXIS_APPLICATION_DEFAULT),
                                     GTK_WINDOW (window),
                                     GTK_APPLICATION_INHIBIT_LOGOUT,
                                     _("A foreground process is running"));
        }
    }
  else
    {
      gtk_application_uninhibit (GTK_APPLICATION (PTYXIS_APPLICATION_DEFAULT),
                                 self->inhibit_cookie);
      self->inhibit_cookie = 0;
    }
}

static void
ptyxis_tab_wait_cb (GObject      *object,
                    GAsyncResult *result,
                    gpointer      user_data)
{
  PtyxisApplication *app = (PtyxisApplication *)object;
  g_autoptr(PtyxisTab) self = user_data;
  g_autoptr(GError) error = NULL;
  PtyxisExitAction exit_action;
  PtyxisWindow *window;
  AdwTabPage *page = NULL;
  GtkWidget *tab_view;
  gboolean is_front = FALSE;
  int exit_code;

  g_assert (PTYXIS_IS_APPLICATION (app));
  g_assert (G_IS_ASYNC_RESULT (result));
  g_assert (PTYXIS_IS_TAB (self));
  g_assert (self->state == PTYXIS_TAB_STATE_RUNNING);

  g_clear_object (&self->process);

  if (self->panes != NULL && self->panes->len > 0)
    {
      PtyxisTabPane *primary = g_ptr_array_index (self->panes, 0);

      if (primary->is_primary)
        g_clear_object (&primary->process);
    }

  /* Update inhibit state when process exits */
  ptyxis_tab_update_inhibit (self);

  exit_code = ptyxis_application_wait_finish (app, result, &error);

  g_debug ("Process completed with exit-code 0x%x %s",
           exit_code,
           error ? error->message : "");

  if (error == NULL && WIFEXITED (exit_code) && WEXITSTATUS (exit_code) == 0)
    self->state = PTYXIS_TAB_STATE_EXITED;
  else
    self->state = PTYXIS_TAB_STATE_FAILED;

  if (self->panes != NULL && self->panes->len > 0)
    {
      PtyxisTabPane *primary = g_ptr_array_index (self->panes, 0);

      if (primary->is_primary)
        primary->state = self->state;
    }

  if (self->forced_exit)
    return;

  if ((window = PTYXIS_WINDOW (gtk_widget_get_ancestor (GTK_WIDGET (self), PTYXIS_TYPE_WINDOW))))
    is_front = self == ptyxis_window_get_active_tab (window);

  if (WIFSIGNALED (exit_code))
    {
      g_autofree char *title = NULL;

      title = g_strdup_printf (_("Process Exited from Signal %d"), WTERMSIG (exit_code));

      adw_banner_set_title (self->banner, title);
      adw_banner_set_button_label (self->banner, _("_Restart"));
      gtk_actionable_set_action_name (GTK_ACTIONABLE (self->banner), "tab.respawn");
      gtk_widget_set_visible (GTK_WIDGET (self->banner), TRUE);
      return;
    }

  exit_action = ptyxis_profile_get_exit_action (self->profile);
  tab_view = gtk_widget_get_ancestor (GTK_WIDGET (self), ADW_TYPE_TAB_VIEW);

  /* If this was started with something like ptyxis_window_new_for_command()
   * then we just want to exit the application (so allow tab to close).
   */
  if (self->command != NULL)
    exit_action = PTYXIS_EXIT_ACTION_CLOSE;

  if (ADW_IS_TAB_VIEW (tab_view))
    page = adw_tab_view_get_page (ADW_TAB_VIEW (tab_view), GTK_WIDGET (self));

  /* Always prepare the banner even if we don't show it because we may
   * display it again if the tab is removed from the parking lot and
   * restored into the window.
   */
  adw_banner_set_title (self->banner, _("Process Exited"));
  adw_banner_set_button_label (self->banner, _("_Restart"));
  gtk_actionable_set_action_name (GTK_ACTIONABLE (self->banner), "tab.respawn");

  /* If we took less than .5 a second to spawn and no key has been
   * pressed in the terminal, then treat this as a failed spawn. Don't
   * allow ourselves to auto-close in that case as it's likely an error
   * the user would want to see.
   */
  if ((self->command == NULL || self->state == PTYXIS_TAB_STATE_FAILED) &&
      (g_get_monotonic_time () - self->respawn_time) < (G_USEC_PER_SEC/2) &&
      !ptyxis_tab_monitor_get_has_pressed_key (self->monitor))
    exit_action = PTYXIS_EXIT_ACTION_NONE;

  switch (exit_action)
    {
    case PTYXIS_EXIT_ACTION_RESTART:
      ptyxis_tab_respawn (self);
      break;

    case PTYXIS_EXIT_ACTION_CLOSE:
      if (ADW_IS_TAB_VIEW (tab_view) && ADW_IS_TAB_PAGE (page))
        {
          if (adw_tab_page_get_pinned (page))
            adw_tab_view_set_page_pinned (ADW_TAB_VIEW (tab_view), page, FALSE);
          adw_tab_view_close_page (ADW_TAB_VIEW (tab_view), page);
        }
      break;

    case PTYXIS_EXIT_ACTION_NONE:
      gtk_widget_set_visible (GTK_WIDGET (self->banner), TRUE);
      if (is_front)
        gtk_widget_child_focus (GTK_WIDGET (self->banner), GTK_DIR_TAB_FORWARD);
      break;

    default:
      g_assert_not_reached ();
    }

  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_TITLE]);
}

static void
ptyxis_tab_spawn_cb (GObject      *object,
                     GAsyncResult *result,
                     gpointer      user_data)
{
  PtyxisApplication *app = (PtyxisApplication *)object;
  g_autoptr(PtyxisIpcProcess) process = NULL;
  g_autoptr(PtyxisTab) self = user_data;
  g_autoptr(GError) error = NULL;

  g_assert (PTYXIS_IS_TAB (self));
  g_assert (G_IS_ASYNC_RESULT (result));
  g_assert (PTYXIS_IS_TAB (self));
  g_assert (self->state == PTYXIS_TAB_STATE_SPAWNING);

  if (!(process = ptyxis_application_spawn_finish (app, result, &error)))
    {
      g_debug ("[spawn] tab_spawn_cb FAIL tab=%p: %s",
               (void*)self, error ? error->message : "(no error)");

      const char *profile_uuid = ptyxis_profile_get_uuid (self->profile);

      self->state = PTYXIS_TAB_STATE_FAILED;

      vte_terminal_feed (VTE_TERMINAL (self->terminal), error->message, -1);
      vte_terminal_feed (VTE_TERMINAL (self->terminal), "\r\n", -1);

      adw_banner_set_title (self->banner, _("Failed to launch terminal"));
      adw_banner_set_button_label (self->banner, _("Edit Profile"));
      gtk_actionable_set_action_target (GTK_ACTIONABLE (self->banner), "s", profile_uuid);
      gtk_actionable_set_action_name (GTK_ACTIONABLE (self->banner), "app.edit-profile");
      gtk_widget_set_visible (GTK_WIDGET (self->banner), TRUE);

      return;
    }

  g_debug ("[spawn] tab_spawn_cb OK tab=%p -> process=%p, queuing wait_async",
           (void*)self, (void*)process);

  /* Post-spawn PTY probe for the primary pane — same as the non-primary
   * path. If VTE has no PTY here, the shell ran but its stdio goes
   * nowhere — the "blinking cursor, no bash" symptom. */
  {
    VtePty *pty = vte_terminal_get_pty (VTE_TERMINAL (self->terminal));
    if (pty == NULL)
      {
        g_debug ("[spawn] PTY_PROBE tab=%p (primary): NO PTY attached to VTE — "
                 "shell ran but its stdio has nowhere to go",
                 (void*)self);
      }
    else
      {
        int fd = vte_pty_get_fd (pty);
        g_debug ("[spawn] PTY_PROBE tab=%p (primary): pty=%p fd=%d",
                 (void*)self, (void*)pty, fd);
      }
  }

  self->state = PTYXIS_TAB_STATE_RUNNING;
  self->respawn_time = g_get_monotonic_time ();

  g_set_object (&self->process, process);

  if (self->active_pane != NULL && self->active_pane->is_primary)
    {
      self->active_pane->state = self->state;
      self->active_pane->respawn_time = self->respawn_time;
      g_set_object (&self->active_pane->process, process);
    }
  else if (self->panes != NULL && self->panes->len > 0)
    {
      PtyxisTabPane *primary = g_ptr_array_index (self->panes, 0);

      if (primary->is_primary)
        {
          primary->state = self->state;
          primary->respawn_time = self->respawn_time;
          g_set_object (&primary->process, process);
        }
    }

  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_ICON]);

  ptyxis_application_wait_async (app,
                                 process,
                                 NULL,
                                 ptyxis_tab_wait_cb,
                                 g_object_ref (self));
}

static void
ptyxis_tab_respawn (PtyxisTab *self)
{
  g_autofree char *default_container = NULL;
  g_autoptr(PtyxisIpcContainer) container = NULL;
  g_autoptr(VtePty) new_pty = NULL;
  PtyxisApplication *app;
  const char *profile_uuid;
  const char *cwd_uri;
  VtePty *pty;

  g_assert (PTYXIS_IS_TAB (self));
  g_assert (self->state == PTYXIS_TAB_STATE_INITIAL ||
            self->state == PTYXIS_TAB_STATE_EXITED ||
            self->state == PTYXIS_TAB_STATE_FAILED);

  g_debug ("[spawn] tab_respawn ENTER tab=%p primary_pane=%p container=%s "
           "cwd_uri=%s state=%d",
           (void*)self, (void*)self->terminal,
           default_container ? default_container : "(null)",
           self->initial_working_directory_uri ?
             self->initial_working_directory_uri : "(null)",
           self->state);

  if (g_getenv ("PTYXIS_DEBUG_RESPAWN"))
    {
      GtkWidget *c = gtk_widget_get_first_child (GTK_WIDGET (self));
      g_print ("[respawn] start tab=%p first_child=%p [%s]\n",
               (void*)self, (void*)c, c ? G_OBJECT_TYPE_NAME (c) : "(null)");
    }

  gtk_widget_set_visible (GTK_WIDGET (self->banner), FALSE);

  app = PTYXIS_APPLICATION_DEFAULT;
  profile_uuid = ptyxis_profile_get_uuid (self->profile);
  default_container = ptyxis_profile_dup_default_container (self->profile);

  if (self->container_at_creation != NULL)
    container = g_object_ref (self->container_at_creation);
  else
    container = ptyxis_application_lookup_container (app, default_container);

  if (container == NULL)
    {
      g_autofree char *title = NULL;

      self->state = PTYXIS_TAB_STATE_FAILED;

      title = g_strdup_printf (_("Cannot locate container “%s”"), default_container);
      adw_banner_set_title (self->banner, title);
      adw_banner_set_button_label (self->banner, _("Edit Profile"));
      gtk_actionable_set_action_target (GTK_ACTIONABLE (self->banner), "s", profile_uuid);
      gtk_actionable_set_action_name (GTK_ACTIONABLE (self->banner), "app.edit-profile");
      gtk_widget_set_visible (GTK_WIDGET (self->banner), TRUE);

      return;
    }

  self->state = PTYXIS_TAB_STATE_SPAWNING;

  /* If the primary terminal was destroyed (e.g. all panes have been
   * closed and only a non-primary remains, or vice versa), there is no
   * terminal to respawn into — bail out gracefully.
   */
  if (self->terminal == NULL)
    {
      self->state = PTYXIS_TAB_STATE_FAILED;
      return;
    }

  pty = vte_terminal_get_pty (VTE_TERMINAL (self->terminal));

  if (pty == NULL)
    {
      g_autoptr(GError) error = NULL;

      new_pty = ptyxis_application_create_pty (PTYXIS_APPLICATION_DEFAULT, &error);

      if (new_pty == NULL)
        {
          self->state = PTYXIS_TAB_STATE_FAILED;

          adw_banner_set_title (self->banner, _("Failed to create pseudo terminal device"));
          adw_banner_set_button_label (self->banner, NULL);
          gtk_actionable_set_action_name (GTK_ACTIONABLE (self->banner), NULL);
          gtk_widget_set_visible (GTK_WIDGET (self->banner), TRUE);

          return;
        }

      vte_terminal_set_pty (VTE_TERMINAL (self->terminal), new_pty);

      pty = new_pty;
    }

  cwd_uri = self->previous_working_directory_uri;
  if (self->initial_working_directory_uri)
    cwd_uri = self->initial_working_directory_uri;

  if (g_getenv ("PTYXIS_DEBUG_RESPAWN"))
    {
      GtkWidget *c = gtk_widget_get_first_child (GTK_WIDGET (self));
      g_print ("[respawn] before spawn_async tab=%p first_child=%p [%s] cwd=%s\n",
               (void*)self, (void*)c, c ? G_OBJECT_TYPE_NAME (c) : "(null)",
               cwd_uri ? cwd_uri : "(null)");
    }

  ptyxis_application_spawn_async (PTYXIS_APPLICATION_DEFAULT,
                                  container,
                                  self->profile,
                                  cwd_uri,
                                  pty,
                                  (const char * const *)self->command,
                                  NULL,
                                  ptyxis_tab_spawn_cb,
                                  g_object_ref (self));

  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_TITLE]);
}

static void
ptyxis_tab_respawn_action (GtkWidget  *widget,
                           const char *action_name,
                           GVariant   *params)
{
  PtyxisTab *self = (PtyxisTab *)widget;
  PtyxisTabPane *pane;

  g_assert (PTYXIS_IS_TAB (self));

  pane = self->active_pane;

  if (pane != NULL && !pane->is_primary)
    {
      if (pane->state == PTYXIS_TAB_STATE_FAILED ||
          pane->state == PTYXIS_TAB_STATE_EXITED)
        ptyxis_tab_pane_respawn (pane);
      return;
    }

  if (self->state == PTYXIS_TAB_STATE_FAILED ||
      self->state == PTYXIS_TAB_STATE_EXITED)
    ptyxis_tab_respawn (self);
}


static void
ptyxis_tab_inspect_action (GtkWidget  *widget,
                           const char *action_name,
                           GVariant   *params)
{
  PtyxisTab *self = (PtyxisTab *)widget;
  PtyxisInspector *inspector;
  GtkRoot *root;

  g_assert (PTYXIS_IS_TAB (self));

  inspector = ptyxis_inspector_new (self);
  root = gtk_widget_get_root (GTK_WIDGET (self));

  gtk_window_set_transient_for (GTK_WINDOW (inspector), GTK_WINDOW (root));
  gtk_window_set_modal (GTK_WINDOW (inspector), FALSE);
  gtk_window_present (GTK_WINDOW (inspector));
}

static void
ptyxis_tab_map (GtkWidget *widget)
{
  PtyxisTab *self = (PtyxisTab *)widget;

  g_assert (PTYXIS_IS_TAB (widget));

  if (g_getenv ("PTYXIS_DEBUG_MAP"))
    {
      GtkWidget *c = gtk_widget_get_first_child (GTK_WIDGET (self));
      g_print ("[map] tab %p: first_child=%p [%s] state=%d\n",
               (void*)self, (void*)c,
               c ? G_OBJECT_TYPE_NAME (c) : "(null)",
               self->state);
    }

  GTK_WIDGET_CLASS (ptyxis_tab_parent_class)->map (widget);

  if (g_getenv ("PTYXIS_DEBUG_MAP"))
    {
      GtkWidget *c = gtk_widget_get_first_child (GTK_WIDGET (self));
      g_print ("[map] tab %p: AFTER parent->map() first_child=%p [%s]\n",
               (void*)self, (void*)c,
               c ? G_OBJECT_TYPE_NAME (c) : "(null)");
    }

  if (self->state == PTYXIS_TAB_STATE_INITIAL)
    ptyxis_tab_respawn (self);

  /* After map (and a possible first respawn), grab focus into the
   * tab so the user's keystrokes route into the active terminal
   * instead of landing on whatever widget had focus before the tab
   * was activated. In multi-pane tabs this prevents the "I clicked
   * tab 2 and nothing happens when I type" report: without an
   * explicit grab_focus here the focus stays on the tab bar /
   * headerbar and the primary terminal never receives the keystrokes
   * until the user clicks into it manually.
   *
   * Note: ptyxis_tab_grab_focus() prefers the active pane's terminal
   * over the primary, but after a session restore the active pane is
   * whichever non-primary was created last by PtyxisTab_split_full().
   * For a freshly-activated tab we want focus in the PRIMARY
   * (left/top slot) — the pane the user is most likely to type into
   * first. Fall through to grab_focus on self->terminal directly. */
  if (gtk_widget_get_mapped (widget) && self->terminal != NULL)
    gtk_widget_grab_focus (GTK_WIDGET (self->terminal));
}

static void
ptyxis_tab_notify_contains_focus_cb (PtyxisTab               *self,
                                     GParamSpec              *pspec,
                                     GtkEventControllerFocus *focus)
{
  g_assert (PTYXIS_IS_TAB (self));
  g_assert (GTK_IS_EVENT_CONTROLLER_FOCUS (focus));

  if (gtk_event_controller_focus_contains_focus (focus))
    {
      ptyxis_tab_set_needs_attention (self, FALSE);
      g_application_withdraw_notification (G_APPLICATION (PTYXIS_APPLICATION_DEFAULT),
                                           self->uuid);
    }
}

static void
ptyxis_tab_notify_window_title_cb (PtyxisTab      *self,
                                   GParamSpec     *pspec,
                                   PtyxisTerminal *terminal)
{
  g_assert (PTYXIS_IS_TAB (self));
  g_assert (PTYXIS_IS_TERMINAL (terminal));

  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_TITLE]);
}

static void
ptyxis_tab_notify_window_subtitle_cb (PtyxisTab      *self,
                                      PtyxisTerminal *terminal)
{
  g_assert (PTYXIS_IS_TAB (self));
  g_assert (PTYXIS_IS_TERMINAL (terminal));

  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_SUBTITLE]);
}

static void
ptyxis_tab_increase_font_size_cb (PtyxisTab      *self,
                                  PtyxisTerminal *terminal)
{
  g_assert (PTYXIS_IS_TAB (self));
  g_assert (PTYXIS_IS_TERMINAL (terminal));

  ptyxis_tab_zoom_in (self);
}

static void
ptyxis_tab_decrease_font_size_cb (PtyxisTab      *self,
                                  PtyxisTerminal *terminal)
{
  g_assert (PTYXIS_IS_TAB (self));
  g_assert (PTYXIS_IS_TERMINAL (terminal));

  ptyxis_tab_zoom_out (self);
}

static void
ptyxis_tab_bell_cb (PtyxisTab      *self,
                    PtyxisTerminal *terminal)
{
  g_assert (PTYXIS_IS_TAB (self));
  g_assert (PTYXIS_IS_TERMINAL (terminal));

  g_signal_emit (self, signals[BELL], 0);
}

static PtyxisIpcContainer *
ptyxis_tab_discover_container (PtyxisTab *self)
{
  PtyxisTerminal *terminal;
  const char *current_container_name;
  const char *current_container_runtime;

  g_assert (PTYXIS_IS_TAB (self));

  /* Route through the safe getter so the weak-pointer / active-pane
   * fallback applies. If there is no live terminal (e.g. the last pane
   * was just closed), bail out instead of dereferencing a freed widget.
   */
  terminal = ptyxis_tab_get_terminal (self);
  if (terminal == NULL)
    return NULL;

  current_container_name = ptyxis_terminal_get_current_container_name (terminal);
  current_container_runtime = ptyxis_terminal_get_current_container_runtime (terminal);

  return ptyxis_application_find_container_by_name (PTYXIS_APPLICATION_DEFAULT,
                                                    current_container_runtime,
                                                    current_container_name);
}

static GIcon *
ptyxis_tab_dup_icon (PtyxisTab *self)
{
  PtyxisProcessLeaderKind kind;

  g_assert (PTYXIS_IS_TAB (self));

  kind = self->leader_kind;

  switch (kind)
    {
    default:
    case PTYXIS_PROCESS_LEADER_KIND_REMOTE:
      return g_themed_icon_new ("process-remote-symbolic");

    case PTYXIS_PROCESS_LEADER_KIND_SUPERUSER:
      return g_themed_icon_new ("process-superuser-symbolic");

    case PTYXIS_PROCESS_LEADER_KIND_CONTAINER:
    case PTYXIS_PROCESS_LEADER_KIND_UNKNOWN:
      {
        g_autoptr(PtyxisIpcContainer) container = NULL;
        const char *icon_name;

        if (!(container = ptyxis_tab_discover_container (self)))
          {
            if (!g_set_object (&container, self->container_at_creation))
              {
                if (self->profile != NULL)
                {
                  g_autofree char *profile_uuid = ptyxis_profile_dup_default_container (self->profile);

                  container = ptyxis_application_lookup_container (PTYXIS_APPLICATION_DEFAULT, profile_uuid);
                }
              }
          }

        if (container != NULL &&
            (icon_name = ptyxis_ipc_container_get_icon_name (container)) &&
            icon_name[0] != 0)
          return g_themed_icon_new (icon_name);
      }
      return NULL;
    }
}

static void
ptyxis_tab_invalidate_thumbnail (PtyxisTab *self)
{
  GtkWidget *view;
  AdwTabPage *page;

  g_assert (PTYXIS_IS_TAB (self));

  g_clear_object (&self->cached_texture);

  gtk_widget_queue_draw (GTK_WIDGET (self));

  if ((view = gtk_widget_get_ancestor (GTK_WIDGET (self), ADW_TYPE_TAB_VIEW)) &&
      (page = adw_tab_view_get_page (ADW_TAB_VIEW (view), GTK_WIDGET (self))))
    adw_tab_page_invalidate_thumbnail (page);
}

static void
ptyxis_tab_notify_palette_cb (PtyxisTab      *self,
                              GParamSpec     *pspec,
                              PtyxisTerminal *terminal)
{
  g_assert (PTYXIS_IS_TAB (self));
  g_assert (PTYXIS_IS_TERMINAL (terminal));

  ptyxis_tab_invalidate_thumbnail (self);
}

static void
ptyxis_tab_update_scrollbar_policy (PtyxisTab *self)
{
  g_assert (PTYXIS_IS_TAB (self));

  if (self->panes != NULL)
    {
      for (guint i = 0; i < self->panes->len; i++)
        {
          PtyxisTabPane *pane = g_ptr_array_index (self->panes, i);

          ptyxis_tab_pane_apply_scrollbar_policy (pane);
        }
    }
  else if (self->scrolled_window != NULL)
    {
      PtyxisTabPane fake = {
        .scrolled_window = self->scrolled_window,
      };

      ptyxis_tab_pane_apply_scrollbar_policy (&fake);
    }
}

static void
ptyxis_tab_update_padding_cb (PtyxisTab      *self,
                              GParamSpec     *pspec,
                              PtyxisSettings *settings)
{
  g_assert (PTYXIS_IS_TAB (self));
  g_assert (PTYXIS_IS_SETTINGS (settings));

  if (self->panes != NULL)
    {
      for (guint i = 0; i < self->panes->len; i++)
        {
          PtyxisTabPane *pane = g_ptr_array_index (self->panes, i);

          if (pane->terminal == NULL)
            continue;

          if (ptyxis_settings_get_disable_padding (settings))
            gtk_widget_remove_css_class (GTK_WIDGET (pane->terminal), "padded");
          else
            gtk_widget_add_css_class (GTK_WIDGET (pane->terminal), "padded");
        }
    }
  else if (self->terminal != NULL)
    {
      if (ptyxis_settings_get_disable_padding (settings))
        gtk_widget_remove_css_class (GTK_WIDGET (self->terminal), "padded");
      else
        gtk_widget_add_css_class (GTK_WIDGET (self->terminal), "padded");
    }
}

static void
ptyxis_tab_update_word_char_exceptions (PtyxisTab      *self,
                                        GParamSpec     *pspec,
                                        PtyxisSettings *settings)
{
  g_autofree char *word_char_exceptions = NULL;

  g_assert (PTYXIS_IS_TAB (self));
  g_assert (PTYXIS_IS_SETTINGS (settings));

  word_char_exceptions = ptyxis_settings_dup_word_char_exceptions (settings);

  if (self->panes != NULL)
    {
      for (guint i = 0; i < self->panes->len; i++)
        {
          PtyxisTabPane *pane = g_ptr_array_index (self->panes, i);

          if (pane->terminal != NULL)
            vte_terminal_set_word_char_exceptions (VTE_TERMINAL (pane->terminal), word_char_exceptions);
        }
    }
  else if (self->terminal != NULL)
    {
      vte_terminal_set_word_char_exceptions (VTE_TERMINAL (self->terminal), word_char_exceptions);
    }
}

static void
ptyxis_tab_constructed (GObject *object)
{
  PtyxisTab *self = (PtyxisTab *)object;
  PtyxisSettings *settings;

  G_OBJECT_CLASS (ptyxis_tab_parent_class)->constructed (object);

  settings = ptyxis_application_get_settings (PTYXIS_APPLICATION_DEFAULT);
  g_object_bind_property (settings, "audible-bell",
                          self->terminal, "audible-bell",
                          G_BINDING_SYNC_CREATE);
  g_object_bind_property (settings, "cursor-shape",
                          self->terminal, "cursor-shape",
                          G_BINDING_SYNC_CREATE);
  g_object_bind_property (settings, "cursor-blink-mode",
                          self->terminal, "cursor-blink-mode",
                          G_BINDING_SYNC_CREATE);
  g_object_bind_property (settings, "enable-a11y",
                          self->terminal, "enable-a11y",
                          G_BINDING_SYNC_CREATE);
  g_object_bind_property (settings, "font-desc",
                          self->terminal, "font-desc",
                          G_BINDING_SYNC_CREATE);
  g_object_bind_property (settings, "text-blink-mode",
                          self->terminal, "text-blink-mode",
                          G_BINDING_SYNC_CREATE);
  g_object_bind_property (settings, "ignore-osc-title",
                          self, "ignore-osc-title",
                          G_BINDING_SYNC_CREATE);

  g_signal_connect_object (settings,
                           "notify::disable-padding",
                           G_CALLBACK (ptyxis_tab_update_padding_cb),
                           self,
                           G_CONNECT_SWAPPED);
  ptyxis_tab_update_padding_cb (self, NULL, settings);

  g_signal_connect_object (PTYXIS_APPLICATION_DEFAULT,
                           "notify::overlay-scrollbars",
                           G_CALLBACK (ptyxis_tab_update_scrollbar_policy),
                           self,
                           G_CONNECT_SWAPPED);
  g_signal_connect_object (settings,
                           "notify::scrollbar-policy",
                           G_CALLBACK (ptyxis_tab_update_scrollbar_policy),
                           self,
                           G_CONNECT_SWAPPED);
  ptyxis_tab_update_scrollbar_policy (self);

  /* Set up signal group for profile signals */
  self->profile_signals = g_signal_group_new (PTYXIS_TYPE_PROFILE);
  g_signal_connect_object (self->profile_signals,
                           "bind",
                           G_CALLBACK (ptyxis_tab_profile_signals_bind_cb),
                           self,
                           G_CONNECT_SWAPPED);
  g_signal_group_connect_object (self->profile_signals,
                                 "notify::limit-scrollback",
                                 G_CALLBACK (ptyxis_tab_update_scrollback_lines),
                                 self,
                                 G_CONNECT_SWAPPED);
  g_signal_group_connect_object (self->profile_signals,
                                 "notify::scrollback-lines",
                                 G_CALLBACK (ptyxis_tab_update_scrollback_lines),
                                 self,
                                 G_CONNECT_SWAPPED);
  g_signal_group_connect_object (self->profile_signals,
                                 "notify::cell-height-scale",
                                 G_CALLBACK (ptyxis_tab_update_cell_height_scale),
                                 self,
                                 G_CONNECT_SWAPPED);
  g_signal_group_connect_object (self->profile_signals,
                                 "notify::cell-width-scale",
                                 G_CALLBACK (ptyxis_tab_update_cell_width_scale),
                                 self,
                                 G_CONNECT_SWAPPED);
  g_signal_group_connect_object (self->profile_signals,
                                 "custom-links-changed",
                                 G_CALLBACK (ptyxis_tab_update_custom_links),
                                 self,
                                 G_CONNECT_SWAPPED);
  g_signal_group_set_target (self->profile_signals, self->profile);

  g_signal_connect_object (settings,
                           "notify::word-char-exceptions",
                           G_CALLBACK (ptyxis_tab_update_word_char_exceptions),
                           self,
                           G_CONNECT_SWAPPED);
  ptyxis_tab_update_word_char_exceptions (self, NULL, settings);

  g_signal_connect_object (settings,
                           "notify::inhibit-logout",
                           G_CALLBACK (ptyxis_tab_update_inhibit),
                           self,
                           G_CONNECT_SWAPPED);
  ptyxis_tab_update_inhibit (self);

  self->monitor = ptyxis_tab_monitor_new (self);
}

static void
ptyxis_tab_profile_signals_bind_cb (PtyxisTab     *self,
                                    PtyxisProfile *profile,
                                    GSignalGroup  *group)
{
  g_assert (PTYXIS_IS_TAB (self));
  g_assert (PTYXIS_IS_PROFILE (profile));
  g_assert (G_IS_SIGNAL_GROUP (group));

  /* Trigger all update functions when profile changes */
  ptyxis_tab_update_scrollback_lines (self);
  ptyxis_tab_update_cell_height_scale (self);
  ptyxis_tab_update_cell_width_scale (self);
  ptyxis_tab_update_custom_links (self);
}

static void
ptyxis_tab_snapshot (GtkWidget   *widget,
                     GtkSnapshot *snapshot)
{
  PtyxisTab *self = (PtyxisTab *)widget;
  PtyxisWindow *window;
  GdkRGBA bg;
  gboolean animating;
  int width;
  int height;

  g_assert (PTYXIS_IS_TAB (self));
  g_assert (GTK_IS_SNAPSHOT (snapshot));

  if (self->ignore_snapshot)
    return;

  window = PTYXIS_WINDOW (gtk_widget_get_root (widget));
  animating = ptyxis_window_is_animating (window);
  width = gtk_widget_get_width (widget);
  height = gtk_widget_get_height (widget);

  /* Guard against calling into libvte when the primary terminal is no
   * longer available — the weak pointer on self->terminal is cleared
   * to NULL by PtyxisTab.dispose / by PtyxisTerminal.dispose, but the
   * snapshot vfunc can still be invoked from a queued draw after that
   * (e.g. during the close-tab animation, or while the tab is being
   * torn down by the tab view). Falling back to opaque black keeps
   * the animation clean and avoids spamming VTE-CRITICAL.
   */
  if (self->terminal != NULL && VTE_IS_TERMINAL (self->terminal))
    vte_terminal_get_color_background_for_draw (VTE_TERMINAL (self->terminal), &bg);
  else
    {
      bg.red = 0;
      bg.green = 0;
      bg.blue = 0;
      bg.alpha = 1;
    }

  if (animating &&
      ptyxis_window_get_active_tab (window) == self)
    {

      if (self->cached_texture == NULL)
        {
          GtkSnapshot *sub_snapshot = gtk_snapshot_new ();
          int scale_factor = gtk_widget_get_scale_factor (widget);
          g_autoptr(GskRenderNode) node = NULL;
          graphene_matrix_t matrix;
          GskRenderer *renderer;

          gtk_snapshot_scale (sub_snapshot, scale_factor, scale_factor);
          gtk_snapshot_append_color (sub_snapshot,
                                     &bg,
                                     &GRAPHENE_RECT_INIT (0, 0, width, height));

          if (gtk_widget_compute_transform (GTK_WIDGET (self->terminal),
                                            GTK_WIDGET (self),
                                            &matrix))
            {
              gtk_snapshot_transform_matrix (sub_snapshot, &matrix);
              GTK_WIDGET_GET_CLASS (self->terminal)->snapshot (GTK_WIDGET (self->terminal), sub_snapshot);
            }

          node = gtk_snapshot_free_to_node (sub_snapshot);
          renderer = gtk_native_get_renderer (GTK_NATIVE (window));

          self->cached_texture = gsk_renderer_render_texture (renderer,
                                                              node,
                                                              &GRAPHENE_RECT_INIT (0,
                                                                                   0,
                                                                                   width * scale_factor,
                                                                                   height * scale_factor));
        }

      gtk_snapshot_append_texture (snapshot,
                                   self->cached_texture,
                                   &GRAPHENE_RECT_INIT (0, 0, width, height));
    }
  else
    {
      g_clear_object (&self->cached_texture);

      if (animating)
        gtk_snapshot_append_color (snapshot,
                                   &bg,
                                   &GRAPHENE_RECT_INIT (0, 0, width, height));

      GTK_WIDGET_CLASS (ptyxis_tab_parent_class)->snapshot (widget, snapshot);
    }
}

static void
ptyxis_tab_size_allocate (GtkWidget *widget,
                          int        width,
                          int        height,
                          int        baseline)
{
  PtyxisTab *self = (PtyxisTab *)widget;

  g_assert (PTYXIS_IS_TAB (self));

  GTK_WIDGET_CLASS (ptyxis_tab_parent_class)->size_allocate (widget, width, height, baseline);

  g_clear_object (&self->cached_texture);
}

static void
ptyxis_tab_invalidate_icon (PtyxisTab *self)
{
  g_assert (PTYXIS_IS_TAB (self));

  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_ICON]);
}

static void
ptyxis_tab_invalidate_progress (PtyxisTab *self)
{
  g_assert (PTYXIS_IS_TAB (self));

  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_PROGRESS]);
  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_PROGRESS_FRACTION]);
  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_INDICATOR_ICON]);
}

static gboolean
ptyxis_tab_match_clicked_cb (PtyxisTab       *self,
                             double           x,
                             double           y,
                             int              button,
                             GdkModifierType  state,
                             const char      *match,
                             PtyxisTerminal  *terminal)
{
  g_assert (PTYXIS_IS_TAB (self));
  g_assert (match != NULL);
  g_assert (PTYXIS_IS_TERMINAL (terminal));

  if (!ptyxis_str_empty0 (match))
    {
      ptyxis_tab_open_uri (self, match);
      return TRUE;
    }

  return FALSE;
}

static void
ptyxis_tab_root (GtkWidget *widget)
{
  PtyxisTab *self = PTYXIS_TAB (widget);

  /* Clear our ignore_snapshot bit in case we've had our tab restored
   * from the parking lot.
   */
  self->ignore_snapshot = FALSE;

  GTK_WIDGET_CLASS (ptyxis_tab_parent_class)->root (widget);
}

static void
ptyxis_tab_unroot (GtkWidget *widget)
{
  PtyxisTab *self = PTYXIS_TAB (widget);

  /* Clear inhibit cookie when widget is unrooted since the window
   * reference may no longer be valid.
   */
  if (self->inhibit_cookie != 0)
    {
      gtk_application_uninhibit (GTK_APPLICATION (PTYXIS_APPLICATION_DEFAULT),
                                 self->inhibit_cookie);
      self->inhibit_cookie = 0;
    }

  GTK_WIDGET_CLASS (ptyxis_tab_parent_class)->unroot (widget);
}

static void
ptyxis_tab_commit_cb (PtyxisTab      *self,
                      const char     *str,
                      guint           length,
                      PtyxisTerminal *terminal)
{
  g_assert (PTYXIS_IS_TAB (self));
  g_assert (PTYXIS_IS_TERMINAL (terminal));

  g_signal_emit (self, signals[COMMIT], 0, str);
}

static void
ptyxis_tab_dispose (GObject *object)
{
  PtyxisTab *self = (PtyxisTab *)object;
  GtkWidget *child;

  g_debug ("Disposing tab");

  /* Drop the weak pointer before any teardown so we don't leave GLib
   * pointing into memory that is about to be freed.
   */
  if (self->terminal != NULL)
    {
      g_object_remove_weak_pointer (G_OBJECT (self->terminal),
                                    (gpointer *)&self->terminal);
      self->terminal = NULL;
    }

  /* Defensive unparent: if PtyxisTab.dispose fires with a parent still
   * set (because something dropped the last ref without going through
   * the standard unparent path), GTK4 logs a CRITICAL and the subsequent
   * accessibility-tree cleanup segfaults inside gtk_accessible_get_at_context.
   * Unparenting here is a no-op in the normal flow (the parent has
   * already cleared us) and a safety net otherwise. */
  if (gtk_widget_get_parent (GTK_WIDGET (self)) != NULL)
    gtk_widget_unparent (GTK_WIDGET (self));

  ptyxis_tab_notify_destroy (&self->notify);

  /* Cancel the SIGKILL safety-net timer scheduled by
   * ptyxis_tab_force_quit() before we drop our last reference. If
   * the shell exited cleanly within the 50 ms window (very common
   * for shell scripts that react to SIGHUP), the timer source's
   * GDestroyNotify (g_object_unref) would otherwise fire against a
   * already-finalized PtyxisTab, and the timer's callback would
   * dereference it and trip the PTYXIS_IS_TAB assertion. */
  g_clear_handle_id (&self->pending_kill_source, g_source_remove);

  ptyxis_tab_force_quit (self);

  self->active_pane = NULL;
  g_clear_pointer (&self->panes, g_ptr_array_unref);
  g_clear_pointer (&self->focus_history, g_ptr_array_unref);

  /* Cancel any pending focus-grab idle so its callback doesn't fire
   * against a partially-disposed PtyxisTab. */
  g_clear_handle_id (&self->pending_focus_source, g_source_remove);

  gtk_widget_dispose_template (GTK_WIDGET (self), PTYXIS_TYPE_TAB);

  while ((child = gtk_widget_get_first_child (GTK_WIDGET (self))))
    gtk_widget_unparent (child);

  g_clear_object (&self->cached_texture);
  g_clear_object (&self->profile);
  g_clear_object (&self->profile_signals);
  g_clear_object (&self->process);
  g_clear_object (&self->monitor);
  g_clear_object (&self->container_at_creation);

  if (self->inhibit_cookie != 0)
    {
      gtk_application_uninhibit (GTK_APPLICATION (PTYXIS_APPLICATION_DEFAULT),
                                 self->inhibit_cookie);
      self->inhibit_cookie = 0;
    }

  g_clear_pointer (&self->initial_working_directory_uri, g_free);
  g_clear_pointer (&self->previous_working_directory_uri, g_free);
  g_clear_pointer (&self->title_prefix, g_free);
  g_clear_pointer (&self->initial_title, g_free);
  g_clear_pointer (&self->command, g_strfreev);
  g_clear_pointer (&self->command_line, g_free);
  g_clear_pointer (&self->program_name, g_free);

  G_OBJECT_CLASS (ptyxis_tab_parent_class)->dispose (object);
}

static void
ptyxis_tab_finalize (GObject *object)
{
  PtyxisTab *self = (PtyxisTab *)object;

  g_clear_pointer (&self->uuid, g_free);

  G_OBJECT_CLASS (ptyxis_tab_parent_class)->finalize (object);
}

static void
ptyxis_tab_get_property (GObject    *object,
                         guint       prop_id,
                         GValue     *value,
                         GParamSpec *pspec)
{
  PtyxisTab *self = PTYXIS_TAB (object);

  switch (prop_id)
    {
    case PROP_COMMAND_LINE:
      g_value_set_string (value, self->command_line);
      break;

    case PROP_ICON:
      g_value_take_object (value, ptyxis_tab_dup_icon (self));
      break;

    case PROP_IGNORE_OSC_TITLE:
      g_value_set_boolean (value, ptyxis_tab_get_ignore_osc_title (self));
      break;

    case PROP_INDICATOR_ICON:
      g_value_take_object (value, ptyxis_tab_dup_indicator_icon (self));
      break;

    case PROP_PROCESS_LEADER_KIND:
      g_value_set_enum (value, self->leader_kind);
      break;

    case PROP_PROGRESS:
      g_value_set_enum (value, ptyxis_tab_get_progress (self));
      break;

    case PROP_PROGRESS_FRACTION:
      g_value_set_double (value, ptyxis_tab_get_progress_fraction (self));
      break;

    case PROP_PROFILE:
      g_value_set_object (value, ptyxis_tab_get_profile (self));
      break;

    case PROP_READ_ONLY:
      g_value_set_boolean (value, !vte_terminal_get_input_enabled (VTE_TERMINAL (self->terminal)));
      break;

    case PROP_SUBTITLE:
      g_value_take_string (value, ptyxis_tab_dup_subtitle (self));
      break;

    case PROP_TITLE:
      g_value_take_string (value, ptyxis_tab_dup_title (self));
      break;

    case PROP_TITLE_PREFIX:
      g_value_set_string (value, ptyxis_tab_get_title_prefix (self));
      break;

    case PROP_UUID:
      g_value_set_string (value, ptyxis_tab_get_uuid (self));
      break;

    case PROP_ZOOM:
      g_value_set_enum (value, ptyxis_tab_get_zoom (self));
      break;

    case PROP_ZOOM_LABEL:
      g_value_take_string (value, ptyxis_tab_dup_zoom_label (self));
      break;

    case PROP_N_PANES:
      g_value_set_uint (value, ptyxis_tab_get_n_panes (self));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
ptyxis_tab_set_property (GObject      *object,
                         guint         prop_id,
                         const GValue *value,
                         GParamSpec   *pspec)
{
  PtyxisTab *self = PTYXIS_TAB (object);

  switch (prop_id)
    {
    case PROP_IGNORE_OSC_TITLE:
      ptyxis_tab_set_ignore_osc_title (self, g_value_get_boolean (value));
      break;

    case PROP_PROFILE:
      self->profile = g_value_dup_object (value);
      break;

    case PROP_READ_ONLY:
      vte_terminal_set_input_enabled (VTE_TERMINAL (self->terminal), !g_value_get_boolean (value));
      break;

    case PROP_TITLE_PREFIX:
      ptyxis_tab_set_title_prefix (self, g_value_get_string (value));
      break;

    case PROP_ZOOM:
      ptyxis_tab_set_zoom (self, g_value_get_enum (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
ptyxis_tab_class_init (PtyxisTabClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->constructed = ptyxis_tab_constructed;
  object_class->dispose = ptyxis_tab_dispose;
  object_class->finalize = ptyxis_tab_finalize;
  object_class->get_property = ptyxis_tab_get_property;
  object_class->set_property = ptyxis_tab_set_property;

  widget_class->map = ptyxis_tab_map;
  widget_class->snapshot = ptyxis_tab_snapshot;
  widget_class->size_allocate = ptyxis_tab_size_allocate;
  widget_class->root = ptyxis_tab_root;
  widget_class->unroot = ptyxis_tab_unroot;

  properties[PROP_COMMAND_LINE] =
    g_param_spec_string ("command-line", NULL, NULL,
                         NULL,
                         (G_PARAM_READABLE |
                          G_PARAM_STATIC_STRINGS));

  properties[PROP_ICON] =
    g_param_spec_object ("icon", NULL, NULL,
                         G_TYPE_ICON,
                         (G_PARAM_READABLE |
                          G_PARAM_STATIC_STRINGS));

  properties[PROP_IGNORE_OSC_TITLE] =
    g_param_spec_boolean ("ignore-osc-title", NULL, NULL,
                         FALSE,
                         (G_PARAM_READWRITE |
                          G_PARAM_EXPLICIT_NOTIFY |
                          G_PARAM_STATIC_STRINGS));

  properties[PROP_INDICATOR_ICON] =
    g_param_spec_object ("indicator-icon", NULL, NULL,
                         G_TYPE_ICON,
                         (G_PARAM_READABLE |
                          G_PARAM_STATIC_STRINGS));

  properties[PROP_PROCESS_LEADER_KIND] =
    g_param_spec_enum ("process-leader-kind", NULL, NULL,
                       PTYXIS_TYPE_PROCESS_LEADER_KIND,
                       PTYXIS_PROCESS_LEADER_KIND_UNKNOWN,
                       (G_PARAM_READABLE |
                        G_PARAM_STATIC_STRINGS));

  properties[PROP_PROFILE] =
    g_param_spec_object ("profile", NULL, NULL,
                         PTYXIS_TYPE_PROFILE,
                         (G_PARAM_READWRITE |
                          G_PARAM_CONSTRUCT_ONLY |
                          G_PARAM_STATIC_STRINGS));

  properties[PROP_PROGRESS] =
    g_param_spec_enum ("progress", NULL, NULL,
                       PTYXIS_TYPE_TAB_PROGRESS,
                       PTYXIS_TAB_PROGRESS_INDETERMINATE,
                       (G_PARAM_READABLE |
                        G_PARAM_STATIC_STRINGS));

  properties[PROP_PROGRESS_FRACTION] =
    g_param_spec_double ("progress-fraction", NULL, NULL,
                         0, 1, 0,
                         (G_PARAM_READABLE |
                          G_PARAM_STATIC_STRINGS));

  properties[PROP_READ_ONLY] =
    g_param_spec_boolean ("read-only", NULL, NULL,
                          FALSE,
                          (G_PARAM_READWRITE |
                           G_PARAM_STATIC_STRINGS));

  properties[PROP_SUBTITLE] =
    g_param_spec_string ("subtitle", NULL, NULL,
                         NULL,
                         (G_PARAM_READABLE |
                          G_PARAM_STATIC_STRINGS));

  properties[PROP_TITLE] =
    g_param_spec_string ("title", NULL, NULL,
                         NULL,
                         (G_PARAM_READABLE |
                          G_PARAM_STATIC_STRINGS));

  properties[PROP_TITLE_PREFIX] =
    g_param_spec_string ("title-prefix", NULL, NULL,
                         NULL,
                         (G_PARAM_READWRITE |
                          G_PARAM_EXPLICIT_NOTIFY |
                          G_PARAM_STATIC_STRINGS));

  properties[PROP_UUID] =
    g_param_spec_string ("uuid", NULL, NULL,
                         NULL,
                         (G_PARAM_READABLE |
                          G_PARAM_STATIC_STRINGS));

  properties[PROP_ZOOM] =
    g_param_spec_enum ("zoom", NULL, NULL,
                       PTYXIS_TYPE_ZOOM_LEVEL,
                       PTYXIS_ZOOM_LEVEL_DEFAULT,
                       (G_PARAM_READWRITE |
                        G_PARAM_EXPLICIT_NOTIFY |
                        G_PARAM_STATIC_STRINGS));

  properties[PROP_ZOOM_LABEL] =
    g_param_spec_string ("zoom-label", NULL, NULL,
                         NULL,
                         (G_PARAM_READABLE |
                          G_PARAM_EXPLICIT_NOTIFY |
                          G_PARAM_STATIC_STRINGS));

  properties[PROP_N_PANES] =
    g_param_spec_uint ("n-panes", NULL, NULL,
                       1, G_MAXUINT, 1,
                       (G_PARAM_READABLE |
                        G_PARAM_EXPLICIT_NOTIFY |
                        G_PARAM_STATIC_STRINGS));

  g_object_class_install_properties (object_class, N_PROPS, properties);

  signals[BELL] =
    g_signal_new_class_handler ("bell",
                                G_TYPE_FROM_CLASS (klass),
                                G_SIGNAL_RUN_LAST,
                                NULL,
                                NULL, NULL,
                                NULL,
                                G_TYPE_NONE, 0);

  signals[COMMIT] =
    g_signal_new_class_handler ("commit",
                                G_TYPE_FROM_CLASS (klass),
                                G_SIGNAL_RUN_LAST,
                                NULL,
                                NULL, NULL,
                                NULL,
                                G_TYPE_NONE,
                                1,
                                G_TYPE_STRING | G_SIGNAL_TYPE_STATIC_SCOPE);

  gtk_widget_class_set_template_from_resource (widget_class, "/org/gnome/Ptyxis/ptyxis-tab.ui");
  gtk_widget_class_set_layout_manager_type (widget_class, GTK_TYPE_BIN_LAYOUT);
  gtk_widget_class_set_css_name (widget_class, "ptyxistab");

  gtk_widget_class_bind_template_child (widget_class, PtyxisTab, content_box);
  gtk_widget_class_bind_template_child (widget_class, PtyxisTab, banner);
  gtk_widget_class_bind_template_child (widget_class, PtyxisTab, terminal);
  gtk_widget_class_bind_template_child (widget_class, PtyxisTab, scrolled_window);

  gtk_widget_class_bind_template_callback (widget_class, ptyxis_tab_notify_contains_focus_cb);
  gtk_widget_class_bind_template_callback (widget_class, ptyxis_tab_notify_window_title_cb);
  gtk_widget_class_bind_template_callback (widget_class, ptyxis_tab_notify_window_subtitle_cb);
  gtk_widget_class_bind_template_callback (widget_class, ptyxis_tab_increase_font_size_cb);
  gtk_widget_class_bind_template_callback (widget_class, ptyxis_tab_decrease_font_size_cb);
  gtk_widget_class_bind_template_callback (widget_class, ptyxis_tab_notify_palette_cb);
  gtk_widget_class_bind_template_callback (widget_class, ptyxis_tab_bell_cb);
  gtk_widget_class_bind_template_callback (widget_class, ptyxis_tab_invalidate_icon);
  gtk_widget_class_bind_template_callback (widget_class, ptyxis_tab_invalidate_progress);
  gtk_widget_class_bind_template_callback (widget_class, ptyxis_tab_match_clicked_cb);
  gtk_widget_class_bind_template_callback (widget_class, ptyxis_tab_commit_cb);

  gtk_widget_class_install_action (widget_class, "tab.respawn", NULL, ptyxis_tab_respawn_action);
  gtk_widget_class_install_action (widget_class, "tab.inspect", NULL, ptyxis_tab_inspect_action);

  g_type_ensure (PTYXIS_TYPE_TERMINAL);
}

static void
ptyxis_tab_init (PtyxisTab *self)
{
  GtkEventController *controller;
  PtyxisTabPane *primary;

  self->state = PTYXIS_TAB_STATE_INITIAL;
  self->zoom = PTYXIS_ZOOM_LEVEL_DEFAULT;
  self->uuid = g_uuid_string_random ();
  self->panes = g_ptr_array_new_with_free_func (ptyxis_tab_pane_free);
  self->focus_history = g_ptr_array_new ();

  gtk_widget_init_template (GTK_WIDGET (self));

  /* One-shot map handler — when the tab is first mapped (i.e. its
   * top-level window is shown), walk the paned tree and re-apply any
   * divider positions stashed during session restore. GTK doesn't
   * preserve gtk_paned_set_position() values across the unmapped →
   * mapped transition, so by the time the user sees the layout the
   * saved positions are gone without this. */
  g_signal_connect (self, "map",
                    G_CALLBACK (ptyxis_tab_map_cb),
                    NULL);

  primary = g_new0 (PtyxisTabPane, 1);
  primary->tab = self;
  primary->box = GTK_WIDGET (self->content_box);
  primary->banner = self->banner;
  primary->scrolled_window = self->scrolled_window;
  primary->terminal = self->terminal;
  primary->state = PTYXIS_TAB_STATE_INITIAL;
  primary->is_primary = TRUE;
  g_ptr_array_add (self->panes, primary);
  self->active_pane = primary;

  {
    GtkEventController *focus = gtk_event_controller_focus_new ();

    g_signal_connect_swapped (focus,
                              "notify::contains-focus",
                              G_CALLBACK (ptyxis_tab_pane_focus_enter_cb),
                              primary);
    gtk_widget_add_controller (primary->box, focus);
  }

  ptyxis_tab_notify_init (&self->notify, self);

  /* Make self->terminal self-cleaning: when the underlying widget is
   * finalized, GLib will atomically NULL the pointer so callers can
   * safely NULL-check rather than dereferencing a dangling reference.
   */
  if (self->terminal != NULL)
    g_object_add_weak_pointer (G_OBJECT (self->terminal),
                               (gpointer *)&self->terminal);

  controller = gtk_event_controller_scroll_new (GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
  gtk_event_controller_set_propagation_phase (controller, GTK_PHASE_CAPTURE);
  g_signal_connect (controller,
                    "scroll",
                    G_CALLBACK (on_scroll_scrolled_cb),
                    self);
  g_signal_connect (controller,
                    "scroll-begin",
                    G_CALLBACK (on_scroll_begin_cb),
                    self);
  g_signal_connect (controller,
                    "scroll-end",
                    G_CALLBACK (on_scroll_end_cb),
                    self);
  gtk_widget_add_controller (GTK_WIDGET (self), controller);

  /* Ensure we redraw when the dark-mode changes so that if the user
   * goes to the tab-overview all the tabs look correct.
   */
  g_signal_connect_object (adw_style_manager_get_default (),
                           "notify::dark",
                           G_CALLBACK (ptyxis_tab_invalidate_thumbnail),
                           self,
                           G_CONNECT_SWAPPED);
}

PtyxisTab *
ptyxis_tab_new (PtyxisProfile *profile)
{
  g_return_val_if_fail (PTYXIS_IS_PROFILE (profile), NULL);

  return g_object_new (PTYXIS_TYPE_TAB,
                       "profile", profile,
                       NULL);
}

/**
 * ptyxis_tab_get_profile:
 * @self: a #PtyxisTab
 *
 * Gets the profile used by the tab.
 *
 * Returns: (transfer none) (not nullable): a #PtyxisProfile
 */
PtyxisProfile *
ptyxis_tab_get_profile (PtyxisTab *self)
{
  g_return_val_if_fail (PTYXIS_IS_TAB (self), NULL);

  return self->profile;
}

/**
 * ptyxis_tab_apply_profile:
 * @self: a #PtyxisTab
 * @new_profile: a #PtyxisProfile to apply
 *
 * Applies a profile to the tab by replacing the tab's profile reference
 * with @new_profile. The tab will share the profile with other tabs,
 * so when the profile is edited in preferences, all tabs using it will
 * be updated automatically.
 */
void
ptyxis_tab_apply_profile (PtyxisTab     *self,
                          PtyxisProfile *new_profile)
{
  g_return_if_fail (PTYXIS_IS_TAB (self));
  g_return_if_fail (PTYXIS_IS_PROFILE (new_profile));

  /* Don't do anything if it's already the same profile */
  if (self->profile == new_profile)
    return;

  /* Replace the profile with the selected one. */
  g_clear_object (&self->profile);
  self->profile = g_object_ref (new_profile);


  g_signal_group_set_target (self->profile_signals, self->profile);

  /* Notify that the profile property changed */
  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_PROFILE]);
}

const char *
ptyxis_tab_get_title_prefix (PtyxisTab *self)
{
  g_return_val_if_fail (PTYXIS_IS_TAB (self), NULL);

  return self->title_prefix ? self->title_prefix : "";
}

void
ptyxis_tab_set_title_prefix (PtyxisTab  *self,
                             const char *title_prefix)
{
  g_return_if_fail (PTYXIS_IS_TAB (self));

  if (ptyxis_str_empty0 (title_prefix))
    title_prefix = NULL;

  if (g_set_str (&self->title_prefix, title_prefix))
    {
      g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_TITLE_PREFIX]);
      g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_TITLE]);
    }
}

char *
ptyxis_tab_dup_title (PtyxisTab *self)
{
  PtyxisTerminal *terminal;
  GString *gstr;

  g_return_val_if_fail (PTYXIS_IS_TAB (self), NULL);

  terminal = ptyxis_tab_get_terminal (self);

  gstr = g_string_new (self->title_prefix);

  if (terminal != NULL && !self->ignore_osc_title)
    {
      const char *window_title;

      G_GNUC_BEGIN_IGNORE_DEPRECATIONS
        window_title = vte_terminal_get_window_title (VTE_TERMINAL (terminal));
      G_GNUC_END_IGNORE_DEPRECATIONS

      if (window_title && window_title[0])
        g_string_append (gstr, window_title);
      else if (self->command != NULL && self->command[0] != NULL)
        g_string_append (gstr, self->command[0]);
      else if (self->initial_title != NULL)
        g_string_append (gstr, self->initial_title);
    }

  if (gstr->len == 0)
    g_string_append (gstr, _("Terminal"));

  if (self->state == PTYXIS_TAB_STATE_EXITED)
    g_string_append_printf (gstr, " (%s)", _("Exited"));
  else if (self->state == PTYXIS_TAB_STATE_FAILED)
    g_string_append_printf (gstr, " (%s)", _("Failed"));
  else if (self->has_foreground_process &&
           !ptyxis_str_empty0 (self->command_line) &&
           !ptyxis_str_empty0 (self->program_name) &&
           !ptyxis_is_shell (self->program_name))
    g_string_append_printf (gstr, " — %s", self->command_line);

  return g_string_free (gstr, FALSE);
}

static char *
ptyxis_tab_collapse_uri (const char *uri)
{
  g_autoptr(GFile) file = NULL;

  if (uri == NULL)
    return NULL;

  if (!(file = g_file_new_for_uri (uri)))
    return NULL;

  if (g_file_is_native (file))
    return ptyxis_path_collapse (g_file_peek_path (file));

  return strdup (uri);
}

char *
ptyxis_tab_dup_subtitle (PtyxisTab *self)
{
  g_autofree char *current_directory_uri = NULL;
  g_autofree char *current_file_uri = NULL;
  PtyxisTerminal *terminal;

  g_return_val_if_fail (PTYXIS_IS_TAB (self), NULL);

  terminal = ptyxis_tab_get_terminal (self);

  current_file_uri = ptyxis_terminal_dup_current_file_uri (terminal);
  if (current_file_uri != NULL && current_file_uri[0] != 0)
    return ptyxis_tab_collapse_uri (current_file_uri);

  current_directory_uri = ptyxis_terminal_dup_current_directory_uri (terminal);
  if (current_directory_uri != NULL && current_directory_uri[0] != 0)
    return ptyxis_tab_collapse_uri (current_directory_uri);

  return g_strdup ("");
}

char *
ptyxis_tab_dup_current_directory_uri (PtyxisTab *self)
{
  g_return_val_if_fail (PTYXIS_IS_TAB (self), NULL);

  if (self->active_pane != NULL && self->active_pane->terminal != NULL)
    return ptyxis_terminal_dup_current_directory_uri (self->active_pane->terminal);

  return ptyxis_terminal_dup_current_directory_uri (self->terminal);
}

void
ptyxis_tab_set_initial_working_directory_uri (PtyxisTab  *self,
                                              const char *initial_working_directory_uri)
{
  g_return_if_fail (PTYXIS_IS_TAB (self));

  g_set_str (&self->initial_working_directory_uri, initial_working_directory_uri);
}

char *
ptyxis_tab_dup_previous_working_directory_uri (PtyxisTab *self)
{
  g_return_val_if_fail (PTYXIS_IS_TAB (self), NULL);

  return g_strdup (self->previous_working_directory_uri);
}


void
ptyxis_tab_set_previous_working_directory_uri (PtyxisTab  *self,
                                               const char *previous_working_directory_uri)
{
  g_return_if_fail (PTYXIS_IS_TAB (self));

  g_set_str (&self->previous_working_directory_uri, previous_working_directory_uri);
}

static void
ptyxis_tab_apply_zoom_to_terminal (PtyxisTab      *self,
                                   PtyxisTerminal *terminal)
{
  g_assert (PTYXIS_IS_TAB (self));
  g_assert (PTYXIS_IS_TERMINAL (terminal));

  vte_terminal_set_font_scale (VTE_TERMINAL (terminal),
                               zoom_font_scales[self->zoom]);
}

static void
ptyxis_tab_apply_zoom (PtyxisTab *self)
{
  g_assert (PTYXIS_IS_TAB (self));

  if (self->panes != NULL)
    {
      for (guint i = 0; i < self->panes->len; i++)
        {
          PtyxisTabPane *pane = g_ptr_array_index (self->panes, i);

          if (pane->terminal != NULL)
            ptyxis_tab_apply_zoom_to_terminal (self, pane->terminal);
        }
    }
  else if (self->terminal != NULL)
    {
      ptyxis_tab_apply_zoom_to_terminal (self, self->terminal);
    }
}

PtyxisZoomLevel
ptyxis_tab_get_zoom (PtyxisTab *self)
{
  g_return_val_if_fail (PTYXIS_IS_TAB (self), 0);

  return self->zoom;
}

void
ptyxis_tab_set_zoom (PtyxisTab       *self,
                     PtyxisZoomLevel  zoom)
{
  g_return_if_fail (PTYXIS_IS_TAB (self));
  g_return_if_fail (zoom >= PTYXIS_ZOOM_LEVEL_MINUS_14 &&
                    zoom <= PTYXIS_ZOOM_LEVEL_PLUS_14);

  if (zoom != self->zoom)
    {
      self->zoom = zoom;
      ptyxis_tab_apply_zoom (self);
      g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_ZOOM]);
      g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_ZOOM_LABEL]);
    }
}

void
ptyxis_tab_zoom_in (PtyxisTab *self)
{
  g_return_if_fail (PTYXIS_IS_TAB (self));

  if (self->zoom < PTYXIS_ZOOM_LEVEL_PLUS_14)
    ptyxis_tab_set_zoom (self, self->zoom + 1);
}

void
ptyxis_tab_zoom_out (PtyxisTab *self)
{
  g_return_if_fail (PTYXIS_IS_TAB (self));

  if (self->zoom > PTYXIS_ZOOM_LEVEL_MINUS_14)
    ptyxis_tab_set_zoom (self, self->zoom - 1);
}

void
ptyxis_tab_raise (PtyxisTab *self)
{
  AdwTabView *tab_view;
  AdwTabPage *tab_page;

  g_return_if_fail (PTYXIS_IS_TAB (self));

  if ((tab_view = ADW_TAB_VIEW (gtk_widget_get_ancestor (GTK_WIDGET (self), ADW_TYPE_TAB_VIEW))) &&
      (tab_page = adw_tab_view_get_page (tab_view, GTK_WIDGET (self))))
    adw_tab_view_set_selected_page (tab_view, tab_page);
}

typedef struct _Wait
{
  GMainContext *context;
  gboolean completed;
  gboolean success;
} Wait;

static void
ptyxis_tab_poll_agent_sync_cb (GObject      *object,
                               GAsyncResult *result,
                               gpointer      user_data)
{
  PtyxisTab *self = (PtyxisTab *)object;
  Wait *wait = user_data;

  g_assert (PTYXIS_IS_TAB (self));
  g_assert (G_IS_ASYNC_RESULT (result));
  g_assert (wait != NULL);

  wait->completed = TRUE;
  wait->success = ptyxis_tab_poll_agent_finish (self, result, NULL);

  g_main_context_wakeup (wait->context);
}

static gboolean
ptyxis_tab_poll_agent_cancel_cb (gpointer user_data)
{
  GCancellable *cancellable = user_data;

  g_cancellable_cancel (cancellable);

  return G_SOURCE_REMOVE;
}

static gboolean
ptyxis_tab_poll_agent (PtyxisTab *self)
{
  Wait wait;
  g_autoptr(GCancellable) cancellable = NULL;
  GSource *timeout_src = NULL;

  g_return_val_if_fail (PTYXIS_IS_TAB (self), FALSE);

  wait.context = g_main_context_get_thread_default ();
  wait.completed = FALSE;
  wait.success = FALSE;

  /* Bound the time we may block the main loop waiting for the agent.
   * If the agent is unresponsive (crashed, proxy stale, dbus hiccup) we
   * must not freeze the UI thread — previously this spun indefinitely,
   * which is what caused the "not responding" symptom when closing a
   * tab containing multiple panes (close page -> is_running -> poll).
   *
   * Refcount ownership: `cancellable` is the sole owner (g_autoptr →
   * refcount drops at function exit). g_task_new() and the GSource do
   * NOT need their own refs because:
   *   - GTask stores the cancellable internally without taking a ref
   *     (g_task_new() does g_object_ref the source object but NOT the
   *     cancellable — see glib/gio documentation).
   *   - We pass NULL as the GSource's destroy_notify, so the source
   *     does NOT take ownership of a ref on user_data either.
   *
   * Therefore: refcount 1 (from g_cancellable_new, owned by us) until
   * the function returns and g_autoptr drops it. No double-free path.
   *
   * The previous version used g_timeout_add_full(..., g_object_unref)
   * which transferred a ref to the source — but never destroyed the
   * source before returning, so its destroy_notify would later fire
   * on a freed cancellable (segfault inside libglib's refcount inline,
   * offset 0x9 of the GCancellable struct). Now we own the source
   * ourselves and destroy it deterministically before exiting.
   */
  cancellable = g_cancellable_new ();
  timeout_src = g_timeout_source_new (5000);
  g_source_set_callback (timeout_src,
                         ptyxis_tab_poll_agent_cancel_cb,
                         cancellable,    /* raw pointer — no ref transfer */
                         NULL);         /* destroy_notify = NULL: source
                                           does NOT consume user_data ref */
  g_source_attach (timeout_src, wait.context);

  ptyxis_tab_poll_agent_async (self,
                               cancellable,
                               ptyxis_tab_poll_agent_sync_cb,
                               &wait);

  while (!wait.completed)
    g_main_context_iteration (wait.context, TRUE);

  /* If the watchdog fired, ptyxis_tab_poll_agent_cancel_cb already
   * cancelled. Either way, cancel now (idempotent) to interrupt any
   * pending D-Bus callback still queued in the main context, then
   * destroy + unref the source so it is fully gone before our
   * g_autoptr releases the cancellable.
   */
  g_cancellable_cancel (cancellable);
  g_source_destroy (timeout_src);
  g_source_unref (timeout_src);

  return wait.success;
}

/**
 * ptyxis_tab_is_running:
 * @self: a #PtyxisTab
 * @cmdline: (out) (nullable): a location for the command line
 *
 * Returns: %TRUE if there is a command running
 */
gboolean
ptyxis_tab_is_running (PtyxisTab  *self,
                       char      **cmdline)
{
  g_return_val_if_fail (PTYXIS_IS_TAB (self), FALSE);

  ptyxis_tab_poll_agent (self);

  if (cmdline != NULL)
    *cmdline = g_strdup (self->command_line);

  if (self->panes != NULL)
    {
      for (guint i = 0; i < self->panes->len; i++)
        {
          PtyxisTabPane *pane = g_ptr_array_index (self->panes, i);

          if (pane->is_primary)
            {
              if (self->has_foreground_process &&
                  self->program_name != NULL &&
                  !ptyxis_is_shell (self->program_name))
                return TRUE;
            }
          else if (pane->has_foreground_process &&
                   pane->program_name != NULL &&
                   !ptyxis_is_shell (pane->program_name))
            {
              if (cmdline != NULL && *cmdline == NULL)
                *cmdline = g_strdup (pane->command_line);
              return TRUE;
            }
          else if (pane->state == PTYXIS_TAB_STATE_RUNNING ||
                   pane->state == PTYXIS_TAB_STATE_SPAWNING)
            {
              /* A live shell still counts as "something running" for multi-pane
               * close prompts when prompt-on-close is enabled for foreground
               * processes only — keep primary semantics for that path.
               */
            }
        }
    }

  if (self->has_foreground_process && self->program_name != NULL)
    return !ptyxis_is_shell (self->program_name);

  return FALSE;
}

static gboolean
ptyxis_tab_force_quit_in_idle (gpointer data)
{
  PtyxisTab *self = data;

  g_assert (PTYXIS_IS_TAB (self));

  if (self->panes != NULL)
    {
      for (guint i = 0; i < self->panes->len; i++)
        {
          PtyxisTabPane *pane = g_ptr_array_index (self->panes, i);

          if (pane->process != NULL)
            ptyxis_tab_pane_send_signal (pane, SIGKILL);
        }
    }
  else if (self->process != NULL)
    {
      ptyxis_tab_send_signal (self, SIGKILL);
    }

  return G_SOURCE_REMOVE;
}

static void
ptyxis_tab_pane_send_signal (PtyxisTabPane *pane,
                             int            signum)
{
  g_assert (pane != NULL);

  if (pane->process != NULL)
    ptyxis_ipc_process_call_send_signal (pane->process, signum, NULL, NULL, NULL);
}

void
ptyxis_tab_force_quit (PtyxisTab *self)
{
  g_return_if_fail (PTYXIS_IS_TAB (self));

  /* Skip the diagnostic log + signal/SIGKILL fan-out when force_quit
   * already ran for this tab. The tab's own ptyxis_tab_dispose() calls
   * us as a defensive safety net, so close paths that already invoked
   * force_quit (close-dialog confirm, parking-lot dispose, window
   * close-request dialog) used to log "Forcing tab to quit" twice per
   * tab — once for the explicit kill and once again inside dispose —
   * flooding the log under G_MESSAGES_DEBUG=Ptyxis when closing a
   * window with many tabs. forced_exit is the canonical "we already
   * kicked off shutdown" marker and is set right after this log line. */
  if (self->forced_exit)
    return;

  g_debug ("Forcing tab to quit");

  self->forced_exit = TRUE;

  if (self->panes != NULL)
    {
      for (guint i = 0; i < self->panes->len; i++)
        {
          PtyxisTabPane *pane = g_ptr_array_index (self->panes, i);

          pane->forced_exit = TRUE;
          ptyxis_tab_pane_send_signal (pane, SIGHUP);
        }
    }
  else if (self->process != NULL)
    {
      ptyxis_tab_send_signal (self, SIGHUP);
    }

  /* In case this was not enough for the process to actually exit, we setup
   * a short timer to send SIGKILL afterwards. Store the source id on
   * self->pending_kill_source so ptyxis_tab_dispose() can cancel it if
   * the shell exits cleanly within the 50 ms window (otherwise the
   * timer would fire against an already-finalized PtyxisTab and trip
   * the PTYXIS_IS_TAB assertion in ptyxis_tab_force_quit_in_idle).
   */
  self->pending_kill_source =
      g_timeout_add_full (G_PRIORITY_HIGH,
                          50,
                          ptyxis_tab_force_quit_in_idle,
                          g_object_ref (self),
                          g_object_unref);
}

PtyxisIpcProcess *
ptyxis_tab_get_process (PtyxisTab *self)
{
  g_return_val_if_fail (PTYXIS_IS_TAB (self), NULL);

  return self->process;
}

char *
ptyxis_tab_dup_zoom_label (PtyxisTab *self)
{
  g_return_val_if_fail (PTYXIS_IS_TAB (self), 0);

  if (self->zoom == PTYXIS_ZOOM_LEVEL_DEFAULT)
    return g_strdup ("100%");

  return g_strdup_printf ("%.0lf%%", zoom_font_scales[self->zoom] * 100.0);
}

void
ptyxis_tab_show_banner (PtyxisTab *self)
{
  g_return_if_fail (PTYXIS_IS_TAB (self));

  gtk_widget_set_visible (GTK_WIDGET (self->banner), TRUE);
}

void
ptyxis_tab_set_needs_attention (PtyxisTab *self,
                                gboolean   needs_attention)
{
  GtkWidget *tab_view;
  AdwTabPage *page;

  g_return_if_fail (PTYXIS_IS_TAB (self));

  if ((tab_view = gtk_widget_get_ancestor (GTK_WIDGET (self), ADW_TYPE_TAB_VIEW)) &&
      (page = adw_tab_view_get_page (ADW_TAB_VIEW (tab_view), GTK_WIDGET (self))))
    adw_tab_page_set_needs_attention (page, needs_attention);
}

const char *
ptyxis_tab_get_uuid (PtyxisTab *self)
{
  g_return_val_if_fail (PTYXIS_IS_TAB (self), NULL);

  return self->uuid;
}

PtyxisIpcContainer *
ptyxis_tab_dup_container (PtyxisTab *self)
{
  g_autoptr(PtyxisIpcContainer) container = NULL;
  PtyxisTerminal *terminal;
  const char *runtime;
  const char *name;

  g_return_val_if_fail (PTYXIS_IS_TAB (self), NULL);

  /* Route through the safe getter so the weak-pointer / active-pane
   * fallback applies. If there is no live terminal, just return the
   * creation-time container instead of dereferencing a freed widget.
   */
  terminal = ptyxis_tab_get_terminal (self);

  if (terminal != NULL &&
      (runtime = ptyxis_terminal_get_current_container_runtime (terminal)) &&
      (name = ptyxis_terminal_get_current_container_name (terminal)))
    container = ptyxis_application_find_container_by_name (PTYXIS_APPLICATION_DEFAULT, runtime, name);

  if (container == NULL)
    g_set_object (&container, self->container_at_creation);

  return g_steal_pointer (&container);
}

void
ptyxis_tab_set_container (PtyxisTab          *self,
                          PtyxisIpcContainer *container)
{
  g_return_if_fail (PTYXIS_IS_TAB (self));
  g_return_if_fail (!container || PTYXIS_IPC_IS_CONTAINER (container));

  g_set_object (&self->container_at_creation, container);
}

static void
ptyxis_tab_poll_agent_cb (GObject      *object,
                          GAsyncResult *result,
                          gpointer      user_data)
{
  PtyxisIpcProcess *process = (PtyxisIpcProcess *)object;
  g_autoptr(GTask) task = user_data;
  g_autoptr(GError) error = NULL;
  g_autofree char *the_cmdline = NULL;
  g_autofree char *the_leader_kind = NULL;
  PtyxisProcessLeaderKind leader_kind;
  gboolean has_foreground_process;
  gboolean changed = FALSE;
  PtyxisTab *self;
  GPid the_pid;

  g_assert (PTYXIS_IPC_IS_PROCESS (process));
  g_assert (G_IS_ASYNC_RESULT (result));
  g_assert (G_IS_TASK (task));

  self = g_task_get_source_object (task);

  g_assert (PTYXIS_IS_TAB (self));

  if (!ptyxis_ipc_process_call_has_foreground_process_finish (process,
                                                              &has_foreground_process,
                                                              &the_pid,
                                                              &the_cmdline,
                                                              &the_leader_kind,
                                                              NULL,
                                                              result,
                                                              &error))
    {
      /* D-Bus call failed — agent likely gone, proxy stale, or cancelled.
       * Do NOT clobber the cached tab state with zeroed output params,
       * otherwise subsequent is_running / has_foreground_process lookups
       * would suddenly think no process is running. Leave state alone and
       * just surface the failure to the waiter.
       */
      g_task_return_error (task, g_steal_pointer (&error));
      return;
    }

  if (self->pid != the_pid)
    {
      changed = TRUE;
      self->pid = the_pid;
    }

  if (self->has_foreground_process != has_foreground_process)
    {
      changed = TRUE;
      self->has_foreground_process = has_foreground_process;
    }

  if (g_strcmp0 (the_leader_kind, "superuser") == 0)
    leader_kind = PTYXIS_PROCESS_LEADER_KIND_SUPERUSER;
  else if (g_strcmp0 (the_leader_kind, "container") == 0)
    leader_kind = PTYXIS_PROCESS_LEADER_KIND_CONTAINER;
  else if (g_strcmp0 (the_leader_kind, "remote") == 0)
    leader_kind = PTYXIS_PROCESS_LEADER_KIND_REMOTE;
  else
    leader_kind = PTYXIS_PROCESS_LEADER_KIND_UNKNOWN;

  if (self->leader_kind != leader_kind)
    {
      changed = TRUE;
      self->leader_kind = leader_kind;

      if (!ptyxis_tab_is_active (self))
        ptyxis_tab_set_needs_attention (self, TRUE);

      g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_PROCESS_LEADER_KIND]);
    }

  if (g_set_str (&self->command_line, the_cmdline))
    {
      g_autofree char *program_name = NULL;
      const char *space;

      changed = TRUE;

      if (the_cmdline != NULL && (space = strchr (the_cmdline, ' ')))
        program_name = g_strndup (the_cmdline, space - the_cmdline);

      g_set_str (&self->program_name, program_name);

      g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_COMMAND_LINE]);
    }

  if (changed)
    g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_TITLE]);

  /* Sync the updated foreground process state to the active pane so
   * that multi-pane bookkeeping (is_running, force_quit, leader_kind,
   * etc.) stays consistent with the tab-level state. The poll targets
   * whichever pane is currently active, so its results belong to that
   * pane and not necessarily the primary one.
   *
   * The lifecycle state is intentionally NOT mirrored here — it is
   * owned by spawn_cb / respawn and must not be clobbered mid-spawn.
   */
  if (self->active_pane != NULL)
    {
      PtyxisTabPane *pane = self->active_pane;

      pane->pid = self->pid;
      pane->has_foreground_process = self->has_foreground_process;
      pane->leader_kind = self->leader_kind;
      g_set_str (&pane->command_line, self->command_line);
      g_set_str (&pane->program_name, self->program_name);
    }

  /* Update inhibit state when foreground process changes */
  ptyxis_tab_update_inhibit (self);

  g_task_return_boolean (task, changed);
}

void
ptyxis_tab_poll_agent_async (PtyxisTab           *self,
                             GCancellable        *cancellable,
                             GAsyncReadyCallback  callback,
                             gpointer             user_data)
{
  g_autoptr(GUnixFDList) fd_list = NULL;
  g_autoptr(GTask) task = NULL;
  VtePty *pty;
  int handle;
  int pty_fd;

  g_assert (PTYXIS_IS_TAB (self));

  task = g_task_new (self, cancellable, callback, user_data);
  g_task_set_source_tag (task, ptyxis_tab_poll_agent_async);

  if (self->process == NULL)
    {
      self->has_foreground_process = FALSE;
      self->pid = -1;

      if (g_set_str (&self->command_line, NULL))
        g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_COMMAND_LINE]);

      if (self->leader_kind != PTYXIS_PROCESS_LEADER_KIND_UNKNOWN)
        {
          self->leader_kind = PTYXIS_PROCESS_LEADER_KIND_UNKNOWN;
          g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_PROCESS_LEADER_KIND]);
        }

      g_task_return_boolean (task, FALSE);

      return;
    }

  pty = vte_terminal_get_pty (VTE_TERMINAL (ptyxis_tab_get_terminal (self)));
  pty_fd = vte_pty_get_fd (pty);
  fd_list = g_unix_fd_list_new ();
  handle = g_unix_fd_list_append (fd_list, pty_fd, NULL);

  ptyxis_ipc_process_call_has_foreground_process (self->process,
                                                  g_variant_new_handle (handle),
                                                  fd_list,
                                                  cancellable,
                                                  ptyxis_tab_poll_agent_cb,
                                                  g_steal_pointer (&task));


}

gboolean
ptyxis_tab_poll_agent_finish (PtyxisTab     *self,
                              GAsyncResult  *result,
                              GError       **error)
{
  g_return_val_if_fail (PTYXIS_IS_TAB (self), FALSE);
  g_return_val_if_fail (G_IS_TASK (result), FALSE);

  return g_task_propagate_boolean (G_TASK (result), error);
}

gboolean
ptyxis_tab_has_foreground_process (PtyxisTab  *self,
                                   GPid       *pid,
                                   char      **cmdline)
{
  g_return_val_if_fail (PTYXIS_IS_TAB (self), FALSE);

  ptyxis_tab_poll_agent (self);

  if (pid != NULL)
    *pid = self->pid;

  if (cmdline != NULL)
    *cmdline = g_strdup (self->command_line);

  return self->has_foreground_process;
}

void
ptyxis_tab_set_command (PtyxisTab          *self,
                        const char * const *command)
{
  char **copy;

  g_return_if_fail (PTYXIS_IS_TAB (self));

  if (command != NULL && command[0] == NULL)
    command = NULL;

  copy = g_strdupv ((char **)command);
  g_strfreev (self->command);
  self->command = copy;
}

const char *
ptyxis_tab_get_initial_title (PtyxisTab *self)
{
  g_return_val_if_fail (PTYXIS_IS_TAB (self), NULL);

  return self->initial_title;
}

void
ptyxis_tab_set_initial_title (PtyxisTab  *self,
                              const char *initial_title)
{
  g_return_if_fail (PTYXIS_IS_TAB (self));

  g_set_str (&self->initial_title, initial_title);
}

const char *
ptyxis_tab_get_command_line (PtyxisTab *self)
{
  g_return_val_if_fail (PTYXIS_IS_TAB (self), NULL);

  return self->command_line;
}

#ifdef __linux__
static void
ptyxis_tab_toast (PtyxisTab  *self,
                  int         timeout,
                  const char *title)
{
  GtkWidget *overlay = gtk_widget_get_ancestor (GTK_WIDGET (self), ADW_TYPE_TOAST_OVERLAY);
  AdwToast *toast;

  if (overlay == NULL)
    return;

  toast = g_object_new (ADW_TYPE_TOAST,
                        "title", title,
                        "timeout", timeout,
                        NULL);
  adw_toast_overlay_add_toast (ADW_TOAST_OVERLAY (overlay), toast);
}

static void
ptyxis_tab_open_uri_cb (GObject      *object,
                        GAsyncResult *result,
                        gpointer      user_data)
{
  g_autoptr(PtyxisTab) self = user_data;
  g_autoptr(GError) error = NULL;

  g_assert (XDP_IS_PORTAL (object));
  g_assert (G_IS_ASYNC_RESULT (result));
  g_assert (PTYXIS_IS_TAB (self));

  if (!xdp_portal_open_uri_finish (XDP_PORTAL (object), result, &error) &&
      !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    ptyxis_tab_toast (self, 3, _("Failed to open link"));
}

void
ptyxis_tab_open_uri (PtyxisTab  *self,
                     const char *uri)
{
  g_autofree char *translated = NULL;
  GtkWindow *window;
  XdpParent *parent;

  g_return_if_fail (PTYXIS_IS_TAB (self));
  g_return_if_fail (uri != NULL);

  window = GTK_WINDOW (gtk_widget_get_root (GTK_WIDGET (self)));

  if (g_str_has_prefix (uri, "file://"))
    {
      g_autoptr(PtyxisIpcContainer) container = ptyxis_tab_dup_container (self);
      g_autoptr(GUri) guri = NULL;

      if (container == NULL)
        {
          g_autofree char *default_container = ptyxis_profile_dup_default_container (self->profile);
          container = ptyxis_application_lookup_container (PTYXIS_APPLICATION_DEFAULT, default_container);
        }

      if (container != NULL)
        {
          if (ptyxis_ipc_container_call_translate_uri_sync (container, uri, &translated, NULL, NULL))
            uri = translated;
        }

      if (ptyxis_get_process_kind () == PTYXIS_PROCESS_KIND_FLATPAK &&
          (guri = g_uri_parse (uri, 0, NULL)) &&
          !g_str_has_prefix (g_uri_get_path (guri), g_get_home_dir ()))
        {
          const char *path = g_uri_get_path (guri);
          g_autofree char *new_path = g_build_filename ("/var/run/host", path, NULL);
          g_autoptr(GUri) rewritten = NULL;

          rewritten = g_uri_build (0,
                                   "file",
                                   g_uri_get_userinfo (guri),
                                   g_uri_get_host (guri),
                                   g_uri_get_port (guri),
                                   new_path,
                                   g_uri_get_query (guri),
                                   g_uri_get_fragment (guri));

          g_clear_pointer (&translated, g_free);
          uri = translated = g_uri_to_string (rewritten);
        }
    }
  else if (!g_utf8_strchr (uri, -1, ':') && g_utf8_strchr (uri, -1, '@'))
    {
      uri = translated = g_strconcat ("mailto:", uri, NULL);
    }

  if (portal == NULL)
    portal = xdp_portal_new ();

  parent = xdp_parent_new_gtk (window);
  xdp_portal_open_uri (portal,
                       parent,
                       uri,
                       XDP_OPEN_URI_FLAG_NONE,
                       NULL,
                       ptyxis_tab_open_uri_cb,
                       g_object_ref (self));
  xdp_parent_free (parent);
}
#else
void
ptyxis_tab_open_uri (PtyxisTab  *self,
                     const char *uri)
{
  G_GNUC_BEGIN_IGNORE_DEPRECATIONS
  gtk_show_uri (GTK_WINDOW (gtk_widget_get_root (GTK_WIDGET (self))), uri, 0);
  G_GNUC_END_IGNORE_DEPRECATIONS
}
#endif

char *
ptyxis_tab_query_working_directory_from_agent (PtyxisTab *self)
{
  g_autofree char *path = NULL;
  g_autoptr(GUnixFDList) fd_list = NULL;
  VtePty *pty;
  int pty_fd;
  int handle;

  g_return_val_if_fail (PTYXIS_IS_TAB (self), NULL);

  if (self->process == NULL)
    return NULL;

  pty = vte_terminal_get_pty (VTE_TERMINAL (ptyxis_tab_get_terminal (self)));
  pty_fd = vte_pty_get_fd (pty);
  fd_list = g_unix_fd_list_new ();
  handle = g_unix_fd_list_append (fd_list, pty_fd, NULL);

  if (ptyxis_ipc_process_call_get_working_directory_sync (self->process,
                                                          g_variant_new_handle (handle),
                                                          fd_list,
                                                          &path,
                                                          NULL, NULL, NULL))
    return g_steal_pointer (&path);

  return NULL;
}

PtyxisTabProgress
ptyxis_tab_get_progress (PtyxisTab *self)
{
  gint64 state;
  PtyxisTerminal *terminal;

  g_return_val_if_fail (PTYXIS_IS_TAB (self), 0);

  terminal = ptyxis_tab_get_terminal (self);
  if (terminal == NULL)
    return 0;

  if (vte_terminal_get_termprop_int_by_id (VTE_TERMINAL (terminal),
                                           VTE_PROPERTY_ID_PROGRESS_HINT,
                                           &state))
    {
      switch (state)
        {
        case VTE_PROGRESS_HINT_ACTIVE:
          return PTYXIS_TAB_PROGRESS_ACTIVE;

        case VTE_PROGRESS_HINT_ERROR:
          return PTYXIS_TAB_PROGRESS_ERROR;

        case VTE_PROGRESS_HINT_PAUSED:
        case VTE_PROGRESS_HINT_INDETERMINATE:
        default:
          return PTYXIS_TAB_PROGRESS_INDETERMINATE;
        }
    }

  return PTYXIS_TAB_PROGRESS_INDETERMINATE;
}

double
ptyxis_tab_get_progress_fraction (PtyxisTab *self)
{
  PtyxisTerminal *terminal;
  guint64 value;

  g_return_val_if_fail (PTYXIS_IS_TAB (self), .0);

  /* Route through the safe getter so the weak-pointer / active-pane
   * fallback applies. If there is no live terminal, return 0 instead of
   * dereferencing a freed widget.
   */
  terminal = ptyxis_tab_get_terminal (self);
  if (terminal == NULL)
    return .0;

  if (ptyxis_tab_get_progress (self) != PTYXIS_TAB_PROGRESS_ACTIVE ||
      !vte_terminal_get_termprop_uint_by_id (VTE_TERMINAL (terminal),
                                             VTE_PROPERTY_ID_PROGRESS_VALUE,
                                             &value))
    return .0;

  return MIN (value, 100) / 100.0;
}

G_GNUC_BEGIN_IGNORE_DEPRECATIONS
static void
draw_progress (cairo_t         *cr,
               GtkStyleContext *style_context,
               int              width,
               int              height,
               double           progress)
{
  GdkRGBA rgba;
  double alpha;

  g_assert (cr != NULL);
  g_assert (style_context != NULL);

  progress = CLAMP (progress, 0, 1);

  gtk_style_context_get_color (style_context, &rgba);

  alpha = rgba.alpha;
  rgba.alpha *= .15;
  gdk_cairo_set_source_rgba (cr, &rgba);

  cairo_arc (cr,
             width / 2,
             height / 2,
             width / 2,
             0.0,
             2 * M_PI);
  cairo_fill (cr);

  if (progress > 0.0)
    {
      rgba.alpha = alpha;
      gdk_cairo_set_source_rgba (cr, &rgba);

      cairo_arc (cr,
                 width / 2,
                 height / 2,
                 width / 2,
                 (-.5 * M_PI),
                 (2 * progress * M_PI) - (.5 * M_PI));

      if (progress != 1.0)
        {
          cairo_line_to (cr, width / 2, height / 2);
          cairo_line_to (cr, width / 2, 0);
        }

      cairo_fill (cr);
    }
}
G_GNUC_END_IGNORE_DEPRECATIONS

/**
 * ptyxis_tab_dup_indicator_icon:
 * @self: a #PtyxisTab
 *
 * Gets the progress indicator icon.
 *
 * Due to libadwaita not providing a way to do progress natively (as of 1.6)
 * this uses indicator icon to generate a progress icon using a drawing.
 *
 * Returns: (transfer full) (nullable): a #GIcon or %NULL
 */
GIcon *
ptyxis_tab_dup_indicator_icon (PtyxisTab *self)
{
  PtyxisTabProgress progress;

  g_return_val_if_fail (PTYXIS_IS_TAB (self), NULL);

  progress = ptyxis_tab_get_progress (self);

  if (progress == PTYXIS_TAB_PROGRESS_ERROR)
    return g_themed_icon_new ("dialog-error-symbolic");

  if (progress == PTYXIS_TAB_PROGRESS_INDETERMINATE)
    return NULL;

  if (progress == PTYXIS_TAB_PROGRESS_ACTIVE)
    {
      g_autoptr(GdkTexture) texture = NULL;
      g_autoptr(GBytes) bytes = NULL;
      cairo_surface_t *surface;
      cairo_t *cr;
      double fraction;
      int stride;
      int scale;
      int width;
      int height;

      fraction = ptyxis_tab_get_progress_fraction (self);
      scale = gtk_widget_get_scale_factor (GTK_WIDGET (self));
      width = 16 * scale;
      height = 16 * scale;

      surface = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, width, height);
      stride = cairo_image_surface_get_stride (surface);
      cr = cairo_create (surface);

      G_GNUC_BEGIN_IGNORE_DEPRECATIONS {
        GtkStyleContext *style_context = gtk_widget_get_style_context (GTK_WIDGET (self));
        draw_progress (cr, style_context, width, height, fraction);
      } G_GNUC_END_IGNORE_DEPRECATIONS

      cairo_destroy (cr);

      bytes = g_bytes_new (cairo_image_surface_get_data (surface), height * stride);
      texture = gdk_memory_texture_new (width, height, GDK_MEMORY_DEFAULT, bytes, stride);

      cairo_surface_destroy (surface);

      return G_ICON (g_steal_pointer (&texture));
    }

  return NULL;
}

gboolean
ptyxis_tab_get_ignore_osc_title (PtyxisTab *self)
{
  g_return_val_if_fail (PTYXIS_IS_TAB (self), FALSE);

  return self->ignore_osc_title;
}

void
ptyxis_tab_set_ignore_osc_title (PtyxisTab *self,
                                 gboolean   ignore_osc_title)
{
  g_return_if_fail (PTYXIS_IS_TAB (self));

  ignore_osc_title = !!ignore_osc_title;

  if (ignore_osc_title != self->ignore_osc_title)
    {
      self->ignore_osc_title = ignore_osc_title;
      g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_IGNORE_OSC_TITLE]);
      g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_TITLE]);
    }
}

void
_ptyxis_tab_ignore_snapshot (PtyxisTab *self)
{
  g_return_if_fail (PTYXIS_IS_TAB (self));

  self->ignore_snapshot = TRUE;
}

PtyxisTerminal *
_ptyxis_tab_get_primary_terminal (PtyxisTab *self)
{
  g_return_val_if_fail (PTYXIS_IS_TAB (self), NULL);

  return self->terminal;
}

/* Try to grab focus on the active pane's terminal. Returns TRUE if
 * the grab actually transferred focus (gtk_widget_grab_focus returns
 * TRUE on success). The result is what we use to decide whether a
 * deferred retry is still needed. */
static gboolean
ptyxis_tab_try_grab_focus (PtyxisTab *self)
{
  GtkWidget *target = NULL;

  g_return_val_if_fail (PTYXIS_IS_TAB (self), FALSE);

  if (self->active_pane != NULL && self->active_pane->terminal != NULL)
    target = GTK_WIDGET (self->active_pane->terminal);
  else if (self->terminal != NULL)
    target = GTK_WIDGET (self->terminal);

  if (target == NULL)
    return FALSE;

  /* If the target isn't mapped yet, gtk_widget_grab_focus is a silent
   * no-op and the user's keystrokes end up wherever focus was before
   * (typically the tab bar in tab-switch cases). Bail out so the
   * scheduler can retry once GTK has had a chance to allocate the
   * widget. */
  if (!gtk_widget_get_mapped (target))
    return FALSE;

  return gtk_widget_grab_focus (target);
}

/* Idle callback that performs the deferred focus grab. It clears
 * pending_focus_source itself so the next call to
 * ptyxis_tab_schedule_focus_grab() can re-arm the source. If the
 * grab still doesn't take (e.g. the paned tree is still being
 * allocated because AdwTabView just changed selected-page), re-arm
 * the source for another iteration. We bound the retries to a small
 * number to avoid spinning forever on a permanently unmapped widget
 * (e.g. one inside a destroyed tab). */
static gboolean
ptyxis_tab_focus_grab_idle_cb (gpointer user_data)
{
  PtyxisTab *self = PTYXIS_TAB (user_data);
  guint retries;

  g_assert (PTYXIS_IS_TAB (self));

  /* Steal the source id first so a re-schedule below installs a fresh
   * source rather than mutating one that's currently firing. */
  self->pending_focus_source = 0;

  if (ptyxis_tab_try_grab_focus (self))
    return G_SOURCE_REMOVE;

  /* Up to ~50 retries (50 * ~10ms = ~500ms) — enough to wait out a
   * size-allocate cascade from an AdwTabView page switch without
   * burning CPU indefinitely. */
  retries = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (self),
                                                 "ptyxis-tab-focus-grab-retries"));
  if (retries >= 50)
    {
      g_object_set_data (G_OBJECT (self),
                         "ptyxis-tab-focus-grab-retries", NULL);
      return G_SOURCE_REMOVE;
    }
  g_object_set_data (G_OBJECT (self),
                     "ptyxis-tab-focus-grab-retries",
                     GUINT_TO_POINTER (retries + 1));

  self->pending_focus_source = g_timeout_add_full (G_PRIORITY_LOW,
                                                   10,
                                                   ptyxis_tab_focus_grab_idle_cb,
                                                   g_object_ref (self),
                                                   g_object_unref);
  return G_SOURCE_REMOVE;
}

/* Arm a deferred focus grab. Coalesces with any previously-scheduled
 * attempt via pending_focus_source, so rapid-fire calls (e.g. a fast
 * user paging through tabs) install a single retry rather than a
 * chain of overlapping ones. The first attempt fires from the main
 * loop's idle phase, which gives GTK a chance to settle the
 * size-allocate cascade for newly-selected AdwTabView pages before
 * we try to grab focus. */
static void
ptyxis_tab_schedule_focus_grab (PtyxisTab *self)
{
  g_return_if_fail (PTYXIS_IS_TAB (self));

  if (self->pending_focus_source != 0)
    return;

  /* Clear any leftover retry counter from a previous schedule. */
  g_object_set_data (G_OBJECT (self),
                     "ptyxis-tab-focus-grab-retries", NULL);

  self->pending_focus_source = g_idle_add_full (G_PRIORITY_LOW,
                                                ptyxis_tab_focus_grab_idle_cb,
                                                g_object_ref (self),
                                                g_object_unref);
}

void
ptyxis_tab_grab_focus (PtyxisTab *self)
{
  g_return_if_fail (PTYXIS_IS_TAB (self));

  /* If the target terminal is already mapped, grab immediately.
   * Otherwise defer so the grab lands after GTK has had a chance to
   * allocate the paned tree — see the comment on
   * pending_focus_source for the failure mode this guards against
   * (tabs 2+ in a multi-pane restored tab going silent on first
   * selection because the new page is still getting its
   * allocation). */
  if (ptyxis_tab_try_grab_focus (self))
    return;

  ptyxis_tab_schedule_focus_grab (self);
}

/* -------------------------------------------------------------------------- */
/* Split pane support                                                         */
/* -------------------------------------------------------------------------- */

static void
ptyxis_tab_pane_free (gpointer data)
{
  PtyxisTabPane *pane = data;

  if (pane == NULL)
    return;

  pane->forced_exit = TRUE;

  g_cancellable_cancel (pane->cancellable);

  if (pane->process != NULL)
    {
      ptyxis_ipc_process_call_send_signal (pane->process, SIGHUP, NULL, NULL, NULL);
      g_clear_object (&pane->process);
    }

  if (!pane->is_primary)
    {
      if (pane->box != NULL)
        {
          GtkWidget *parent = gtk_widget_get_parent (pane->box);

          /* Drop our reference to the terminal first — the box destroy
           * below cascades into terminal destruction, and any signal
           * handlers that fire during that cascade must not observe a
           * dangling pane->terminal.
           */
          pane->terminal = NULL;

          if (parent != NULL)
            {
              if (GTK_IS_PANED (parent))
                {
                  if (gtk_paned_get_start_child (GTK_PANED (parent)) == pane->box)
                    gtk_paned_set_start_child (GTK_PANED (parent), NULL);
                  else if (gtk_paned_get_end_child (GTK_PANED (parent)) == pane->box)
                    gtk_paned_set_end_child (GTK_PANED (parent), NULL);
                }
              else
                {
                  gtk_widget_unparent (pane->box);
                }
            }

          g_clear_object (&pane->box);
        }
      else
        {
          /* Box already gone (e.g. on tab dispose via the child-unparent
           * loop) — still clear the terminal pointer.
           */
          pane->terminal = NULL;
        }
    }

  g_clear_object (&pane->container_at_creation);
  g_clear_object (&pane->cancellable);
  g_clear_pointer (&pane->command, g_strfreev);
  g_clear_pointer (&pane->command_line, g_free);
  g_clear_pointer (&pane->program_name, g_free);
  g_clear_pointer (&pane->initial_working_directory_uri, g_free);
  g_clear_pointer (&pane->previous_working_directory_uri, g_free);

  g_free (pane);
}

static void
ptyxis_tab_sync_active_pane (PtyxisTab     *self,
                             PtyxisTabPane *pane)
{
  g_assert (PTYXIS_IS_TAB (self));
  g_assert (pane != NULL);

  self->active_pane = pane;

  /* Update the visual dimming of panes so the focused one stands out. */
  if (self->panes != NULL)
    {
      for (guint i = 0; i < self->panes->len; i++)
        {
          PtyxisTabPane *p = g_ptr_array_index (self->panes, i);

          if (p == pane)
            {
              gtk_widget_remove_css_class (p->box, "ptyxis-pane-inactive");
              gtk_widget_add_css_class (p->box, "ptyxis-pane-active");
            }
          else
            {
              gtk_widget_remove_css_class (p->box, "ptyxis-pane-active");
              gtk_widget_add_css_class (p->box, "ptyxis-pane-inactive");
            }
        }
    }

  /* Mirror the pane's session fields onto the tab-level state so the
   * window chrome (title, icon, superuser/remote/container tint driven
   * by process-leader-kind) reflects the currently focused pane. This
   * must also run for the primary pane so switching back to it restores
   * the correct process/leader-kind after visiting a secondary pane.
   *
   * The lifecycle state is only mirrored when the pane has actually
   * finished spawning — otherwise a focus event arriving while the
   * primary is still in SPAWNING (with pane->state == INITIAL) would
   * clobber self->state and break the assertion in ptyxis_tab_spawn_cb.
   *
   * The new self->state guard closes the inverse case observed when
   * restoring a session with multiple panes. Each non-primary pane is
   * spawned eagerly by ptyxis_tab_create_pane_internal() during
   * ptyxis_tab_restore_panes_state(), and one of those spawns can
   * complete while the primary's own ptyxis_application_spawn_async()
   * is still in flight (state == SPAWNING). If that completed pane is
   * the active one, the pane-side guard alone would copy pane->state
   * (RUNNING) onto self->state and the primary's spawn_cb would then
   * abort at the line 622 assertion. Skipping the mirror while
   * self->state is SPAWNING keeps the primary's lifecycle undisturbed
   * until its own callback updates it.
   *
   * ALSO skip the mirror while self->state is INITIAL: the primary's
   * first spawn is gated on the map vfunc, which fires (and triggers
   * ptyxis_tab_respawn) only when self->state == PTYXIS_TAB_STATE_INITIAL.
   * During session restore, non-primary panes are spawned eagerly before
   * the primary has ever been mapped — if a non-primary pane completes
   * its spawn while active (which it always is, because ptyxis_tab_split_full
   * sets the newly-created pane as active), it would copy RUNNING onto
   * self->state here and the primary's map-time respawn would be skipped,
   * leaving the primary VTE with no PTY and the user staring at a blinking
   * cursor with no shell. Guard the mirror on the primary having at least
   * started its own lifecycle (state past INITIAL) so the primary's
   * map-time spawn chain still fires for restored tabs the user later
   * activates. */
  if (self->state != PTYXIS_TAB_STATE_INITIAL &&
      self->state != PTYXIS_TAB_STATE_SPAWNING &&
      pane->state != PTYXIS_TAB_STATE_INITIAL &&
      pane->state != PTYXIS_TAB_STATE_SPAWNING)
    self->state = pane->state;
  g_set_object (&self->process, pane->process);
  self->pid = pane->pid;
  self->has_foreground_process = pane->has_foreground_process;
  self->leader_kind = pane->leader_kind;
  g_set_str (&self->command_line, pane->command_line);
  g_set_str (&self->program_name, pane->program_name);

  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_TITLE]);
  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_SUBTITLE]);
  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_ICON]);
  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_COMMAND_LINE]);
  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_PROCESS_LEADER_KIND]);
}

static void
ptyxis_tab_bind_terminal_settings (PtyxisTab      *self,
                                   PtyxisTerminal *terminal)
{
  PtyxisSettings *settings;
  g_autoptr(GListModel) custom_links_list = NULL;
  g_autofree char *word_char_exceptions = NULL;
  long scrollback_lines = -1;

  g_assert (PTYXIS_IS_TAB (self));
  g_assert (PTYXIS_IS_TERMINAL (terminal));

  settings = ptyxis_application_get_settings (PTYXIS_APPLICATION_DEFAULT);

  g_object_bind_property (settings, "audible-bell",
                          terminal, "audible-bell",
                          G_BINDING_SYNC_CREATE);
  g_object_bind_property (settings, "cursor-shape",
                          terminal, "cursor-shape",
                          G_BINDING_SYNC_CREATE);
  g_object_bind_property (settings, "cursor-blink-mode",
                          terminal, "cursor-blink-mode",
                          G_BINDING_SYNC_CREATE);
  g_object_bind_property (settings, "enable-a11y",
                          terminal, "enable-a11y",
                          G_BINDING_SYNC_CREATE);
  g_object_bind_property (settings, "font-desc",
                          terminal, "font-desc",
                          G_BINDING_SYNC_CREATE);
  g_object_bind_property (settings, "text-blink-mode",
                          terminal, "text-blink-mode",
                          G_BINDING_SYNC_CREATE);

  g_object_bind_property (self->profile, "palette",
                          terminal, "palette",
                          G_BINDING_SYNC_CREATE);
  g_object_bind_property (self->profile, "scroll-on-keystroke",
                          terminal, "scroll-on-keystroke",
                          G_BINDING_SYNC_CREATE);
  g_object_bind_property (self->profile, "scroll-on-output",
                          terminal, "scroll-on-output",
                          G_BINDING_SYNC_CREATE);
  g_object_bind_property (self->profile, "backspace-binding",
                          terminal, "backspace-binding",
                          G_BINDING_SYNC_CREATE);
  g_object_bind_property (self->profile, "delete-binding",
                          terminal, "delete-binding",
                          G_BINDING_SYNC_CREATE);
  g_object_bind_property (self->profile, "cjk-ambiguous-width",
                          terminal, "cjk-ambiguous-width",
                          G_BINDING_SYNC_CREATE);
  g_object_bind_property (self->profile, "bold-is-bright",
                          terminal, "bold-is-bright",
                          G_BINDING_SYNC_CREATE);

  if (ptyxis_profile_get_limit_scrollback (self->profile))
    scrollback_lines = ptyxis_profile_get_scrollback_lines (self->profile);
  vte_terminal_set_scrollback_lines (VTE_TERMINAL (terminal), scrollback_lines);

  vte_terminal_set_cell_height_scale (VTE_TERMINAL (terminal),
                                      ptyxis_profile_get_cell_height_scale (self->profile));
  vte_terminal_set_cell_width_scale (VTE_TERMINAL (terminal),
                                     ptyxis_profile_get_cell_width_scale (self->profile));

  custom_links_list = ptyxis_profile_list_custom_links (self->profile);
  ptyxis_terminal_update_custom_links_list (terminal, custom_links_list);

  word_char_exceptions = ptyxis_settings_dup_word_char_exceptions (settings);
  vte_terminal_set_word_char_exceptions (VTE_TERMINAL (terminal), word_char_exceptions);

  if (ptyxis_settings_get_disable_padding (settings))
    gtk_widget_remove_css_class (GTK_WIDGET (terminal), "padded");
  else
    gtk_widget_add_css_class (GTK_WIDGET (terminal), "padded");

  ptyxis_tab_apply_zoom_to_terminal (self, terminal);
}

static void
ptyxis_tab_pane_apply_scrollbar_policy (PtyxisTabPane *pane)
{
  PtyxisSettings *settings;
  PtyxisScrollbarPolicy policy;

  g_assert (pane != NULL);
  g_assert (pane->scrolled_window != NULL);

  settings = ptyxis_application_get_settings (PTYXIS_APPLICATION_DEFAULT);
  policy = ptyxis_settings_get_scrollbar_policy (settings);

  switch (policy)
    {
    case PTYXIS_SCROLLBAR_POLICY_NEVER:
      gtk_scrolled_window_set_overlay_scrolling (pane->scrolled_window, FALSE);
      gtk_scrolled_window_set_policy (pane->scrolled_window, GTK_POLICY_NEVER, GTK_POLICY_EXTERNAL);
      break;

    case PTYXIS_SCROLLBAR_POLICY_ALWAYS:
      gtk_scrolled_window_set_overlay_scrolling (pane->scrolled_window, FALSE);
      gtk_scrolled_window_set_policy (pane->scrolled_window, GTK_POLICY_NEVER, GTK_POLICY_ALWAYS);
      break;

    case PTYXIS_SCROLLBAR_POLICY_SYSTEM:
      if (ptyxis_application_get_overlay_scrollbars (PTYXIS_APPLICATION_DEFAULT))
        {
          gtk_scrolled_window_set_overlay_scrolling (pane->scrolled_window, TRUE);
          gtk_scrolled_window_set_policy (pane->scrolled_window, GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        }
      else
        {
          gtk_scrolled_window_set_overlay_scrolling (pane->scrolled_window, FALSE);
          gtk_scrolled_window_set_policy (pane->scrolled_window, GTK_POLICY_NEVER, GTK_POLICY_ALWAYS);
        }
      break;

    default:
      g_assert_not_reached ();
    }
}

/* Move @pane to the front of @self->focus_history, removing any prior
 * entry. Called whenever a pane gains focus so that close_pane_widget
 * can find the most-recently-focused sibling pane in O(focus_history->len).
 */
static void
ptyxis_tab_push_focus_history (PtyxisTab     *self,
                              PtyxisTabPane *pane)
{
  g_assert (PTYXIS_IS_TAB (self));
  g_assert (pane != NULL);

  if (self->focus_history == NULL)
    return;

  /* Remove any existing entry for this pane (no-op if absent). */
  for (guint i = 0; i < self->focus_history->len; i++)
    {
      if (g_ptr_array_index (self->focus_history, i) == pane)
        {
          g_ptr_array_remove_index (self->focus_history, i);
          break;
        }
    }

  g_ptr_array_insert (self->focus_history, 0, pane);
}

/* Return the first pane in focus_history that is still in self->panes
 * and isn't @skip. Used by close_pane_widget to pick a focus-restoration
 * target that is guaranteed to still be alive.
 */
static PtyxisTabPane *
ptyxis_tab_find_focus_history_target (PtyxisTab     *self,
                                      PtyxisTabPane *skip)
{
  if (self->focus_history == NULL)
    return NULL;

  for (guint i = 0; i < self->focus_history->len; i++)
    {
      PtyxisTabPane *candidate = g_ptr_array_index (self->focus_history, i);
      gboolean found = FALSE;

      if (candidate == skip || candidate == NULL)
        continue;

      for (guint j = 0; j < self->panes->len; j++)
        {
          if (g_ptr_array_index (self->panes, j) == candidate)
            {
              found = TRUE;
              break;
            }
        }

      if (found)
        return candidate;
    }

  return NULL;
}

static void
ptyxis_tab_pane_focus_enter_cb (PtyxisTabPane            *pane,
                                GParamSpec               *pspec,
                                GtkEventControllerFocus  *focus)
{
  g_assert (pane != NULL);
  g_assert (pane->tab != NULL);

  if (gtk_event_controller_focus_contains_focus (focus))
    {
      /* Record the focus transition before changing active_pane so
       * the new pane sits at the front of focus_history and the
       * previous active pane is now at index 1.
       */
      ptyxis_tab_push_focus_history (pane->tab, pane);
      ptyxis_tab_sync_active_pane (pane->tab, pane);
      ptyxis_tab_set_needs_attention (pane->tab, FALSE);
      g_application_withdraw_notification (G_APPLICATION (PTYXIS_APPLICATION_DEFAULT),
                                           pane->tab->uuid);
    }
}

/* Per-pane async callback user data.
 *
 * The PtyxisTabPane struct is owned by PtyxisTab (freed via the
 * g_ptr_array_unref free func). If a pane is removed by close_pane_widget
 * before the IPC layer fires its callback, the struct memory is gone and
 * the raw pointer carried in user_data would be dangling — reading it
 * would be undefined behaviour even for a pointer comparison.
 *
 * To keep both the spawn and wait callbacks safe we instead ref the
 * owning PtyxisTab (which outlives its panes) and ref the pane's
 * GCancellable. The cancellable is owned by the pane and gets g_clear_object'd
 * in ptyxis_tab_pane_free, but our ref here keeps the object alive after the
 * pane is gone. The callback then locates the live pane (if any) via the
 * cancellable pointer inside tab->panes — once the pane is freed/removed the
 * lookup returns NULL and the callback bails out cleanly.
 */
typedef struct
{
  PtyxisTab       *tab;
  GCancellable    *cancellable;
} PtyxisPaneRefData;

static void
ptyxis_pane_ref_data_free (PtyxisPaneRefData *data)
{
  g_clear_object (&data->tab);
  g_clear_object (&data->cancellable);
  g_free (data);
}

static PtyxisTabPane *
ptyxis_tab_pane_lookup (PtyxisTab    *tab,
                        GCancellable *cancellable)
{
  g_assert (PTYXIS_IS_TAB (tab));
  g_assert (cancellable != NULL);

  /* `tab->panes` may already be NULL if ptyxis_tab_dispose ran before
   * this callback fired (e.g. closing a tab/window with multiple panes
   * while a spawn/wait async call was still in flight). Treat that as
   * "pane gone" — the caller bails. Dereferencing NULL here used to
   * segfault inside the g_ptr_array_unref's freed slot, manifesting as
   * a glib ref-count crash because the surrounding free list is the
   * same allocator.
   */
  if (tab->panes == NULL)
    return NULL;

  for (guint i = 0; i < tab->panes->len; i++)
    {
      PtyxisTabPane *pane = g_ptr_array_index (tab->panes, i);

      if (pane->cancellable == cancellable)
        return pane;
    }

  return NULL;
}

static void
ptyxis_tab_pane_wait_cb (GObject      *object,
                         GAsyncResult *result,
                         gpointer      user_data)
{
  PtyxisApplication *app = (PtyxisApplication *)object;
  PtyxisPaneRefData *wait_data = user_data;
  g_autoptr(PtyxisTab) tab = NULL;
  PtyxisTabPane *pane;
  g_autoptr(GError) error = NULL;
  int exit_code;

  g_assert (PTYXIS_IS_APPLICATION (app));
  g_assert (wait_data != NULL);

  /* Borrow the tab pointer first (it is still kept alive by the ref in
   * wait_data->tab), look up the pane by cancellable, then drop the
   * ref_data. Order matters: if we free wait_data before the lookup,
   * wait_data->tab might be the last reference — dropping it could
   * finalize PtyxisTab and turn the subsequent deref into a UAF.
   */
  tab = g_object_ref (wait_data->tab);
  pane = ptyxis_tab_pane_lookup (tab, wait_data->cancellable);
  ptyxis_pane_ref_data_free (wait_data);

  if (pane == NULL || tab->panes == NULL)
    return;

  g_clear_object (&pane->process);

  exit_code = ptyxis_application_wait_finish (app, result, &error);

  g_debug ("Pane process completed with exit-code 0x%x %s",
           exit_code,
           error ? error->message : "");

  if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    return;

  if (error == NULL && WIFEXITED (exit_code) && WEXITSTATUS (exit_code) == 0)
    pane->state = PTYXIS_TAB_STATE_EXITED;
  else
    pane->state = PTYXIS_TAB_STATE_FAILED;

  if (pane->forced_exit || tab->forced_exit)
    return;

  /* Secondary panes auto-close on shell exit; primary keeps banner UX. */
  if (!pane->is_primary)
    {
      PtyxisExitAction exit_action = ptyxis_profile_get_exit_action (tab->profile);

      if (exit_action == PTYXIS_EXIT_ACTION_RESTART)
        {
          pane->state = PTYXIS_TAB_STATE_EXITED;
          ptyxis_tab_pane_respawn (pane);
          return;
        }

      /* If the shell died in less than 0.5s and no key has been
       * pressed in this pane, treat it as a failed spawn rather
       * than an intentional exit. Auto-closing a non-primary pane
       * immediately on a likely-spawn-failure drops the user's
       * restored layout (the pane just disappears mid-tab-switch,
       * leaving the surviving paned children re-laid out) and
       * leaves them with a frozen-looking first pane in tabs 2+:
       * the shell exited before the user could see the prompt, the
       * pane auto-closed, and the visual that remains is just the
       * adjacent pane with its idle cursor. Mirror the primary's
       * 0.5s/never-typed guard so a broken spawn shows a banner
       * the user can act on, instead of silently shrinking the
       * tab. */
      if ((g_get_monotonic_time () - pane->respawn_time) < (G_USEC_PER_SEC/2) &&
          !ptyxis_tab_monitor_get_has_pressed_key (tab->monitor))
        {
          pane->state = PTYXIS_TAB_STATE_FAILED;
          adw_banner_set_title (pane->banner, _("Failed to launch terminal"));
          gtk_widget_set_visible (GTK_WIDGET (pane->banner), TRUE);
          return;
        }

      if (exit_action == PTYXIS_EXIT_ACTION_CLOSE ||
          exit_action == PTYXIS_EXIT_ACTION_NONE)
        {
          /* If this is the last remaining pane, closing it means closing
           * the whole tab — otherwise the tab would be stuck with a dead
           * shell and no way to dismiss it.
           */
          if (tab->panes->len <= 1)
            {
              gtk_widget_activate_action (GTK_WIDGET (tab), "page.close", NULL);
              return;
            }

          if (tab->active_pane == pane)
            {
              for (guint i = 0; i < tab->panes->len; i++)
                {
                  PtyxisTabPane *other = g_ptr_array_index (tab->panes, i);

                  if (other != pane)
                    {
                      ptyxis_tab_sync_active_pane (tab, other);
                      break;
                    }
                }
            }

          ptyxis_tab_close_pane_widget (tab, pane);
          return;
        }
    }

  if (tab->active_pane == pane)
    {
      g_object_notify_by_pspec (G_OBJECT (tab), properties[PROP_TITLE]);
      adw_banner_set_title (pane->banner, _("Process Exited"));
      adw_banner_set_button_label (pane->banner, _("_Restart"));
      gtk_actionable_set_action_name (GTK_ACTIONABLE (pane->banner), "tab.respawn");
      gtk_widget_set_visible (GTK_WIDGET (pane->banner), TRUE);
    }
}

static void
ptyxis_tab_pane_spawn_cb (GObject      *object,
                          GAsyncResult *result,
                          gpointer      user_data)
{
  PtyxisApplication *app = (PtyxisApplication *)object;
  PtyxisPaneRefData *spawn_data = user_data;
  g_autoptr(PtyxisIpcProcess) process = NULL;
  g_autoptr(PtyxisTab) tab = NULL;
  PtyxisTabPane *pane;
  PtyxisPaneRefData *wait_data;
  g_autoptr(GError) error = NULL;

  g_assert (spawn_data != NULL);

  /* Borrow the tab pointer first (kept alive by spawn_data->tab's ref),
   * look up the pane by cancellable, then drop the ref_data. Freeing
   * wait_data before the lookup could drop the last tab ref and turn
   * the subsequent deref into a UAF. */
  tab = g_object_ref (spawn_data->tab);

  /* The pane may have been freed (by close_pane_widget → pane_free)
   * while the spawn was in flight, OR the tab may have been disposed
   * (tab->panes == NULL). Locate by the ref'd cancellable; bail out
   * cleanly either way. */
  pane = ptyxis_tab_pane_lookup (tab, spawn_data->cancellable);
  if (pane == NULL)
    {
      g_debug ("[spawn] pane_spawn_cb BAIL tab=%p: pane lookup by cancellable failed "
               "(pane freed during spawn?)", (void*)tab);
      ptyxis_pane_ref_data_free (spawn_data);
      return;
    }

  g_assert (pane->state == PTYXIS_TAB_STATE_SPAWNING);

  if (g_cancellable_is_cancelled (pane->cancellable))
    {
      g_debug ("[spawn] pane_spawn_cb BAIL tab=%p pane=%p: cancellable already cancelled",
               (void*)tab, (void*)pane);
      ptyxis_pane_ref_data_free (spawn_data);
      return;
    }

  if (!(process = ptyxis_application_spawn_finish (app, result, &error)))
    {
      g_debug ("[spawn] pane_spawn_cb FAIL tab=%p pane=%p: %s",
               (void*)tab, (void*)pane,
               error ? error->message : "(no error message)");
      ptyxis_pane_ref_data_free (spawn_data);

      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        return;

      pane->state = PTYXIS_TAB_STATE_FAILED;
      vte_terminal_feed (VTE_TERMINAL (pane->terminal), error->message, -1);
      vte_terminal_feed (VTE_TERMINAL (pane->terminal), "\r\n", -1);
      adw_banner_set_title (pane->banner, _("Failed to launch terminal"));
      gtk_widget_set_visible (GTK_WIDGET (pane->banner), TRUE);
      return;
    }

  g_debug ("[spawn] pane_spawn_cb OK tab=%p pane=%p -> process=%p, queuing wait_async",
           (void*)tab, (void*)pane, (void*)process);

  /* Post-spawn PTY probe: confirm that the VTE widget actually has a
   * backing PTY and that the PTY's read side is open. If VTE has no
   * PTY, the spawn "succeeded" on the IPC side but the shell will
   * never produce visible output — explaining the "blinking cursor,
   * no bash" symptom. */
  {
    VtePty *pty = vte_terminal_get_pty (VTE_TERMINAL (pane->terminal));
    if (pty == NULL)
      {
        g_debug ("[spawn] PTY_PROBE tab=%p pane=%p: NO PTY attached to VTE widget — "
                 "shell ran but its stdio has nowhere to go",
                 (void*)tab, (void*)pane);
      }
    else
      {
        int fd = vte_pty_get_fd (pty);
        g_debug ("[spawn] PTY_PROBE tab=%p pane=%p: pty=%p fd=%d",
                 (void*)tab, (void*)pane, (void*)pty, fd);
      }
  }

  pane->state = PTYXIS_TAB_STATE_RUNNING;
  pane->respawn_time = g_get_monotonic_time ();
  g_set_object (&pane->process, process);

  if (tab->active_pane == pane)
    ptyxis_tab_sync_active_pane (tab, pane);

  /* Hand a ref'd PtyxisTab + a ref'd GCancellable to the wait callback.
   * PtyxisTab outlives its panes, so even if close_pane_widget frees
   * `pane` before the IPC "exited" callback fires, the wait_cb can
   * still safely dereference PtyxisTab and resolve the pane via the
   * cancellable. The cancellable's ref keeps it alive even after
   * ptyxis_tab_pane_free drops the pane's own reference. */
  wait_data = g_new0 (PtyxisPaneRefData, 1);
  wait_data->tab = g_object_ref (tab);
  wait_data->cancellable = g_object_ref (pane->cancellable);

  ptyxis_application_wait_async (app,
                                 process,
                                 pane->cancellable,
                                 ptyxis_tab_pane_wait_cb,
                                 wait_data);

  /* Done with the spawn_data wrapper now that the wait has been queued. */
  ptyxis_pane_ref_data_free (spawn_data);
}

static void
ptyxis_tab_pane_respawn (PtyxisTabPane *pane)
{
  g_autofree char *default_container = NULL;
  g_autoptr(PtyxisIpcContainer) container = NULL;
  g_autoptr(VtePty) new_pty = NULL;
  PtyxisTab *self;
  const char *cwd_uri;
  VtePty *pty;
  guint pane_index;

  g_assert (pane != NULL);
  g_assert (pane->tab != NULL);

  self = pane->tab;

  /* Capture pane index BEFORE the optional early return so the log
   * line is meaningful when the primary path takes over. */
  pane_index = pane == self->active_pane && self->panes != NULL
      ? (guint)(self->active_pane - (PtyxisTabPane *)self->panes->pdata)
      : (self->panes ? self->panes->len : 0);

  if (pane->is_primary)
    {
      g_debug ("[spawn] pane_respawn tab=%p pane=%p (primary, delegating to tab_respawn) "
               "pane_index=%u cwd_uri=%s",
               (void*)self, (void*)pane, pane_index,
               pane->initial_working_directory_uri ? pane->initial_working_directory_uri : "(null)");
      ptyxis_tab_respawn (self);
      return;
    }

  gtk_widget_set_visible (GTK_WIDGET (pane->banner), FALSE);

  default_container = ptyxis_profile_dup_default_container (self->profile);

  g_debug ("[spawn] pane_respawn ENTER tab=%p pane=%p pane_index=%u is_primary=%d "
           "container_at_creation=%s default_container=%s tab_panes=%u",
           (void*)self, (void*)pane, pane_index, pane->is_primary,
           pane->container_at_creation ?
             ptyxis_ipc_container_get_id (pane->container_at_creation) : "(null)",
           default_container ? default_container : "(null)",
           self->panes ? self->panes->len : 0);

  if (pane->container_at_creation != NULL)
    container = g_object_ref (pane->container_at_creation);
  else if (self->container_at_creation != NULL)
    container = g_object_ref (self->container_at_creation);
  else
    container = ptyxis_application_lookup_container (PTYXIS_APPLICATION_DEFAULT,
                                                     default_container);

  if (container == NULL)
    {
      g_debug ("[spawn] pane_respawn FAIL tab=%p pane=%p pane_index=%u: "
               "container lookup returned NULL (no usable container)",
               (void*)self, (void*)pane, pane_index);
      pane->state = PTYXIS_TAB_STATE_FAILED;
      adw_banner_set_title (pane->banner, _("Failed to launch terminal"));
      gtk_widget_set_visible (GTK_WIDGET (pane->banner), TRUE);
      return;
    }

  pane->state = PTYXIS_TAB_STATE_SPAWNING;

  g_clear_object (&pane->cancellable);
  pane->cancellable = g_cancellable_new ();

  pty = vte_terminal_get_pty (VTE_TERMINAL (pane->terminal));
  if (pty == NULL)
    {
      g_autoptr(GError) error = NULL;

      g_debug ("[spawn] pane_respawn creating PTY tab=%p pane=%p pane_index=%u",
               (void*)self, (void*)pane, pane_index);
      new_pty = ptyxis_application_create_pty (PTYXIS_APPLICATION_DEFAULT, &error);
      if (new_pty == NULL)
        {
          g_debug ("[spawn] pane_respawn FAIL tab=%p pane=%p pane_index=%u: "
                   "PTY create failed: %s",
                   (void*)self, (void*)pane, pane_index,
                   error ? error->message : "(no error)");
          pane->state = PTYXIS_TAB_STATE_FAILED;
          adw_banner_set_title (pane->banner, _("Failed to create pseudo terminal device"));
          gtk_widget_set_visible (GTK_WIDGET (pane->banner), TRUE);
          return;
        }

      vte_terminal_set_pty (VTE_TERMINAL (pane->terminal), new_pty);
      pty = new_pty;
    }

  cwd_uri = pane->previous_working_directory_uri;
  if (!ptyxis_str_empty0 (pane->initial_working_directory_uri))
    cwd_uri = pane->initial_working_directory_uri;
  else if (ptyxis_str_empty0 (cwd_uri))
    cwd_uri = self->previous_working_directory_uri;

  g_debug ("[spawn] pane_respawn -> spawn_async tab=%p pane=%p pane_index=%u "
           "container=%s cwd_uri=%s argv0=%s",
           (void*)self, (void*)pane, pane_index,
           ptyxis_ipc_container_get_id (container),
           cwd_uri ? cwd_uri : "(null)",
           pane->command && pane->command[0] ? pane->command[0] : "(null)");

  /* Wrap the tab + cancellable so spawn_cb can resolve the pane
   * safely even if close_pane_widget frees the pane before the IPC
   * completion fires. See PtyxisPaneRefData. */
  PtyxisPaneRefData *spawn_data = g_new0 (PtyxisPaneRefData, 1);
  spawn_data->tab = g_object_ref (self);
  spawn_data->cancellable = g_object_ref (pane->cancellable);

  ptyxis_application_spawn_async (PTYXIS_APPLICATION_DEFAULT,
                                  container,
                                  self->profile,
                                  cwd_uri,
                                  pty,
                                  (const char * const *)pane->command,
                                  pane->cancellable,
                                  ptyxis_tab_pane_spawn_cb,
                                  spawn_data);
}

static gboolean
ptyxis_tab_close_pane_widget (PtyxisTab     *self,
                              PtyxisTabPane *pane)
{
  GtkWidget *box;
  GtkWidget *parent;
  GtkWidget *sibling = NULL;
  GtkWidget *grandparent;
  PtyxisTabPane *restore;
  guint index = GTK_INVALID_LIST_POSITION;

  g_assert (PTYXIS_IS_TAB (self));
  g_assert (pane != NULL);

  if (self->panes == NULL || self->panes->len <= 1)
    return FALSE;

  box = pane->box;
  parent = gtk_widget_get_parent (box);

  if (!GTK_IS_PANED (parent))
    return FALSE;

  if (gtk_paned_get_start_child (GTK_PANED (parent)) == box)
    sibling = gtk_paned_get_end_child (GTK_PANED (parent));
  else
    sibling = gtk_paned_get_start_child (GTK_PANED (parent));

  if (sibling == NULL)
    return FALSE;

  grandparent = gtk_widget_get_parent (parent);

  /* Determine the focus restoration target. Walk focus_history (the
   * ordered list of recently-focused panes, most recent first) and
   * pick the first entry that is still in self->panes and isn't the
   * pane we're about to close. This gives the user "focus returns to
   * the previously-focused pane" semantics that mirror common IDE/editor
   * tab-close behaviour, instead of GTK's default which tends to land
   * on the first/top-left pane after the widget tree has been mutated.
   */
  restore = ptyxis_tab_find_focus_history_target (self, pane);

  /* Move focus AWAY from the dying pane's terminal *before* the unparent
   * chain. Two failure modes we have to defend against here:
   *
   * 1. GtkPaned's `gtk_paned_set_focus_child()` runs whenever a paned
   *    child is unparented while it was the paned's focus_child. The
   *    implementation walks up from the window's focused widget looking
   *    for the paned, and emits
   *        "Error finding last focus widget of GtkPaned …"
   *    if the focused widget is not a descendant of the paned any
   *    longer. We avoid this by giving the paned itself focus: the
   *    walk-up from "paned" hits the paned immediately on the first
   *    iteration, exits the loop cleanly, and never reaches the
   *    warning branch. Crucially, focusing the paned also means the
   *    paned's `focus_child` is now the paned (not the dying box or
   *    its sibling), so neither unparent triggers the set_focus_child
   *    call in the first place.
   *
   * 2. After the unparent chain settles, focus needs to actually be on
   *    a focusable descendant of the surviving pane (the terminal) so
   *    that VTE starts blinking the cursor and receives key events.
   *    We do that explicit grab_focus below, after the tree has been
   *    rewritten.
   */
  gtk_widget_grab_focus (parent);

  g_object_ref (sibling);
  gtk_paned_set_start_child (GTK_PANED (parent), NULL);
  gtk_paned_set_end_child (GTK_PANED (parent), NULL);

  if (PTYXIS_IS_TAB (grandparent))
    {
      gtk_widget_unparent (parent);
      gtk_widget_set_parent (sibling, grandparent);
    }
  else if (GTK_IS_PANED (grandparent))
    {
      if (gtk_paned_get_start_child (GTK_PANED (grandparent)) == parent)
        gtk_paned_set_start_child (GTK_PANED (grandparent), sibling);
      else
        gtk_paned_set_end_child (GTK_PANED (grandparent), sibling);
    }

  g_object_unref (sibling);

  for (guint i = 0; i < self->panes->len; i++)
    {
      if (g_ptr_array_index (self->panes, i) == pane)
        {
          index = i;
          break;
        }
    }

  /* Drop the dying pane from focus_history so it can't be picked as a
   * restoration target on a later close (it'll be freed by
   * g_ptr_array_unref below, leaving a dangling pointer otherwise).
   */
  if (self->focus_history != NULL)
    {
      for (guint i = 0; i < self->focus_history->len; i++)
        {
          if (g_ptr_array_index (self->focus_history, i) == pane)
            {
              g_ptr_array_remove_index (self->focus_history, i);
              break;
            }
        }
    }

  if (self->active_pane == pane)
    {
      PtyxisTabPane *next = NULL;

      /* Prefer the focus-history target (chosen before the unparent
       * chain) so focus returns to the pane that was focused right
       * before the now-closing pane, matching how users expect tab
       * focus to behave. Fall back to the first remaining pane in
       * self->panes if the history target is unavailable.
       */
      if (restore != NULL && restore != pane)
        {
          next = restore;
        }
      else
        {
          for (guint i = 0; i < self->panes->len; i++)
            {
              PtyxisTabPane *other = g_ptr_array_index (self->panes, i);

              if (other != pane)
                {
                  next = other;
                  break;
                }
            }
        }

      if (next != NULL && next->terminal != NULL)
        {
          /* Promote @next in the focus history so a subsequent close of
           * another pane will, in turn, restore focus to the pane that
           * was focused before @next — the history just keeps stepping
           * backwards through the focus chain.
           */
          ptyxis_tab_push_focus_history (self, next);
          ptyxis_tab_sync_active_pane (self, next);
	  gtk_widget_grab_focus (GTK_WIDGET (next->terminal));
        }
    }

  if (index != GTK_INVALID_LIST_POSITION)
    {
      /* If the closing pane owned self->terminal (i.e. was primary),
       * redirect self->terminal to the surviving pane's terminal so
       * subsequent code paths (respawn, progress, banner, ...) don't
       * operate on a dangling pointer. Move the weak pointer to track
       * the new terminal.
       */
      if (pane->is_primary && pane->terminal == self->terminal)
        {
          PtyxisTerminal *replacement = NULL;

          for (guint i = 0; i < self->panes->len; i++)
            {
              PtyxisTabPane *other = g_ptr_array_index (self->panes, i);

              if (other != pane && other->terminal != NULL)
                {
                  replacement = other->terminal;
                  break;
                }
            }

          if (replacement != NULL)
            {
              if (self->terminal != NULL)
                g_object_remove_weak_pointer (G_OBJECT (self->terminal),
                                              (gpointer *)&self->terminal);
              self->terminal = replacement;
              g_object_add_weak_pointer (G_OBJECT (self->terminal),
                                         (gpointer *)&self->terminal);
            }
        }

      g_ptr_array_remove_index (self->panes, index);
    }

  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_N_PANES]);

  return TRUE;
}

static PtyxisTabPane *
ptyxis_tab_create_pane_internal (PtyxisTab          *self,
                                 const char         *override_cwd_uri,
                                 PtyxisIpcContainer *override_container)
{
  PtyxisTabPane *pane;
  PtyxisTabPane *source;
  GtkEventController *focus;
  GtkWidget *box;
  AdwBanner *banner;
  GtkScrolledWindow *scrolled;
  PtyxisTerminal *terminal;
  g_autofree char *cwd_uri = NULL;

  g_assert (PTYXIS_IS_TAB (self));

  source = self->active_pane != NULL ? self->active_pane : g_ptr_array_index (self->panes, 0);

  box = g_object_ref_sink (gtk_box_new (GTK_ORIENTATION_VERTICAL, 0));
  gtk_widget_set_hexpand (box, TRUE);
  gtk_widget_set_vexpand (box, TRUE);

  banner = ADW_BANNER (adw_banner_new (""));
  adw_banner_set_revealed (banner, TRUE);
  gtk_widget_set_visible (GTK_WIDGET (banner), FALSE);
  gtk_box_append (GTK_BOX (box), GTK_WIDGET (banner));

  scrolled = GTK_SCROLLED_WINDOW (gtk_scrolled_window_new ());
  gtk_scrolled_window_set_propagate_natural_width (scrolled, TRUE);
  gtk_scrolled_window_set_propagate_natural_height (scrolled, TRUE);
  gtk_scrolled_window_set_policy (scrolled, GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_vexpand (GTK_WIDGET (scrolled), TRUE);
  gtk_box_append (GTK_BOX (box), GTK_WIDGET (scrolled));

  terminal = g_object_new (PTYXIS_TYPE_TERMINAL,
                           "enable-fallback-scrolling", FALSE,
                           "scroll-unit-is-pixels", TRUE,
                           NULL);
  gtk_scrolled_window_set_child (scrolled, GTK_WIDGET (terminal));

  pane = g_new0 (PtyxisTabPane, 1);
  pane->tab = self;
  pane->box = box;
  pane->banner = banner;
  pane->scrolled_window = scrolled;
  pane->terminal = terminal;
  pane->state = PTYXIS_TAB_STATE_INITIAL;
  pane->is_primary = FALSE;

  /* Explicit overrides (used by session restore) win over inheritance. */
  if (override_cwd_uri != NULL)
    {
      pane->initial_working_directory_uri = g_strdup (override_cwd_uri);
    }
  else
    {
      /* Inherit working directory from the source pane. */
      if (source->is_primary)
        cwd_uri = ptyxis_tab_dup_current_directory_uri (self);
      else
        cwd_uri = ptyxis_terminal_dup_current_directory_uri (source->terminal);

      if (cwd_uri != NULL)
        pane->initial_working_directory_uri = g_steal_pointer (&cwd_uri);
    }

  if (override_container != NULL)
    {
      pane->container_at_creation = g_object_ref (override_container);
    }
  else
    {
      /* Inherit container from the source pane. */
      if (source->is_primary)
        {
          if (self->container_at_creation != NULL)
            pane->container_at_creation = g_object_ref (self->container_at_creation);
        }
      else
        {
          if (source->container_at_creation != NULL)
            pane->container_at_creation = g_object_ref (source->container_at_creation);
        }
    }

  focus = gtk_event_controller_focus_new ();
  g_signal_connect_swapped (focus,
                            "notify::contains-focus",
                            G_CALLBACK (ptyxis_tab_pane_focus_enter_cb),
                            pane);
  gtk_widget_add_controller (box, focus);

  g_signal_connect_swapped (terminal,
                            "notify::window-title",
                            G_CALLBACK (ptyxis_tab_notify_window_title_cb),
                            self);
  g_signal_connect_swapped (terminal,
                            "current-directory-uri-changed",
                            G_CALLBACK (ptyxis_tab_notify_window_subtitle_cb),
                            self);
  g_signal_connect_swapped (terminal,
                            "current-file-uri-changed",
                            G_CALLBACK (ptyxis_tab_notify_window_subtitle_cb),
                            self);
  g_signal_connect_swapped (terminal,
                            "increase-font-size",
                            G_CALLBACK (ptyxis_tab_increase_font_size_cb),
                            self);
  g_signal_connect_swapped (terminal,
                            "decrease-font-size",
                            G_CALLBACK (ptyxis_tab_decrease_font_size_cb),
                            self);
  g_signal_connect_swapped (terminal,
                            "bell",
                            G_CALLBACK (ptyxis_tab_bell_cb),
                            self);
  g_signal_connect_swapped (terminal,
                            "commit",
                            G_CALLBACK (ptyxis_tab_commit_cb),
                            self);
  g_signal_connect_object (terminal,
                           "match-clicked",
                           G_CALLBACK (ptyxis_tab_match_clicked_cb),
                           self,
                           G_CONNECT_SWAPPED);

  ptyxis_tab_bind_terminal_settings (self, terminal);
  ptyxis_tab_pane_apply_scrollbar_policy (pane);

  g_ptr_array_add (self->panes, pane);
  g_debug ("[spawn] create_pane_internal DONE tab=%p pane=%p pane_index=%u "
           "is_primary=%d cwd=%s container=%s -> calling pane_respawn",
           (void*)self, (void*)pane,
           self->panes->len - 1, pane->is_primary,
           pane->initial_working_directory_uri ?
             pane->initial_working_directory_uri : "(null)",
           pane->container_at_creation ?
             ptyxis_ipc_container_get_id (pane->container_at_creation) : "(null)");
  ptyxis_tab_pane_respawn (pane);

  return pane;
}

/* Thin wrapper kept for the interactive split action: inherit cwd and
 * container from the active pane. */
static PtyxisTabPane *
ptyxis_tab_create_pane (PtyxisTab *self)
{
  return ptyxis_tab_create_pane_internal (self, NULL, NULL);
}

/* "map" signal handler for the tab root widget — fires exactly once
 * when the tab is first mapped (i.e. when its top-level window is
 * shown). The actual work is in ptyxis_tab_install_position_guard
 * below; this callback is just the trigger that walks the paned tree.
 */

/* notify::position handler for restoring split-pane divider positions.
 *
 * Connected by ptyxis_tab_install_position_guard() for every paned that
 * has a stashed saved position from session restore. The reason we
 * can't just call gtk_paned_set_position once at map time:
 *
 * In an AdwTabView, only the *active* page is allocated real size.
 * Inactive tabs (siblings of the focused one) get map events but with
 * 0x0 allocations. Calling gtk_paned_set_position on a 0x0 paned stores
 * the value but GTK may silently overwrite it later when the tab is
 * finally activated and its paneds get real size — GTK recomputes the
 * divider position from the children's natural sizes at that point
 * unless position-set is TRUE *and* the value was set after allocation.
 *
 * The fix: connect to notify::position. Whenever GTK changes the
 * position away from our saved value (which it does during the
 * size-allocate cascade when the tab gets its real allocation), we
 * re-apply the saved value. We self-disconnect after the position
 * has been stable for a few cycles, so we don't fight the user when
 * they later drag the divider.
 */
static void
ptyxis_tab_position_notify_cb (GObject    *gobject,
                               GParamSpec *pspec,
                               gpointer    user_data)
{
  GtkPaned *paned = GTK_PANED (gobject);
  int saved = GPOINTER_TO_INT (g_object_get_data (gobject, "ptyxis-tab-saved-position"));
  int current = gtk_paned_get_position (paned);
  int stable_count;

  if (saved < 0)
    {
      /* Already cleared by a previous callback pass — just disconnect. */
      g_signal_handlers_disconnect_by_func (gobject, ptyxis_tab_position_notify_cb, NULL);
      return;
    }

  if (current != saved)
    {
      gtk_paned_set_position (paned, saved);
      /* Reset stability counter — we're still fighting GTK. */
      g_object_set_data (gobject, "ptyxis-tab-stable-count", GINT_TO_POINTER (0));
      return;
    }

  /* Position matches the saved value. After a couple of stable
   * notifications (position no longer changes), GTK has finished its
   * size-allocate settling and we can release control back to the user. */
  stable_count = GPOINTER_TO_INT (g_object_get_data (gobject, "ptyxis-tab-stable-count"));
  stable_count++;
  g_object_set_data (gobject, "ptyxis-tab-stable-count",
                     GINT_TO_POINTER (stable_count));
  if (stable_count >= 2)
    {
      g_signal_handlers_disconnect_by_func (gobject, ptyxis_tab_position_notify_cb, NULL);
      g_object_set_data (gobject, "ptyxis-tab-saved-position", GINT_TO_POINTER (-1));
      g_object_set_data (gobject, "ptyxis-tab-stable-count", GINT_TO_POINTER (0));
    }
}

static void
ptyxis_tab_paned_destroy_cb (GtkWidget *widget, gpointer user_data)
{
  if (g_getenv ("PTYXIS_DEBUG_DESTROY"))
    g_print ("[destroy] paned %p destroyed, parent at destroy=%p\n",
             (void*)widget,
             (void*)gtk_widget_get_parent (widget));
}

static void
ptyxis_tab_widget_parent_notify_cb (GObject *gobject, GParamSpec *pspec, gpointer user_data)
{
  GtkWidget *w = GTK_WIDGET (gobject);
  g_print ("[parent] %s %p parent -> %p\n",
           G_OBJECT_TYPE_NAME (w),
           (void*)w, (void*)gtk_widget_get_parent (w));
}

static void
ptyxis_tab_widget_destroy_cb (GtkWidget *widget, gpointer user_data)
{
  g_print ("[destroy] %s %p destroyed, parent at destroy=%p\n",
           G_OBJECT_TYPE_NAME (widget),
           (void*)widget,
           (void*)gtk_widget_get_parent (widget));
}

static void
ptyxis_tab_install_position_guard (GtkWidget *widget)
{
  GtkWidget *child;

  if (g_getenv ("PTYXIS_DEBUG_GUARD"))
    g_print ("[guard] walking %p [%s] first_child=%p\n",
             (void*)widget, G_OBJECT_TYPE_NAME (widget),
             (void*)gtk_widget_get_first_child (widget));

  if (GTK_IS_PANED (widget))
    {
      int saved = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (widget),
                                                      "ptyxis-tab-saved-position"));
      if (saved >= 0)
        {
          g_object_set_data (G_OBJECT (widget),
                             "ptyxis-tab-stable-count",
                             GINT_TO_POINTER (0));
          g_signal_connect (widget, "notify::position",
                            G_CALLBACK (ptyxis_tab_position_notify_cb),
                            NULL);
          /* Apply now too in case the paned already has an allocation. */
          gtk_paned_set_position (GTK_PANED (widget), saved);
        }
    }

  for (child = gtk_widget_get_first_child (widget);
       child != NULL;
       child = gtk_widget_get_next_sibling (child))
    ptyxis_tab_install_position_guard (child);
}

static gboolean
ptyxis_tab_dump_layout_cb (gpointer user_data)
{
  GtkWidget *tab = GTK_WIDGET (user_data);
  if (g_getenv ("PTYXIS_DEBUG_LAYOUT") == NULL)
    return G_SOURCE_REMOVE;

  g_print ("=== PTYXIS DEBUG: layout of tab @ %p ===\n", tab);
  for (GtkWidget *c = gtk_widget_get_first_child (tab); c != NULL;
       c = gtk_widget_get_next_sibling (c))
    {
      if (GTK_IS_PANED (c))
        {
          g_print ("  root paned orient=%s pos=%d alloc=%s\n",
                   gtk_orientable_get_orientation (GTK_ORIENTABLE (c)) == GTK_ORIENTATION_HORIZONTAL ? "H" : "V",
                   gtk_paned_get_position (GTK_PANED (c)),
                   "see parent");
          for (GtkWidget *sc = gtk_paned_get_start_child (GTK_PANED (c));
               sc != NULL;
               sc = gtk_widget_get_first_child (sc))
            {
              if (GTK_IS_PANED (sc))
                g_print ("    inner paned (start) orient=%s pos=%d\n",
                         gtk_orientable_get_orientation (GTK_ORIENTABLE (sc)) == GTK_ORIENTATION_HORIZONTAL ? "H" : "V",
                         gtk_paned_get_position (GTK_PANED (sc)));
            }
          for (GtkWidget *ec = gtk_paned_get_end_child (GTK_PANED (c));
               ec != NULL;
               ec = gtk_widget_get_first_child (ec))
            {
              if (GTK_IS_PANED (ec))
                g_print ("    inner paned (end) orient=%s pos=%d\n",
                         gtk_orientable_get_orientation (GTK_ORIENTABLE (ec)) == GTK_ORIENTATION_HORIZONTAL ? "H" : "V",
                         gtk_paned_get_position (GTK_PANED (ec)));
            }
        }
    }
  g_print ("=== END ===\n");
  return G_SOURCE_REMOVE;
}

static void
ptyxis_tab_map_cb (GtkWidget *widget, gpointer user_data)
{
  /* Walk the paned tree and connect a notify::position guard on every
   * paned that has a stashed saved position. The guard re-applies the
   * saved value whenever GTK tries to overwrite it (typically during
   * the size-allocate cascade that fires when the tab gets its real
   * allocation, which may be much later than map time if the tab is
   * not the active tab in its AdwTabView). */
  ptyxis_tab_install_position_guard (widget);

  /* Debug dump if PTYXIS_DEBUG_LAYOUT is set, after 1.5s for layout to settle. */
  if (g_getenv ("PTYXIS_DEBUG_LAYOUT") != NULL)
    g_timeout_add (1500, ptyxis_tab_dump_layout_cb, widget);

  /* One-shot — disconnect so we don't fire again on unmap/remap. */
  g_signal_handlers_disconnect_by_func (widget, ptyxis_tab_map_cb, NULL);
}

void
ptyxis_tab_split_full (PtyxisTab       *self,
                       GtkOrientation   orientation,
                       int              position,
                       const char      *cwd_uri,
                       PtyxisIpcContainer *container)
{
  PtyxisTabPane *source;
  PtyxisTabPane *created;
  GtkWidget *source_box;
  GtkWidget *parent;
  GtkWidget *paned;
  int size;

  g_return_if_fail (PTYXIS_IS_TAB (self));
  g_return_if_fail (self->panes != NULL && self->panes->len > 0);

  source = self->active_pane != NULL ? self->active_pane : g_ptr_array_index (self->panes, 0);
  source_box = source->box;
  parent = gtk_widget_get_parent (source_box);

  g_debug ("[spawn] split_full ENTER tab=%p orient=%d pos=%d cwd=%s "
           "container=%s source_pane=%p is_primary=%d n_panes_before=%u",
           (void*)self, (int)orientation, position,
           cwd_uri ? cwd_uri : "(null)",
           container ? ptyxis_ipc_container_get_id (container) : "(null)",
           (void*)source, source->is_primary,
           self->panes->len);

  created = ptyxis_tab_create_pane_internal (self, cwd_uri, container);

  paned = gtk_paned_new (orientation);
  gtk_widget_add_css_class (paned, "ptyxis-split");
  gtk_widget_set_hexpand (paned, TRUE);
  gtk_widget_set_vexpand (paned, TRUE);
  gtk_paned_set_wide_handle (GTK_PANED (paned), TRUE);
  gtk_paned_set_resize_start_child (GTK_PANED (paned), TRUE);
  gtk_paned_set_resize_end_child (GTK_PANED (paned), TRUE);
  gtk_paned_set_shrink_start_child (GTK_PANED (paned), FALSE);
  gtk_paned_set_shrink_end_child (GTK_PANED (paned), FALSE);

  if (orientation == GTK_ORIENTATION_HORIZONTAL)
    size = gtk_widget_get_width (source_box);
  else
    size = gtk_widget_get_height (source_box);

  /* Detach source from its parent and wrap both panes in a paned. */
  g_object_ref (source_box);

  if (PTYXIS_IS_TAB (parent))
    {
      gtk_widget_unparent (source_box);
      gtk_widget_set_parent (paned, parent);
    }
  else if (GTK_IS_PANED (parent))
    {
      if (gtk_paned_get_start_child (GTK_PANED (parent)) == source_box)
        gtk_paned_set_start_child (GTK_PANED (parent), paned);
      else
        gtk_paned_set_end_child (GTK_PANED (parent), paned);
    }
  else
    {
      g_object_unref (source_box);
      g_return_if_reached ();
    }

  gtk_paned_set_start_child (GTK_PANED (paned), source_box);
  gtk_paned_set_end_child (GTK_PANED (paned), created->box);
  g_object_unref (source_box);

  if (g_getenv ("PTYXIS_DEBUG_DESTROY"))
    {
      g_signal_connect (paned, "destroy", G_CALLBACK (ptyxis_tab_paned_destroy_cb), NULL);
      g_signal_connect (paned, "notify::parent", G_CALLBACK (ptyxis_tab_widget_parent_notify_cb), NULL);
      g_signal_connect (created->box, "notify::parent", G_CALLBACK (ptyxis_tab_widget_parent_notify_cb), NULL);
      g_signal_connect (created->box, "destroy", G_CALLBACK (ptyxis_tab_widget_destroy_cb), NULL);
    }

  /* @position < 0 means "compute from source box size" (the default for
   * an interactive split). Otherwise honor the explicit position so
   * session restore can reproduce the saved divider placement.
   *
   * For session restore, the paned is built while the tab is being
   * constructed (during ptyxis_session_restore, BEFORE the window has
   * been presented via gtk_window_present). At that point the paned
   * has no allocation yet, so gtk_paned_set_position is a no-op if
   * called too early.
   *
   * The deeper problem the user reported: when we build a chain of N
   * nested paneds, each split_full() call replaces an end_child slot
   * with a new paned, which queues a resize. GTK then re-allocates the
   * outer paned with its new child, and during that re-allocation GTK
   * RECOMPUTES the position based on the children's natural sizes —
   * silently overwriting the value we just set in the previous
   * iteration. By the time the whole chain is built, the outer
   * paneds' positions have all been clobbered with GTK defaults, which
   * is why the restored layout collapses to "all horizontal" or "all
   * vertical" instead of the saved H,H,V,V,V,V,V shape.
   *
   * Fix: stash every non-default position on the paned itself, and
   * re-apply it on every size-allocate until it sticks. We do this via
   * a "size-allocate" signal handler that fires after GTK's own
   * allocation logic — at that point we know the paned has its final
   * geometry for the current size cycle. We keep re-applying on
   * subsequent size-allocates (e.g. window resize) so the saved
   * proportions are preserved across window size changes.
   */
  if (position >= 0)
    {
      /* Stash the saved position on the paned so the one-shot map
       * handler on the tab (see ptyxis_tab_apply_pane_positions) can
       * re-apply it once the window has been mapped and all paneds
       * have stable allocations. */
      g_object_set_data (G_OBJECT (paned),
                         "ptyxis-tab-saved-position",
                         GINT_TO_POINTER (position));

      /* Try the direct set first — it works for the interactive-split
       * case (paned already realized). If we're in restore mode this
       * set will likely be a no-op, but the map handler picks it up. */
      gtk_paned_set_position (GTK_PANED (paned), position);

      /* Install the notify::position guard NOW, not later on map. The
       * map handler only fires for the active tab — AdwTabView doesn't
       * map (and therefore doesn't allocate) inactive pages until the
       * user selects them. If we wait for map, the guard wouldn't be
       * connected for tabs that were restored but not yet focused, and
       * when the user finally selects them GTK's first allocation would
       * overwrite our saved divider position with whatever fits the
       * children's natural sizes. Connecting the guard right here means
       * it's ready by the time the paned is first allocated, regardless
       * of which tab is currently visible. The map handler will see
       * saved-position already cleared and skip. */
      ptyxis_tab_install_position_guard (paned);
      g_object_set_data (G_OBJECT (paned),
                         "ptyxis-tab-saved-position",
                         GINT_TO_POINTER (-1));
    }
  else if (size > 0)
    gtk_paned_set_position (GTK_PANED (paned), size / 2);

  ptyxis_tab_sync_active_pane (self, created);
  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_N_PANES]);
  gtk_widget_grab_focus (GTK_WIDGET (created->terminal));
}

void
ptyxis_tab_split (PtyxisTab      *self,
                  GtkOrientation  orientation)
{
  ptyxis_tab_split_full (self, orientation, -1, NULL, NULL);
}

gboolean
ptyxis_tab_close_pane (PtyxisTab *self)
{
  g_return_val_if_fail (PTYXIS_IS_TAB (self), FALSE);

  if (self->panes == NULL || self->panes->len <= 1)
    return FALSE;

  if (self->active_pane == NULL)
    return FALSE;

  return ptyxis_tab_close_pane_widget (self, self->active_pane);
}

guint
ptyxis_tab_get_n_panes (PtyxisTab *self)
{
  g_return_val_if_fail (PTYXIS_IS_TAB (self), 0);

  if (self->panes == NULL)
    return 1;

  return self->panes->len;
}

/* Walk the paned widget tree from @pane's box up to its split source.
 *
 * The split tree mirrors how ptyxis_tab_split builds the widget tree:
 * when pane S is split into { S, N }, the new paned has S as start_child
 * and N as end_child. S becomes the start_child of a paned that was
 * just created around it, so to recover S's own split source we have to
 * climb up to S's enclosing paned's parent paned and take its
 * start_child. N is always the end_child, so N's split source is just
 * the start_child of its enclosing paned.
 *
 * Returns the index in self->panes of the pane that was split to
 * create @pane, or G_MAXUINT for the primary pane (or on layout
 * corruption).
 */
static guint
ptyxis_tab_find_split_parent_index (PtyxisTab     *self,
                                    PtyxisTabPane *pane)
{
  GtkWidget *current;
  guint index;

  g_assert (PTYXIS_IS_TAB (self));
  g_assert (pane != NULL);

  if (pane->is_primary)
    return G_MAXUINT;

  current = pane->box;

  for (;;)
    {
      GtkWidget *parent = gtk_widget_get_parent (current);
      GtkWidget *sibling;

      if (parent == NULL || PTYXIS_IS_TAB (parent))
        return G_MAXUINT;

      if (!GTK_IS_PANED (parent))
        return G_MAXUINT;

      if (gtk_paned_get_end_child (GTK_PANED (parent)) == current)
        {
          /* current is the NEW pane of this paned; its source is the
           * pane that owns the start_child.
           */
          sibling = gtk_paned_get_start_child (GTK_PANED (parent));
          for (index = 0; index < self->panes->len; index++)
            {
              PtyxisTabPane *other = g_ptr_array_index (self->panes, index);

              if (other != pane && other->box == sibling)
                return index;
            }

          return G_MAXUINT;
        }

      /* current is the SOURCE pane of this paned. Step out to the
       * enclosing paned and try again from there.
       */
      current = parent;
    }
}

/* Find the GtkPaned that was created when @pane was first split off.
 *
 * This is NOT necessarily the immediate parent of @pane->box. When a
 * pane is later split again, the source pane gets re-wrapped: the new
 * paned replaces the source's old paned-end-child slot, so the source
 * is now the start-child of a NEW paned (which is itself the end-child
 * of the OLD paned). The paned that was created FOR the source — and
 * that carries the orientation/position of the original split — is
 * therefore one level UP, at the top of the end-child chain.
 *
 * Concretely, for a 3-pane layout built by splitting pane[0] H, then
 * pane[0] V, then pane[1] H, the tree is:
 *
 *   paned_a (H, pos=400)  ← created when pane[0] was split (entry 0)
 *     start: pane[0]
 *     end:   paned_b (V, pos=250)  ← created when pane[1] was split (entry 1)
 *       start: pane[1]
 *       end:   paned_c (H, pos=300)  ← created when pane[2] was split (entry 2)
 *         start: pane[2]
 *         end:   pane[3]
 *
 * Here:
 *   pane[1] is start-child of paned_b. Its "owning" paned (the one
 *     with the saved H/400 values) is paned_a — the paned whose
 *     end-child is paned_b. Going up from pane[1].box → paned_b → ONE
 *     MORE STEP (parent of paned_b) → paned_a. We must stop there.
 *   pane[2] is start-child of paned_c. Its owning paned is paned_b —
 *     the paned whose end-child is paned_c. One step up from
 *     pane[2].box → paned_c → parent of paned_c → paned_b. Stop.
 *   pane[3] is end-child of paned_c. Its owning paned is paned_c
 *     itself — no walking needed.
 *
 * The rule is therefore:
 *   immediate P = gtk_widget_get_parent(pane->box)
 *   if P is null/not-a-paned: return NULL (corrupt)
 *   if pane->box is P's end-child:  return P
 *   else (pane->box is P's start-child): return gtk_widget_get_parent(P)
 *     — that is, the paned whose end-child is P.
 *
 * Returns: (transfer none) (nullable): the GtkPaned that owns the
 *   saved orientation/position for this pane, or NULL if @pane is the
 *   primary (direct child of the tab) or the tree is corrupt.
 */
static GtkPaned *
ptyxis_tab_find_paned_for_pane (PtyxisTabPane *pane)
{
  GtkWidget *parent;

  g_assert (pane != NULL);
  g_assert (pane->box != NULL);

  parent = gtk_widget_get_parent (pane->box);

  if (!GTK_IS_PANED (parent))
    return NULL;

  if (gtk_paned_get_end_child (GTK_PANED (parent)) == pane->box)
    {
      /* pane->box is end-child of parent — no re-wrapping has happened.
       * The "owning" paned is the immediate parent. */
      return GTK_PANED (parent);
    }

  /* pane->box is start-child of parent (re-wrapped). The owning paned
   * is the paned whose end-child is `parent`. That's one step up. */
  parent = gtk_widget_get_parent (parent);

  if (!GTK_IS_PANED (parent))
    return NULL;

  return GTK_PANED (parent);
}

/**
 * ptyxis_tab_dup_panes_state:
 * @self: a #PtyxisTab
 *
 * Serializes the split-pane layout into a `aa{sv}` GVariant for inclusion
 * in the saved session. The first pane (primary) is not emitted — it is
 * reconstructed from the tab-level fields already present in the saved
 * session.
 *
 * Returns: (transfer full): a new GVariant describing every non-primary
 *   pane in DFS order, or an empty array if there are no extra panes.
 */
GVariant *
ptyxis_tab_dup_panes_state (PtyxisTab *self)
{
  GVariantBuilder builder;
  g_autoptr(GVariant) ret = NULL;

  g_return_val_if_fail (PTYXIS_IS_TAB (self), NULL);

  g_variant_builder_init (&builder, G_VARIANT_TYPE ("aa{sv}"));

  /* Empty / single-pane case: skip building entirely rather than calling
   * g_variant_builder_end() on a builder with zero children. GLib permits
   * ending an indefinite-type builder with no children, but the resulting
   * GVariant's type-info node points into builder state that is about to
   * become stack garbage; if anything then reuses that info (e.g. the
   * outer `g_variant_builder_add("v", panes_state)` sink + the eventual
   * g_variant_get_data_as_bytes walk in ptyxis_application_save_session)
   * it can read freed memory. Returning NULL is the safe contract: the
   * caller skips the "panes" entry and serializes the tab as a single
   * pane, which is what we want for non-split tabs anyway. */
  if (self->panes == NULL || self->panes->len <= 1)
    {
      g_variant_builder_clear (&builder);
      return NULL;
    }

  for (guint i = 1; i < self->panes->len; i++)
    {
      PtyxisTabPane *pane = g_ptr_array_index (self->panes, i);
      GtkPaned *paned;
      guint parent_index;
      g_autofree char *cwd = NULL;
      const char *container_id = NULL;
      int position = -1;

      g_debug ("[spawn] save pane[%u] start: pane=%p is_primary=%d",
               i, (void*)pane, pane->is_primary);

      parent_index = ptyxis_tab_find_split_parent_index (self, pane);

      /* Sanity check: panes are always appended to self->panes in
       * creation order (via g_ptr_array_add at the end of
       * ptyxis_tab_create_pane_internal). The pane at index i was
       * therefore created AFTER pane[0..i-1], so its split source
       * must have an index strictly less than i. A parent_index >= i
       * indicates the widget tree is in an unexpected state — for
       * example, find_split_parent_index climbed into a paned whose
       * start_child was created later than pane[i] (impossible in a
       * well-formed tree, but can be observed if a previous split
       * used a stale source pointer). Log it as a debug message so we
       * can identify the bug, then fold it into G_MAXUINT so the
       * existing synthesizer below produces a sensible
       * (parent=0) entry instead of writing the bogus index to disk
       * for the restore side to clamp. */
      if (parent_index != G_MAXUINT && parent_index >= i)
        {
          g_debug ("Pane %u has parent_index=%u which is >= %u; "
                   "treating as detached",
                   i, parent_index, i);
          parent_index = G_MAXUINT;
        }

      /* Use the GtkPaned that was actually created when this pane was
       * split off (its "owning" paned), NOT the immediate parent of
       * pane->box. If the source was later split again, the source
       * gets re-wrapped: it moves from being the end-child of the
       * original paned to the start-child of a new paned, which
       * becomes the end-child of the original. So the immediate
       * parent of pane->box is the new paned (entry N+1), not the
       * one that holds this entry's saved orientation/position. */
      paned = ptyxis_tab_find_paned_for_pane (pane);
      if (paned == NULL)
        {
          g_warning ("Pane %u has no owning GtkPaned; skipping", i);
          continue;
        }

      /* If we couldn't recover a parent index for this pane (the
       * GtkPaned tree is detached or in some other unexpected state),
       * write the entry anyway but synthesize parent=0 so the restore
       * side can still reconstruct a valid tree. Writing G_MAXUINT
       * here is the actual cause of the cascade-of-warnings the user
       * saw: G_MAXUINT triggers "Saved pane is missing 'parent' or
       * parent is detached" at restore, which skips the entry; the
       * next entry's parent index then doesn't match the (now-shorter)
       * self->panes array and triggers "out of range". Falling back
       * to 0 means restore replays every pane as a child of the
       * primary, which may not be geometrically correct but at least
       * produces a structurally complete, non-empty tree — exactly the
       * behavior the user wants ("show all my panes") versus the
       * current "show nothing, panic".
       *
       * Demoted from g_warning to g_debug: this can fire during normal
       * shutdown if a pane's GtkPaned has been torn down (e.g. the
       * shell exited and close_pane_widget unwrapped the paned before
       * save ran), but the fallback produces a correct-enough tree on
       * restore, so the user-facing warning was noise. Re-enable with
       * G_MESSAGES_DEBUG=Ptyxis-Tab if a real bug needs investigation.
       */
      if (parent_index == G_MAXUINT)
        {
          g_debug ("Pane %u has detached parent in save walk; "
                   "synthesizing parent=0 to keep restore complete",
                   i);
          parent_index = 0;
        }

      position = gtk_paned_get_position (paned);
      if (position < 0)
        position = -1;

      cwd = ptyxis_terminal_dup_current_directory_uri (pane->terminal);
      if (ptyxis_str_empty0 (cwd))
        {
          g_clear_pointer (&cwd, g_free);
          if (!ptyxis_str_empty0 (pane->initial_working_directory_uri))
            cwd = g_strdup (pane->initial_working_directory_uri);
        }

      if (pane->container_at_creation != NULL)
        container_id = ptyxis_ipc_container_get_id (pane->container_at_creation);

      g_variant_builder_open (&builder, G_VARIANT_TYPE ("a{sv}"));
      g_variant_builder_add_parsed (&builder, "{'parent', <%u>}", parent_index);
      g_variant_builder_add_parsed (&builder,
                                    "{'orientation', <%u>}",
                                    gtk_orientable_get_orientation (GTK_ORIENTABLE (paned)));
      g_variant_builder_add_parsed (&builder, "{'position', <%i>}", position);
      if (cwd != NULL)
        g_variant_builder_add_parsed (&builder, "{'cwd', <%s>}", cwd);
      if (container_id != NULL)
        g_variant_builder_add_parsed (&builder, "{'container', <%s>}", container_id);
      g_variant_builder_close (&builder);

      g_debug ("[spawn] save pane[%u] EMITTED: parent=%u orient=%u pos=%d "
               "cwd=%s container=%s",
               i, parent_index,
               gtk_orientable_get_orientation (GTK_ORIENTABLE (paned)),
               position,
               cwd ? cwd : "(null)",
               container_id ? container_id : "(null)");
    }

  /* g_variant_builder_end() returns a floating reference. The outer
   * ptyxis_session_save will sink it via g_variant_builder_add("v", ...),
   * but we still return a floating reference to match the documented
   * (transfer full) contract — the caller sinks either way, but making
   * this explicit via g_variant_ref_sink avoids any window where the
   * builder's internal children[] could be observed before the sink
   * happens (which is the failure mode seen in the core dump taken on
   * the user host: g_variant_get_data_as_bytes walking children[].type
   * found a dangling pointer inside a child builder's storage). */
  return g_variant_ref_sink (g_variant_builder_end (&builder));
}

/**
 * ptyxis_tab_restore_panes_state:
 * @self: a #PtyxisTab
 * @state: a `aa{sv}` previously returned by ptyxis_tab_dup_panes_state()
 *
 * Replays a saved split-pane layout onto @self. Each entry in @state
 * describes a non-primary pane that was created by splitting off an
 * existing pane; this function calls ptyxis_tab_split_full() with the
 * saved orientation, divider position, working directory and container
 * so the resulting tree mirrors what the user had at save time.
 *
 * The order of entries matters: a pane's `parent` index must already be
 * a valid index in self->panes at the moment that entry is replayed.
 * ptyxis_tab_dup_panes_state() emits entries in DFS order so this
 * invariant holds.
 *
 * Returns: TRUE if at least one pane was restored, FALSE otherwise.
 */
gboolean
ptyxis_tab_restore_panes_state (PtyxisTab *self,
                                GVariant  *state)
{
  GVariantIter iter;
  GVariant *pane_var;
  guint restored = 0;
  PtyxisApplication *app;

  g_return_val_if_fail (PTYXIS_IS_TAB (self), FALSE);
  g_return_val_if_fail (state != NULL, FALSE);
  g_return_val_if_fail (g_variant_is_of_type (state, G_VARIANT_TYPE ("aa{sv}")), FALSE);

  if (g_variant_n_children (state) == 0)
    return FALSE;

  app = PTYXIS_APPLICATION_DEFAULT;

  g_debug ("[spawn] restore_panes_state ENTER tab=%p n_entries=%lu current_n_panes=%u",
           (void*)self,
           (unsigned long)g_variant_n_children (state),
           self->panes ? self->panes->len : 0);

  g_variant_iter_init (&iter, state);
  while (g_variant_iter_loop (&iter, "@a{sv}", &pane_var))
    {
      guint32 parent_index = G_MAXUINT;
      guint32 orientation = GTK_ORIENTATION_HORIZONTAL;
      gint32 position = -1;
      g_autofree char *cwd = NULL;
      g_autofree char *container_id = NULL;
      PtyxisTabPane *parent_pane;
      g_autoptr(PtyxisIpcContainer) container = NULL;

      /* g_variant_lookup returns FALSE when the key is absent but leaves
       * parent_index unchanged from its initial G_MAXUINT sentinel, so a
       * missing "parent" key would fall through into the "out of range"
       * branch and produce the misleading warning
       *   "Saved pane parent index 4294967295 out of range ..."
       * The duplicate ordering check (parent_index >= self->panes->len
       * covers both out-of-range and the G_MAXUINT sentinel) collapses
       * both cases into one branch and prints a single accurate warning.
       *
       * Why G_MAXUINT: ptyxis_tab_find_split_parent_index() returns
       * G_MAXUINT for the primary pane and for any non-primary pane
       * that lost its GtkPaned parent (e.g. mid-tear-down). The save
       * side skips the primary (loop starts at i=1) but a detached
       * non-primary could still slip through and write G_MAXUINT; the
       * restore side treats that as corrupt and skips it. */
      if (!g_variant_lookup (pane_var, "parent", "u", &parent_index) ||
          parent_index == G_MAXUINT)
        {
          if (parent_index == G_MAXUINT)
            g_warning ("Saved pane is missing 'parent' or parent is detached; falling back to parent=0");
          else
            g_warning ("Saved pane is missing 'parent' key; falling back to parent=0");
          parent_index = 0;
        }

      if (parent_index >= self->panes->len)
        {
          /* Earlier entries may have been dropped (G_MAXUINT or other
           * reconstruction failure), so this entry's recorded parent
           * index now points past the end. Clamp to the highest valid
           * index so we still create the pane instead of silently
           * dropping it. */
          g_warning ("Saved pane parent index %u out of range (have %u panes); clamping to %u",
                     parent_index, self->panes->len, self->panes->len - 1);
          parent_index = self->panes->len - 1;
        }

      g_variant_lookup (pane_var, "orientation", "u", &orientation);
      g_variant_lookup (pane_var, "position", "i", &position);
      /* Use format "s" (not "&s"). Per the GVariant format strings
       * reference, "s" tells GLib to allocate a fresh copy of the string
       * via g_strdup(), which is safe to pass to g_autoptr/free.
       * Format "&s" returns a pointer directly into the parent GVariant's
       * serialized data buffer — borrowed, NOT allocated — so freeing it
       * is undefined behaviour and glibc detects the resulting non-heap
       * pointer as "free(): invalid pointer" on the g_autoptr cleanup
       * when the loop iteration exits. (The original reverse-the-docs
       * confusion here is what produced the new abort on restore after
       * the previous session-save fix: the string was fine inside the
       * loop body, but the g_autoptr cleanup on scope-exit called g_free()
       * on a pointer the variant still owned.) */
      g_variant_lookup (pane_var, "cwd", "s", &cwd);
      g_variant_lookup (pane_var, "container", "s", &container_id);

      parent_pane = g_ptr_array_index (self->panes, parent_index);

      if (!ptyxis_str_empty0 (container_id))
        container = ptyxis_application_lookup_container (app, container_id);

      /* ptyxis_tab_split_full() reads self->active_pane to decide which
       * existing pane is the split source. Override it for each entry
       * so nested splits replay against the correct parent rather than
       * against whichever pane happened to be focused last. */
      self->active_pane = parent_pane;

      ptyxis_tab_split_full (self,
                             (GtkOrientation)orientation,
                             (int)position,
                             cwd,
                             container);

      g_debug ("[spawn] restore entry[%u] processed: parent=%u orient=%u pos=%d "
               "cwd=%s container=%s -> pane_index_after=%u",
               restored, parent_index, orientation, position,
               cwd ? cwd : "(null)",
               container_id ? container_id : "(null)",
               self->panes ? self->panes->len : 0);

      restored++;
    }

  return restored > 0;
}

static PtyxisTabPane *
ptyxis_tab_find_pane_in_direction (PtyxisTab         *self,
                                   PtyxisTabPane     *from,
                                   GtkDirectionType   direction)
{
  graphene_rect_t from_bounds;
  PtyxisTabPane *best = NULL;
  double best_score = G_MAXDOUBLE;

  g_assert (PTYXIS_IS_TAB (self));
  g_assert (from != NULL);

  if (!gtk_widget_compute_bounds (from->box, GTK_WIDGET (self), &from_bounds))
    return NULL;

  for (guint i = 0; i < self->panes->len; i++)
    {
      PtyxisTabPane *pane = g_ptr_array_index (self->panes, i);
      graphene_rect_t bounds;
      double score;
      double dx, dy;

      if (pane == from)
        continue;

      if (!gtk_widget_compute_bounds (pane->box, GTK_WIDGET (self), &bounds))
        continue;

      dx = (bounds.origin.x + bounds.size.width / 2.0) -
           (from_bounds.origin.x + from_bounds.size.width / 2.0);
      dy = (bounds.origin.y + bounds.size.height / 2.0) -
           (from_bounds.origin.y + from_bounds.size.height / 2.0);

      switch (direction)
        {
        case GTK_DIR_UP:
          if (dy >= -1.0)
            continue;
          score = -dy + fabs (dx) * 0.5;
          break;
        case GTK_DIR_DOWN:
          if (dy <= 1.0)
            continue;
          score = dy + fabs (dx) * 0.5;
          break;
        case GTK_DIR_LEFT:
          if (dx >= -1.0)
            continue;
          score = -dx + fabs (dy) * 0.5;
          break;
        case GTK_DIR_RIGHT:
          if (dx <= 1.0)
            continue;
          score = dx + fabs (dy) * 0.5;
          break;
        case GTK_DIR_TAB_FORWARD:
        case GTK_DIR_TAB_BACKWARD:
        default:
          continue;
        }

      if (score < best_score)
        {
          best_score = score;
          best = pane;
        }
    }

  return best;
}

void
ptyxis_tab_focus_pane (PtyxisTab         *self,
                       GtkDirectionType   direction)
{
  PtyxisTabPane *next;

  g_return_if_fail (PTYXIS_IS_TAB (self));

  if (self->panes == NULL || self->panes->len <= 1 || self->active_pane == NULL)
    return;

  next = ptyxis_tab_find_pane_in_direction (self, self->active_pane, direction);
  if (next == NULL)
    return;

  ptyxis_tab_sync_active_pane (self, next);
  gtk_widget_grab_focus (GTK_WIDGET (next->terminal));
}

void
ptyxis_tab_zoom_all (PtyxisTab       *self,
                     PtyxisZoomLevel  zoom)
{
  g_return_if_fail (PTYXIS_IS_TAB (self));

  ptyxis_tab_set_zoom (self, zoom);
}

PtyxisTerminal *
ptyxis_tab_get_terminal (PtyxisTab *self)
{
  g_return_val_if_fail (PTYXIS_IS_TAB (self), NULL);

  if (self->active_pane != NULL && self->active_pane->terminal != NULL)
    return self->active_pane->terminal;

  return self->terminal;
}

/**
 * ptyxis_tab_get_primary_terminal:
 * @self: a #PtyxisTab
 *
 * Returns the template-level PtyxisTerminal widget that is the
 * primary pane's terminal. This is the widget that survives the
 * tab's full lifetime, including across split/close/re-split
 * cycles, and is the same PtyxisTerminal that
 * PtyxisTab::ptyxis_tab_respawn() will use as the target of the
 * next spawn.
 *
 * Contrast with ptyxis_tab_get_terminal() which returns the active
 * pane's terminal — useful for "the terminal I am currently typing
 * into" but wrong for code paths that need to read the primary's
 * own state (e.g. the session-save walker: a tab with multi-pane
 * layout has self->active_pane pointing at whichever pane the user
 * last focused, but the cwd we save and later feed back to the
 * primary at respawn time must be the primary pane's own cwd,
 * otherwise the primary restores into whatever directory some
 * non-primary pane happened to be in at quit time).
 *
 * Returns: (transfer none) (nullable): the primary terminal.
 */
PtyxisTerminal *
ptyxis_tab_get_primary_terminal (PtyxisTab *self)
{
  g_return_val_if_fail (PTYXIS_IS_TAB (self), NULL);

  return self->terminal;
}
