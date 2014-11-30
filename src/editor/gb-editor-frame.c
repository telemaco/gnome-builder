/* gb-editor-frame.c
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

#define G_LOG_DOMAIN "editor-frame"

#include <glib/gi18n.h>

#include "gb-editor-frame.h"
#include "gb-log.h"
#include "gb-source-change-gutter-renderer.h"
#include "gb-source-code-assistant-renderer.h"
#include "gb-source-search-highlighter.h"
#include "gb-source-view.h"
#include "gd-tagged-entry.h"
#include "gca-structs.h"
#include "nautilus-floating-bar.h"

struct _GbEditorFramePrivate
{
  /* Widgets owned by GtkBuilder */
  GtkSpinner                    *busy_spinner;
  GbSourceChangeGutterRenderer  *diff_renderer;
  GbSourceCodeAssistantRenderer *code_assistant_renderer;
  NautilusFloatingBar           *floating_bar;
  GtkButton                     *forward_search;
  GtkButton                     *backward_search;
  GtkScrolledWindow             *scrolled_window;
  GtkRevealer                   *search_revealer;
  GdTaggedEntry                 *search_entry;
  GdTaggedEntryTag              *search_entry_tag;
  GbSourceView                  *source_view;

  /* Objects owned by GbEditorFrame */
  GbEditorDocument              *document;
  GtkSourceSearchContext        *search_context;
  GtkSourceSearchSettings       *search_settings;
  GbSourceSearchHighlighter     *search_highlighter;

  /* Signal handler identifiers */
  gulong                         cursor_moved_handler;
};

G_DEFINE_TYPE_WITH_PRIVATE (GbEditorFrame, gb_editor_frame, GTK_TYPE_OVERLAY)

enum {
  PROP_0,
  PROP_DOCUMENT,
  LAST_PROP
};

enum {
  FOCUSED,
  LAST_SIGNAL
};

static GParamSpec *gParamSpecs [LAST_PROP];
static guint       gSignals [FOCUSED];

GtkWidget *
gb_editor_frame_new (void)
{
  return g_object_new (GB_TYPE_EDITOR_FRAME, NULL);
}

/**
 * gb_editor_frame_link:
 * @src: (in): The source frame.
 * @dst: (in): The destination frame.
 *
 * This function is intended to link two #GbEditorFrame instances to use the
 * same backend buffer. This is useful when you want two separate views of the
 * same content.
 */
void
gb_editor_frame_link (GbEditorFrame *src,
                      GbEditorFrame *dst)
{
  g_return_if_fail (GB_IS_EDITOR_FRAME (src));
  g_return_if_fail (GB_IS_EDITOR_FRAME (dst));

  g_object_bind_property (src, "document", dst, "document",
                          G_BINDING_SYNC_CREATE);
}

/**
 * gb_editor_frame_move_next_match:
 *
 * Move to the next search match after the cursor position.
 */
static void
gb_editor_frame_move_next_match (GbEditorFrame *frame)
{
  GbEditorFramePrivate *priv;
  GtkTextBuffer *buffer;
  GtkTextIter select_begin;
  GtkTextIter select_end;
  GtkTextIter match_begin;
  GtkTextIter match_end;
  gboolean has_selection;

  ENTRY;

  g_return_if_fail (GB_IS_EDITOR_FRAME (frame));

  priv = frame->priv;

  buffer = GTK_TEXT_BUFFER (priv->document);

  /*
   * Start by trying from our current location.
   */
  has_selection = gtk_text_buffer_get_selection_bounds (buffer, &select_begin,
                                                        &select_end);
  if (!has_selection)
    if (!gtk_text_iter_forward_char (&select_end))
      gtk_text_buffer_get_end_iter (buffer, &select_end);

  if (gtk_source_search_context_forward (priv->search_context, &select_end,
                                         &match_begin, &match_end))
    GOTO (found_match);

  /*
   * Didn't find anything, let's try from the beginning of the buffer.
   */
  gtk_text_buffer_get_bounds (GTK_TEXT_BUFFER (priv->document),
                              &select_begin, &select_end);

  if (gtk_source_search_context_forward (priv->search_context, &select_begin,
                                         &match_begin, &match_end))
    GOTO (found_match);

  EXIT;

found_match:
  gtk_text_buffer_select_range (GTK_TEXT_BUFFER (priv->document),
                                &match_begin, &match_end);
  gtk_text_view_scroll_to_iter (GTK_TEXT_VIEW (priv->source_view),
                                &match_begin, 0.0, TRUE, 0.5, 0.5);

  EXIT;
}

/**
 * gb_editor_frame_move_previous_match:
 *
 * Move to the first match before the cursor position.
 */
static void
gb_editor_frame_move_previous_match (GbEditorFrame *frame)
{
  GbEditorFramePrivate *priv;
  GtkTextIter select_begin;
  GtkTextIter select_end;
  GtkTextIter match_begin;
  GtkTextIter match_end;

  ENTRY;

  g_return_if_fail (GB_IS_EDITOR_FRAME (frame));

  priv = frame->priv;

  gtk_text_buffer_get_selection_bounds (GTK_TEXT_BUFFER (priv->document),
                                        &select_begin, &select_end);

  if (gtk_source_search_context_backward (priv->search_context, &select_begin,
                                          &match_begin, &match_end))
    GOTO (found_match);
  else
    {
      /*
       * We need to wrap around from the end to find the last search result.
       */
      gtk_text_buffer_get_end_iter (GTK_TEXT_BUFFER (priv->document),
                                    &select_begin);
      if (gtk_source_search_context_backward (priv->search_context,
                                              &select_begin, &match_begin,
                                              &match_end))
        GOTO (found_match);
    }

  EXIT;

found_match:
  gtk_text_buffer_select_range (GTK_TEXT_BUFFER (priv->document),
                                &match_begin, &match_end);
  gtk_text_view_scroll_to_iter (GTK_TEXT_VIEW (priv->source_view),
                                &match_begin, 0.0, TRUE, 0.5, 0.5);

  EXIT;
}

/**
 * gb_editor_frame_on_cursor_moved:
 *
 * Update cursor ruler in the floating bar upon changing of insert text mark.
 */
static void
gb_editor_frame_on_cursor_moved (GbEditorFrame    *frame,
                                 GbEditorDocument *document)
{
  GtkSourceView *source_view;
  GtkTextBuffer *buffer;
  GtkTextIter iter;
  GtkTextMark *mark;
  gchar *text;
  guint ln;
  guint col;

  g_return_if_fail (GB_IS_EDITOR_FRAME (frame));
  g_return_if_fail (GB_IS_EDITOR_DOCUMENT (document));

  source_view = GTK_SOURCE_VIEW (frame->priv->source_view);
  buffer = GTK_TEXT_BUFFER (document);

  mark = gtk_text_buffer_get_insert (buffer);
  gtk_text_buffer_get_iter_at_mark (buffer, &iter, mark);

  ln = gtk_text_iter_get_line (&iter);
  col = gtk_source_view_get_visual_column (source_view, &iter);

  text = g_strdup_printf (_("Line %u, Column %u"), ln + 1, col + 1);
  nautilus_floating_bar_set_primary_label (frame->priv->floating_bar, text);
  g_free (text);
}

/**
 * gb_editor_frame_connect:
 *
 * Attach to dynamic signals for the #GtkTextBuffer. Create any objects that
 * are dependent on the buffer.
 */
static void
gb_editor_frame_connect (GbEditorFrame    *frame,
                         GbEditorDocument *document)
{
  GbEditorFramePrivate *priv;
  GbSourceChangeMonitor *monitor;
  GbSourceCodeAssistant *code_assistant;

  ENTRY;

  g_return_if_fail (GB_IS_EDITOR_FRAME (frame));
  g_return_if_fail (GB_IS_EDITOR_DOCUMENT (document));
  g_return_if_fail (!frame->priv->document);

  priv = frame->priv;

  /*
   * Save the document for later.
   */
  priv->document = g_object_ref (document);
  gtk_text_view_set_buffer (GTK_TEXT_VIEW (priv->source_view),
                            GTK_TEXT_BUFFER (priv->document));

  /*
   * Connect change monitor to gutter.
   */
  monitor = gb_editor_document_get_change_monitor (document);
  g_object_set (priv->diff_renderer,
                "change-monitor", monitor,
                NULL);

  /*
   * Connect code assistance to gutter and spinner.
   */
  code_assistant = gb_editor_document_get_code_assistant (document);
  g_object_set (priv->code_assistant_renderer,
                "code-assistant", code_assistant,
                NULL);
  g_object_bind_property (code_assistant, "active",
                          priv->busy_spinner, "active",
                          G_BINDING_SYNC_CREATE);
  g_object_bind_property (code_assistant, "active",
                          priv->busy_spinner, "visible",
                          G_BINDING_SYNC_CREATE);

  /*
   * Create search defaults for this frame.
   */
  priv->search_context = g_object_new (GTK_SOURCE_TYPE_SEARCH_CONTEXT,
                                       "buffer", priv->document,
                                       "settings", priv->search_settings,
                                       "highlight", TRUE,
                                       NULL);
  g_object_set (priv->search_highlighter,
                "search-context", priv->search_context,
                NULL);

  /*
   * Connect to cursor-moved signal to update cursor position label.
   */
  if (GB_IS_EDITOR_DOCUMENT (priv->document))
    {
      priv->cursor_moved_handler =
        g_signal_connect_swapped (priv->document,
                                  "cursor-moved",
                                  G_CALLBACK (gb_editor_frame_on_cursor_moved),
                                  frame);
    }

  EXIT;
}

/**
 * gb_editor_frame_disconnect:
 *
 * Cleanup any signals or objects that are related to the #GtkTextBuffer.
 */
static void
gb_editor_frame_disconnect (GbEditorFrame *frame)
{
  GbEditorFramePrivate *priv;

  ENTRY;

  g_return_if_fail (GB_IS_EDITOR_FRAME (frame));

  priv = frame->priv;

  if (priv->document)
    {
      g_signal_handler_disconnect (priv->document, priv->cursor_moved_handler);
      priv->cursor_moved_handler = 0;
    }

  g_object_set (priv->diff_renderer,
                "change-monitor", NULL,
                NULL);

  g_object_set (priv->code_assistant_renderer,
                "code-assistant", NULL,
                NULL);

  g_object_set (priv->search_highlighter,
                "search-context", NULL,
                NULL);

  g_clear_object (&priv->document);
  g_clear_object (&priv->search_context);

  EXIT;
}

/**
 * gb_editor_frame_get_document:
 *
 * Gets the #GbEditorDocument associated with the #GbEditorFrame.
 *
 * Returns: (transfer none): A #GbEditorDocument.
 */
GbEditorDocument *
gb_editor_frame_get_document (GbEditorFrame *frame)
{
  g_return_val_if_fail (GB_IS_EDITOR_FRAME (frame), NULL);

  return frame->priv->document;
}

/**
 * gb_editor_frame_set_document:
 *
 * Set the #GbEditorDocument to be displayed by the #GbEditorFrame.
 */
void
gb_editor_frame_set_document (GbEditorFrame    *frame,
                              GbEditorDocument *document)
{
  g_return_if_fail (GB_IS_EDITOR_FRAME (frame));
  g_return_if_fail (GB_IS_EDITOR_DOCUMENT (document));

  if (document != frame->priv->document)
    {
      gb_editor_frame_disconnect (frame);
      if (document)
        gb_editor_frame_connect (frame, document);
      g_object_notify_by_pspec (G_OBJECT (frame), gParamSpecs [PROP_DOCUMENT]);
    }
}

/**
 * gb_editor_frame_on_focus_in_event:
 *
 * Handle the "focus-in-event" on the #GbSourceView. Ensure the search entry
 * is hidden and we are no longer highlighting search results.
 */
static gboolean
gb_editor_frame_on_focus_in_event (GbEditorFrame *frame,
                                   GdkEvent      *event,
                                   GbSourceView  *source_view)
{
  g_return_val_if_fail (GB_IS_EDITOR_FRAME (frame), FALSE);
  g_return_val_if_fail (GB_IS_SOURCE_VIEW (source_view), FALSE);

  gtk_revealer_set_reveal_child (frame->priv->search_revealer, FALSE);
  gtk_source_search_context_set_highlight (frame->priv->search_context, FALSE);

  g_signal_emit (frame, gSignals [FOCUSED], 0);

  return GDK_EVENT_PROPAGATE;
}

/**
 * gb_editor_frame_on_populate_popup:
 *
 * Update the popup menu to include choices for language highlight.
 */
static void
gb_editor_frame_on_populate_popup (GbEditorFrame *frame,
                                   GtkWidget     *popup,
                                   GtkTextView   *text_view)
{
  g_return_if_fail (GB_IS_EDITOR_FRAME (frame));
  g_return_if_fail (GTK_IS_TEXT_VIEW (text_view));
  g_return_if_fail (GTK_IS_WIDGET (popup));

  /* TODO: Highlight Language Widget */
}

/**
 * gb_editor_frame_on_push_snippet:
 *
 * Update snippet context with the filename of the current document.
 */
static void
gb_editor_frame_on_push_snippet (GbEditorFrame          *frame,
                                 GbSourceSnippet        *snippet,
                                 GbSourceSnippetContext *context,
                                 GtkTextIter            *iter,
                                 GbSourceView           *source_view)
{
  GtkSourceFile *source_file;
  GFile *file;

  g_return_if_fail (GB_IS_SOURCE_VIEW (source_view));
  g_return_if_fail (GB_IS_SOURCE_SNIPPET (snippet));
  g_return_if_fail (GB_IS_SOURCE_SNIPPET_CONTEXT (context));
  g_return_if_fail (iter);
  g_return_if_fail (GB_IS_EDITOR_FRAME (frame));

  source_file = gb_editor_document_get_file (frame->priv->document);
  file = gtk_source_file_get_location (source_file);
  g_assert (!file || G_IS_FILE (file));

  if (file)
    {
      gchar *name;

      name = g_file_get_basename (file);
      gb_source_snippet_context_add_variable (context, "filename", name);
      g_free (name);
    }
}

static gboolean
gb_editor_frame_on_search_entry_key_press (GbEditorFrame *frame,
                                           GdkEventKey   *event,
                                           GdTaggedEntry *entry)
{
  g_assert (GD_IS_TAGGED_ENTRY (entry));
  g_assert (GB_IS_EDITOR_FRAME (frame));

  if (event->keyval == GDK_KEY_Escape)
    {
      gtk_revealer_set_reveal_child (frame->priv->search_revealer, FALSE);
      gb_source_view_set_show_shadow (frame->priv->source_view, FALSE);
      gtk_widget_grab_focus (GTK_WIDGET (frame->priv->source_view));
      return GDK_EVENT_STOP;
    }

  return GDK_EVENT_PROPAGATE;
}

static void
gb_editor_frame_on_search_entry_activate (GbEditorFrame *frame,
                                          GdTaggedEntry *entry)
{
  g_assert (GD_IS_TAGGED_ENTRY (entry));
  g_assert (GB_IS_EDITOR_FRAME (frame));

  gb_editor_frame_move_next_match (frame);
  gtk_widget_grab_focus (GTK_WIDGET (frame->priv->source_view));
}

static void
gb_editor_frame_on_forward_search_clicked (GbEditorFrame *frame,
                                           GtkButton     *button)
{
  g_return_if_fail (GB_IS_EDITOR_FRAME (frame));
  g_return_if_fail (GTK_IS_BUTTON (button));

  gb_editor_frame_move_next_match (frame);
}

static void
gb_editor_frame_on_backward_search_clicked (GbEditorFrame *frame,
                                            GtkButton     *button)
{
  g_return_if_fail (GB_IS_EDITOR_FRAME (frame));
  g_return_if_fail (GTK_IS_BUTTON (button));

  gb_editor_frame_move_previous_match (frame);
}

/**
 * gb_editor_frame_on_begin_search:
 *
 * Show the search machinery when a request to begin a search has occurred.
 */
static void
gb_editor_frame_on_begin_search (GbEditorFrame    *frame,
                                 GtkDirectionType  direction,
                                 const gchar      *search_text,
                                 GbSourceView     *source_view)
{
  GbEditorFramePrivate *priv;

  ENTRY;

  g_return_if_fail (GB_IS_EDITOR_FRAME (frame));
  g_return_if_fail (GB_IS_SOURCE_VIEW (source_view));

  priv = frame->priv;

  if (search_text)
    gtk_entry_set_text (GTK_ENTRY (priv->search_entry), search_text);

  gtk_revealer_set_reveal_child (priv->search_revealer, TRUE);
  gtk_source_search_context_set_highlight (priv->search_context, TRUE);
  gtk_widget_grab_focus (GTK_WIDGET (priv->search_entry));

  if (search_text)
    {
      if (direction == GTK_DIR_DOWN)
        gb_editor_frame_move_next_match (frame);
      else if (direction == GTK_DIR_UP)
        gb_editor_frame_move_previous_match (frame);
    }
  else
    {
      const gchar *text;
      guint len;

      text = gtk_entry_get_text (GTK_ENTRY (priv->search_entry));
      len = g_utf8_strlen (text, -1);
      gtk_editable_select_region (GTK_EDITABLE (priv->search_entry), 0, len);
    }

  EXIT;
}

static gboolean
gb_editor_frame_on_query_tooltip (GbEditorFrame *frame,
                                  gint           x,
                                  gint           y,
                                  gboolean       keyboard_mode,
                                  GtkTooltip    *tooltip,
                                  GbSourceView  *source_view)
{
  GbEditorFramePrivate *priv;
  GbSourceCodeAssistant *code_assistant;
  GtkTextIter iter;
  GArray *ar;
  gboolean ret = FALSE;
  guint line;
  guint i;

  g_assert (GB_IS_SOURCE_VIEW (source_view));
  g_assert (GB_IS_EDITOR_FRAME (frame));

  priv = frame->priv;

  code_assistant = gb_editor_document_get_code_assistant (priv->document);
  if (!code_assistant)
    return FALSE;

  ar = gb_source_code_assistant_get_diagnostics (code_assistant);
  if (!ar)
    return FALSE;

  gtk_text_view_window_to_buffer_coords (GTK_TEXT_VIEW (source_view),
                                         GTK_TEXT_WINDOW_WIDGET,
                                         x, y, &x, &y);

  gtk_text_view_get_iter_at_location (GTK_TEXT_VIEW (source_view),
                                      &iter, x, y);

  line = gtk_text_iter_get_line (&iter);

  for (i = 0; i < ar->len; i++)
    {
      GcaDiagnostic *diag;
      guint j;

      diag = &g_array_index (ar, GcaDiagnostic, i);

      for (j = 0; j < diag->locations->len; j++)
        {
          GcaSourceRange *loc;

          loc = &g_array_index (diag->locations, GcaSourceRange, j);

          if ((loc->begin.line <= line) && (loc->end.line >= line))
            {
              gtk_tooltip_set_text (tooltip, diag->message);
              ret = TRUE;
              goto cleanup;
            }
        }
    }

cleanup:
  g_array_unref (ar);

  return ret;
}

static void
gb_editor_frame_grab_focus (GtkWidget *widget)
{
  GbEditorFrame *frame = (GbEditorFrame *)widget;

  g_return_if_fail (GB_IS_EDITOR_FRAME (frame));

  gtk_widget_grab_focus (GTK_WIDGET (frame->priv->source_view));
}

static void
gb_editor_frame_finalize (GObject *object)
{
  GbEditorFrame *frame = GB_EDITOR_FRAME (object);

  gb_editor_frame_disconnect (frame);

  g_clear_object (&frame->priv->code_assistant_renderer);
  g_clear_object (&frame->priv->diff_renderer);
  g_clear_object (&frame->priv->search_settings);
  g_clear_object (&frame->priv->search_highlighter);

  G_OBJECT_CLASS (gb_editor_frame_parent_class)->finalize (object);
}

static void
gb_editor_frame_constructed (GObject *object)
{
  GbSourceChangeMonitor *monitor = NULL;
  GbEditorFramePrivate *priv = GB_EDITOR_FRAME (object)->priv;
  GtkSourceGutter *gutter;
  GbEditorFrame *frame = GB_EDITOR_FRAME (object);

  G_OBJECT_CLASS (gb_editor_frame_parent_class)->constructed (object);

  if (priv->document)
    monitor = gb_editor_document_get_change_monitor (priv->document);

  gutter = gtk_source_view_get_gutter (GTK_SOURCE_VIEW (priv->source_view),
                                       GTK_TEXT_WINDOW_LEFT);

  priv->diff_renderer = g_object_new (GB_TYPE_SOURCE_CHANGE_GUTTER_RENDERER,
                                      "change-monitor", monitor,
                                      "size", 2,
                                      "visible", TRUE,
                                      "xpad", 1,
                                      NULL);
  priv->diff_renderer = g_object_ref (priv->diff_renderer);
  gtk_source_gutter_insert (gutter,
                            GTK_SOURCE_GUTTER_RENDERER (priv->diff_renderer),
                            0);

  priv->code_assistant_renderer =
    g_object_new (GB_TYPE_SOURCE_CODE_ASSISTANT_RENDERER,
                  "code-assistant", NULL,
                  "size", 16,
                  "visible", TRUE,
                  NULL);
  priv->code_assistant_renderer = g_object_ref (priv->code_assistant_renderer);
  gtk_source_gutter_insert (gutter,
                            GTK_SOURCE_GUTTER_RENDERER (priv->code_assistant_renderer),
                            -50);

  priv->search_settings = g_object_new (GTK_SOURCE_TYPE_SEARCH_SETTINGS,
                                        NULL);
  g_object_bind_property (priv->search_entry, "text",
                          priv->search_settings, "search-text",
                          G_BINDING_SYNC_CREATE);

  priv->search_highlighter =
    g_object_new (GB_TYPE_SOURCE_SEARCH_HIGHLIGHTER,
                  "search-settings", priv->search_settings,
                  NULL);
  g_object_set (priv->source_view,
                "search-highlighter", priv->search_highlighter,
                NULL);
  g_object_bind_property (priv->search_revealer, "reveal-child",
                          priv->source_view, "show-shadow",
                          G_BINDING_SYNC_CREATE);

  g_signal_connect_object (priv->source_view,
                           "focus-in-event",
                           G_CALLBACK (gb_editor_frame_on_focus_in_event),
                           frame,
                           G_CONNECT_SWAPPED);

  g_signal_connect_object (priv->source_view,
                           "populate-popup",
                           G_CALLBACK (gb_editor_frame_on_populate_popup),
                           frame,
                           G_CONNECT_SWAPPED);

  g_signal_connect_object (priv->source_view,
                           "push-snippet",
                           G_CALLBACK (gb_editor_frame_on_push_snippet),
                           frame,
                           G_CONNECT_SWAPPED);

  g_signal_connect_object (priv->source_view,
                           "begin-search",
                           G_CALLBACK (gb_editor_frame_on_begin_search),
                           frame,
                           G_CONNECT_SWAPPED);

  g_signal_connect_object (priv->source_view,
                           "query-tooltip",
                           G_CALLBACK (gb_editor_frame_on_query_tooltip),
                           frame,
                           G_CONNECT_SWAPPED);

  g_signal_connect_object (priv->search_entry,
                           "key-press-event",
                           G_CALLBACK (gb_editor_frame_on_search_entry_key_press),
                           frame,
                           G_CONNECT_SWAPPED);

  g_signal_connect_object (priv->search_entry,
                           "activate",
                           G_CALLBACK (gb_editor_frame_on_search_entry_activate),
                           frame,
                           G_CONNECT_SWAPPED);

  g_signal_connect_object (priv->forward_search,
                           "clicked",
                           G_CALLBACK (gb_editor_frame_on_forward_search_clicked),
                           frame,
                           G_CONNECT_SWAPPED);

  g_signal_connect_object (priv->backward_search,
                           "clicked",
                           G_CALLBACK (gb_editor_frame_on_backward_search_clicked),
                           frame,
                           G_CONNECT_SWAPPED);
}

static void
gb_editor_frame_get_property (GObject    *object,
                              guint       prop_id,
                              GValue     *value,
                              GParamSpec *pspec)
{
  GbEditorFrame *self = GB_EDITOR_FRAME (object);

  switch (prop_id)
    {
    case PROP_DOCUMENT:
      g_value_set_object (value, gb_editor_frame_get_document (self));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
gb_editor_frame_set_property (GObject      *object,
                              guint         prop_id,
                              const GValue *value,
                              GParamSpec   *pspec)
{
  GbEditorFrame *self = GB_EDITOR_FRAME (object);

  switch (prop_id)
    {
    case PROP_DOCUMENT:
      gb_editor_frame_set_document (self, g_value_get_object (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
gb_editor_frame_class_init (GbEditorFrameClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->constructed = gb_editor_frame_constructed;
  object_class->finalize = gb_editor_frame_finalize;
  object_class->get_property = gb_editor_frame_get_property;
  object_class->set_property = gb_editor_frame_set_property;

  widget_class->grab_focus = gb_editor_frame_grab_focus;

  gParamSpecs [PROP_DOCUMENT] =
    g_param_spec_object ("document",
                         _("Document"),
                         _("The document for the editor."),
                         GB_TYPE_EDITOR_DOCUMENT,
                         (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class, PROP_DOCUMENT,
                                   gParamSpecs [PROP_DOCUMENT]);

  gSignals [FOCUSED] =
    g_signal_new ("focused",
                  GB_TYPE_EDITOR_FRAME,
                  G_SIGNAL_RUN_LAST,
                  0,
                  NULL,
                  NULL,
                  g_cclosure_marshal_VOID__VOID,
                  G_TYPE_NONE,
                  0);

  gtk_widget_class_set_template_from_resource (widget_class,
                                               "/org/gnome/builder/ui/gb-editor-frame.ui");
  gtk_widget_class_bind_template_child_private (widget_class, GbEditorFrame, busy_spinner);
  gtk_widget_class_bind_template_child_private (widget_class, GbEditorFrame, floating_bar);
  gtk_widget_class_bind_template_child_private (widget_class, GbEditorFrame, forward_search);
  gtk_widget_class_bind_template_child_private (widget_class, GbEditorFrame, backward_search);
  gtk_widget_class_bind_template_child_private (widget_class, GbEditorFrame, scrolled_window);
  gtk_widget_class_bind_template_child_private (widget_class, GbEditorFrame, search_revealer);
  gtk_widget_class_bind_template_child_private (widget_class, GbEditorFrame, search_entry);
  gtk_widget_class_bind_template_child_private (widget_class, GbEditorFrame, source_view);

  g_type_ensure (GB_TYPE_SOURCE_VIEW);
  g_type_ensure (GD_TYPE_TAGGED_ENTRY);
  g_type_ensure (NAUTILUS_TYPE_FLOATING_BAR);
}

static void
gb_editor_frame_init (GbEditorFrame *self)
{
  self->priv = gb_editor_frame_get_instance_private (self);

  gtk_widget_init_template (GTK_WIDGET (self));
}
