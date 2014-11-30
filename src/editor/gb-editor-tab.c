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

#define G_LOG_DOMAIN "editor-tab"

#include <glib/gi18n.h>

#include "gb-doc-seq.h"
#include "gb-editor-frame-private.h"
#include "gb-editor-tab.h"
#include "gb-editor-tab-private.h"
#include "gb-log.h"
#include "gb-widget.h"

G_DEFINE_TYPE_WITH_PRIVATE (GbEditorTab, gb_editor_tab, GB_TYPE_TAB)

static void
gb_editor_tab_progress_cb (goffset  current_num_bytes,
                           goffset  total_num_bytes,
                           gpointer user_data)
{
  GbEditorTabPrivate *priv;
  GbEditorTab *tab = user_data;
  gdouble fraction;

  g_return_if_fail (GB_IS_EDITOR_TAB (tab));

  priv = tab->priv;

  if (priv->progress_animation)
    {
      g_object_remove_weak_pointer (G_OBJECT (priv->progress_animation),
                                    (gpointer *)&priv->progress_animation);
      gb_animation_stop (priv->progress_animation);
      priv->progress_animation = NULL;
    }

  fraction = total_num_bytes
           ? ((gdouble)current_num_bytes / (gdouble)total_num_bytes)
           : 1.0;

  priv->progress_animation =
    gb_object_animate (priv->progress_bar,
                       GB_ANIMATION_LINEAR,
                       250,
                       NULL,
                       "fraction", fraction,
                       NULL);
  g_object_add_weak_pointer (G_OBJECT (priv->progress_animation),
                             (gpointer *)&priv->progress_animation);
}

static void
gb_editor_tab_save_cb (GObject      *source_object,
                       GAsyncResult *result,
                       gpointer      user_data)
{
  GbEditorTab *tab = user_data;
  GbEditorDocument *document = (GbEditorDocument *)source_object;
  GError *error = NULL;

  ENTRY;

  g_return_if_fail (GB_IS_EDITOR_DOCUMENT (document));
  g_return_if_fail (G_IS_ASYNC_RESULT (result));
  g_return_if_fail (GB_IS_EDITOR_TAB (tab));

  if (!gb_editor_document_save_finish (document, result, &error))
    {
      g_print ("%s", error->message);
      //gb_editor_tab_set_error (tab, error);
      g_clear_error (&error);
    }

  gb_widget_fade_hide (GTK_WIDGET (tab->priv->progress_bar));

  g_object_unref (tab);

  EXIT;
}

static void
gb_editor_tab_do_save (GbEditorTab *tab)
{
  ENTRY;

  g_return_if_fail (GB_IS_EDITOR_TAB (tab));

  gtk_progress_bar_set_fraction (tab->priv->progress_bar, 0.0);
  gtk_widget_show (GTK_WIDGET (tab->priv->progress_bar));

  gb_editor_document_save_async (tab->priv->document,
                                 NULL, /* cancellable */
                                 gb_editor_tab_progress_cb,
                                 tab,
                                 NULL,
                                 gb_editor_tab_save_cb,
                                 g_object_ref (tab));

  EXIT;
}

void
gb_editor_tab_save_as (GbEditorTab *tab)
{
  GtkWidget *toplevel;
  GtkDialog *dialog;
  GtkWidget *suggested;
  GFile *chosen_file;
  guint response;

  ENTRY;

  g_return_if_fail (GB_IS_EDITOR_TAB (tab));

  toplevel = gtk_widget_get_toplevel (GTK_WIDGET (tab));
  dialog = g_object_new (GTK_TYPE_FILE_CHOOSER_DIALOG,
                         "action", GTK_FILE_CHOOSER_ACTION_SAVE,
                         "do-overwrite-confirmation", TRUE,
                         "local-only", FALSE,
                         "select-multiple", FALSE,
                         "show-hidden", FALSE,
                         "transient-for", toplevel,
                         "title", _("Save Document As"),
                         NULL);

  gtk_dialog_add_buttons (GTK_DIALOG (dialog),
                          _("Cancel"), GTK_RESPONSE_CANCEL,
                          _("Save"), GTK_RESPONSE_OK,
                          NULL);

  gtk_dialog_set_default_response (GTK_DIALOG (dialog), GTK_RESPONSE_OK);

  suggested = gtk_dialog_get_widget_for_response (GTK_DIALOG (dialog),
                                                  GTK_RESPONSE_OK);
  gtk_style_context_add_class (gtk_widget_get_style_context (suggested),
                               GTK_STYLE_CLASS_SUGGESTED_ACTION);

  response = gtk_dialog_run (GTK_DIALOG (dialog));
  gtk_widget_hide (GTK_WIDGET (dialog));

  if (response == GTK_RESPONSE_OK)
    {
      GtkSourceFile *sfile;

      sfile = gb_editor_document_get_file (tab->priv->document);
      chosen_file = gtk_file_chooser_get_file (GTK_FILE_CHOOSER (dialog));
      gtk_source_file_set_location (sfile, chosen_file);
      gb_editor_tab_do_save (tab);
      g_clear_object (&chosen_file);
    }

  gtk_widget_destroy (GTK_WIDGET (dialog));

  EXIT;
}

void
gb_editor_tab_save (GbEditorTab *tab)
{
  GtkSourceFile *file;
  gboolean has_location;

  ENTRY;

  g_return_if_fail (GB_IS_EDITOR_TAB (tab));

  file = gb_editor_document_get_file (tab->priv->document);
  has_location = !!gtk_source_file_get_location (file);

  if (has_location)
    gb_editor_tab_do_save (tab);
  else
    gb_editor_tab_save_as (tab);

  EXIT;
}

void
gb_editor_tab_open (GbEditorTab *tab)
{
}

static void
gb_editor_tab_on_frame_focused (GbEditorTab   *tab,
                                GbEditorFrame *frame)
{
  GbEditorTabPrivate *priv;

  g_return_if_fail (GB_IS_EDITOR_TAB (tab));
  g_return_if_fail (GB_IS_EDITOR_FRAME (frame));

  priv = tab->priv;

  if (priv->last_frame)
    {
      g_object_remove_weak_pointer (G_OBJECT (priv->last_frame),
                                    (gpointer *)&priv->last_frame);
      priv->last_frame = NULL;
    }

  if (frame)
    {
      priv->last_frame = frame;
      g_object_add_weak_pointer (G_OBJECT (priv->last_frame),
                                  (gpointer *)&priv->last_frame);
    }
}

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
      g_signal_connect_object (child2,
                               "focused",
                               G_CALLBACK (gb_editor_tab_on_frame_focused),
                               tab,
                               G_CONNECT_SWAPPED);
      gtk_widget_grab_focus (child2);
    }
}

GbEditorFrame *
gb_editor_tab_get_last_frame (GbEditorTab *tab)
{
  g_return_val_if_fail (GB_IS_EDITOR_TAB (tab), NULL);

  if (tab->priv->last_frame)
    return tab->priv->last_frame;

  return tab->priv->frame;
}

static void
gb_editor_tab_scroll (GbEditorTab      *tab,
                      GtkDirectionType  dir)
{
  GtkAdjustment *vadj;
  GbEditorFrame *last_frame;
  GtkScrolledWindow *scroller;
  GtkTextMark *insert;
  GtkTextView *view;
  GtkTextBuffer *buffer;
  GdkRectangle rect;
  GtkTextIter iter;
  gdouble amount;
  gdouble value;
  gdouble upper;

  g_assert (GB_IS_EDITOR_TAB (tab));

  last_frame = gb_editor_tab_get_last_frame (tab);
  scroller = last_frame->priv->scrolled_window;
  view = GTK_TEXT_VIEW (last_frame->priv->source_view);
  buffer = GTK_TEXT_BUFFER (last_frame->priv->document);

  insert = gtk_text_buffer_get_insert (buffer);
  gtk_text_buffer_get_iter_at_mark (buffer, &iter, insert);
  gtk_text_view_get_iter_location (view, &iter, &rect);

  amount = (dir == GTK_DIR_UP) ? -rect.height : rect.height;

  vadj = gtk_scrolled_window_get_vadjustment (scroller);
  value = gtk_adjustment_get_value (vadj);
  upper = gtk_adjustment_get_upper (vadj);
  gtk_adjustment_set_value (vadj, CLAMP (value + amount, 0, upper));
}

void
gb_editor_tab_scroll_up (GbEditorTab *tab)
{
  g_return_if_fail (GB_IS_EDITOR_TAB (tab));

  gb_editor_tab_scroll (tab, GTK_DIR_UP);
}

void
gb_editor_tab_scroll_down (GbEditorTab *tab)
{
  g_return_if_fail (GB_IS_EDITOR_TAB (tab));

  gb_editor_tab_scroll (tab, GTK_DIR_DOWN);
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
gb_editor_tab_update_title (GbEditorTab *tab)
{
  GtkSourceFile *file;
  GFile *location;
  gchar *title;

  g_return_if_fail (GB_IS_EDITOR_TAB (tab));

  file = gb_editor_document_get_file (tab->priv->document);
  location = gtk_source_file_get_location (file);

  if (location)
    {
      if (tab->priv->unsaved_id)
        {
          gb_doc_seq_release (tab->priv->unsaved_id);
          tab->priv->unsaved_id = 0;
        }

      title = g_file_get_basename (location);
      gb_tab_set_title (GB_TAB (tab), title);
      g_free (title);
    }
  else
    {
      if (!tab->priv->unsaved_id)
        {
          tab->priv->unsaved_id = gb_doc_seq_acquire ();
          title = g_strdup_printf (_("unsaved %u"), tab->priv->unsaved_id);
          gb_tab_set_title (GB_TAB (tab), title);
          g_free (title);
        }
    }
}

static void
gb_editor_tab_on_notify_location (GbEditorTab   *tab,
                                  GParamSpec    *pspec,
                                  GtkSourceFile *file)
{

  g_return_if_fail (GB_IS_EDITOR_TAB (tab));
  g_return_if_fail (GTK_SOURCE_IS_FILE (file));

  gb_editor_tab_update_title (tab);
}

static void
gb_editor_tab_constructed (GObject *object)
{
  GbEditorTabPrivate *priv;
  GbEditorTab *tab = (GbEditorTab *)object;
  GtkSourceFile *file;

  g_return_if_fail (GB_IS_EDITOR_TAB (tab));

  priv = tab->priv;

  G_OBJECT_CLASS (gb_editor_tab_parent_class)->constructed (object);

  priv->document = g_object_new (GB_TYPE_EDITOR_DOCUMENT,
                                 NULL);
  gb_editor_frame_set_document (priv->frame, priv->document);

  file = gb_editor_document_get_file (priv->document);
  g_signal_connect_object (file,
                           "notify::location",
                           G_CALLBACK (gb_editor_tab_on_notify_location),
                           tab,
                           G_CONNECT_SWAPPED);

  g_signal_connect_object (priv->frame,
                           "focused",
                           G_CALLBACK (gb_editor_tab_on_frame_focused),
                           tab,
                           G_CONNECT_SWAPPED);

  g_signal_connect_object (priv->split_button,
                           "toggled",
                           G_CALLBACK (gb_editor_tab_on_split_toggled),
                           tab,
                           G_CONNECT_SWAPPED);

  gb_editor_tab_update_title (tab);
}

static void
gb_editor_tab_dispose (GObject *object)
{
  GbEditorTabPrivate *priv = GB_EDITOR_TAB (object)->priv;

  g_clear_object (&priv->document);

  if (priv->last_frame)
    {
      g_object_remove_weak_pointer (G_OBJECT (priv->last_frame),
                                    (gpointer *)&priv->last_frame);
      priv->last_frame = NULL;
    }

  if (priv->progress_animation)
    {
      g_object_remove_weak_pointer (G_OBJECT (priv->progress_animation),
                                    (gpointer *)&priv->progress_animation);
      priv->progress_animation = NULL;
    }

  G_OBJECT_CLASS (gb_editor_tab_parent_class)->dispose (object);
}

static void
gb_editor_tab_finalize (GObject *object)
{
  GbEditorTabPrivate *priv = GB_EDITOR_TAB (object)->priv;

  if (priv->unsaved_id)
    {
      gb_doc_seq_release (priv->unsaved_id);
      priv->unsaved_id = 0;
    }

  G_OBJECT_CLASS (gb_editor_tab_parent_class)->finalize (object);
}

static void
gb_editor_tab_class_init (GbEditorTabClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->constructed = gb_editor_tab_constructed;
  object_class->dispose = gb_editor_tab_dispose;
  object_class->finalize = gb_editor_tab_finalize;

  widget_class->grab_focus = gb_editor_tab_grab_focus;

  gtk_widget_class_set_template_from_resource (widget_class,
                                               "/org/gnome/builder/ui/gb-editor-tab.ui");
  gtk_widget_class_bind_template_child_private (widget_class, GbEditorTab, frame);
  gtk_widget_class_bind_template_child_private (widget_class, GbEditorTab, paned);
  gtk_widget_class_bind_template_child_private (widget_class, GbEditorTab, progress_bar);
  gtk_widget_class_bind_template_child_private (widget_class, GbEditorTab, split_button);

  g_type_ensure (GB_TYPE_EDITOR_FRAME);
}

static void
gb_editor_tab_init (GbEditorTab *tab)
{
  tab->priv = gb_editor_tab_get_instance_private (tab);

  gtk_widget_init_template (GTK_WIDGET (tab));
}
