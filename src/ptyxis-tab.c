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

  GTK_WIDGET_CLASS (ptyxis_tab_parent_class)->map (widget);

  if (self->state == PTYXIS_TAB_STATE_INITIAL)
    ptyxis_tab_respawn (self);
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

  vte_terminal_get_color_background_for_draw (VTE_TERMINAL (self->terminal), &bg);

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

  ptyxis_tab_notify_destroy (&self->notify);

  ptyxis_tab_force_quit (self);

  self->active_pane = NULL;
  g_clear_pointer (&self->panes, g_ptr_array_unref);

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

  gtk_widget_init_template (GTK_WIDGET (self));

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
ptyxis_tab_poll_agent (PtyxisTab *self)
{
  Wait wait;

  g_return_val_if_fail (PTYXIS_IS_TAB (self), FALSE);

  wait.context = g_main_context_get_thread_default ();
  wait.completed = FALSE;
  wait.success = FALSE;

  ptyxis_tab_poll_agent_async (self,
                               NULL,
                               ptyxis_tab_poll_agent_sync_cb,
                               &wait);

  while (!wait.completed)
    g_main_context_iteration (wait.context, TRUE);

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
   * a short timer to send SIGKILL afterwards.
   */
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

  ptyxis_ipc_process_call_has_foreground_process_finish (process,
                                                         &has_foreground_process,
                                                         &the_pid,
                                                         &the_cmdline,
                                                         &the_leader_kind,
                                                         NULL,
                                                         result,
                                                         NULL);

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

void
ptyxis_tab_grab_focus (PtyxisTab *self)
{
  g_return_if_fail (PTYXIS_IS_TAB (self));

  if (self->active_pane != NULL && self->active_pane->terminal != NULL)
    gtk_widget_grab_focus (GTK_WIDGET (self->active_pane->terminal));
  else if (self->terminal != NULL)
    gtk_widget_grab_focus (GTK_WIDGET (self->terminal));
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
   */
  if (pane->state != PTYXIS_TAB_STATE_INITIAL &&
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

static void
ptyxis_tab_pane_focus_enter_cb (PtyxisTabPane            *pane,
                                GParamSpec               *pspec,
                                GtkEventControllerFocus  *focus)
{
  g_assert (pane != NULL);
  g_assert (pane->tab != NULL);

  if (gtk_event_controller_focus_contains_focus (focus))
    {
      ptyxis_tab_sync_active_pane (pane->tab, pane);
      ptyxis_tab_set_needs_attention (pane->tab, FALSE);
      g_application_withdraw_notification (G_APPLICATION (PTYXIS_APPLICATION_DEFAULT),
                                           pane->tab->uuid);
    }
}

static void
ptyxis_tab_pane_wait_cb (GObject      *object,
                         GAsyncResult *result,
                         gpointer      user_data)
{
  PtyxisApplication *app = (PtyxisApplication *)object;
  g_autoptr(PtyxisTab) tab = NULL;
  PtyxisTabPane *pane = user_data;
  g_autoptr(GError) error = NULL;
  int exit_code;

  g_assert (PTYXIS_IS_APPLICATION (app));
  g_assert (pane != NULL);

  tab = g_object_ref (pane->tab);
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
  g_autoptr(PtyxisIpcProcess) process = NULL;
  g_autoptr(PtyxisTab) tab = NULL;
  PtyxisTabPane *pane = user_data;
  g_autoptr(GError) error = NULL;

  g_assert (pane != NULL);
  g_assert (pane->state == PTYXIS_TAB_STATE_SPAWNING);

  tab = g_object_ref (pane->tab);

  if (g_cancellable_is_cancelled (pane->cancellable))
    return;

  if (!(process = ptyxis_application_spawn_finish (app, result, &error)))
    {
      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        return;

      pane->state = PTYXIS_TAB_STATE_FAILED;
      vte_terminal_feed (VTE_TERMINAL (pane->terminal), error->message, -1);
      vte_terminal_feed (VTE_TERMINAL (pane->terminal), "\r\n", -1);
      adw_banner_set_title (pane->banner, _("Failed to launch terminal"));
      gtk_widget_set_visible (GTK_WIDGET (pane->banner), TRUE);
      return;
    }

  pane->state = PTYXIS_TAB_STATE_RUNNING;
  pane->respawn_time = g_get_monotonic_time ();
  g_set_object (&pane->process, process);

  if (tab->active_pane == pane)
    ptyxis_tab_sync_active_pane (tab, pane);

  ptyxis_application_wait_async (app,
                                 process,
                                 pane->cancellable,
                                 ptyxis_tab_pane_wait_cb,
                                 pane);
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

  g_assert (pane != NULL);
  g_assert (pane->tab != NULL);

  self = pane->tab;

  if (pane->is_primary)
    {
      ptyxis_tab_respawn (self);
      return;
    }

  gtk_widget_set_visible (GTK_WIDGET (pane->banner), FALSE);

  default_container = ptyxis_profile_dup_default_container (self->profile);

  if (pane->container_at_creation != NULL)
    container = g_object_ref (pane->container_at_creation);
  else if (self->container_at_creation != NULL)
    container = g_object_ref (self->container_at_creation);
  else
    container = ptyxis_application_lookup_container (PTYXIS_APPLICATION_DEFAULT,
                                                     default_container);

  if (container == NULL)
    {
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

      new_pty = ptyxis_application_create_pty (PTYXIS_APPLICATION_DEFAULT, &error);
      if (new_pty == NULL)
        {
          pane->state = PTYXIS_TAB_STATE_FAILED;
          adw_banner_set_title (pane->banner, _("Failed to create pseudo terminal device"));
          gtk_widget_set_visible (GTK_WIDGET (pane->banner), TRUE);
          return;
        }

      vte_terminal_set_pty (VTE_TERMINAL (pane->terminal), new_pty);
      pty = new_pty;
    }

  cwd_uri = pane->previous_working_directory_uri;
  if (pane->initial_working_directory_uri)
    cwd_uri = pane->initial_working_directory_uri;
  else if (cwd_uri == NULL)
    cwd_uri = self->previous_working_directory_uri;

  ptyxis_application_spawn_async (PTYXIS_APPLICATION_DEFAULT,
                                  container,
                                  self->profile,
                                  cwd_uri,
                                  pty,
                                  (const char * const *)pane->command,
                                  pane->cancellable,
                                  ptyxis_tab_pane_spawn_cb,
                                  pane);
}

static gboolean
ptyxis_tab_close_pane_widget (PtyxisTab     *self,
                              PtyxisTabPane *pane)
{
  GtkWidget *box;
  GtkWidget *parent;
  GtkWidget *sibling = NULL;
  GtkWidget *grandparent;
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

  if (self->active_pane == pane)
    {
      PtyxisTabPane *next = NULL;

      for (guint i = 0; i < self->panes->len; i++)
        {
          PtyxisTabPane *other = g_ptr_array_index (self->panes, i);

          if (other != pane)
            {
              next = other;
              break;
            }
        }

      if (next != NULL)
        {
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
ptyxis_tab_create_pane (PtyxisTab *self)
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

  /* Inherit working directory / container from the source pane. */
  if (source->is_primary)
    {
      cwd_uri = ptyxis_tab_dup_current_directory_uri (self);
      if (self->container_at_creation != NULL)
        pane->container_at_creation = g_object_ref (self->container_at_creation);
    }
  else
    {
      cwd_uri = ptyxis_terminal_dup_current_directory_uri (source->terminal);
      if (source->container_at_creation != NULL)
        pane->container_at_creation = g_object_ref (source->container_at_creation);
    }

  if (cwd_uri != NULL)
    pane->initial_working_directory_uri = g_steal_pointer (&cwd_uri);

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
  ptyxis_tab_pane_respawn (pane);

  return pane;
}

void
ptyxis_tab_split (PtyxisTab      *self,
                  GtkOrientation  orientation)
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

  created = ptyxis_tab_create_pane (self);

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

  if (size > 0)
    gtk_paned_set_position (GTK_PANED (paned), size / 2);

  ptyxis_tab_sync_active_pane (self, created);
  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_N_PANES]);
  gtk_widget_grab_focus (GTK_WIDGET (created->terminal));
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
