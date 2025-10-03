/*
 * ptyxis-terminal-intent.c
 *
 * Copyright 2025 Christian Hergert <chergert@redhat.com>
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

#include "ptyxis-agent-ipc.h"

#include "ptyxis-application.h"
#include "ptyxis-profile.h"
#include "ptyxis-tab.h"
#include "ptyxis-terminal-intent.h"
#include "ptyxis-window.h"

struct _PtyxisTerminalIntent
{
  XdgTerminal1Skeleton parent_instance;
};

static void default_init_terminal1 (XdgTerminal1Iface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (PtyxisTerminalIntent, ptyxis_terminal_intent, XDG_TYPE_TERMINAL1_SKELETON,
                               G_IMPLEMENT_INTERFACE (XDG_TYPE_TERMINAL1, default_init_terminal1))

static void
ptyxis_terminal_intent_class_init (PtyxisTerminalIntentClass *klass)
{
}

static void
ptyxis_terminal_intent_init (PtyxisTerminalIntent *self)
{
}

static gboolean
ptyxis_terminal_intent_handle_launch_command (XdgTerminal1          *intent,
                                              GDBusMethodInvocation *invocation,
                                              const char * const    *argv,
                                              const char            *working_directory,
                                              const char            *desktop_entry_path,
                                              const char * const    *env,
                                              GVariant              *options,
                                              GVariant              *platform_data)
{
  g_autoptr(PtyxisIpcContainer) container = NULL;
  g_autoptr(PtyxisProfile) profile = NULL;
  g_autoptr(GStrvBuilder) builder = NULL;
  g_autoptr(GKeyFile) key_file = NULL;
  g_autoptr(GFile) file = NULL;
  g_autofree char *cwd_uri = NULL;
  g_auto(GStrv) real_argv = NULL;
  PtyxisWindow *window;
  PtyxisTab *tab;
  gboolean keep_terminal_open;

  g_assert (PTYXIS_IS_TERMINAL_INTENT (intent));
  g_assert (G_IS_DBUS_METHOD_INVOCATION (invocation));
  g_assert (argv != NULL);
  g_assert (working_directory != NULL);
  g_assert (desktop_entry_path != NULL);
  g_assert (options != NULL);
  g_assert (g_variant_is_of_type (options, G_VARIANT_TYPE_VARDICT));
  g_assert (platform_data != NULL);
  g_assert (g_variant_is_of_type (platform_data, G_VARIANT_TYPE_VARDICT));

  if (argv[0] == NULL)
    {
      window = ptyxis_window_new ();
      gtk_window_present (GTK_WINDOW (window));
      xdg_terminal1_complete_launch_command (intent, g_steal_pointer (&invocation));
      return TRUE;
    }

  profile = ptyxis_application_dup_default_profile (PTYXIS_APPLICATION_DEFAULT);
  container = ptyxis_application_lookup_container (PTYXIS_APPLICATION_DEFAULT, "session");

  if (working_directory[0] == '/')
    file = g_file_new_for_path (working_directory);
  else if (working_directory[0] != 0)
    file = g_file_new_build_filename (g_get_home_dir (), working_directory, NULL);

  if (file != NULL)
    cwd_uri = g_file_get_uri (file);

  if (container == NULL)
    {
      g_dbus_method_invocation_return_gerror (g_steal_pointer (&invocation),
                                              g_error_new_literal (G_DBUS_ERROR,
                                                                   G_DBUS_ERROR_INVALID_ARGS,
                                                                   "Failed to locate session container"));
      return TRUE;
    }

  for (gsize i = 0; env[i]; i++)
    {
      if (strchr (env[i], '=') == NULL)
        {
          g_dbus_method_invocation_return_gerror (g_steal_pointer (&invocation),
                                                  g_error_new_literal (G_DBUS_ERROR,
                                                                       G_DBUS_ERROR_INVALID_ARGS,
                                                                       "environ pair missing `=`"));
          return FALSE;
        }
    }

  builder = g_strv_builder_new ();

  if (env[0] != NULL)
    {
      g_strv_builder_add (builder, "env");
      g_strv_builder_addv (builder, (const char **)env);
    }

  g_strv_builder_addv (builder, (const char **)argv);
  real_argv = g_strv_builder_end (builder);

  window = ptyxis_window_new_empty ();
  tab = ptyxis_window_add_tab_for_command (window,
                                           profile,
                                           (const char * const *)real_argv,
                                           cwd_uri);

  if (g_variant_lookup (options, "keep-terminal-open", "b", &keep_terminal_open) && keep_terminal_open)
    ptyxis_tab_set_keep_terminal_open (tab, TRUE);

  if (cwd_uri != NULL)
    ptyxis_tab_set_initial_working_directory_uri (tab, cwd_uri);

  ptyxis_window_set_active_tab (window, tab);

  gtk_window_present (GTK_WINDOW (window));

  xdg_terminal1_complete_launch_command (intent, g_steal_pointer (&invocation));

  return TRUE;
}

static void
default_init_terminal1 (XdgTerminal1Iface *iface)
{
  iface->handle_launch_command = ptyxis_terminal_intent_handle_launch_command;
}

PtyxisTerminalIntent *
ptyxis_terminal_intent_new (void)
{
  return g_object_new (PTYXIS_TYPE_TERMINAL_INTENT, NULL);
}
