/*
 * ptyxis-profile-menu.c
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

#include "ptyxis-profile.h"
#include "ptyxis-profile-menu.h"

struct _PtyxisProfileMenu
{
  GMenuModel       parent_instance;
  PtyxisSettings  *settings;
  char            **uuids;
};

enum {
  PROP_0,
  PROP_SETTINGS,
  N_PROPS
};

G_DEFINE_FINAL_TYPE (PtyxisProfileMenu, ptyxis_profile_menu, G_TYPE_MENU_MODEL)

static GParamSpec *properties [N_PROPS];

static int
ptyxis_profile_menu_get_n_items (GMenuModel *model)
{
  PtyxisProfileMenu *self = PTYXIS_PROFILE_MENU (model);
  guint n_items = self->uuids ? g_strv_length (self->uuids) : 0;

  /* Ignore profiles when only 1 item (session) is available */
  if (n_items <= 1)
    return 0;

  return n_items;
}

static gboolean
ptyxis_profile_menu_is_mutable (GMenuModel *model)
{
  return TRUE;
}

static GMenuModel *
ptyxis_profile_menu_get_item_link (GMenuModel *model,
                                   int         position,
                                   const char *link)
{
  return NULL;
}

static void
ptyxis_profile_menu_get_item_links (GMenuModel  *model,
                                    int          position,
                                    GHashTable **links)
{
  *links = NULL;
}

static void
ptyxis_profile_menu_get_item_attributes (GMenuModel  *model,
                                         int          position,
                                         GHashTable **attributes)
{
  PtyxisProfileMenu *self = PTYXIS_PROFILE_MENU (model);
  g_autoptr(PtyxisProfile) profile = NULL;
  g_autofree char *label = NULL;
  const char *uuid;
  GHashTable *ht;

  *attributes = NULL;

  if (position < 0 ||
      self->uuids == NULL ||
      position >= g_strv_length (self->uuids))
    return;

  uuid = self->uuids[position];
  profile = ptyxis_profile_new (uuid);
  label = g_strdelimit (ptyxis_profile_dup_label (profile), "_", ' ');

  ht = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_variant_unref);
  g_hash_table_insert (ht, g_strdup (G_MENU_ATTRIBUTE_ACTION), g_variant_ref_sink (g_variant_new_string ("win.new-terminal")));
  g_hash_table_insert (ht, g_strdup (G_MENU_ATTRIBUTE_TARGET), g_variant_ref_sink (g_variant_new ("(ss)", uuid, "")));
  g_hash_table_insert (ht, g_strdup (G_MENU_ATTRIBUTE_LABEL), g_variant_ref_sink (g_variant_new_string (label)));

  *attributes = ht;
}

static void
ptyxis_profile_menu_notify_profile_uuids_cb (PtyxisProfileMenu *self,
                                             GParamSpec        *pspec,
                                             PtyxisSettings    *settings)
{
  char **profile_uuids;
  guint old_len;
  guint new_len;

  g_assert (PTYXIS_IS_PROFILE_MENU (self));
  g_assert (PTYXIS_IS_SETTINGS (settings));

  profile_uuids = ptyxis_settings_dup_profile_uuids (settings);

  old_len = ptyxis_profile_menu_get_n_items (G_MENU_MODEL (self));
  g_clear_pointer (&self->uuids, g_strfreev);
  self->uuids = profile_uuids;
  new_len = ptyxis_profile_menu_get_n_items (G_MENU_MODEL (self));

  g_menu_model_items_changed (G_MENU_MODEL (self), 0, old_len, new_len);
}

static void
ptyxis_profile_menu_constructed (GObject *object)
{
  PtyxisProfileMenu *self = (PtyxisProfileMenu *)object;

  G_OBJECT_CLASS (ptyxis_profile_menu_parent_class)->constructed (object);

  self->uuids = ptyxis_settings_dup_profile_uuids (self->settings);

  g_signal_connect_object (self->settings,
                           "notify::profile-uuids",
                           G_CALLBACK (ptyxis_profile_menu_notify_profile_uuids_cb),
                           self,
                           G_CONNECT_SWAPPED);
}

static void
ptyxis_profile_menu_dispose (GObject *object)
{
  PtyxisProfileMenu *self = (PtyxisProfileMenu *)object;

  g_clear_object (&self->settings);
  g_clear_pointer (&self->uuids, g_strfreev);

  G_OBJECT_CLASS (ptyxis_profile_menu_parent_class)->dispose (object);
}

static void
ptyxis_profile_menu_get_property (GObject    *object,
                                   guint       prop_id,
                                   GValue     *value,
                                   GParamSpec *pspec)
{
  PtyxisProfileMenu *self = PTYXIS_PROFILE_MENU (object);

  switch (prop_id)
    {
    case PROP_SETTINGS:
      g_value_set_object (value, self->settings);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
ptyxis_profile_menu_set_property (GObject      *object,
                                   guint         prop_id,
                                   const GValue *value,
                                   GParamSpec   *pspec)
{
  PtyxisProfileMenu *self = PTYXIS_PROFILE_MENU (object);

  switch (prop_id)
    {
    case PROP_SETTINGS:
      self->settings = g_value_dup_object (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
ptyxis_profile_menu_class_init (PtyxisProfileMenuClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GMenuModelClass *menu_model_class = G_MENU_MODEL_CLASS (klass);

  object_class->constructed = ptyxis_profile_menu_constructed;
  object_class->dispose = ptyxis_profile_menu_dispose;
  object_class->get_property = ptyxis_profile_menu_get_property;
  object_class->set_property = ptyxis_profile_menu_set_property;

  menu_model_class->get_n_items = ptyxis_profile_menu_get_n_items;
  menu_model_class->is_mutable = ptyxis_profile_menu_is_mutable;
  menu_model_class->get_item_link = ptyxis_profile_menu_get_item_link;
  menu_model_class->get_item_links = ptyxis_profile_menu_get_item_links;
  menu_model_class->get_item_attributes = ptyxis_profile_menu_get_item_attributes;

  properties[PROP_SETTINGS] =
    g_param_spec_object ("settings", NULL, NULL,
                         PTYXIS_TYPE_SETTINGS,
                         (G_PARAM_READWRITE |
                          G_PARAM_CONSTRUCT_ONLY |
                          G_PARAM_STATIC_STRINGS));

  g_object_class_install_properties (object_class, N_PROPS, properties);
}

static void
ptyxis_profile_menu_init (PtyxisProfileMenu *self)
{
}

PtyxisProfileMenu *
ptyxis_profile_menu_new (PtyxisSettings *settings)
{
  g_return_val_if_fail (PTYXIS_IS_SETTINGS (settings), NULL);

  return g_object_new (PTYXIS_TYPE_PROFILE_MENU,
                       "settings", settings,
                       NULL);
}

void
ptyxis_profile_menu_invalidate (PtyxisProfileMenu *self)
{
  g_return_if_fail (PTYXIS_IS_PROFILE_MENU (self));

  g_menu_model_items_changed (G_MENU_MODEL (self),
                              0,
                              g_menu_model_get_n_items (G_MENU_MODEL (self)),
                              g_menu_model_get_n_items (G_MENU_MODEL (self)));
}
