/* gb-tab-stack.c
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

struct _GbTabStackPrivate
{
   GtkListStore *tabs;
   GtkWidget    *combo;
   GtkWidget    *controls;
   GtkWidget    *close;
   GtkWidget    *notebook;
};

G_DEFINE_TYPE_WITH_PRIVATE (GbTabStack, gb_tab_stack, GTK_TYPE_BOX)

enum
{
   PROP_0,
   LAST_PROP
};

enum
{
   CHANGED,
   LAST_SIGNAL
};

//static GParamSpec *gParamSpecs[LAST_PROP];
static guint gSignals[LAST_SIGNAL];

guint
gb_tab_stack_get_n_tabs (GbTabStack *stack)
{
   guint ret;

   ENTRY;
   g_return_val_if_fail(GB_IS_TAB_STACK(stack), 0);
   ret = gtk_tree_model_iter_n_children(GTK_TREE_MODEL(stack->priv->tabs), NULL);
   RETURN(ret);
}

gboolean
gb_tab_stack_focus_first (GbTabStack *stack)
{
   GbTabStackPrivate *priv;
   GtkTreeIter iter;

   ENTRY;

   g_return_val_if_fail(GB_IS_TAB_STACK(stack), FALSE);

   priv = stack->priv;

   if (gtk_tree_model_get_iter_first(GTK_TREE_MODEL(priv->tabs), &iter)) {
      gtk_combo_box_set_active_iter(GTK_COMBO_BOX(priv->combo), &iter);
      RETURN(TRUE);
   }

   RETURN(FALSE);
}

gboolean
gb_tab_stack_focus_next (GbTabStack *stack)
{
   GbTabStackPrivate *priv;
   guint n_tabs;
   gint idx;

   ENTRY;

   g_return_val_if_fail(GB_IS_TAB_STACK(stack), FALSE);

   priv = stack->priv;

   if ((idx = gtk_combo_box_get_active(GTK_COMBO_BOX(priv->combo))) >= 0) {
      n_tabs = gb_tab_stack_get_n_tabs(stack);
      if ((idx + 1) < n_tabs) {
         gtk_combo_box_set_active(GTK_COMBO_BOX(priv->combo), idx + 1);
         RETURN(TRUE);
      }
   }

   RETURN(FALSE);
}

gboolean
gb_tab_stack_focus_previous (GbTabStack *stack)
{
   GbTabStackPrivate *priv;
   gint idx;

   ENTRY;

   g_return_val_if_fail(GB_IS_TAB_STACK(stack), FALSE);

   priv = stack->priv;

   if ((idx = gtk_combo_box_get_active(GTK_COMBO_BOX(priv->combo))) > 0) {
      gtk_combo_box_set_active(GTK_COMBO_BOX(priv->combo), idx - 1);
      RETURN(TRUE);
   }

   RETURN(FALSE);
}

gboolean
gb_tab_stack_focus_view (GbTabStack *stack,
                          GbTab      *view)
{
   GbTabStackPrivate *priv;
   GtkTreeModel *model;
   GtkTreeIter iter;
   GObject *object;

   ENTRY;

   g_return_val_if_fail(GB_IS_TAB_STACK(stack), FALSE);

   priv = stack->priv;

   model = gtk_combo_box_get_model(GTK_COMBO_BOX(priv->combo));

   if (gtk_tree_model_get_iter_first(model, &iter)) {
      do {
         gtk_tree_model_get(model, &iter, 0, &object, -1);
         g_object_unref(object);
         if (object == (GObject *)view) {
            gtk_combo_box_set_active_iter(GTK_COMBO_BOX(priv->combo), &iter);
            RETURN(TRUE);
         }
      } while (gtk_tree_model_iter_next(model, &iter));
   }

   RETURN(FALSE);
}

gboolean
gb_tab_stack_contains_tab (GbTabStack *stack,
                           GbTab      *view)
{
   GbTabStackPrivate *priv;
   GtkTreeModel *model;
   GtkTreeIter iter;
   GObject *object;

   ENTRY;

   g_return_val_if_fail(GB_IS_TAB_STACK(stack), FALSE);
   g_return_val_if_fail(GB_IS_TAB(view), FALSE);

   priv = stack->priv;

   model = GTK_TREE_MODEL(priv->tabs);

   if (gtk_tree_model_get_iter_first(model, &iter)) {
      do {
         gtk_tree_model_get(model, &iter, 0, &object, -1);
         g_object_unref(object);
         if (object == (GObject *)view) {
            RETURN(TRUE);
         }
      } while (gtk_tree_model_iter_next(model, &iter));
   }

   RETURN(FALSE);
}

static void
gb_tab_stack_set_page (GbTabStack *stack,
                        gint         page)
{
   GbTabStackPrivate *priv;
   GtkWidget *controls;
   GtkWidget *view;
   gint current;

   ENTRY;

   g_return_if_fail(GB_IS_TAB_STACK(stack));
   g_return_if_fail(page >= 0);

   priv = stack->priv;

   current = gtk_notebook_get_current_page(GTK_NOTEBOOK(priv->controls));
   if (current >= 0) {
      controls = gtk_notebook_get_nth_page(GTK_NOTEBOOK(priv->controls), current);
      gtk_widget_hide(controls);
   }

   gtk_notebook_set_current_page(GTK_NOTEBOOK(priv->notebook), page);
   gtk_notebook_set_current_page(GTK_NOTEBOOK(priv->controls), page);

   if ((controls = gtk_notebook_get_nth_page(GTK_NOTEBOOK(priv->controls), page))) {
      gtk_widget_show(controls);
   }

   if ((view = gtk_notebook_get_nth_page(GTK_NOTEBOOK(priv->notebook), page))) {
      gtk_widget_grab_focus(view);
   }

   EXIT;
}

void
gb_tab_stack_remove_tab (GbTabStack *stack,
                         GbTab      *view)
{
   GbTabStackPrivate *priv;
   GtkTreeModel *model;
   GtkTreeIter iter;
   GtkWidget *controls;
   GObject *object;
   gint page = 0;
   gboolean active = FALSE;

   ENTRY;

   g_return_if_fail(GB_IS_TAB_STACK(stack));
   g_return_if_fail(GB_IS_TAB(view));

   priv = stack->priv;

   if (!gb_tab_stack_contains_tab (stack, view)) {
      g_warning ("%s(): view is missing from stack.", G_STRFUNC);
      EXIT;
   }

   /*
    * TODO: Disconnect signals.
    */

   /*
    * Remove the view from the drop down list.
    */
   model = GTK_TREE_MODEL(priv->tabs);
   if (gtk_tree_model_get_iter_first(model, &iter)) {
      do {
         gtk_tree_model_get(model, &iter, 0, &object, -1);
         g_object_unref(object);
         if (object == (GObject *)view) {
            active = (page == gtk_combo_box_get_active(GTK_COMBO_BOX(priv->combo)));
            gtk_list_store_remove(priv->tabs, &iter);
            break;
         }
         page++;
      } while (gtk_tree_model_iter_next(model, &iter));
   }

   /*
    * Remove the controls from the notebook.
    */
   if ((controls = gb_tab_get_controls(view))) {
      gtk_container_remove(GTK_CONTAINER(priv->controls), controls);
   }

   /*
    * Remove the view from the notebook.
    */
   gtk_container_remove(GTK_CONTAINER(priv->notebook), GTK_WIDGET(view));

   /*
    * Hide the close button if there are no views left.
    */
   if (!gtk_notebook_get_n_pages(GTK_NOTEBOOK(priv->notebook))) {
      gtk_widget_hide(priv->close);
   }

   /*
    * Try to set the page to the new item in the same slot if we can,
    * otherwise the item previous.
    */
   if (active) {
      page = MIN(page, gtk_notebook_get_n_pages(GTK_NOTEBOOK(priv->notebook)) - 1);
      if (page >= 0) {
         gtk_combo_box_set_active(GTK_COMBO_BOX(priv->combo), page);
      }
   }

   g_signal_emit(stack, gSignals[CHANGED], 0);

   EXIT;
}

static void
gb_tab_stack_remove (GtkContainer *container,
                      GtkWidget    *child)
{
   GbTabStack *stack = (GbTabStack *)container;

   ENTRY;

   g_return_if_fail(GB_IS_TAB_STACK(stack));
   g_return_if_fail(GTK_IS_WIDGET(child));

   if (GB_IS_TAB(child)) {
      gb_tab_stack_remove_tab (stack, GB_TAB(child));
   } else {
      GTK_CONTAINER_CLASS(gb_tab_stack_parent_class)->remove(container, child);
   }

   EXIT;
}

/**
 * gb_tab_stack_add_view:
 * @stack: (in): A #GbTabStack.
 * @view: (in): A #GbTab.
 *
 * Adds a view to the stack. An item is added to the #GtkComboBox
 * for the view. When selected from the combo box, the view will be
 * raised in the stack.
 *
 * The stack will take ownership of any of the controls provided by
 * the view. In the case the view is removed from the stack, the
 * controls will no longer be children of the stack. TODO
 */
static void
gb_tab_stack_add_view (GbTabStack *stack,
                        GbTab      *view)
{
   GbTabStackPrivate *priv;
   GtkTreeIter iter;
   gint page;

   ENTRY;

   g_return_if_fail(GB_IS_TAB_STACK(stack));
   g_return_if_fail(GB_IS_TAB(view));

   priv = stack->priv;

   gtk_container_add(GTK_CONTAINER(priv->notebook), GTK_WIDGET(view));

   gtk_list_store_append(priv->tabs, &iter);
   gtk_list_store_set(priv->tabs, &iter, 0, view, -1);
   gtk_combo_box_set_active_iter(GTK_COMBO_BOX(priv->combo), &iter);
   gtk_container_add(GTK_CONTAINER(priv->controls), gb_tab_get_controls(view));

   g_signal_connect_swapped(view, "notify::can-save",
                            G_CALLBACK(gtk_widget_queue_draw),
                            priv->combo);
   g_signal_connect_swapped(view, "notify::name",
                            G_CALLBACK(gtk_widget_queue_draw),
                            priv->combo);
   g_signal_connect_swapped(view, "closed",
                            G_CALLBACK(gb_tab_stack_remove_tab),
                            stack);

   if ((page = gtk_notebook_get_n_pages(GTK_NOTEBOOK(priv->notebook)))) {
      gb_tab_stack_set_page(stack, page - 1);
   }

   gtk_widget_show(priv->close);

   g_signal_emit(stack, gSignals[CHANGED], 0);

   EXIT;
}

/**
 * gb_tab_stack_add:
 * @container: (in): A #GbTabStack.
 * @child: (in): A #GtkWidget.
 *
 * Handle the addition of a child to the view. If the child is a view,
 * then we pack it into our internal notebook.
 */
static void
gb_tab_stack_add (GtkContainer *container,
                   GtkWidget    *child)
{
   GbTabStack *stack = (GbTabStack *)container;

   ENTRY;

   g_return_if_fail(GB_IS_TAB_STACK(stack));
   g_return_if_fail(GTK_IS_WIDGET(child));

   if (GB_IS_TAB(child)) {
      gb_tab_stack_add_view(stack, GB_TAB(child));
   } else {
      GTK_CONTAINER_CLASS(gb_tab_stack_parent_class)->add(container, child);
   }

   EXIT;
}

/**
 * gb_tab_stack_get_active:
 * @stack: (in): A #GbTabStack.
 *
 * Gets the active view based on the current focus.
 *
 * Returns: (transfer none): A #GtkWidget or %NULL.
 */
GtkWidget *
gb_tab_stack_get_active (GbTabStack *stack)
{
   GtkWidget *toplevel;
   GtkWidget *focus = NULL;

   ENTRY;

   g_return_val_if_fail(GB_IS_TAB_STACK(stack), NULL);

   if ((toplevel = gtk_widget_get_toplevel(GTK_WIDGET(stack)))) {
      if ((focus = gtk_window_get_focus(GTK_WINDOW(toplevel)))) {
         while (focus && !GB_IS_TAB(focus)) {
            focus = gtk_widget_get_parent(focus);
         }
      }
   }

   RETURN(focus);
}

static void
gb_tab_stack_icon_name_func (GtkCellLayout   *cell_layout,
                              GtkCellRenderer *cell,
                              GtkTreeModel    *tree_model,
                              GtkTreeIter     *iter,
                              gpointer         user_data)
{
   GbTab *view;

   gtk_tree_model_get(tree_model, iter, 0, &view, -1);
   g_assert(GB_IS_TAB(view));
   g_object_set(cell, "icon-name", gb_tab_get_icon_name(view), NULL);
   g_object_unref(view);
}

static void
gb_tab_stack_name_func (GtkCellLayout   *cell_layout,
                         GtkCellRenderer *cell,
                         GtkTreeModel    *tree_model,
                         GtkTreeIter     *iter,
                         gpointer         user_data)
{
   gchar *text;
   GbTab *view;

   gtk_tree_model_get(tree_model, iter, 0, &view, -1);
   g_assert(GB_IS_TAB(view));
   text = g_strdup_printf("%s%s",
                          gb_tab_get_title (view),
                          gb_tab_get_dirty (view) ? " •" : "");
   g_object_set(cell, "text", text, NULL);
   g_object_unref(view);
   g_free(text);
}

static void
gb_tab_stack_combo_changed (GbTabStack *stack,
                             GtkComboBox *combo)
{
   gint page;

   g_return_if_fail(GB_IS_TAB_STACK(stack));
   g_return_if_fail(GTK_IS_COMBO_BOX(combo));

   if ((page = gtk_combo_box_get_active(combo)) >= 0) {
      gb_tab_stack_set_page(stack, page);
      gtk_widget_grab_focus(stack->priv->notebook);
   }
}

static void
gb_tab_stack_grab_focus (GtkWidget *widget)
{
   GbTabStack *stack = (GbTabStack *)widget;
   g_return_if_fail(GB_IS_TAB_STACK(stack));
   gtk_widget_grab_focus(stack->priv->notebook);
}

static void
gb_tab_stack_close_current (GbTabStack *stack,
                             GtkButton   *button)
{
   GbTabStackPrivate *priv;
   GtkWidget *child;
   gint page;

   ENTRY;

   g_return_if_fail(GB_IS_TAB_STACK(stack));

   priv = stack->priv;

   page = gtk_notebook_get_current_page(GTK_NOTEBOOK(priv->notebook));
   if (page >= 0) {
      child = gtk_notebook_get_nth_page(GTK_NOTEBOOK(priv->notebook), page);
      if (GB_IS_TAB(child)) {
         gb_tab_close(GB_TAB(child));
      }
   }

   EXIT;
}

/**
 * gb_tab_stack_get_tabs:
 * @stack: (in): A #GbTabStack.
 *
 * Get all views in the stack.
 *
 * Returns: (transfer container) (element-type GbTab*): A #GList.
 */
GList *
gb_tab_stack_get_tabs (GbTabStack *stack)
{
   GtkTreeModel *model;
   GtkTreeIter iter;
   GbTab *view;
   GList *ret = NULL;

   ENTRY;

   g_return_val_if_fail(GB_IS_TAB_STACK(stack), NULL);

   model = GTK_TREE_MODEL(stack->priv->tabs);
   if (gtk_tree_model_get_iter_first(model, &iter)) {
      do {
         /*
          * The code below looks unsafe. However, the model holds our
          * reference to the view, so it is fine.
          */
         gtk_tree_model_get(model, &iter, 0, &view, -1);
         ret = g_list_append(ret, view);
         g_object_unref(view);
      } while (gtk_tree_model_iter_next(model, &iter));
   }

   RETURN(ret);
}

/**
 * gb_tab_stack_finalize:
 * @object: (in): A #GbTabStack.
 *
 * Finalizer for a #GbTabStack instance. Frees any resources held by
 * the instance.
 */
static void
gb_tab_stack_finalize (GObject *object)
{
   GbTabStackPrivate *priv;

   ENTRY;

   priv = GB_TAB_STACK(object)->priv;

   g_clear_object(&priv->tabs);

   G_OBJECT_CLASS(gb_tab_stack_parent_class)->finalize(object);

   EXIT;
}

/**
 * gb_tab_stack_class_init:
 * @klass: (in): A #GbTabStackClass.
 *
 * Initializes the #GbTabStackClass and prepares the vtable.
 */
static void
gb_tab_stack_class_init (GbTabStackClass *klass)
{
   GObjectClass *object_class;
   GtkWidgetClass *widget_class;
   GtkContainerClass *container_class;

   object_class = G_OBJECT_CLASS(klass);
   object_class->finalize = gb_tab_stack_finalize;

   widget_class = GTK_WIDGET_CLASS(klass);
   widget_class->grab_focus = gb_tab_stack_grab_focus;

   container_class = GTK_CONTAINER_CLASS(klass);
   container_class->add = gb_tab_stack_add;
   container_class->remove = gb_tab_stack_remove;

   /**
    * GbTabStack::changed:
    *
    * The "changed" signal is emitted when the children of the stack have
    * changed.
    */
   gSignals[CHANGED] = g_signal_new("changed",
                                    GB_TYPE_TAB_STACK,
                                    G_SIGNAL_RUN_LAST,
                                    0,
                                    NULL,
                                    NULL,
                                    g_cclosure_marshal_VOID__VOID,
                                    G_TYPE_NONE,
                                    0);
}

/**
 * gb_tab_stack_init:
 * @stack: (in): A #GbTabStack.
 *
 * Initializes the newly created #GbTabStack instance.
 */
static void
gb_tab_stack_init (GbTabStack *stack)
{
   GbTabStackPrivate *priv;
   GtkCellRenderer *cell;
   GtkWidget *hbox;

   stack->priv = priv = gb_tab_stack_get_instance_private (stack);

   gtk_orientable_set_orientation (GTK_ORIENTABLE (stack),
                                   GTK_ORIENTATION_VERTICAL);

   priv->tabs = gtk_list_store_new(1, GB_TYPE_TAB);

   hbox = g_object_new(GTK_TYPE_BOX,
                       "orientation", GTK_ORIENTATION_HORIZONTAL,
                       "visible", TRUE,
                       NULL);
   gtk_container_add_with_properties(GTK_CONTAINER(stack), hbox,
                                     "expand", FALSE,
                                     NULL);

   priv->combo = g_object_new(GTK_TYPE_COMBO_BOX,
                              "has-frame", FALSE,
                              "height-request", 30,
                              "model", priv->tabs,
                              "visible", TRUE,
                              NULL);
   gtk_container_add_with_properties(GTK_CONTAINER(hbox), priv->combo,
                                     "expand", TRUE,
                                     NULL);
   g_signal_connect_swapped(priv->combo, "changed",
                            G_CALLBACK(gb_tab_stack_combo_changed),
                            stack);

   priv->controls = g_object_new(GTK_TYPE_NOTEBOOK,
                                 "visible", TRUE,
                                 "show-border", FALSE,
                                 "show-tabs", FALSE,
                                 NULL);
   gtk_container_add_with_properties(GTK_CONTAINER(hbox), priv->controls,
                                     "expand", FALSE,
                                     NULL);

   priv->close = g_object_new(GTK_TYPE_BUTTON,
                              "child", g_object_new(GTK_TYPE_IMAGE,
                                                    "icon-name", "window-close-symbolic",
                                                    "icon-size", GTK_ICON_SIZE_MENU,
                                                    "visible", TRUE,
                                                    "tooltip-text", _("Close the current view."),
                                                    NULL),
                              "visible", FALSE,
                              NULL);
   g_signal_connect_swapped(priv->close, "clicked",
                            G_CALLBACK(gb_tab_stack_close_current),
                            stack);
   gtk_container_add_with_properties(GTK_CONTAINER(hbox), priv->close,
                                     "expand", FALSE,
                                     NULL);

   cell = g_object_new(GTK_TYPE_CELL_RENDERER_PIXBUF,
                       "icon-name", "text-x-generic",
                       "width", 24,
                       "xalign", 0.5f,
                       "xpad", 3,
                       NULL);
   gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(priv->combo), cell, FALSE);
   gtk_cell_layout_set_cell_data_func(GTK_CELL_LAYOUT(priv->combo), cell,
                                      gb_tab_stack_icon_name_func,
                                      NULL, NULL);

   cell = g_object_new(GTK_TYPE_CELL_RENDERER_TEXT,
                       "size-points", 9.0,
                       "xpad", 3,
                       NULL);
   gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(priv->combo), cell, TRUE);
   gtk_cell_layout_set_cell_data_func(GTK_CELL_LAYOUT(priv->combo), cell,
                                      gb_tab_stack_name_func,
                                      NULL, NULL);

   priv->notebook = g_object_new(GTK_TYPE_NOTEBOOK,
                                 "visible", TRUE,
                                 "show-border", FALSE,
                                 "show-tabs", FALSE,
                                 NULL);
   gtk_container_add_with_properties(GTK_CONTAINER(stack), priv->notebook,
                                     "expand", TRUE,
                                     NULL);
}
