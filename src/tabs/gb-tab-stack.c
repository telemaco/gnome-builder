/* gb-tab-stack.c
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

#include "gb-tab-stack.h"

struct _GbTabStackPrivate
{
  GtkButton    *close;
  GtkComboBox  *combo;
  GtkStack     *controls;
  GtkStack     *stack;
  GtkListStore *store;
};

enum
{
  PROP_0,
  LAST_PROP
};

G_DEFINE_TYPE_WITH_PRIVATE (GbTabStack, gb_tab_stack, GTK_TYPE_BOX)

#if 0
static GParamSpec *gParamSpecs [LAST_PROP];
#endif

GtkWidget *
gb_tab_stack_new (void)
{
  return g_object_new (GB_TYPE_TAB_STACK, NULL);
}

guint
gb_tab_stack_get_n_tabs (GbTabStack *stack)
{
  GbTabStackPrivate *priv;
  GList *children;
  guint n_tabs;

  g_return_val_if_fail (GB_IS_TAB_STACK (stack), 0);

  priv = stack->priv;

  children = gtk_container_get_children (GTK_CONTAINER (priv->stack));
  n_tabs = g_list_length (children);
  g_list_free (children);

  return n_tabs;
}

/**
 * gb_tab_stack_get_tabs:
 * @stack: (in): A #GbTabStack.
 *
 * Returns all of the tabs within the stack.
 *
 * Returns: (transfer container) (element-type GbTab*): A #GList of #GbTab.
 */
GList *
gb_tab_stack_get_tabs (GbTabStack *stack)
{
  g_return_val_if_fail (GB_IS_TAB_STACK (stack), NULL);

  return gtk_container_get_children (GTK_CONTAINER (stack->priv->stack));
}

static gboolean
gb_tab_stack_get_tab_iter (GbTabStack  *stack,
                           GbTab       *tab,
                           GtkTreeIter *iter)
{
  GtkTreeModel *model;
  gint position = -1;

  g_return_val_if_fail (GB_IS_TAB_STACK (stack), FALSE);
  g_return_val_if_fail (iter, FALSE);

  gtk_container_child_get (GTK_CONTAINER (stack->priv->stack), GTK_WIDGET (tab),
                           "position", &position,
                           NULL);

  if (position != -1)
    {
      model = GTK_TREE_MODEL (stack->priv->store);

      if (gtk_tree_model_get_iter_first (model, iter))
        {
          for (; position; position--)
            {
              if (!gtk_tree_model_iter_next (model, iter))
                return FALSE;
            }
          return TRUE;
        }
    }

  return FALSE;
}

void
gb_tab_stack_remove_tab (GbTabStack *stack,
                         GbTab      *tab)
{
  GtkTreeIter iter;

  g_return_if_fail (GB_IS_TAB_STACK (stack));
  g_return_if_fail (GB_IS_TAB (tab));

  if (gb_tab_stack_get_tab_iter (stack, tab, &iter))
    {
      gtk_container_remove (GTK_CONTAINER (stack->priv->stack),
                            GTK_WIDGET (tab));
      gtk_list_store_remove (stack->priv->store, &iter);
    }
}

static gboolean
gb_tab_stack_focus_iter (GbTabStack  *stack,
                         GtkTreeIter *iter)
{
  gboolean ret = FALSE;
  GbTab *tab = NULL;

  g_return_val_if_fail (GB_IS_TAB_STACK (stack), FALSE);
  g_return_val_if_fail (iter, FALSE);

  gtk_tree_model_get (GTK_TREE_MODEL (stack->priv->store), iter,
                      0, &tab,
                      -1);

  if (GB_IS_TAB (tab))
    {
      gtk_combo_box_set_active_iter (stack->priv->combo, iter);
      ret = TRUE;
    }

  g_clear_object (&tab);

  return ret;
}

gboolean
gb_tab_stack_focus_next (GbTabStack *stack)
{
  GtkWidget *child;
  GtkTreeIter iter;
  gboolean ret = FALSE;

  g_return_val_if_fail (GB_IS_TAB_STACK (stack), FALSE);

  if (!(child = gtk_stack_get_visible_child (stack->priv->stack)))
    return FALSE;

  if (gb_tab_stack_get_tab_iter (stack, GB_TAB (child), &iter) &&
      gtk_tree_model_iter_next (GTK_TREE_MODEL (stack->priv->store), &iter))
    ret = gb_tab_stack_focus_iter (stack, &iter);

  return ret;
}

gboolean
gb_tab_stack_focus_previous (GbTabStack *stack)
{
  GtkWidget *child;
  GtkTreeIter iter;
  gboolean ret = FALSE;

  g_return_val_if_fail (GB_IS_TAB_STACK (stack), FALSE);

  if (!(child = gtk_stack_get_visible_child (stack->priv->stack)))
    return FALSE;

  if (gb_tab_stack_get_tab_iter (stack, GB_TAB (child), &iter) &&
      gtk_tree_model_iter_previous (GTK_TREE_MODEL (stack->priv->store), &iter))
    ret = gb_tab_stack_focus_iter (stack, &iter);

  return ret;
}

gboolean
gb_tab_stack_focus_first (GbTabStack *stack)
{
  GtkTreeIter iter;

  g_return_val_if_fail (GB_IS_TAB_STACK (stack), FALSE);

  if (gtk_tree_model_get_iter_first (GTK_TREE_MODEL (stack->priv->store),
                                     &iter))
    return gb_tab_stack_focus_iter (stack, &iter);

  return FALSE;
}

gboolean
gb_tab_stack_contains_tab (GbTabStack *stack,
                           GbTab      *tab)
{
  gboolean ret = FALSE;
  GList *list;
  GList *iter;

  g_return_val_if_fail (GB_IS_TAB_STACK (stack), FALSE);
  g_return_val_if_fail (GB_IS_TAB (tab), FALSE);

  list = gb_tab_stack_get_tabs (stack);

  for (iter = list; iter; iter = iter->next)
    {
      if (iter->data == (void *)tab)
        {
          ret = TRUE;
          break;
        }
    }

  g_list_free (list);

  return ret;
}

static void
gb_tab_stack_combobox_changed (GbTabStack  *stack,
                               GtkComboBox *combobox)
{
  GtkTreeModel *model;
  GtkTreeIter iter;
  GbTab *tab = NULL;

  g_return_if_fail (GB_IS_TAB_STACK (stack));
  g_print ("changed\n");

  model = gtk_combo_box_get_model (combobox);

  if (gtk_combo_box_get_active_iter (combobox, &iter))
    {
      gtk_tree_model_get (model, &iter, 0, &tab, -1);

      if (GB_IS_TAB (tab))
        {
          GtkWidget *controls;

          gtk_stack_set_visible_child (stack->priv->stack, GTK_WIDGET (tab));
          gtk_widget_set_sensitive (GTK_WIDGET (stack->priv->close), TRUE);

          if ((controls = gb_tab_get_controls (tab)))
            gtk_stack_set_visible_child (stack->priv->controls, controls);

          g_print ("Controls: %p\n", controls);
        }
      else
        {
          gtk_widget_set_sensitive (GTK_WIDGET (stack->priv->close), FALSE);
        }

      g_clear_object (&tab);
    }
}

static gboolean
gb_tab_stack_queue_draw (gpointer data)
{
  g_return_val_if_fail (GTK_IS_WIDGET (data), NULL);

  gtk_widget_queue_draw (GTK_WIDGET (data));

  return G_SOURCE_REMOVE;
}

static void
gb_tab_stack_add_tab (GbTabStack *stack,
                      GbTab      *tab)
{
  GtkTreeIter iter;
  GtkWidget *controls;

  g_return_if_fail (GB_IS_TAB_STACK (stack));
  g_return_if_fail (GB_IS_TAB (tab));

  gtk_list_store_append (stack->priv->store, &iter);
  g_object_freeze_notify (G_OBJECT (stack->priv->stack));
  gtk_list_store_set (stack->priv->store, &iter, 0, tab, -1);
  gtk_container_add (GTK_CONTAINER (stack->priv->stack), GTK_WIDGET (tab));
  if ((controls = gb_tab_get_controls (tab)))
    gtk_container_add (GTK_CONTAINER (stack->priv->controls), controls);
  g_object_thaw_notify (G_OBJECT (stack->priv->stack));
  gtk_combo_box_set_active_iter (stack->priv->combo, &iter);

  /* TODO: need to disconnect on (re)move */
  g_signal_connect_object (tab,
                           "notify::title",
                           G_CALLBACK (gb_tab_stack_queue_draw),
                           stack,
                           G_CONNECT_SWAPPED);
}

static void
gb_tab_stack_add (GtkContainer *container,
                  GtkWidget    *widget)
{
  GbTabStack *stack = (GbTabStack *)container;

  g_return_if_fail (GB_IS_TAB_STACK (stack));
  g_return_if_fail (GTK_IS_WIDGET (widget));

  if (GB_IS_TAB (widget))
    gb_tab_stack_add_tab (stack, GB_TAB (widget));
  else
    GTK_CONTAINER_CLASS (gb_tab_stack_parent_class)->add (container, widget);
}

static void
gb_tab_stack_combobox_text_func (GtkCellLayout   *cell_layout,
                                 GtkCellRenderer *cell,
                                 GtkTreeModel    *tree_model,
                                 GtkTreeIter     *iter,
                                 gpointer         data)
{
  const gchar *title = NULL;
  gchar *str = NULL;
  GbTab *tab = NULL;

  gtk_tree_model_get (tree_model, iter, 0, &tab, -1);

  if (GB_IS_TAB (tab))
    title = gb_tab_get_title (tab);

  if (!title)
    {
      /* TODO: temp tab name */
      title = _("untitled");
    }

  g_object_set (cell, "text", title, NULL);

  g_clear_object (&tab);
  g_free (str);
}

static void
gb_tab_stack_finalize (GObject *object)
{
  G_OBJECT_CLASS (gb_tab_stack_parent_class)->finalize (object);
}

static void
gb_tab_stack_get_property (GObject    *object,
                           guint       prop_id,
                           GValue     *value,
                           GParamSpec *pspec)
{
#if 0
  GbTabStack *stack = GB_TAB_STACK (object);
#endif

  switch (prop_id)
    {
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
gb_tab_stack_set_property (GObject      *object,
                           guint         prop_id,
                           const GValue *value,
                           GParamSpec   *pspec)
{
#if 0
  GbTabStack *stack = GB_TAB_STACK (object);
#endif

  switch (prop_id)
    {
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
gb_tab_stack_class_init (GbTabStackClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);
  GtkContainerClass *container_class = GTK_CONTAINER_CLASS (klass);

  object_class->finalize = gb_tab_stack_finalize;
  object_class->get_property = gb_tab_stack_get_property;
  object_class->set_property = gb_tab_stack_set_property;

  container_class->add = gb_tab_stack_add;

  gtk_widget_class_set_template_from_resource (widget_class,
                                               "/org/gnome/builder/ui/gb-tab-stack.ui");
  gtk_widget_class_bind_template_child_internal_private (widget_class, GbTabStack, controls);
  gtk_widget_class_bind_template_child_private (widget_class, GbTabStack, close);
  gtk_widget_class_bind_template_child_private (widget_class, GbTabStack, combo);
  gtk_widget_class_bind_template_child_private (widget_class, GbTabStack, stack);
  gtk_widget_class_bind_template_child_private (widget_class, GbTabStack, store);

  g_type_ensure (GB_TYPE_TAB);
}

static void
gb_tab_stack_init (GbTabStack *stack)
{
  GtkCellLayout *layout;
  GtkCellRenderer *cell;

  stack->priv = gb_tab_stack_get_instance_private (stack);

  gtk_orientable_set_orientation (GTK_ORIENTABLE (stack),
                                  GTK_ORIENTATION_VERTICAL);

  gtk_widget_init_template (GTK_WIDGET (stack));

  g_signal_connect_object (stack->priv->combo,
                           "changed",
                           G_CALLBACK (gb_tab_stack_combobox_changed),
                           stack,
                           G_CONNECT_SWAPPED);

  layout = GTK_CELL_LAYOUT (stack->priv->combo);
  cell = gtk_cell_renderer_text_new ();
  gtk_cell_layout_pack_start (layout, cell, TRUE);
  gtk_cell_layout_set_cell_data_func (layout, cell,
                                      gb_tab_stack_combobox_text_func,
                                      NULL, NULL);
  gtk_cell_renderer_text_set_fixed_height_from_font (
      GTK_CELL_RENDERER_TEXT (cell), 1);
}
