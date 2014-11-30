/* gb-editor-tab.c
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

#include "gb-editor-document.h"
#include "gb-editor-frame.h"
#include "gb-editor-tab.h"

struct _GbEditorTabPrivate
{
  GbEditorFrame    *frame;
  GtkPaned         *paned;
  GtkToggleButton  *split_button;

  GbEditorDocument *document;
};

enum
{
  PROP_0,
  LAST_PROP
};

G_DEFINE_TYPE_WITH_PRIVATE (GbEditorTab, gb_editor_tab, GB_TYPE_TAB)

static GParamSpec *gParamSpecs [LAST_PROP];

static void
gb_editor_tab_on_split_toggled (GbEditorTab     *tab,
                                GtkToggleButton *button)
{
  GbEditorTabPrivate *priv;
  GtkWidget *child2;

  g_return_if_fail (GB_IS_EDITOR_TAB (tab));

  priv = tab->priv;

  child2 = gtk_paned_get_child2 (priv->paned);

  if (child2)
    {
      gtk_container_remove (GTK_CONTAINER (priv->paned), child2);
    }
  else
    {
      child2 = g_object_new (GB_TYPE_EDITOR_FRAME,
                             "document", priv->document,
                             "visible", TRUE,
                             NULL);
      gtk_paned_add2 (priv->paned, child2);
      gtk_container_child_set (GTK_CONTAINER (priv->paned), child2,
                               "resize", TRUE,
                               "shrink", FALSE,
                               NULL);
    }
}

GbEditorFrame *
gb_editor_tab_get_last_frame (GbEditorTab *tab)
{
  g_return_val_if_fail (GB_IS_EDITOR_TAB (tab), NULL);

  /* TODO: track the frame */

  return tab->priv->frame;
}

static void
gb_editor_tab_grab_focus (GtkWidget *widget)
{
  GbEditorFrame *last_frame;

  g_return_if_fail (GB_IS_EDITOR_TAB (widget));

  last_frame = gb_editor_tab_get_last_frame (GB_EDITOR_TAB (widget));

  gtk_widget_grab_focus (GTK_WIDGET (last_frame));
}

static void
gb_editor_tab_constructed (GObject *object)
{
  GbEditorTabPrivate *priv;
  GbEditorTab *tab = (GbEditorTab *)object;

  g_return_if_fail (GB_IS_EDITOR_TAB (tab));

  priv = tab->priv;

  G_OBJECT_CLASS (gb_editor_tab_parent_class)->constructed (object);

  priv->document = g_object_new (GB_TYPE_EDITOR_DOCUMENT,
                                 NULL);
  gb_editor_frame_set_document (priv->frame, priv->document);

  g_signal_connect_object (priv->split_button,
                           "toggled",
                           G_CALLBACK (gb_editor_tab_on_split_toggled),
                           tab,
                           G_CONNECT_SWAPPED);
}

static void
gb_editor_tab_dispose (GObject *object)
{
  GbEditorTabPrivate *priv = GB_EDITOR_TAB (object)->priv;

  g_clear_object (&priv->document);

  G_OBJECT_CLASS (gb_editor_tab_parent_class)->dispose (object);
}

static void
gb_editor_tab_finalize (GObject *object)
{
  G_OBJECT_CLASS (gb_editor_tab_parent_class)->finalize (object);
}

static void
gb_editor_tab_get_property (GObject    *object,
                            guint       prop_id,
                            GValue     *value,
                            GParamSpec *pspec)
{
  GbEditorTab *self = GB_EDITOR_TAB (object);

  switch (prop_id)
    {
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
gb_editor_tab_set_property (GObject      *object,
                            guint         prop_id,
                            const GValue *value,
                            GParamSpec   *pspec)
{
  GbEditorTab *self = GB_EDITOR_TAB (object);

  switch (prop_id)
    {
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
gb_editor_tab_class_init (GbEditorTabClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->constructed = gb_editor_tab_constructed;
  object_class->dispose = gb_editor_tab_dispose;
  object_class->finalize = gb_editor_tab_finalize;
  object_class->get_property = gb_editor_tab_get_property;
  object_class->set_property = gb_editor_tab_set_property;

  widget_class->grab_focus = gb_editor_tab_grab_focus;

  gtk_widget_class_set_template_from_resource (widget_class,
                                               "/org/gnome/builder/ui/gb-editor-tab.ui");
  gtk_widget_class_bind_template_child_private (widget_class, GbEditorTab, frame);
  gtk_widget_class_bind_template_child_private (widget_class, GbEditorTab, paned);
  gtk_widget_class_bind_template_child_private (widget_class, GbEditorTab, split_button);

  g_type_ensure (GB_TYPE_EDITOR_FRAME);
}

static void
gb_editor_tab_init (GbEditorTab *tab)
{
  tab->priv = gb_editor_tab_get_instance_private (tab);

  gtk_widget_init_template (GTK_WIDGET (tab));
}
