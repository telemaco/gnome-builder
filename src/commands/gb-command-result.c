/* gb-command-result.c
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

#include "gb-command-result.h"

struct _GbCommandResultPrivate
{
  gchar *command_text;
  gchar *result_text;
  guint is_error : 1;
  guint is_running : 1;
};

G_DEFINE_TYPE_WITH_PRIVATE (GbCommandResult, gb_command_result, G_TYPE_OBJECT)

enum {
  PROP_0,
  PROP_COMMAND_TEXT,
  PROP_IS_ERROR,
  PROP_IS_RUNNING,
  PROP_RESULT_TEXT,
  LAST_PROP
};

static GParamSpec *gParamSpecs [LAST_PROP];

GbCommandResult *
gb_command_result_new (void)
{
  return g_object_new (GB_TYPE_COMMAND_RESULT, NULL);
}

const gchar *
gb_command_result_get_command_text (GbCommandResult *result)
{
  g_return_val_if_fail (GB_IS_COMMAND_RESULT (result), NULL);

  return result->priv->command_text;
}

void
gb_command_result_set_command_text (GbCommandResult *result,
                                    const gchar     *command_text)
{
  g_return_if_fail (GB_IS_COMMAND_RESULT (result));

  if (result->priv->command_text != command_text)
    {
      g_free (result->priv->command_text);
      result->priv->command_text = g_strdup (command_text);
      g_object_notify_by_pspec (G_OBJECT (result),
                                gParamSpecs [PROP_COMMAND_TEXT]);
    }
}

const gchar *
gb_command_result_get_result_text (GbCommandResult *result)
{
  g_return_val_if_fail (GB_IS_COMMAND_RESULT (result), NULL);

  return result->priv->result_text;
}

void
gb_command_result_set_result_text (GbCommandResult *result,
                                   const gchar     *result_text)
{
  g_return_if_fail (GB_IS_COMMAND_RESULT (result));

  if (result->priv->result_text != result_text)
    {
      g_free (result->priv->result_text);
      result->priv->result_text = g_strdup (result_text);
      g_object_notify_by_pspec (G_OBJECT (result),
                                gParamSpecs [PROP_RESULT_TEXT]);
    }
}

gboolean
gb_command_result_get_is_running (GbCommandResult *result)
{
  g_return_val_if_fail (GB_IS_COMMAND_RESULT (result), FALSE);

  return result->priv->is_running;
}

void
gb_command_result_set_is_running (GbCommandResult *result,
                                  gboolean         is_running)
{
  g_return_if_fail (GB_IS_COMMAND_RESULT (result));

  if (result->priv->is_running != is_running)
    {
      result->priv->is_running = !!is_running;
      g_object_notify_by_pspec (G_OBJECT (result),
                                gParamSpecs [PROP_IS_RUNNING]);
    }
}

gboolean
gb_command_result_get_is_error (GbCommandResult *result)
{
  g_return_val_if_fail (GB_IS_COMMAND_RESULT (result), FALSE);

  return result->priv->is_error;
}

void
gb_command_result_set_is_error (GbCommandResult *result,
                                gboolean         is_error)
{
  g_return_if_fail (GB_IS_COMMAND_RESULT (result));

  if (result->priv->is_error != is_error)
    {
      result->priv->is_error = !!is_error;
      g_object_notify_by_pspec (G_OBJECT (result),
                                gParamSpecs [PROP_IS_ERROR]);
    }
}

static void
gb_command_result_finalize (GObject *object)
{
  GbCommandResultPrivate *priv = GB_COMMAND_RESULT (object)->priv;

  g_clear_pointer (&priv->command_text, g_free);
  g_clear_pointer (&priv->result_text, g_free);

  G_OBJECT_CLASS (gb_command_result_parent_class)->finalize (object);
}

static void
gb_command_result_get_property (GObject    *object,
                                guint       prop_id,
                                GValue     *value,
                                GParamSpec *pspec)
{
  GbCommandResult *self = GB_COMMAND_RESULT (object);

  switch (prop_id)
    {
    case PROP_COMMAND_TEXT:
      g_value_set_string (value, gb_command_result_get_command_text (self));
      break;

    case PROP_IS_ERROR:
      g_value_set_boolean (value, gb_command_result_get_is_error (self));
      break;

    case PROP_IS_RUNNING:
      g_value_set_boolean (value, gb_command_result_get_is_running (self));
      break;

    case PROP_RESULT_TEXT:
      g_value_set_string (value, gb_command_result_get_result_text (self));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
gb_command_result_set_property (GObject      *object,
                                guint         prop_id,
                                const GValue *value,
                                GParamSpec   *pspec)
{
  GbCommandResult *self = GB_COMMAND_RESULT (object);

  switch (prop_id)
    {
    case PROP_COMMAND_TEXT:
      gb_command_result_set_command_text (self, g_value_get_string (value));
      break;

    case PROP_IS_ERROR:
      gb_command_result_set_is_error (self, g_value_get_boolean (value));
      break;

    case PROP_IS_RUNNING:
      gb_command_result_set_is_running (self, g_value_get_boolean (value));
      break;

    case PROP_RESULT_TEXT:
      gb_command_result_set_result_text (self, g_value_get_string (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
gb_command_result_class_init (GbCommandResultClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = gb_command_result_finalize;
  object_class->get_property = gb_command_result_get_property;
  object_class->set_property = gb_command_result_set_property;

  gParamSpecs [PROP_COMMAND_TEXT] =
    g_param_spec_string ("command-text",
                         _("Command Text"),
                         _("The command text if any."),
                         NULL,
                         (G_PARAM_READWRITE |
                          G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class, PROP_COMMAND_TEXT,
                                   gParamSpecs [PROP_COMMAND_TEXT]);

  gParamSpecs [PROP_IS_ERROR] =
    g_param_spec_boolean ("is-error",
                          _("Is Error"),
                          _("If the result is an error."),
                          FALSE,
                          (G_PARAM_READWRITE |
                           G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class, PROP_IS_ERROR,
                                   gParamSpecs [PROP_IS_ERROR]);

  gParamSpecs [PROP_IS_RUNNING] =
    g_param_spec_boolean ("is-running",
                          _("Is Running"),
                          _("If the command is still running."),
                          FALSE,
                          (G_PARAM_READWRITE |
                           G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class, PROP_IS_RUNNING,
                                   gParamSpecs [PROP_IS_RUNNING]);

  gParamSpecs [PROP_RESULT_TEXT] =
    g_param_spec_string ("result-text",
                         _("Result Text"),
                         _("The result text if any."),
                         NULL,
                         (G_PARAM_READWRITE |
                          G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class, PROP_RESULT_TEXT,
                                   gParamSpecs [PROP_RESULT_TEXT]);
}

static void
gb_command_result_init (GbCommandResult *self)
{
  self->priv = gb_command_result_get_instance_private (self);
}
