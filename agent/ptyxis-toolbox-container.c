/* ptyxis-toolbox-container.c
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

#include "ptyxis-run-context.h"
#include "ptyxis-toolbox-container.h"

struct _PtyxisToolboxContainer
{
  PtyxisPodmanContainer parent_instance;
};

G_DEFINE_TYPE (PtyxisToolboxContainer, ptyxis_toolbox_container, PTYXIS_TYPE_PODMAN_CONTAINER)

static gboolean
ptyxis_toolbox_container_run_context_cb (PtyxisRunContext    *run_context,
                                         const char * const  *argv,
                                         const char * const  *env,
                                         const char          *cwd,
                                         PtyxisUnixFDMap     *unix_fd_map,
                                         gpointer             user_data,
                                         GError             **error)
{
  PtyxisToolboxContainer *self = user_data;
  const char *name;
  int max_dest_fd;

  g_assert (PTYXIS_IS_TOOLBOX_CONTAINER (self));
  g_assert (PTYXIS_IS_RUN_CONTEXT (run_context));
  g_assert (argv != NULL);
  g_assert (env != NULL);
  g_assert (PTYXIS_IS_UNIX_FD_MAP (unix_fd_map));

  name = ptyxis_ipc_container_get_display_name (PTYXIS_IPC_CONTAINER (self));

  /* Make sure we can pass the FDs down */
  if (!ptyxis_run_context_merge_unix_fd_map (run_context, unix_fd_map, error))
    return FALSE;

  /* Use toolbox run instead of podman exec so that Toolbx can handle
   * container lifecycle, CA certificates, NVIDIA driver support, and
   * future automatic container stopping.
   */
  ptyxis_run_context_append_argv (run_context, "toolbox");
  ptyxis_run_context_append_argv (run_context, "run");

  ptyxis_run_context_append_formatted (run_context, "--container=%s", name);

  /* Pass environment variables via --env */
  for (guint i = 0; env[i]; i++)
    ptyxis_run_context_append_formatted (run_context, "--env=%s", env[i]);

  /* From podman-exec(1):
   *
   * Pass down to the process N additional file descriptors (in addition to
   * 0, 1, 2).  The total FDs will be 3+N.
   */
  if ((max_dest_fd = ptyxis_unix_fd_map_get_max_dest_fd (unix_fd_map)) > 2)
    ptyxis_run_context_append_formatted (run_context, "--preserve-fds=%d", max_dest_fd-2);

  /* Enable fallback to /bin/bash and /bin/sh if the requested shell
   * is not available inside the container.
   */
  ptyxis_run_context_append_argv (run_context, "--use-fallback-shell");

  /* Set the working directory inside the container */
  if (cwd != NULL)
    ptyxis_run_context_append_formatted (run_context, "--workdir=%s", cwd);

  ptyxis_run_context_append_argv (run_context, "--");

  /* Finally, propagate the upper layer's command arguments */
  ptyxis_run_context_append_args (run_context, argv);

  return TRUE;
}

static void
ptyxis_toolbox_container_prepare_run_context (PtyxisPodmanContainer *container,
                                              PtyxisRunContext      *run_context)
{
  g_assert (PTYXIS_IS_TOOLBOX_CONTAINER (container));
  g_assert (PTYXIS_IS_RUN_CONTEXT (run_context));

  /* In case we got sandboxed due to incompatible host */
  ptyxis_run_context_push_host (run_context);

  ptyxis_run_context_push (run_context,
                           ptyxis_toolbox_container_run_context_cb,
                           g_object_ref (container),
                           g_object_unref);

  /* Give access to some minimal state in the environment from
   * our host system.
   */
  ptyxis_run_context_add_minimal_environment (run_context);

  /* We don't want HOME propagated because toolbox will set it
   * up for us inside the container.
   */
  ptyxis_run_context_setenv (run_context, "HOME", NULL);
}

static void
ptyxis_toolbox_container_class_init (PtyxisToolboxContainerClass *klass)
{
  PtyxisPodmanContainerClass *podman_container_class = PTYXIS_PODMAN_CONTAINER_CLASS (klass);

  podman_container_class->prepare_run_context = ptyxis_toolbox_container_prepare_run_context;
}

static void
ptyxis_toolbox_container_init (PtyxisToolboxContainer *self)
{
  ptyxis_ipc_container_set_icon_name (PTYXIS_IPC_CONTAINER (self), "container-toolbox-symbolic");
  ptyxis_ipc_container_set_provider (PTYXIS_IPC_CONTAINER (self), "toolbox");
}
