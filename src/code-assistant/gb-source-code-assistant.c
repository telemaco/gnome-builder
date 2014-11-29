/* gb-source-code-assistant.c
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

#define G_LOG_DOMAIN "code-assistant"

#include <glib/gi18n.h>

#include "gb-source-code-assistant.h"
#include "gca-structs.h"

struct _GbSourceCodeAssistantPrivate
{
  GtkTextBuffer *buffer;
  GArray        *diagnostics;
  gulong         changed_handler;
  guint          parse_timeout;
  guint          active : 1;
};

enum {
  PROP_0,
  PROP_ACTIVE,
  PROP_BUFFER,
  LAST_PROP
};

enum {
  CHANGED,
  LAST_SIGNAL
};

G_DEFINE_TYPE_WITH_PRIVATE (GbSourceCodeAssistant,
                            gb_source_code_assistant,
                            G_TYPE_OBJECT)

static GParamSpec *gParamSpecs [LAST_PROP];
static guint       gSignals [LAST_SIGNAL];

#define PARSE_TIMEOUT_MSEC 350

GbSourceCodeAssistant *
gb_source_code_assistant_new (GtkTextBuffer *buffer)
{
  return g_object_new (GB_TYPE_SOURCE_CODE_ASSISTANT,
                       "buffer", buffer,
                       NULL);
}

/**
 * gb_source_code_assistant_get_diagnostics:
 * @assistant: (in): A #GbSourceCodeAssistant.
 *
 * Fetches the diagnostics for the buffer. Free the result with
 * g_array_unref().
 *
 * Returns: (transfer full): A #GArray of #GcaDiagnostic.
 */
GArray *
gb_source_code_assistant_get_diagnostics (GbSourceCodeAssistant *assistant)
{
  g_return_val_if_fail (GB_IS_SOURCE_CODE_ASSISTANT (assistant), NULL);

  if (assistant->priv->diagnostics)
    return g_array_ref (assistant->priv->diagnostics);

  return NULL;
}

static gboolean
gb_source_code_assistant_do_parse (gpointer data)
{
  GbSourceCodeAssistant *assistant = data;

  assistant->priv->parse_timeout = 0;

  return G_SOURCE_REMOVE;
}

static void
gb_source_code_assistant_queue_parse (GbSourceCodeAssistant *assistant)
{
  g_return_if_fail (GB_IS_SOURCE_CODE_ASSISTANT (assistant));

  if (assistant->priv->parse_timeout)
    g_source_remove (assistant->priv->parse_timeout);

  assistant->priv->parse_timeout =
    g_timeout_add (PARSE_TIMEOUT_MSEC,
                   gb_source_code_assistant_do_parse,
                   assistant);
}

static void
gb_source_code_assistant_buffer_changed (GbSourceCodeAssistant *assistant,
                                         GtkTextBuffer         *buffer)
{
  g_return_if_fail (GB_IS_SOURCE_CODE_ASSISTANT (assistant));
  g_return_if_fail (GTK_IS_TEXT_BUFFER (buffer));

  gb_source_code_assistant_queue_parse (assistant);
}

static void
gb_source_code_assistant_disconnect (GbSourceCodeAssistant *assistant)
{
  g_return_if_fail (GB_IS_SOURCE_CODE_ASSISTANT (assistant));

  g_signal_handler_disconnect (assistant->priv->buffer,
                               assistant->priv->changed_handler);
  assistant->priv->changed_handler = 0;
}

static void
gb_source_code_assistant_connect (GbSourceCodeAssistant *assistant)
{
  g_return_if_fail (GB_IS_SOURCE_CODE_ASSISTANT (assistant));

  assistant->priv->changed_handler =
    g_signal_connect_object (assistant->priv->buffer,
                             "changed",
                             G_CALLBACK (gb_source_code_assistant_buffer_changed),
                             assistant,
                             G_CONNECT_SWAPPED);
}

/**
 * gb_source_code_assistant_get_buffer:
 * @assistant: (in): A #GbSourceCodeAssistant.
 *
 * Fetches the underlying text buffer.
 *
 * Returns: (transfer none): A #GtkTextBuffer.
 */
GtkTextBuffer *
gb_source_code_assistant_get_buffer (GbSourceCodeAssistant *assistant)
{
  g_return_val_if_fail (GB_IS_SOURCE_CODE_ASSISTANT (assistant), NULL);

  return assistant->priv->buffer;
}

static void
gb_source_code_assistant_set_buffer (GbSourceCodeAssistant *assistant,
                                     GtkTextBuffer         *buffer)
{
  GbSourceCodeAssistantPrivate *priv;

  g_return_if_fail (GB_IS_SOURCE_CODE_ASSISTANT (assistant));

  priv = assistant->priv;

  if (priv->buffer != buffer)
    {
      if (priv->buffer)
        {
          gb_source_code_assistant_disconnect (assistant);
          g_object_remove_weak_pointer (G_OBJECT (priv->buffer),
                                        (gpointer *)&priv->buffer);
          priv->buffer = NULL;
        }

      if (buffer)
        {
          priv->buffer = buffer;
          g_object_add_weak_pointer (G_OBJECT (priv->buffer),
                                     (gpointer *)&priv->buffer);
          gb_source_code_assistant_connect (assistant);
        }

      g_object_notify_by_pspec (G_OBJECT (assistant),
                                gParamSpecs [PROP_BUFFER]);
    }
}

/**
 * gb_source_code_assistant_get_active:
 * @assistant: (in): A #GbSourceCodeAssistant.
 *
 * Fetches the "active" property, indicating if the code assistanace service
 * is currently parsing the buffer.
 *
 * Returns: %TRUE if the file is being parsed.
 */
gboolean
gb_source_code_assistant_get_active (GbSourceCodeAssistant *assistant)
{
  g_return_val_if_fail (GB_IS_SOURCE_CODE_ASSISTANT (assistant), FALSE);

  return assistant->priv->active;
}

static void
gb_source_code_assistant_finalize (GObject *object)
{
  GbSourceCodeAssistantPrivate *priv;

  priv = GB_SOURCE_CODE_ASSISTANT (object)->priv;

  if (priv->parse_timeout)
    {
      g_source_remove (priv->parse_timeout);
      priv->parse_timeout = 0;
    }

  if (priv->buffer)
    {
      g_object_add_weak_pointer (G_OBJECT (priv->buffer),
                                 (gpointer *)&priv->buffer);
      priv->buffer = NULL;
    }

  G_OBJECT_CLASS (gb_source_code_assistant_parent_class)->finalize (object);
}

static void
gb_source_code_assistant_get_property (GObject    *object,
                                       guint       prop_id,
                                       GValue     *value,
                                       GParamSpec *pspec)
{
  GbSourceCodeAssistant *self = GB_SOURCE_CODE_ASSISTANT (object);

  switch (prop_id)
    {
    case PROP_ACTIVE:
      g_value_set_boolean (value, gb_source_code_assistant_get_active (self));
      break;

    case PROP_BUFFER:
      g_value_set_object (value, gb_source_code_assistant_get_buffer (self));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
gb_source_code_assistant_set_property (GObject      *object,
                                       guint         prop_id,
                                       const GValue *value,
                                       GParamSpec   *pspec)
{
  GbSourceCodeAssistant *self = GB_SOURCE_CODE_ASSISTANT (object);

  switch (prop_id)
    {
    case PROP_BUFFER:
      gb_source_code_assistant_set_buffer (self, g_value_get_object (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
gb_source_code_assistant_class_init (GbSourceCodeAssistantClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = gb_source_code_assistant_finalize;
  object_class->get_property = gb_source_code_assistant_get_property;
  object_class->set_property = gb_source_code_assistant_set_property;

  gParamSpecs [PROP_BUFFER] =
    g_param_spec_object ("buffer",
                         _("Buffer"),
                         _("The buffer "),
                         GTK_TYPE_TEXT_BUFFER,
                         (G_PARAM_READWRITE |
                          G_PARAM_CONSTRUCT_ONLY |
                          G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class, PROP_BUFFER,
                                   gParamSpecs [PROP_BUFFER]);

  gSignals [CHANGED] =
    g_signal_new ("changed",
                  GB_TYPE_SOURCE_CODE_ASSISTANT,
                  G_SIGNAL_RUN_FIRST,
                  G_STRUCT_OFFSET (GbSourceCodeAssistantClass, changed),
                  NULL,
                  NULL,
                  g_cclosure_marshal_VOID__VOID,
                  G_TYPE_NONE,
                  0);
}

static void
gb_source_code_assistant_init (GbSourceCodeAssistant *assistant)
{
  assistant->priv = gb_source_code_assistant_get_instance_private (assistant);
}
