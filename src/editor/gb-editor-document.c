/* gb-editor-document.c
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

#define G_LOG_DOMAIN "editor-document"

#include <glib/gi18n.h>
#include <gtksourceview/gtksource.h>

#include "gb-editor-document.h"
#include "gb-log.h"
#include "gca-structs.h"

struct _GbEditorDocumentPrivate
{
  GtkSourceFile         *file;
  GbSourceChangeMonitor *change_monitor;
  GbSourceCodeAssistant *code_assistant;
};

enum {
  PROP_0,
  PROP_CHANGE_MONITOR,
  PROP_FILE,
  PROP_STYLE_SCHEME_NAME,
  LAST_PROP
};

enum {
  CURSOR_MOVED,
  LAST_SIGNAL
};

G_DEFINE_TYPE_WITH_PRIVATE (GbEditorDocument, gb_editor_document, GTK_SOURCE_TYPE_BUFFER)

static  GParamSpec *gParamSpecs [LAST_PROP];
static guint gSignals [LAST_SIGNAL];

GbEditorDocument *
gb_editor_document_new (void)
{
  return g_object_new (GB_TYPE_EDITOR_DOCUMENT, NULL);
}

GbSourceChangeMonitor *
gb_editor_document_get_change_monitor (GbEditorDocument *document)
{
  g_return_val_if_fail (GB_IS_EDITOR_DOCUMENT (document), NULL);

  return document->priv->change_monitor;
}

GbSourceCodeAssistant *
gb_editor_document_get_code_assistant (GbEditorDocument *document)
{
  g_return_val_if_fail (GB_IS_EDITOR_DOCUMENT (document), NULL);

  return document->priv->code_assistant;
}

GtkSourceFile *
gb_editor_document_get_file (GbEditorDocument *document)
{
  g_return_val_if_fail (GB_IS_EDITOR_DOCUMENT (document), NULL);

  return document->priv->file;
}

void
gb_editor_document_set_file (GbEditorDocument *document,
                             GtkSourceFile    *file)
{
  GbEditorDocumentPrivate *priv;

  g_return_if_fail (GB_IS_EDITOR_DOCUMENT (document));
  g_return_if_fail (!file || GTK_SOURCE_IS_FILE (file));

  priv = document->priv;

  if (file != priv->file)
    {
      g_clear_object (&priv->file);

      if (file)
        {
          priv->file = g_object_ref (file);
          g_object_bind_property (priv->file, "location",
                                  priv->change_monitor, "file",
                                  G_BINDING_SYNC_CREATE);
        }

      g_object_notify_by_pspec (G_OBJECT (document), gParamSpecs [PROP_FILE]);
    }
}

static void
gb_editor_document_set_style_scheme_name (GbEditorDocument *document,
                                          const gchar      *style_scheme_name)
{
  GtkSourceStyleSchemeManager *manager;
  GtkSourceStyleScheme *scheme;

  g_return_if_fail (GB_IS_EDITOR_DOCUMENT (document));

  manager = gtk_source_style_scheme_manager_get_default ();
  scheme = gtk_source_style_scheme_manager_get_scheme (manager,
                                                       style_scheme_name);
  gtk_source_buffer_set_style_scheme (GTK_SOURCE_BUFFER (document), scheme);
}

static void
gb_editor_document_mark_set (GtkTextBuffer     *buffer,
                             const GtkTextIter *iter,
                             GtkTextMark       *mark)
{
  if (GTK_TEXT_BUFFER_CLASS (gb_editor_document_parent_class)->mark_set)
    GTK_TEXT_BUFFER_CLASS (gb_editor_document_parent_class)->mark_set (buffer, iter, mark);

  if (mark == gtk_text_buffer_get_insert (buffer))
    g_signal_emit (buffer, gSignals [CURSOR_MOVED], 0);
}

static void
gb_editor_document_changed (GtkTextBuffer *buffer)
{
  g_assert (GB_IS_EDITOR_DOCUMENT (buffer));

  g_signal_emit (buffer, gSignals [CURSOR_MOVED], 0);

  GTK_TEXT_BUFFER_CLASS (gb_editor_document_parent_class)->changed (buffer);
}

static void
gb_editor_document_add_diagnostic (GbEditorDocument *document,
                                   GcaDiagnostic    *diag,
                                   GcaSourceRange   *range)
{
  GtkTextBuffer *buffer;
  GtkTextIter begin;
  GtkTextIter end;
  guint column;

  g_assert (GB_IS_EDITOR_DOCUMENT (document));
  g_assert (diag);
  g_assert (range);

  if (range->begin.line == -1 || range->end.line == -1)
    return;

  buffer = GTK_TEXT_BUFFER (document);

  gtk_text_buffer_get_iter_at_line (buffer, &begin, range->begin.line);
  for (column = range->begin.column; column; column--)
    if (gtk_text_iter_ends_line (&begin) || !gtk_text_iter_forward_char (&begin))
      break;

  gtk_text_buffer_get_iter_at_line (buffer, &end, range->end.line);
  for (column = range->end.column; column; column--)
    if (gtk_text_iter_ends_line (&end) || !gtk_text_iter_forward_char (&end))
      break;

  if (gtk_text_iter_equal (&begin, &end))
    gtk_text_iter_forward_to_line_end (&end);

  gtk_text_buffer_apply_tag_by_name (buffer, "ErrorTag", &begin, &end);
}

static void
apply_tag_style (GbEditorDocument *document,
                 GtkTextTag       *tag,
                 const gchar      *style_id)
{
  GtkSourceStyleScheme *scheme;
  GtkSourceStyle *style;
  gboolean background_set;
  gboolean bold_set;
  gboolean foreground_set;
  gboolean line_background_set;
  gchar *str;

  g_return_if_fail (GB_IS_EDITOR_DOCUMENT (document));

  scheme = gtk_source_buffer_get_style_scheme (GTK_SOURCE_BUFFER (document));
  if (!scheme)
    return;

  style = gtk_source_style_scheme_get_style (scheme, style_id);
  if (!style)
    return;

  g_object_get (style,
                "background-set", &background_set,
                "bold-set", &bold_set,
                "foreground-set", &foreground_set,
                "line-background-set", &line_background_set,
                NULL);

  if (background_set)
    {
      g_object_get (style, "background", &str, NULL);
      g_object_set (tag, "background", str, NULL);
      g_free (str);
    }
  else
    g_object_set (tag, "background-set", FALSE, NULL);

  if (bold_set)
    {
      PangoWeight weight;
      gboolean bold;

      g_object_get (style, "bold", &bold, NULL);
      weight = bold ? PANGO_WEIGHT_NORMAL : PANGO_WEIGHT_BOLD;
      g_object_set (tag, "weight", weight, NULL);
    }
  else
    g_object_set (tag, "weight-set", FALSE, NULL);

  if (foreground_set)
    {
      g_object_get (style, "foreground", &str, NULL);
      g_object_set (tag, "foreground", str, NULL);
      g_free (str);
    }
  else
    g_object_set (tag, "foreground-set", FALSE, NULL);

  if (line_background_set)
    {
      g_object_get (style, "line-background", &str, NULL);
      g_object_set (tag, "paragraph-background", str, NULL);
      g_free (str);
    }
  else
    g_object_set (tag, "paragraph-background-set", FALSE, NULL);
}

static GtkTextTag *
gb_editor_document_get_error_tag (GbEditorDocument *document)
{
  GtkTextBuffer *buffer;
  GtkTextTagTable *tag_table;
  GtkTextTag *tag;

  g_return_val_if_fail (GB_IS_EDITOR_DOCUMENT (document), NULL);

  buffer = GTK_TEXT_BUFFER (document);
  tag_table = gtk_text_buffer_get_tag_table (buffer);
  tag = gtk_text_tag_table_lookup (tag_table, "ErrorTag");

  if (!tag)
    {
      tag = gtk_text_buffer_create_tag (buffer, "ErrorTag",
                                        "underline", PANGO_UNDERLINE_ERROR,
                                        NULL);
      apply_tag_style (document, tag, "def:error");
    }

  return tag;
}

static void
gb_editor_document_notify_style_scheme (GbEditorDocument *document,
                                        GParamSpec       *pspec,
                                        gpointer          unused)
{
  GtkTextTag *tag;

  g_return_if_fail (GB_IS_EDITOR_DOCUMENT (document));

  tag = gb_editor_document_get_error_tag (document);
  apply_tag_style (document, tag, "def:error");
}

static void
gb_editor_document_code_assistant_changed (GbEditorDocument      *document,
                                           GbSourceCodeAssistant *code_assistant)
{
  GtkTextIter begin;
  GtkTextIter end;
  GtkTextTag *tag;
  GArray *ar;
  guint i;

  g_return_if_fail (GB_IS_EDITOR_DOCUMENT (document));
  g_return_if_fail (GB_IS_SOURCE_CODE_ASSISTANT (code_assistant));

  /*
   * Update all of the error tags in the buffer based on the diagnostics
   * returned from code assistance. We might want to find a way to do this
   * iteratively in the background based interactivity.
   */

  tag = gb_editor_document_get_error_tag (document);

  gtk_text_buffer_get_bounds (GTK_TEXT_BUFFER (document), &begin, &end);
  gtk_text_buffer_remove_tag (GTK_TEXT_BUFFER (document), tag, &begin, &end);

  ar = gb_source_code_assistant_get_diagnostics (code_assistant);

  for (i = 0; i < ar->len; i++)
    {
      GcaDiagnostic *diag;
      guint j;

      diag = &g_array_index (ar, GcaDiagnostic, i);

      for (j = 0; j < diag->locations->len; j++)
        {
          GcaSourceRange *range;

          range = &g_array_index (diag->locations, GcaSourceRange, j);
          gb_editor_document_add_diagnostic (document, diag, range);
        }
    }

  g_array_unref (ar);
}

static void
gb_editor_document_finalize (GObject *object)
{
  GbEditorDocumentPrivate *priv = GB_EDITOR_DOCUMENT (object)->priv;

  ENTRY;

  g_clear_object (&priv->file);
  g_clear_object (&priv->change_monitor);
  g_clear_object (&priv->code_assistant);

  G_OBJECT_CLASS(gb_editor_document_parent_class)->finalize (object);

  EXIT;
}

static void
gb_editor_document_get_property (GObject    *object,
                                 guint       prop_id,
                                 GValue     *value,
                                 GParamSpec *pspec)
{
  GbEditorDocument *self = (GbEditorDocument *)object;

  switch (prop_id)
    {
    case PROP_CHANGE_MONITOR:
      g_value_set_object (value, gb_editor_document_get_change_monitor (self));
      break;

    case PROP_FILE:
      g_value_set_object (value, gb_editor_document_get_file (self));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    }
}

static void
gb_editor_document_set_property (GObject      *object,
                                 guint         prop_id,
                                 const GValue *value,
                                 GParamSpec   *pspec)
{
  GbEditorDocument *self = (GbEditorDocument *)object;

  switch (prop_id)
    {
    case PROP_FILE:
      gb_editor_document_set_file (self, g_value_get_object (value));
      break;

    case PROP_STYLE_SCHEME_NAME:
      gb_editor_document_set_style_scheme_name (self,
                                                g_value_get_string (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
gb_editor_document_class_init (GbEditorDocumentClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkTextBufferClass *text_buffer_class = GTK_TEXT_BUFFER_CLASS (klass);

  object_class->finalize = gb_editor_document_finalize;
  object_class->get_property = gb_editor_document_get_property;
  object_class->set_property = gb_editor_document_set_property;

  text_buffer_class->mark_set = gb_editor_document_mark_set;
  text_buffer_class->changed = gb_editor_document_changed;

  gParamSpecs [PROP_CHANGE_MONITOR] =
    g_param_spec_object ("change-monitor",
                         _("Change Monitor"),
                         _("The change monitor for the backing file."),
                         GB_TYPE_SOURCE_CHANGE_MONITOR,
                         (G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class, PROP_CHANGE_MONITOR,
                                   gParamSpecs [PROP_CHANGE_MONITOR]);

  gParamSpecs [PROP_FILE] =
    g_param_spec_object ("file",
                         _("File"),
                         _("The backing file for the document."),
                         GTK_SOURCE_TYPE_FILE,
                         (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class, PROP_FILE,
                                   gParamSpecs [PROP_FILE]);

  gParamSpecs [PROP_STYLE_SCHEME_NAME] =
    g_param_spec_string ("style-scheme-name",
                         _("Style Scheme Name"),
                         _("The style scheme name."),
                         NULL,
                         (G_PARAM_WRITABLE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class, PROP_STYLE_SCHEME_NAME,
                                   gParamSpecs [PROP_STYLE_SCHEME_NAME]);

  gSignals[CURSOR_MOVED] =
    g_signal_new ("cursor-moved",
                  G_OBJECT_CLASS_TYPE (object_class),
                  G_SIGNAL_RUN_LAST,
                  G_STRUCT_OFFSET (GbEditorDocumentClass, cursor_moved),
                  NULL,
                  NULL,
                  g_cclosure_marshal_VOID__VOID,
                  G_TYPE_NONE,
                  0);
}

static void
gb_editor_document_init (GbEditorDocument *document)
{
  document->priv = gb_editor_document_get_instance_private (document);

  document->priv->file = gtk_source_file_new ();
  document->priv->change_monitor = gb_source_change_monitor_new (GTK_TEXT_BUFFER (document));
  document->priv->code_assistant = gb_source_code_assistant_new (GTK_TEXT_BUFFER (document));

  g_signal_connect_object (document->priv->code_assistant,
                           "changed",
                           G_CALLBACK (gb_editor_document_code_assistant_changed),
                           document,
                           G_CONNECT_SWAPPED);

  g_signal_connect (document,
                    "notify::style-scheme",
                    G_CALLBACK (gb_editor_document_notify_style_scheme),
                    NULL);
}
