/* gb-tab-grid.c
 *
 * Copyright (C) 2011 Christian Hergert <chris@dronelabs.com>
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

#include "gb-log.h"
#include "gb-tab.h"
#include "gb-tab-stack.h"
#include "gb-tab-grid.h"

struct _GbTabGridPrivate
{
  GtkWidget *top_hpaned;
};

G_DEFINE_TYPE_WITH_PRIVATE (GbTabGrid, gb_tab_grid, GTK_TYPE_BIN)

static GtkWidget *
gb_tab_grid_get_first_stack (GbTabGrid*);

GtkWidget *
gb_tab_grid_new (void)
{
  return g_object_new (GB_TYPE_TAB_GRID, NULL);
}

static void
gb_tab_grid_remove_empty (GbTabGrid *self)
{
  GbTabGridPrivate *priv;
  GtkWidget *paned;
  GtkWidget *stack;
  GtkWidget *parent;
  GtkWidget *child;

  ENTRY;

  g_return_if_fail (GB_IS_TAB_GRID (self));

  priv = self->priv;

  paned = gtk_paned_get_child2 (GTK_PANED (priv->top_hpaned));
  g_assert (GTK_IS_PANED (paned));

  while (paned)
    {
      stack = gtk_paned_get_child1 (GTK_PANED (paned));
      g_assert (GB_IS_TAB_STACK (stack));

      if (!gb_tab_stack_get_n_tabs (GB_TAB_STACK (stack)))
        {
          child = gtk_paned_get_child2 (GTK_PANED (paned));
          g_object_ref (child);
          parent = gtk_widget_get_parent (paned);
          gtk_container_remove (GTK_CONTAINER (paned), child);
          gtk_container_remove (GTK_CONTAINER (parent), paned);
          gtk_paned_add2 (GTK_PANED (parent), child);
          g_object_unref (child);
          paned = parent;
        }

      paned = gtk_paned_get_child2 (GTK_PANED (paned));
    }

  /*
   * If everything got removed, re-add a default stack.
   */
  if (!gtk_paned_get_child2 (GTK_PANED (priv->top_hpaned)))
    (void)gb_tab_grid_get_first_stack (self);

  EXIT;
}

static GtkWidget *
gb_tab_grid_get_first_stack (GbTabGrid *self)
{
  GbTabGridPrivate *priv;
  GtkWidget *child;
  GtkWidget *paned;

  ENTRY;

  g_return_val_if_fail (GB_IS_TAB_GRID (self), NULL);

  priv = self->priv;

  if (!(paned = gtk_paned_get_child2 (GTK_PANED (priv->top_hpaned))))
    {
      paned = g_object_new (GTK_TYPE_PANED,
                            "orientation", GTK_ORIENTATION_HORIZONTAL,
                            "visible", TRUE,
                            NULL);
      gtk_paned_add2 (GTK_PANED (priv->top_hpaned), paned);
      gtk_container_child_set (GTK_CONTAINER (priv->top_hpaned), paned,
                               "resize", TRUE,
                               "shrink", FALSE,
                               NULL);
      child = g_object_new (GB_TYPE_TAB_STACK,
                            "visible", TRUE,
                            NULL);
      g_signal_connect_swapped (child, "changed",
                                G_CALLBACK (gb_tab_grid_remove_empty),
                                self);
      gtk_paned_add1 (GTK_PANED (paned), child);
    }

  child = gtk_paned_get_child1 (GTK_PANED (paned));

  RETURN (child);
}

static void
gb_tab_grid_add (GtkContainer *container,
                 GtkWidget    *child)
{
  GbTabGridPrivate *priv;
  GbTabGrid *self = (GbTabGrid *) container;
  GtkWidget *stack = NULL;
  GtkWidget *toplevel;

  g_return_if_fail (GB_IS_TAB_GRID (self));
  g_return_if_fail (GTK_IS_WIDGET (child));

  priv = self->priv;

  if (GB_IS_TAB (child))
    {
      /*
       * Try to find the currently focused view.
       */
      toplevel = gtk_widget_get_toplevel (GTK_WIDGET (self));
      if (toplevel && GTK_IS_WINDOW (toplevel))
        {
          if ((stack = gtk_window_get_focus (GTK_WINDOW (toplevel))))
            while (stack && !GB_IS_TAB_STACK (stack))
              stack = gtk_widget_get_parent (stack);

        }

      if (!stack)
        stack = gb_tab_grid_get_first_stack (self);

      gtk_container_add (GTK_CONTAINER (stack), child);
    }
  else
    gtk_paned_add1 (GTK_PANED (priv->top_hpaned), child);
}

static GList *
gb_tab_grid_get_stacks (GbTabGrid *self)
{
  GbTabGridPrivate *priv;
  GtkWidget *child;
  GtkWidget *paned;
  GList *list = NULL;

  ENTRY;

  g_return_val_if_fail (GB_IS_TAB_GRID (self), NULL);

  priv = self->priv;

  paned = priv->top_hpaned;

  for (; paned; paned = gtk_paned_get_child2 (GTK_PANED (paned)))
    {
      child = gtk_paned_get_child1 (GTK_PANED (paned));
      if (GB_IS_TAB_STACK (child))
        list = g_list_append (list, child);
    }

  RETURN (list);
}

GList *
gb_tab_grid_get_tabs (GbTabGrid *self)
{
  GList *stacks;
  GList *iter;
  GList *ret = NULL;

  ENTRY;

  g_return_val_if_fail (GB_IS_TAB_GRID (self), NULL);

  stacks = gb_tab_grid_get_stacks (self);
  for (iter = stacks; iter; iter = iter->next)
    ret = g_list_concat (ret, gb_tab_stack_get_tabs (iter->data));
  g_list_free (stacks);

  RETURN (ret);
}

static void
gb_tab_grid_realign (GbTabGrid *self)
{
  GbTabGridPrivate *priv;
  GtkAllocation alloc;
  GtkWidget *paned;
  guint n_paneds = 0;
  guint width;

  ENTRY;

  g_return_if_fail (GB_IS_TAB_GRID (self));

  priv = self->priv;

  paned = gtk_paned_get_child2 (GTK_PANED (priv->top_hpaned));
  do
    n_paneds++;
  while ((paned = gtk_paned_get_child2 (GTK_PANED (paned))));
  g_assert_cmpint (n_paneds, >, 0);

  paned = gtk_paned_get_child2 (GTK_PANED (priv->top_hpaned));
  gtk_widget_get_allocation (paned, &alloc);
  width = alloc.width / n_paneds;

  do
    gtk_paned_set_position (GTK_PANED (paned), width);
  while ((paned = gtk_paned_get_child2 (GTK_PANED (paned))));

  EXIT;
}

static GbTabStack *
gb_tab_grid_add_stack (GbTabGrid *self)
{
  GbTabGridPrivate *priv;
  GtkWidget *stack_paned;
  GtkWidget *stack = NULL;
  GtkWidget *paned;

  ENTRY;

  g_return_val_if_fail (GB_IS_TAB_GRID (self), NULL);

  priv = self->priv;

  stack = g_object_new (GB_TYPE_TAB_STACK,
                        "visible", TRUE,
                        NULL);
  g_signal_connect_swapped (stack, "changed",
                            G_CALLBACK (gb_tab_grid_remove_empty),
                            self);

  paned = priv->top_hpaned;
  while (gtk_paned_get_child2 (GTK_PANED (paned)))
    paned = gtk_paned_get_child2 (GTK_PANED (paned));

  stack_paned = g_object_new (GTK_TYPE_PANED,
                              "orientation", GTK_ORIENTATION_HORIZONTAL,
                              "visible", TRUE,
                              NULL);
  gtk_paned_add1 (GTK_PANED (stack_paned), stack);
  gtk_container_child_set (GTK_CONTAINER (stack_paned), stack,
                           "resize", TRUE,
                           "shrink", FALSE,
                           NULL);

  gtk_paned_add2 (GTK_PANED (paned), stack_paned);
  gtk_container_child_set (GTK_CONTAINER (paned), stack_paned,
                           "resize", TRUE,
                           "shrink", FALSE,
                           NULL);

  gb_tab_grid_realign (self);

  RETURN (GB_TAB_STACK (stack));
}

void
gb_tab_grid_move_tab_right (GbTabGrid *self,
                            GbTab     *tab)
{
  GbTabStack *stack;
  GList *iter;
  GList *stacks;

  ENTRY;

  g_return_if_fail (GB_IS_TAB_GRID (self));
  g_return_if_fail (GB_IS_TAB (tab));

  stacks = gb_tab_grid_get_stacks (self);

  for (iter = stacks; iter; iter = iter->next)
    {
      if (gb_tab_stack_contains_tab (iter->data, tab))
        {
          g_object_ref (tab);
          gb_tab_stack_remove_tab (iter->data, tab);
          if (!iter->next)
            stack = gb_tab_grid_add_stack (self);
          else
            stack = iter->next->data;
#if 0
          gb_tab_stack_add_tab (stack, tab);
#endif
          gtk_container_add (GTK_CONTAINER (stack), GTK_WIDGET (tab));
          g_object_unref (tab);
          break;
        }
    }

  g_list_free (stacks);

  gb_tab_grid_remove_empty (self);

  EXIT;
}

void
gb_tab_grid_focus_next_view (GbTabGrid *self,
                             GbTab     *tab)
{
  GList *iter;
  GList *stacks;

  ENTRY;

  g_return_if_fail (GB_IS_TAB_GRID (self));
  g_return_if_fail (GB_IS_TAB (tab));

  /* TODO: track focus so we can drop @tab parameter */

  stacks = gb_tab_grid_get_stacks (self);
  for (iter = stacks; iter; iter = iter->next)
    {
      if (gb_tab_stack_contains_tab (iter->data, tab))
        {
          if (!gb_tab_stack_focus_next (iter->data))
            if (iter->next)
              gb_tab_stack_focus_first (iter->next->data);

          break;
        }
    }
  g_list_free (stacks);

  EXIT;
}

void
gb_tab_grid_focus_previous_view (GbTabGrid *self,
                                 GbTab     *view)
{
  GList *iter;
  GList *stacks;

  ENTRY;

  g_return_if_fail (GB_IS_TAB_GRID (self));
  g_return_if_fail (GB_IS_TAB (view));

  /* TODO: track focus so we can drop @tab parameter */

  stacks = gb_tab_grid_get_stacks (self);
  stacks = g_list_reverse (stacks);
  for (iter = stacks; iter; iter = iter->next)
    {
      if (gb_tab_stack_contains_tab (iter->data, view))
        {
          gb_tab_stack_focus_previous (iter->data);
          break;
        }
    }
  g_list_free (stacks);

  EXIT;
}

/**
 * gb_tab_grid_class_init:
 * @klass: (in): A #GbTabGridClass.
 *
 * Initializes the #GbTabGridClass and prepares the vtable.
 */
static void
gb_tab_grid_class_init (GbTabGridClass *klass)
{
  GtkContainerClass *container_class = GTK_CONTAINER_CLASS (klass);

  container_class->add = gb_tab_grid_add;
}

/**
 * gb_tab_grid_init:
 * @self: (in): A #GbTabGrid.
 *
 * Initializes the newly created #GbTabGrid instance.
 */
static void
gb_tab_grid_init (GbTabGrid *self)
{
  GtkWidget *paned;
  GtkWidget *stack;

  self->priv = gb_tab_grid_get_instance_private (self);

  self->priv->top_hpaned =
    g_object_new (GTK_TYPE_PANED,
                  "orientation", GTK_ORIENTATION_HORIZONTAL,
                  "visible", TRUE,
                  NULL);
  GTK_CONTAINER_CLASS (gb_tab_grid_parent_class)->add (GTK_CONTAINER (self),
                                                       self->priv->top_hpaned);

  paned = g_object_new (GTK_TYPE_PANED,
                        "orientation", GTK_ORIENTATION_HORIZONTAL,
                        "visible", TRUE,
                        NULL);
  gtk_paned_add2 (GTK_PANED (self->priv->top_hpaned), paned);

  stack = g_object_new (GB_TYPE_TAB_STACK,
                        "visible", TRUE,
                        NULL);
  g_signal_connect_swapped (stack, "changed",
                            G_CALLBACK (gb_tab_grid_remove_empty),
                            self);
  gtk_paned_add1 (GTK_PANED (paned), stack);
  gtk_container_child_set (GTK_CONTAINER (paned), stack,
                           "resize", TRUE,
                           "shrink", FALSE,
                           NULL);
}
