/* gb-source-code-assistant-renderer.c
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

#include "gb-source-code-assistant.h"
#include "gb-source-code-assistant-renderer.h"

struct _GbSourceCodeAssistantRendererPrivate
{
  GbSourceCodeAssistant *code_assistant;
  GArray                *diagnostics;
  gulong                 changed_handler;
};

enum
{
  PROP_0,
  PROP_CODE_ASSISTANT,
  LAST_PROP
};

G_DEFINE_TYPE_WITH_PRIVATE (GbSourceCodeAssistantRenderer,
                            gb_source_code_assistant_renderer,
                            GTK_SOURCE_TYPE_GUTTER_RENDERER_PIXBUF)

static GParamSpec *gParamSpecs [LAST_PROP];

GbSourceCodeAssistant *
gb_source_code_assistant_renderer_get_code_assistant (GbSourceCodeAssistantRenderer *renderer)
{
  g_return_val_if_fail (GB_IS_SOURCE_CODE_ASSISTANT_RENDERER (renderer), NULL);

  return renderer->priv->code_assistant;
}

static void
gb_source_code_assistant_renderer_changed (GbSourceCodeAssistantRenderer *renderer,
                                           GbSourceCodeAssistant         *code_assistant)
{
  GbSourceCodeAssistantRendererPrivate *priv;

  g_return_if_fail (GB_IS_SOURCE_CODE_ASSISTANT_RENDERER (renderer));
  g_return_if_fail (GB_IS_SOURCE_CODE_ASSISTANT (code_assistant));

  priv = renderer->priv;

  if (priv->diagnostics)
    {
      g_array_unref (priv->diagnostics);
      priv->diagnostics = NULL;
    }

  priv->diagnostics = gb_source_code_assistant_get_diagnostics (code_assistant);

  gtk_source_gutter_renderer_queue_draw (GTK_SOURCE_GUTTER_RENDERER (renderer));
}

static void
gb_source_code_assistant_renderer_connect (GbSourceCodeAssistantRenderer *renderer)
{
  g_return_if_fail (GB_IS_SOURCE_CODE_ASSISTANT_RENDERER (renderer));

  renderer->priv->changed_handler =
    g_signal_connect_object (renderer->priv->code_assistant,
                             "changed",
                             G_CALLBACK (gb_source_code_assistant_renderer_changed),
                             renderer,
                             G_CONNECT_SWAPPED);
}

static void
gb_source_code_assistant_renderer_disconnect (GbSourceCodeAssistantRenderer *renderer)
{
  g_return_if_fail (GB_IS_SOURCE_CODE_ASSISTANT_RENDERER (renderer));

  g_signal_handler_disconnect (renderer->priv->code_assistant,
                               renderer->priv->changed_handler);
  renderer->priv->code_assistant = 0;
}

void
gb_source_code_assistant_renderer_set_code_assistant (GbSourceCodeAssistantRenderer *renderer,
                                                      GbSourceCodeAssistant         *code_assistant)
{
  GbSourceCodeAssistantRendererPrivate *priv;

  g_return_if_fail (GB_IS_SOURCE_CODE_ASSISTANT_RENDERER (renderer));
  g_return_if_fail (!code_assistant || GB_IS_SOURCE_CODE_ASSISTANT (code_assistant));

  priv = renderer->priv;

  if (code_assistant != priv->code_assistant)
    {
      if (priv->code_assistant)
        {
          gb_source_code_assistant_renderer_disconnect (renderer);
          g_clear_object (&priv->code_assistant);
        }

      if (code_assistant)
        {
          priv->code_assistant = g_object_ref (code_assistant);
          gb_source_code_assistant_renderer_connect (renderer);
        }

      gtk_source_gutter_renderer_queue_draw (GTK_SOURCE_GUTTER_RENDERER (renderer));

      g_object_notify_by_pspec (G_OBJECT (renderer),
                                gParamSpecs [PROP_CODE_ASSISTANT]);
    }
}

static void
gb_source_code_assistant_renderer_finalize (GObject *object)
{
  GbSourceCodeAssistantRendererPrivate *priv;

  priv = GB_SOURCE_CODE_ASSISTANT_RENDERER (object)->priv;

  g_clear_object (&priv->code_assistant);

  G_OBJECT_CLASS (gb_source_code_assistant_renderer_parent_class)->finalize (object);
}

static void
gb_source_code_assistant_renderer_get_property (GObject    *object,
                                                guint       prop_id,
                                                GValue     *value,
                                                GParamSpec *pspec)
{
  GbSourceCodeAssistantRenderer *self = GB_SOURCE_CODE_ASSISTANT_RENDERER (object);

  switch (prop_id)
    {
    case PROP_CODE_ASSISTANT:
      g_value_set_object (value, gb_source_code_assistant_renderer_get_code_assistant (self));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
gb_source_code_assistant_renderer_set_property (GObject      *object,
                                                guint         prop_id,
                                                const GValue *value,
                                                GParamSpec   *pspec)
{
  GbSourceCodeAssistantRenderer *self = GB_SOURCE_CODE_ASSISTANT_RENDERER (object);

  switch (prop_id)
    {
    case PROP_CODE_ASSISTANT:
      gb_source_code_assistant_renderer_set_code_assistant (self, g_value_get_object (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
gb_source_code_assistant_renderer_class_init (GbSourceCodeAssistantRendererClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = gb_source_code_assistant_renderer_finalize;
  object_class->get_property = gb_source_code_assistant_renderer_get_property;
  object_class->set_property = gb_source_code_assistant_renderer_set_property;

  gParamSpecs [PROP_CODE_ASSISTANT] =
    g_param_spec_object ("code-assistant",
                         _("Code Assistant"),
                         _("The code assistant to render."),
                         GB_TYPE_SOURCE_CODE_ASSISTANT,
                         (G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class, PROP_CODE_ASSISTANT,
                                   gParamSpecs [PROP_CODE_ASSISTANT]);
}

static void
gb_source_code_assistant_renderer_init (GbSourceCodeAssistantRenderer *renderer)
{
  renderer->priv = gb_source_code_assistant_renderer_get_instance_private (renderer);
}
