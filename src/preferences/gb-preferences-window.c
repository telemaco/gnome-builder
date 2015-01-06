/* gb-preferences-window.c
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

/*
 * TODO: We will probably have to split all the preferences stuff out into
 *       child widgets just to keep things under control.
 *       Feel free to do that if you beat me to it.
 */

#include <glib/gi18n.h>

#include "gb-preferences-page-editor.h"
#include "gb-preferences-page-git.h"
#include "gb-preferences-page-language.h"
#include "gb-preferences-page-vim.h"
#include "gb-preferences-page.h"
#include "gb-preferences-window.h"

struct _GbPreferencesWindowPrivate
{
  GtkHeaderBar    *right_header_bar;
  GtkSearchEntry  *search_entry;
  GtkSearchBar    *search_bar;
  GtkStack        *stack;

  GtkWidget       *vim_page;

  GSettings       *editor_settings;
};

G_DEFINE_TYPE_WITH_PRIVATE (GbPreferencesWindow, gb_preferences_window,
                            GTK_TYPE_WINDOW)

enum {
  CLOSE,
  LAST_SIGNAL
};

static guint gSignals [LAST_SIGNAL];

GtkWidget *
gb_preferences_window_new (void)
{
  return g_object_new (GB_TYPE_PREFERENCES_WINDOW, NULL);
}

static void
gb_preferences_window_section_changed (GtkStack            *stack,
                                       GParamSpec          *pspec,
                                       GbPreferencesWindow *window)
{
  GtkWidget *visible_child;
  gchar *title = NULL;

  g_return_if_fail (GTK_IS_STACK (stack));
  g_return_if_fail (GB_IS_PREFERENCES_WINDOW (window));

  visible_child = gtk_stack_get_visible_child (stack);
  if (visible_child)
    gtk_container_child_get (GTK_CONTAINER (stack), visible_child,
                             "title", &title,
                             NULL);

  gtk_header_bar_set_title (window->priv->right_header_bar, title);

  g_free (title);
}

static void
gb_preferences_window_close (GbPreferencesWindow *window)
{
  g_return_if_fail (GB_IS_PREFERENCES_WINDOW (window));

  gtk_window_close (GTK_WINDOW (window));
}

static void
gb_preferences_window_search_changed (GbPreferencesWindow *window,
                                      GtkSearchEntry      *entry)
{
  GList *pages;
  GList *iter;
  const gchar *text;
  gchar **keywords;

  g_return_if_fail (GB_IS_PREFERENCES_WINDOW (window));
  g_return_if_fail (GTK_IS_ENTRY (entry));

  text = gtk_entry_get_text (GTK_ENTRY (entry));
  keywords = g_strsplit (text, " ", -1);

  if (g_strv_length (keywords) == 0)
    g_clear_pointer (&keywords, g_strfreev);

  pages = gtk_container_get_children (GTK_CONTAINER (window->priv->stack));

  for (iter = pages; iter; iter = iter->next)
    {
      GbPreferencesPage *page = GB_PREFERENCES_PAGE (iter->data);

      if (0 == gb_preferences_page_set_keywords (page, (const gchar * const *)keywords))
        gtk_widget_set_visible (GTK_WIDGET (page), FALSE);
      else
        gtk_widget_set_visible (GTK_WIDGET (page),
                                gb_preferences_page_get_active (page));
    }

  g_list_free (pages);
  g_strfreev (keywords);
}

static void
gb_preferences_window_constructed (GObject *object)
{
  GbPreferencesWindow *window = (GbPreferencesWindow *)object;

  G_OBJECT_CLASS (gb_preferences_window_parent_class)->constructed (object);

  gtk_search_bar_connect_entry (window->priv->search_bar,
                                GTK_ENTRY (window->priv->search_entry));

  g_signal_connect (window->priv->stack,
                    "notify::visible-child",
                    G_CALLBACK (gb_preferences_window_section_changed),
                    window);
  gb_preferences_window_section_changed (window->priv->stack, NULL, window);

  g_signal_connect_object (window->priv->search_entry,
                           "changed",
                           G_CALLBACK (gb_preferences_window_search_changed),
                           window,
                           G_CONNECT_SWAPPED);
}

static void
gb_preferences_window_finalize (GObject *object)
{
  GbPreferencesWindowPrivate *priv = GB_PREFERENCES_WINDOW (object)->priv;

  g_clear_object (&priv->editor_settings);

  G_OBJECT_CLASS (gb_preferences_window_parent_class)->finalize (object);
}

static void
gb_preferences_window_get_property (GObject    *object,
                                    guint       prop_id,
                                    GValue     *value,
                                    GParamSpec *pspec)
{
  switch (prop_id)
    {
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
gb_preferences_window_set_property (GObject      *object,
                                    guint         prop_id,
                                    const GValue *value,
                                    GParamSpec   *pspec)
{
  switch (prop_id)
    {
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
gb_prefereces_window_vim_mode_changed (GbPreferencesWindow *window,
                                       const gchar         *key,
                                       GSettings           *settings)
{
  gboolean active;
  g_return_if_fail (GB_IS_PREFERENCES_WINDOW (window));
  g_return_if_fail (G_IS_SETTINGS (settings));

  active = g_settings_get_boolean (settings, "vim-mode");
  gtk_widget_set_visible (window->priv->vim_page, active);
  gb_preferences_page_set_active (GB_PREFERENCES_PAGE (window->priv->vim_page),
                                  active);
}

static void
gb_preferences_window_class_init (GbPreferencesWindowClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);
  GtkBindingSet *binding_set;

  object_class->constructed = gb_preferences_window_constructed;
  object_class->finalize = gb_preferences_window_finalize;
  object_class->get_property = gb_preferences_window_get_property;
  object_class->set_property = gb_preferences_window_set_property;

  klass->close = gb_preferences_window_close;

  gSignals [CLOSE] =
    g_signal_new ("close",
                  GB_TYPE_PREFERENCES_WINDOW,
                  G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION,
                  G_STRUCT_OFFSET (GbPreferencesWindowClass, close),
                  NULL, NULL,
                  g_cclosure_marshal_VOID__VOID,
                  G_TYPE_NONE,
                  0);

  binding_set = gtk_binding_set_by_class (klass);
  gtk_binding_entry_add_signal (binding_set, GDK_KEY_Escape, 0, "close", 0);

  gtk_widget_class_set_template_from_resource (widget_class,
                                               "/org/gnome/builder/ui/gb-preferences-window.ui");

  gtk_widget_class_bind_template_child_private (widget_class, GbPreferencesWindow, right_header_bar);
  gtk_widget_class_bind_template_child_private (widget_class, GbPreferencesWindow, search_bar);
  gtk_widget_class_bind_template_child_private (widget_class, GbPreferencesWindow, search_entry);
  gtk_widget_class_bind_template_child_private (widget_class, GbPreferencesWindow, stack);
  gtk_widget_class_bind_template_child_private (widget_class, GbPreferencesWindow, vim_page);

  g_type_ensure (GB_TYPE_PREFERENCES_PAGE_GIT);
  g_type_ensure (GB_TYPE_PREFERENCES_PAGE_EDITOR);
  g_type_ensure (GB_TYPE_PREFERENCES_PAGE_LANGUAGE);
  g_type_ensure (GB_TYPE_PREFERENCES_PAGE_VIM);
}

static void
gb_preferences_window_init (GbPreferencesWindow *self)
{
  self->priv = gb_preferences_window_get_instance_private (self);

  gtk_widget_init_template (GTK_WIDGET (self));

  self->priv->editor_settings = g_settings_new ("org.gnome.builder.editor");
  g_signal_connect_object (self->priv->editor_settings,
                           "changed::vim-mode",
                           G_CALLBACK (gb_prefereces_window_vim_mode_changed),
                           self,
                           G_CONNECT_SWAPPED);
  gb_prefereces_window_vim_mode_changed (self, NULL, self->priv->editor_settings);
}
