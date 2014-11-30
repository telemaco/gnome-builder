/* gb-editor-workspace.c
 *
 * Copyright (C) 2014 Christian Hergert <christian@hergert.me>
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
 */

#include <glib/gi18n.h>

#include "gb-editor-workspace.h"
#include "gb-editor-workspace-private.h"
#include "gb-tab-grid.h"
#include "gb-tree.h"

enum {
  PROP_0,
  LAST_PROP
};

G_DEFINE_TYPE_WITH_PRIVATE (GbEditorWorkspace, gb_editor_workspace, GB_TYPE_WORKSPACE)

void
gb_editor_workspace_open (GbEditorWorkspace *workspace,
                          GFile             *file)
{
  GbEditorTab *tab;

  g_return_if_fail (GB_IS_EDITOR_WORKSPACE (workspace));
  g_return_if_fail (G_IS_FILE (file));

  tab = g_object_new (GB_TYPE_EDITOR_TAB,
                      "visible", TRUE,
                      NULL);
  gtk_container_add (GTK_CONTAINER (workspace->priv->tab_grid),
                     GTK_WIDGET (tab));
  gb_tab_grid_focus_tab (workspace->priv->tab_grid, GB_TAB (tab));

  gb_editor_tab_open_file (tab, file);
}

static void
save_tab (GSimpleAction *action,
          GVariant      *parameter,
          gpointer       user_data)
{
  GbEditorWorkspace *workspace = user_data;
  GbTab *tab;

  tab = gb_tab_grid_get_active (workspace->priv->tab_grid);
  if (GB_IS_EDITOR_TAB (tab))
    gb_editor_tab_save (GB_EDITOR_TAB (tab));
}

static void
save_as_tab (GSimpleAction *action,
             GVariant      *parameter,
             gpointer       user_data)
{
  GbEditorWorkspace *workspace = user_data;
  GbTab *tab;

  tab = gb_tab_grid_get_active (workspace->priv->tab_grid);
  if (GB_IS_EDITOR_TAB (tab))
    gb_editor_tab_save_as (GB_EDITOR_TAB (tab));
}

static void
scroll_up_tab (GSimpleAction *action,
               GVariant      *parameter,
               gpointer       user_data)
{
  GbEditorWorkspace *workspace = user_data;
  GbTab *tab;

  tab = gb_tab_grid_get_active (workspace->priv->tab_grid);
  if (GB_IS_EDITOR_TAB (tab))
    gb_editor_tab_scroll_up (GB_EDITOR_TAB (tab));
}

static void
scroll_down_tab (GSimpleAction *action,
                 GVariant      *parameter,
                 gpointer       user_data)
{
  GbEditorWorkspace *workspace = user_data;
  GbTab *tab;

  tab = gb_tab_grid_get_active (workspace->priv->tab_grid);
  if (GB_IS_EDITOR_TAB (tab))
    gb_editor_tab_scroll_down (GB_EDITOR_TAB (tab));
}

static void
new_tab (GSimpleAction *action,
         GVariant      *parameter,
         gpointer       user_data)
{
  GbEditorWorkspace *workspace = user_data;
  GbEditorTab *tab;

  tab = g_object_new (GB_TYPE_EDITOR_TAB,
                      "visible", TRUE,
                      NULL);
  gtk_container_add (GTK_CONTAINER (workspace->priv->tab_grid),
                     GTK_WIDGET (tab));
  gtk_widget_grab_focus (GTK_WIDGET (tab));
}

static GActionGroup *
gb_editor_workspace_get_actions (GbWorkspace *workspace)
{
  return G_ACTION_GROUP (GB_EDITOR_WORKSPACE (workspace)->priv->actions);
}

static void
gb_editor_workspace_grab_focus (GtkWidget *widget)
{
  GbEditorWorkspace *workspace = GB_EDITOR_WORKSPACE (widget);

  g_return_if_fail (GB_IS_EDITOR_WORKSPACE (workspace));

  gtk_widget_grab_focus (GTK_WIDGET (workspace->priv->tab_grid));
}

static void
gb_editor_workspace_finalize (GObject *object)
{
  GbEditorWorkspacePrivate *priv = GB_EDITOR_WORKSPACE (object)->priv;

  g_clear_object (&priv->actions);
  g_clear_pointer (&priv->command_map, g_hash_table_unref);

  G_OBJECT_CLASS (gb_editor_workspace_parent_class)->finalize (object);
}

static void
gb_editor_workspace_class_init (GbEditorWorkspaceClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GbWorkspaceClass *workspace_class = GB_WORKSPACE_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->finalize = gb_editor_workspace_finalize;

  workspace_class->get_actions = gb_editor_workspace_get_actions;

  widget_class->grab_focus = gb_editor_workspace_grab_focus;

  gtk_widget_class_set_template_from_resource (widget_class,
                                               "/org/gnome/builder/ui/gb-editor-workspace.ui");
  gtk_widget_class_bind_template_child_private (widget_class, GbEditorWorkspace, paned);
  gtk_widget_class_bind_template_child_private (widget_class, GbEditorWorkspace, tab_grid);

  g_type_ensure (GB_TYPE_EDITOR_TAB);
  g_type_ensure (GB_TYPE_TAB_GRID);
  g_type_ensure (GB_TYPE_TREE);
}

static void
gb_editor_workspace_init (GbEditorWorkspace *workspace)
{
    const GActionEntry entries[] = {
      { "new-tab", new_tab },
      { "save", save_tab },
      { "save-as", save_as_tab },
      { "scroll-up", scroll_up_tab },
      { "scroll-down", scroll_down_tab },
    };

  workspace->priv = gb_editor_workspace_get_instance_private (workspace);

  workspace->priv->actions = g_simple_action_group_new ();
  g_action_map_add_action_entries (G_ACTION_MAP (workspace->priv->actions),
                                   entries, G_N_ELEMENTS (entries),
                                   workspace);

  workspace->priv->command_map = g_hash_table_new (g_str_hash, g_str_equal);

  gtk_widget_init_template (GTK_WIDGET (workspace));

#if 0
  gb_editor_commands_init (workspace);
#endif
}
