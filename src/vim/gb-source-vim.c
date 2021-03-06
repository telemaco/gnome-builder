/* gb-source-vim.c
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

#define G_LOG_DOMAIN "vim"

#include <errno.h>
#include <glib/gi18n.h>
#include <gtksourceview/gtksource.h>
#include <stdio.h>
#include <stdlib.h>

#include "gb-source-vim.h"
#include "gb-string.h"

#ifndef GB_SOURCE_VIM_EXTERNAL
# include "gb-source-view.h"
#endif

/*
 *  I can't possibly know all of VIM features. So this doesn't implement
 *  all of them. Just the main ones I know about. File bugs if you like.
 *
 * TODO:
 *
 *  - Registers
 *  - Multi-character verb/noun/modifiers.
 *  - Marks
 *  - Jumps
 */

/**
 * GbSourceVimCommandFunc:
 * @vim: The #GbSourceVim instance.
 * @count: The number modifier for the command.
 * @modifier: A potential trailing modifer character.
 *
 * This is a function prototype for commands to implement themselves. They
 * can potentially use the count to perform the operation multiple times.
 *
 * However, not all commands support this or will use it.
 */
typedef void (*GbSourceVimCommandFunc) (GbSourceVim        *vim,
                                        guint               count,
                                        gchar               modifier);

/**
 * GbSourceVimOperation:
 * @command_text: text command to execute.
 *
 * This is a function declaration for functions that can process an operation.
 * Operations are things that are entered into the command mode entry.
 *
 * Unfortunately, we already have a command abstraction that should possibly
 * be renamed. But such is life!
 */
typedef void (*GbSourceVimOperation) (GbSourceVim *vim,
                                      const gchar *command_text);

struct _GbSourceVimPrivate
{
  GtkTextView             *text_view;
  GString                 *phrase;
  GtkTextMark             *selection_anchor_begin;
  GtkTextMark             *selection_anchor_end;
  GtkSourceSearchContext  *search_context;
  GtkSourceSearchSettings *search_settings;
  GtkDirectionType         search_direction;
  GPtrArray               *captured_events;
  GbSourceVimMode          mode;
  GSettings               *vim_settings;
  gulong                   key_press_event_handler;
  gulong                   event_after_handler;
  gulong                   key_release_event_handler;
  gulong                   focus_in_event_handler;
  gulong                   focus_out_event_handler;
  gulong                   mark_set_handler;
  gulong                   delete_range_handler;
  guint                    target_line_offset;
  guint                    stash_line;
  guint                    stash_line_offset;
  guint                    anim_timeout;
  guint                    scroll_off;
  gchar                    recording_trigger;
  gchar                    recording_modifier;
  guint                    enabled : 1;
  guint                    connected : 1;
  guint                    recording : 1;
  guint                    in_replay : 1;
  guint                    in_ctrl_w : 1;
};

typedef enum
{
  GB_SOURCE_VIM_PAGE_UP,
  GB_SOURCE_VIM_PAGE_DOWN,
  GB_SOURCE_VIM_HALF_PAGE_UP,
  GB_SOURCE_VIM_HALF_PAGE_DOWN,
} GbSourceVimPageDirectionType;

typedef enum
{
  GB_SOURCE_VIM_ALIGNMENT_NONE,
  GB_SOURCE_VIM_ALIGNMENT_KEEP,
  GB_SOURCE_VIM_ALIGNMENT_TOP,
  GB_SOURCE_VIM_ALIGNMENT_BOTTOM
} GbSourceVimAlignment;

typedef enum
{
  GB_SOURCE_VIM_COMMAND_NOOP,
  GB_SOURCE_VIM_COMMAND_MOVEMENT,
  GB_SOURCE_VIM_COMMAND_CHANGE,
  GB_SOURCE_VIM_COMMAND_JUMP,
} GbSourceVimCommandType;

typedef enum
{
  GB_SOURCE_VIM_COMMAND_FLAG_NONE,
  GB_SOURCE_VIM_COMMAND_FLAG_REQUIRES_MODIFIER = 1 << 0,
  GB_SOURCE_VIM_COMMAND_FLAG_VISUAL            = 1 << 1,
  GB_SOURCE_VIM_COMMAND_FLAG_MOTION_EXCLUSIVE  = 1 << 2,
  GB_SOURCE_VIM_COMMAND_FLAG_MOTION_LINEWISE   = 1 << 3,
} GbSourceVimCommandFlags;

/**
 * GbSourceVimCommand:
 *
 * This structure encapsulates what we need to know about a command before
 * we can dispatch it. GB_EDiTOR_VIM_COMMAND_FLAG_REQUIRES_MODIFIER in flags
 * means there needs to be a supplimental character provided after the key.
 * Such an example would be "dd", "dw", "yy", or "gg".
 */
typedef struct
{
  GbSourceVimCommandFunc  func;
  GbSourceVimCommandType  type;
  gchar                   key;
  GbSourceVimCommandFlags flags;
} GbSourceVimCommand;

typedef struct
{
  gfloat yalign;
  gint   line;
} GbSourceVimAdjustedScroll;

typedef enum
{
  GB_SOURCE_VIM_PHRASE_FAILED,
  GB_SOURCE_VIM_PHRASE_SUCCESS,
  GB_SOURCE_VIM_PHRASE_NEED_MORE,
} GbSourceVimPhraseStatus;

typedef struct
{
  guint count;
  gchar key;
  gchar modifier;
} GbSourceVimPhrase;

typedef struct
{
  gunichar jump_to;
  gunichar jump_from;
  guint    depth;
} MatchingBracketState;

enum
{
  PROP_0,
  PROP_ENABLED,
  PROP_MODE,
  PROP_PHRASE,
  PROP_SEARCH_TEXT,
  PROP_SEARCH_DIRECTION,
  PROP_TEXT_VIEW,
  LAST_PROP
};

enum
{
  BEGIN_SEARCH,
  COMMAND_VISIBILITY_TOGGLED,
  EXECUTE_COMMAND,
  JUMP_TO_DOC,
  SPLIT,
  SWITCH_TO_FILE,
  LAST_SIGNAL
};

enum
{
  CLASS_0,
  CLASS_SPACE,
  CLASS_SPECIAL,
  CLASS_WORD,
};

G_DEFINE_TYPE_WITH_PRIVATE (GbSourceVim, gb_source_vim, G_TYPE_OBJECT)

static GHashTable *gCommands;
static GParamSpec *gParamSpecs [LAST_PROP];
static guint       gSignals [LAST_SIGNAL];

static void text_iter_swap (GtkTextIter *a,
                            GtkTextIter *b);
static void gb_source_vim_select_range (GbSourceVim *vim,
                                        GtkTextIter *insert_iter,
                                        GtkTextIter *selection_iter);
static void gb_source_vim_cmd_select_line (GbSourceVim *vim,
                                           guint        count,
                                           gchar        modifier);
static void gb_source_vim_cmd_delete (GbSourceVim *vim,
                                      guint        count,
                                      gchar        modifier);
static void gb_source_vim_cmd_delete_to_end (GbSourceVim *vim,
                                             guint        count,
                                             gchar        modifier);
static void gb_source_vim_cmd_insert_before_line (GbSourceVim *vim,
                                                  guint        count,
                                                  gchar        modifier);
static GbSourceVimAdjustedScroll*
gb_source_vim_adjust_scroll (GbSourceVim          *vim,
                             gint                  line,
                             GbSourceVimAlignment  aligment);
static void
gb_source_vim_ensure_scroll (GbSourceVim *vim);

GbSourceVim *
gb_source_vim_new (GtkTextView *text_view)
{
  g_return_val_if_fail (GTK_IS_TEXT_VIEW (text_view), NULL);

  return g_object_new (GB_TYPE_SOURCE_VIM,
                       "text-view", text_view,
                       NULL);
}

/**
 * gb_source_vim_recording_begin:
 * @trigger: the character used to trigger recording
 *
 * This begins capturing keys so that they may be replayed later using a
 * command such as ".". @trigger is the key that was used to begin this
 * recording such as (A, a, i, I, s, etc).
 *
 * gb_source_vim_recording_end() must be called to complete the capture.
 */
static void
gb_source_vim_recording_begin (GbSourceVim *vim,
                               gchar        trigger,
                               gchar        modifier)
{
  g_return_if_fail (!vim->priv->recording || vim->priv->in_replay);
  g_return_if_fail (trigger);

  if (vim->priv->in_replay)
    return;

  if (vim->priv->captured_events->len)
    g_ptr_array_remove_range (vim->priv->captured_events, 0,
                              vim->priv->captured_events->len);

  g_assert (vim->priv->captured_events->len == 0);

  vim->priv->recording = TRUE;
  vim->priv->recording_trigger = trigger;
  vim->priv->recording_modifier = modifier;
}

static void
gb_source_vim_recording_capture (GbSourceVim *vim,
                                 GdkEvent    *event)
{
  GdkEvent *copy;

  g_return_if_fail (GB_IS_SOURCE_VIM (vim));
  g_return_if_fail (event);
  g_return_if_fail ((event->type == GDK_KEY_PRESS) ||
                    (event->type == GDK_KEY_RELEASE));

  copy = gdk_event_copy (event);
  g_ptr_array_add (vim->priv->captured_events, copy);
}

static void
gb_source_vim_recording_replay (GbSourceVim *vim)
{
  GbSourceVimCommand *cmd;
  guint i;

  g_return_if_fail (GB_IS_SOURCE_VIM (vim));
  g_return_if_fail (vim->priv->recording_trigger);
  g_return_if_fail (!vim->priv->in_replay);

  cmd = g_hash_table_lookup (gCommands,
                             GINT_TO_POINTER (vim->priv->recording_trigger));
  if (!cmd)
    return;

  vim->priv->in_replay = TRUE;

  cmd->func (vim, 1, vim->priv->recording_modifier);

  for (i = 0; i < vim->priv->captured_events->len; i++)
    {
      GdkEventKey *event;

      event = g_ptr_array_index (vim->priv->captured_events, i);
      event->time = GDK_CURRENT_TIME;
      gtk_widget_event (GTK_WIDGET (vim->priv->text_view), (GdkEvent *)event);
    }

  vim->priv->in_replay = FALSE;
}

static void
gb_source_vim_recording_end (GbSourceVim *vim)
{
  g_return_if_fail (vim->priv->recording);

  vim->priv->recording = FALSE;
}

static int
gb_source_vim_classify (gunichar ch)
{
  switch (ch)
    {
    case ' ':
    case '\t':
    case '\n':
      return CLASS_SPACE;

    case '"': case '\'':
    case '(': case ')':
    case '{': case '}':
    case '[': case ']':
    case '<': case '>':
    case '-': case '+': case '*': case '/':
    case '!': case '@': case '#': case '$': case '%':
    case '^': case '&': case ':': case ';': case '?':
    case '|': case '=': case '\\': case '.': case ',':
      return CLASS_SPECIAL;

    case '_':
    default:
      return CLASS_WORD;
    }
}

static guint
gb_source_vim_get_line_offset (GbSourceVim *vim)
{
  GtkTextBuffer *buffer;
  GtkTextMark *insert;
  GtkTextIter iter;

  buffer = gtk_text_view_get_buffer (vim->priv->text_view);
  insert = gtk_text_buffer_get_insert (buffer);
  gtk_text_buffer_get_iter_at_mark (buffer, &iter, insert);

  return gtk_text_iter_get_line_offset (&iter);
}

static void
gb_source_vim_save_position (GbSourceVim *vim)
{
  GtkTextBuffer *buffer;
  GtkTextIter iter;
  GtkTextIter selection;

  g_return_if_fail (GB_IS_SOURCE_VIM (vim));

  buffer = gtk_text_view_get_buffer (vim->priv->text_view);
  gtk_text_buffer_get_selection_bounds (buffer, &iter, &selection);

  vim->priv->stash_line = gtk_text_iter_get_line (&iter);
  vim->priv->stash_line_offset = gtk_text_iter_get_line_offset (&iter);
}

static void
gb_source_vim_restore_position (GbSourceVim *vim)
{
  GtkTextBuffer *buffer;
  GtkTextIter iter;
  guint offset;

  g_return_if_fail (GB_IS_SOURCE_VIM (vim));

  buffer = gtk_text_view_get_buffer (vim->priv->text_view);
  gtk_text_buffer_get_iter_at_line (buffer, &iter, vim->priv->stash_line);

  for (offset = vim->priv->stash_line_offset; offset; offset--)
    if (!gtk_text_iter_forward_char (&iter))
      break;

  gtk_text_buffer_select_range (buffer, &iter, &iter);

  vim->priv->target_line_offset = gb_source_vim_get_line_offset (vim);
}

static void
gb_source_vim_set_selection_anchor (GbSourceVim       *vim,
                                    const GtkTextIter *begin,
                                    const GtkTextIter *end)
{
  GbSourceVimPrivate *priv;
  GtkTextBuffer *buffer;
  GtkTextIter left_anchor;
  GtkTextIter right_anchor;

  g_assert (GB_IS_SOURCE_VIM (vim));
  g_assert (begin);
  g_assert (end);

  priv = vim->priv;

  buffer = gtk_text_view_get_buffer (priv->text_view);

  if (gtk_text_iter_compare (begin, end) < 0)
    {
      gtk_text_iter_assign (&left_anchor, begin);
      gtk_text_iter_assign (&right_anchor, end);
    }
  else
    {
      gtk_text_iter_assign (&left_anchor, end);
      gtk_text_iter_assign (&right_anchor, begin);
    }

  if (!priv->selection_anchor_begin)
    priv->selection_anchor_begin =
      gtk_text_buffer_create_mark (buffer,
                                   "selection-anchor-begin",
                                   &left_anchor,
                                   TRUE);
  else
    gtk_text_buffer_move_mark (buffer,
                               priv->selection_anchor_begin,
                               &left_anchor);

  if (!priv->selection_anchor_end)
    priv->selection_anchor_end =
      gtk_text_buffer_create_mark (buffer,
                                   "selection-anchor-end",
                                   &right_anchor,
                                   FALSE);
  else
    gtk_text_buffer_move_mark (buffer,
                               priv->selection_anchor_end,
                               &right_anchor);
}

static void
gb_source_vim_ensure_anchor_selected (GbSourceVim *vim)
{
  GbSourceVimPrivate *priv;
  GtkTextBuffer *buffer;
  GtkTextMark *selection_mark;
  GtkTextMark *insert_mark;
  GtkTextIter anchor_begin;
  GtkTextIter anchor_end;
  GtkTextIter insert_iter;
  GtkTextIter selection_iter;

  g_assert (GB_IS_SOURCE_VIM (vim));

  priv = vim->priv;

  if (!priv->selection_anchor_begin || !priv->selection_anchor_end)
    return;

  buffer = gtk_text_view_get_buffer (priv->text_view);

  gtk_text_buffer_get_iter_at_mark (buffer, &anchor_begin,
                                    priv->selection_anchor_begin);
  gtk_text_buffer_get_iter_at_mark (buffer, &anchor_end,
                                    priv->selection_anchor_end);

  insert_mark = gtk_text_buffer_get_insert (buffer);
  gtk_text_buffer_get_iter_at_mark (buffer, &insert_iter, insert_mark);

  selection_mark = gtk_text_buffer_get_selection_bound (buffer);
  gtk_text_buffer_get_iter_at_mark (buffer, &selection_iter, selection_mark);

  if ((gtk_text_iter_compare (&selection_iter, &anchor_end) < 0) &&
      (gtk_text_iter_compare (&insert_iter, &anchor_end) < 0))
    {
      if (gtk_text_iter_compare (&insert_iter, &selection_iter) < 0)
        gb_source_vim_select_range (vim, &insert_iter, &anchor_end);
      else
        gb_source_vim_select_range (vim, &anchor_end, &selection_iter);
    }
  else if ((gtk_text_iter_compare (&selection_iter, &anchor_begin) > 0) &&
           (gtk_text_iter_compare (&insert_iter, &anchor_begin) > 0))
    {
      if (gtk_text_iter_compare (&insert_iter, &selection_iter) < 0)
        gb_source_vim_select_range (vim, &anchor_begin, &selection_iter);
      else
        gb_source_vim_select_range (vim, &insert_iter, &anchor_begin);
    }
}

static void
gb_source_vim_clear_selection (GbSourceVim *vim)
{
  GbSourceVimPrivate *priv;
  GtkTextBuffer *buffer;
  GtkTextMark *insert;
  GtkTextIter iter;

  g_assert (GB_IS_SOURCE_VIM (vim));

  priv = vim->priv;

  buffer = gtk_text_view_get_buffer (vim->priv->text_view);
  insert = gtk_text_buffer_get_insert (buffer);

  if (gtk_text_buffer_get_has_selection (buffer))
    {
      gtk_text_buffer_get_iter_at_mark (buffer, &iter, insert);
      gtk_text_buffer_select_range (buffer, &iter, &iter);
    }

  if (priv->selection_anchor_begin)
    {
      GtkTextMark *mark;

      mark = priv->selection_anchor_begin;
      priv->selection_anchor_begin = NULL;

      gtk_text_buffer_delete_mark (buffer, mark);
    }

  if (priv->selection_anchor_end)
    {
      GtkTextMark *mark;

      mark = priv->selection_anchor_end;
      priv->selection_anchor_end = NULL;

      gtk_text_buffer_delete_mark (buffer, mark);
    }

  vim->priv->target_line_offset = gb_source_vim_get_line_offset (vim);
}

GbSourceVimMode
gb_source_vim_get_mode (GbSourceVim *vim)
{
  g_return_val_if_fail (GB_IS_SOURCE_VIM (vim), 0);

  return vim->priv->mode;
}

const gchar *
gb_source_vim_get_phrase (GbSourceVim *vim)
{
  g_return_val_if_fail (GB_IS_SOURCE_VIM (vim), NULL);

  return vim->priv->phrase->str;
}

static void
gb_source_vim_clear_phrase (GbSourceVim *vim)
{
  g_assert (GB_IS_SOURCE_VIM (vim));

  g_string_truncate (vim->priv->phrase, 0);
  g_object_notify_by_pspec (G_OBJECT (vim), gParamSpecs [PROP_PHRASE]);
}

void
gb_source_vim_set_mode (GbSourceVim     *vim,
                        GbSourceVimMode  mode)
{
  GtkTextBuffer *buffer;

  g_return_if_fail (GB_IS_SOURCE_VIM (vim));

  /*
   * Ignore if we are already in this mode.
   */
  if (mode == vim->priv->mode)
    return;

  /*
   * If we are leaving insert mode, stop recording.
   */
  if ((vim->priv->mode == GB_SOURCE_VIM_INSERT) && vim->priv->recording)
    gb_source_vim_recording_end (vim);

  buffer = gtk_text_view_get_buffer (vim->priv->text_view);

  /*
   * If we are starting insert mode, let's try to coalesce all changes
   * into one undo stack item like VIM.
   */
  if (mode == GB_SOURCE_VIM_INSERT)
    gtk_text_buffer_begin_user_action (buffer);
  else if (vim->priv->mode == GB_SOURCE_VIM_INSERT)
    gtk_text_buffer_end_user_action (buffer);

  vim->priv->mode = mode;

  /*
   * Switch to the "block mode" cursor for non-insert mode. We are totally
   * abusing "overwrite" here simply to look more like VIM.
   */
  gtk_text_view_set_overwrite (vim->priv->text_view,
                               (mode != GB_SOURCE_VIM_INSERT));

  /*
   * Clear any in flight phrases.
   */
  gb_source_vim_clear_phrase (vim);

  /*
   * If we are going back to navigation mode, stash our current buffer
   * position for use in commands like j and k.
   */
  if (mode == GB_SOURCE_VIM_NORMAL)
    vim->priv->target_line_offset = gb_source_vim_get_line_offset (vim);

  /*
   * Clear the current selection too.
   */
  if (mode != GB_SOURCE_VIM_COMMAND)
    gb_source_vim_clear_selection (vim);

  /*
   * Make the command entry visible if necessary.
   */
  g_signal_emit (vim, gSignals [COMMAND_VISIBILITY_TOGGLED], 0,
                 (mode == GB_SOURCE_VIM_COMMAND));

  g_object_notify_by_pspec (G_OBJECT (vim), gParamSpecs [PROP_MODE]);
}

static void
gb_source_vim_maybe_auto_indent (GbSourceVim *vim)
{
#ifndef GB_SOURCE_VIM_EXTERNAL
  GbSourceAutoIndenter *auto_indenter;
  GbSourceVimPrivate *priv;
  GbSourceView *source_view;
  GdkEvent fake_event = { 0 };

  g_return_if_fail (GB_IS_SOURCE_VIM (vim));

  priv = vim->priv;

  if (!GB_IS_SOURCE_VIEW (priv->text_view))
    return;

  source_view = GB_SOURCE_VIEW (priv->text_view);

  auto_indenter = gb_source_view_get_auto_indenter (source_view);
  if (!auto_indenter)
    return;

  fake_event.key.type = GDK_KEY_PRESS;
  fake_event.key.window = gtk_text_view_get_window (priv->text_view,
                                                    GTK_TEXT_WINDOW_TEXT);
  fake_event.key.send_event = FALSE;
  fake_event.key.time = GDK_CURRENT_TIME;
  fake_event.key.state = 0;
  fake_event.key.keyval = GDK_KEY_Return;
  fake_event.key.length = 1;
  fake_event.key.string = (char *)"";
  fake_event.key.hardware_keycode = 0;
  fake_event.key.group = 0;
  fake_event.key.is_modifier = 0;

  if (gb_source_auto_indenter_is_trigger (auto_indenter, &fake_event.key))
    {
      GtkTextBuffer *buffer;
      GtkTextMark *insert;
      GtkTextIter begin;
      GtkTextIter end;
      gint cursor_offset = 0;
      gchar *indent;

      buffer = gtk_text_view_get_buffer (priv->text_view);
      insert = gtk_text_buffer_get_insert (buffer);
      gtk_text_buffer_get_iter_at_mark (buffer, &begin, insert);
      gtk_text_iter_assign (&end, &begin);

      indent = gb_source_auto_indenter_format (auto_indenter, priv->text_view,
                                               buffer, &begin, &end,
                                               &cursor_offset, &fake_event.key);

      if (indent)
        {
          /*
           * Insert the indention text.
           */
          gtk_text_buffer_begin_user_action (buffer);
          if (!gtk_text_iter_equal (&begin, &end))
            gtk_text_buffer_delete (buffer, &begin, &end);
          gtk_text_buffer_insert (buffer, &begin, indent, -1);
          gtk_text_buffer_end_user_action (buffer);

          /*
           * Place the cursor, as it could be somewhere within our indent text.
           */
          gtk_text_buffer_get_iter_at_mark (buffer, &begin, insert);
          if (cursor_offset > 0)
            gtk_text_iter_forward_chars (&begin, cursor_offset);
          else if (cursor_offset < 0)
            gtk_text_iter_backward_chars (&begin, ABS (cursor_offset));
          gtk_text_buffer_select_range (buffer, &begin, &begin);
        }

      g_free (indent);
    }
#endif
}

static void
gb_source_vim_get_next_char_iter (GbSourceVim      *vim,
                                  GtkDirectionType  direction,
                                  gboolean          wrap_around,
                                  GtkTextIter      *iter)
{
  GtkTextBuffer *buffer;
  GtkTextMark *insert;

  g_assert (GB_IS_SOURCE_VIM (vim));
  g_assert (direction == GTK_DIR_UP || direction == GTK_DIR_DOWN);
  g_assert (iter != NULL);

  buffer = gtk_text_view_get_buffer (vim->priv->text_view);
  insert = gtk_text_buffer_get_insert (buffer);

  gtk_text_buffer_get_iter_at_mark (buffer, iter, insert);

  if (direction == GTK_DIR_UP)
    {
      if (!gtk_text_iter_backward_char (iter) && wrap_around)
        gtk_text_buffer_get_end_iter (buffer, iter);
    }
  else if (direction == GTK_DIR_DOWN)
    {
      if (!gtk_text_iter_forward_char (iter) && wrap_around)
        gtk_text_buffer_get_start_iter (buffer, iter);
    }
  else
    {
      g_assert_not_reached ();
    }
}

static gboolean
gb_source_vim_get_selection_bounds (GbSourceVim *vim,
                                    GtkTextIter *insert_iter,
                                    GtkTextIter *selection_iter)
{
  GtkTextBuffer *buffer;
  GtkTextMark *insert;
  GtkTextMark *selection;

  g_assert (GB_IS_SOURCE_VIM (vim));

  buffer = gtk_text_view_get_buffer (vim->priv->text_view);
  insert = gtk_text_buffer_get_insert (buffer);
  selection = gtk_text_buffer_get_selection_bound (buffer);

  if (insert_iter)
    gtk_text_buffer_get_iter_at_mark (buffer, insert_iter, insert);

  if (selection_iter)
    gtk_text_buffer_get_iter_at_mark (buffer, selection_iter, selection);

  return gtk_text_buffer_get_has_selection (buffer);
}

static void
gb_source_vim_select_range (GbSourceVim *vim,
                            GtkTextIter *insert_iter,
                            GtkTextIter *selection_iter)
{
  GtkTextBuffer *buffer;
  GtkTextMark *insert;
  GtkTextMark *selection;
  gint insert_off;
  gint selection_off;

  g_assert (GB_IS_SOURCE_VIM (vim));
  g_assert (insert_iter);
  g_assert (selection_iter);

  buffer = gtk_text_view_get_buffer (vim->priv->text_view);
  insert = gtk_text_buffer_get_insert (buffer);
  selection = gtk_text_buffer_get_selection_bound (buffer);

  /*
   * If the caller is requesting that we select a single character, we will
   * keep the iter before that character. This more closely matches the visual
   * mode in VIM.
   */
  insert_off = gtk_text_iter_get_offset (insert_iter);
  selection_off = gtk_text_iter_get_offset (selection_iter);
  if ((insert_off - selection_off) == 1)
    text_iter_swap (insert_iter, selection_iter);

  gtk_text_buffer_move_mark (buffer, insert, insert_iter);
  gtk_text_buffer_move_mark (buffer, selection, selection_iter);
}

static void
gb_source_vim_ensure_scroll (GbSourceVim *vim)
{
  GtkTextBuffer *buffer;
  gboolean cursor_selection_start;
  GtkTextMark *insert;
  GtkTextIter iter_end;
  GtkTextIter iter_start;
  GtkTextIter iter;
  guint line;
  GbSourceVimAdjustedScroll *scroll;
  GtkTextIter selection;
  gboolean has_selection;

  has_selection = gb_source_vim_get_selection_bounds (vim, &iter, &selection);

  cursor_selection_start = TRUE;
  /* The cursor is at the end of the selection */
  if (has_selection && gtk_text_iter_compare (&iter, &selection) > 0)
    cursor_selection_start = FALSE;

  g_return_if_fail (GB_IS_SOURCE_VIM (vim));

  buffer = gtk_text_view_get_buffer (vim->priv->text_view);
  gtk_text_buffer_get_selection_bounds (buffer, &iter_start, &iter_end);
  iter = cursor_selection_start ? iter_start : iter_end;
  line = gtk_text_iter_get_line (&iter);
  scroll = gb_source_vim_adjust_scroll (vim, line, GB_SOURCE_VIM_ALIGNMENT_NONE);

  /* Only adjust scroll if necesary. In this way we avoid
   * odd jumpings because of yalign imprecision.
   * But if it's not necessary to scroll vertically, make
   * sure the cursor is visible horizontally.
   */
  if (scroll->yalign >= 0) {
    gtk_text_view_scroll_to_iter (vim->priv->text_view, &iter, 0.0,
                                  TRUE, 1.0, scroll->yalign);
  } else {
    insert = gtk_text_buffer_get_insert (buffer);
    gtk_text_view_scroll_mark_onscreen (vim->priv->text_view, insert);
  }

  g_free (scroll);
}

static void
gb_source_vim_move_line0 (GbSourceVim *vim)
{
  GtkTextBuffer *buffer;
  GtkTextIter iter;
  GtkTextIter selection;
  gboolean has_selection;

  g_assert (GB_IS_SOURCE_VIM (vim));

  buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (vim->priv->text_view));
  has_selection = gb_source_vim_get_selection_bounds (vim, &iter, &selection);

  gtk_text_iter_set_line_offset (&iter, 0);

  if (has_selection)
    {
      gb_source_vim_select_range (vim, &iter, &selection);
      gb_source_vim_ensure_anchor_selected (vim);
    }
  else
    gtk_text_buffer_select_range (buffer, &iter, &iter);

  vim->priv->target_line_offset = gb_source_vim_get_line_offset (vim);

  gb_source_vim_ensure_scroll (vim);
}

static void
gb_source_vim_move_line_start (GbSourceVim *vim,
                               gboolean     can_move_forward)
{
  GtkTextBuffer *buffer;
  GtkTextIter iter;
  GtkTextIter original;
  GtkTextIter selection;
  gboolean has_selection;
  guint line;

  g_assert (GB_IS_SOURCE_VIM (vim));

  buffer = gtk_text_view_get_buffer (GTK_TEXT_VIEW (vim->priv->text_view));
  has_selection = gb_source_vim_get_selection_bounds (vim, &iter, &selection);
  line = gtk_text_iter_get_line (&iter);
  gtk_text_iter_assign (&original, &iter);

  gtk_text_buffer_get_iter_at_line (buffer, &iter, line);

  while (!gtk_text_iter_ends_line (&iter) &&
         g_unichar_isspace (gtk_text_iter_get_char (&iter)))
    if (!gtk_text_iter_forward_char (&iter))
      break;

  /*
   * If we failed to find a non-whitespace character or ended up at the
   * same place we already were, just use the 0 index position.
   */
  if (!can_move_forward)
    {
      if (g_unichar_isspace (gtk_text_iter_get_char (&iter)) ||
          gtk_text_iter_equal (&iter, &original))
        {
          gb_source_vim_move_line0 (vim);
          return;
        }
    }

  if (has_selection)
    {
      if (gtk_text_iter_compare (&iter, &selection) > 0)
        gtk_text_iter_forward_char (&iter);
      gb_source_vim_select_range (vim, &iter, &selection);
      gb_source_vim_ensure_anchor_selected (vim);
    }
  else
    gtk_text_buffer_select_range (buffer, &iter, &iter);

  vim->priv->target_line_offset = gb_source_vim_get_line_offset (vim);

  gb_source_vim_ensure_scroll (vim);
}

static void
gb_source_vim_move_line_end (GbSourceVim *vim)
{
  GbSourceVimPrivate *priv;
  GtkTextBuffer *buffer;
  GtkTextIter iter;
  GtkTextIter selection;
  gboolean has_selection;

  g_assert (GB_IS_SOURCE_VIM (vim));

  priv = vim->priv;

  buffer = gtk_text_view_get_buffer (priv->text_view);
  has_selection = gb_source_vim_get_selection_bounds (vim, &iter, &selection);

  while (!gtk_text_iter_ends_line (&iter))
    if (!gtk_text_iter_forward_char (&iter))
      break;

  if (has_selection)
    {
      gb_source_vim_select_range (vim, &iter, &selection);
      gb_source_vim_ensure_anchor_selected (vim);
    }
  else
    gtk_text_buffer_select_range (buffer, &iter, &iter);

  vim->priv->target_line_offset = gb_source_vim_get_line_offset (vim);

  gb_source_vim_ensure_scroll (vim);
}

static void
gb_source_vim_move_backward (GbSourceVim *vim)
{
  GtkTextBuffer *buffer;
  GtkTextIter iter;
  GtkTextIter selection;
  gboolean has_selection;
  guint line;

  g_assert (GB_IS_SOURCE_VIM (vim));

  buffer = gtk_text_view_get_buffer (vim->priv->text_view);
  has_selection = gb_source_vim_get_selection_bounds (vim, &iter, &selection);
  line = gtk_text_iter_get_line (&iter);

  if (gtk_text_iter_backward_char (&iter) &&
      (line == gtk_text_iter_get_line (&iter)))
    {
      if (has_selection)
        {
          if (gtk_text_iter_equal (&iter, &selection))
            {
              gtk_text_iter_backward_char (&iter);
              gtk_text_iter_forward_char (&selection);
            }
          gb_source_vim_select_range (vim, &iter, &selection);
          gb_source_vim_ensure_anchor_selected (vim);
        }
      else
        gtk_text_buffer_select_range (buffer, &iter, &iter);

      vim->priv->target_line_offset = gb_source_vim_get_line_offset (vim);
    }

  gb_source_vim_ensure_scroll (vim);
}

static gboolean
text_iter_backward_vim_word (GtkTextIter *iter)
{
  gunichar ch;
  gint begin_class;
  gint cur_class;

  g_assert (iter);

  if (!gtk_text_iter_backward_char (iter))
    return FALSE;

  /*
   * If we are on space, walk until we get to non-whitespace. Then work our way
   * back to the beginning of the word.
   */
  ch = gtk_text_iter_get_char (iter);
  if (gb_source_vim_classify (ch) == CLASS_SPACE)
    {
      for (;;)
        {
          if (!gtk_text_iter_backward_char (iter))
            return FALSE;

          ch = gtk_text_iter_get_char (iter);
          if (gb_source_vim_classify (ch) != CLASS_SPACE)
            break;
        }

      ch = gtk_text_iter_get_char (iter);
      begin_class = gb_source_vim_classify (ch);

      for (;;)
        {
          if (!gtk_text_iter_backward_char (iter))
            return FALSE;

          ch = gtk_text_iter_get_char (iter);
          cur_class = gb_source_vim_classify (ch);

          if (cur_class != begin_class)
            {
              gtk_text_iter_forward_char (iter);
              return TRUE;
            }
        }

      return FALSE;
    }

  ch = gtk_text_iter_get_char (iter);
  begin_class = gb_source_vim_classify (ch);

  for (;;)
    {
      if (!gtk_text_iter_backward_char (iter))
        return FALSE;

      ch = gtk_text_iter_get_char (iter);
      cur_class = gb_source_vim_classify (ch);

      if (cur_class != begin_class)
        {
          gtk_text_iter_forward_char (iter);
          return TRUE;
        }
    }

  return FALSE;
}

static void
gb_source_vim_move_backward_word (GbSourceVim *vim)
{
  GtkTextBuffer *buffer;
  GtkTextIter iter;
  GtkTextIter selection;
  gboolean has_selection;

  g_assert (GB_IS_SOURCE_VIM (vim));

  buffer = gtk_text_view_get_buffer (vim->priv->text_view);
  has_selection = gb_source_vim_get_selection_bounds (vim, &iter, &selection);

  if (!text_iter_backward_vim_word (&iter))
    gtk_text_buffer_get_start_iter (buffer, &iter);

  if (has_selection)
    {
      if (gtk_text_iter_equal (&iter, &selection))
        gtk_text_iter_backward_word_start (&iter);
      gb_source_vim_select_range (vim, &iter, &selection);
      gb_source_vim_ensure_anchor_selected (vim);
    }
  else
    gtk_text_buffer_select_range (buffer, &iter, &iter);

  vim->priv->target_line_offset = gb_source_vim_get_line_offset (vim);

  gb_source_vim_ensure_scroll (vim);
}

static void
gb_source_vim_move_forward (GbSourceVim *vim)
{
  GtkTextBuffer *buffer;
  GtkTextIter iter;
  GtkTextIter selection;
  gboolean has_selection;
  guint line;

  g_assert (GB_IS_SOURCE_VIM (vim));

  buffer = gtk_text_view_get_buffer (vim->priv->text_view);
  has_selection = gb_source_vim_get_selection_bounds (vim, &iter, &selection);
  line = gtk_text_iter_get_line (&iter);

  if (!gtk_text_iter_forward_char (&iter))
    gtk_text_buffer_get_end_iter (buffer, &iter);

  if (line == gtk_text_iter_get_line (&iter))
    {
      if (has_selection)
        {
          if (gtk_text_iter_equal (&iter, &selection))
            {
              gtk_text_iter_forward_char (&iter);
              gtk_text_iter_backward_char (&selection);
              gb_source_vim_ensure_anchor_selected (vim);
            }
          gb_source_vim_select_range (vim, &iter, &selection);
        }
      else
        gtk_text_buffer_select_range (buffer, &iter, &iter);

      vim->priv->target_line_offset = gb_source_vim_get_line_offset (vim);
    }

  gb_source_vim_ensure_scroll (vim);
}

static gboolean
text_iter_forward_vim_word (GtkTextIter *iter)
{
  gint begin_class;
  gint cur_class;
  gunichar ch;

  g_assert (iter);

  ch = gtk_text_iter_get_char (iter);
  begin_class = gb_source_vim_classify (ch);

  /* Move to the first non-whitespace character if necessary. */
  if (begin_class == CLASS_SPACE)
    {
      for (;;)
        {
          if (!gtk_text_iter_forward_char (iter))
            return FALSE;

          ch = gtk_text_iter_get_char (iter);
          cur_class = gb_source_vim_classify (ch);
          if (cur_class != CLASS_SPACE)
            return TRUE;
        }
    }

  /* move to first character not at same class level. */
  while (gtk_text_iter_forward_char (iter))
    {
      ch = gtk_text_iter_get_char (iter);
      cur_class = gb_source_vim_classify (ch);

      if (cur_class == CLASS_SPACE)
        {
          begin_class = CLASS_0;
          continue;
        }

      if (cur_class != begin_class)
        return TRUE;
    }

  return FALSE;
}

static gboolean
text_iter_forward_vim_word_end (GtkTextIter *iter)
{
  gunichar ch;
  gint begin_class;
  gint cur_class;

  g_assert (iter);

  if (!gtk_text_iter_forward_char (iter))
    return FALSE;

  /* If we are on space, walk to the start of the next word. */
  ch = gtk_text_iter_get_char (iter);
  if (gb_source_vim_classify (ch) == CLASS_SPACE)
    if (!text_iter_forward_vim_word (iter))
      return FALSE;

  ch = gtk_text_iter_get_char (iter);
  begin_class = gb_source_vim_classify (ch);

  for (;;)
    {
      if (!gtk_text_iter_forward_char (iter))
        return FALSE;

      ch = gtk_text_iter_get_char (iter);
      cur_class = gb_source_vim_classify (ch);

      if (cur_class != begin_class)
        {
          gtk_text_iter_backward_char (iter);
          return TRUE;
        }
    }

  return FALSE;
}

static void
gb_source_vim_move_forward_word (GbSourceVim *vim)
{
  GtkTextBuffer *buffer;
  GtkTextIter iter;
  GtkTextIter selection;
  gboolean has_selection;

  g_assert (GB_IS_SOURCE_VIM (vim));

  /*
   * TODO: VIM will jump to an empty line before going to the next word.
   */

  buffer = gtk_text_view_get_buffer (vim->priv->text_view);
  has_selection = gb_source_vim_get_selection_bounds (vim, &iter, &selection);

  if (!text_iter_forward_vim_word (&iter))
    gtk_text_buffer_get_end_iter (buffer, &iter);

  if (has_selection)
    {
      if (!gtk_text_iter_forward_char (&iter))
        gtk_text_buffer_get_end_iter (buffer, &iter);
      gb_source_vim_select_range (vim, &iter, &selection);
      gb_source_vim_ensure_anchor_selected (vim);
    }
  else
    gtk_text_buffer_select_range (buffer, &iter, &iter);

  vim->priv->target_line_offset = gb_source_vim_get_line_offset (vim);

  gb_source_vim_ensure_scroll (vim);
}

static void
gb_source_vim_move_forward_word_end (GbSourceVim *vim)
{
  GtkTextBuffer *buffer;
  GtkTextIter iter;
  GtkTextIter selection;
  gboolean has_selection;

  g_assert (GB_IS_SOURCE_VIM (vim));

  buffer = gtk_text_view_get_buffer (vim->priv->text_view);
  has_selection = gb_source_vim_get_selection_bounds (vim, &iter, &selection);

  if (!text_iter_forward_vim_word_end (&iter))
    gtk_text_buffer_get_end_iter (buffer, &iter);

  if (has_selection)
    {
      if (!gtk_text_iter_forward_char (&iter))
        gtk_text_buffer_get_end_iter (buffer, &iter);
      gb_source_vim_select_range (vim, &iter, &selection);
      gb_source_vim_ensure_anchor_selected (vim);
    }
  else
    gtk_text_buffer_select_range (buffer, &iter, &iter);

  vim->priv->target_line_offset = gb_source_vim_get_line_offset (vim);

  gb_source_vim_ensure_scroll (vim);
}

static gboolean
bracket_predicate (gunichar ch,
                   gpointer user_data)
{
  MatchingBracketState *state = user_data;

  if (ch == state->jump_from)
    state->depth++;
  else if (ch == state->jump_to)
    state->depth--;

  return (state->depth == 0);
}

static void
gb_source_vim_move_matching_bracket (GbSourceVim *vim)
{
  MatchingBracketState state;
  GtkTextBuffer *buffer;
  GtkTextIter iter;
  GtkTextIter selection;
  gboolean has_selection;
  gboolean is_forward;
  gboolean ret;

  g_return_if_fail (GB_IS_SOURCE_VIM (vim));

  buffer = gtk_text_view_get_buffer (vim->priv->text_view);
  has_selection = gb_source_vim_get_selection_bounds (vim, &iter, &selection);

  state.depth = 1;
  state.jump_from = gtk_text_iter_get_char (&iter);

  switch (state.jump_from)
    {
    case '{':
      state.jump_to = '}';
      is_forward = TRUE;
      break;

    case '[':
      state.jump_to = ']';
      is_forward = TRUE;
      break;

    case '(':
      state.jump_to = ')';
      is_forward = TRUE;
      break;

    case '}':
      state.jump_to = '{';
      is_forward = FALSE;
      break;

    case ']':
      state.jump_to = '[';
      is_forward = FALSE;
      break;

    case ')':
      state.jump_to = '(';
      is_forward = FALSE;
      break;

    default:
      return;
    }

  if (is_forward)
    ret = gtk_text_iter_forward_find_char (&iter, bracket_predicate, &state,
                                           NULL);
  else
    ret = gtk_text_iter_backward_find_char (&iter, bracket_predicate, &state,
                                            NULL);

  if (ret)
    {
      if (has_selection)
        {
          if (is_forward)
            gtk_text_iter_forward_char (&iter);
          gb_source_vim_select_range (vim, &iter, &selection);
          gb_source_vim_ensure_anchor_selected (vim);
        }
      else
        gtk_text_buffer_select_range (buffer, &iter, &iter);

      gb_source_vim_ensure_scroll (vim);
    }
}

static gboolean
is_single_line_selection (const GtkTextIter *begin,
                          const GtkTextIter *end)
{
  if (gtk_text_iter_compare (begin, end) < 0)
    return ((gtk_text_iter_get_line_offset (begin) == 0) &&
            (gtk_text_iter_get_line_offset (end) == 0) &&
            ((gtk_text_iter_get_line (begin) + 1) ==
             gtk_text_iter_get_line (end)));
  else
    return ((gtk_text_iter_get_line_offset (begin) == 0) &&
            (gtk_text_iter_get_line_offset (end) == 0) &&
            ((gtk_text_iter_get_line (end) + 1) ==
             gtk_text_iter_get_line (begin)));
}

static gboolean
is_single_char_selection (const GtkTextIter *begin,
                          const GtkTextIter *end)
{
  GtkTextIter tmp;

  g_assert (begin);
  g_assert (end);

  gtk_text_iter_assign (&tmp, begin);
  if (gtk_text_iter_forward_char (&tmp) && gtk_text_iter_equal (&tmp, end))
    return TRUE;

  gtk_text_iter_assign (&tmp, end);
  if (gtk_text_iter_forward_char (&tmp) && gtk_text_iter_equal (&tmp, begin))
    return TRUE;

  return FALSE;
}

static void
text_iter_swap (GtkTextIter *a,
                GtkTextIter *b)
{
  GtkTextIter tmp;

  gtk_text_iter_assign (&tmp, a);
  gtk_text_iter_assign (a, b);
  gtk_text_iter_assign (b, &tmp);
}

static void
gb_source_vim_move_forward_paragraph (GbSourceVim *vim)
{
  GtkTextBuffer *buffer;
  GtkTextIter iter, selection;
  gboolean has_selection;

  g_assert (GB_IS_SOURCE_VIM (vim));

  buffer = gtk_text_view_get_buffer (vim->priv->text_view);
  has_selection = gb_source_vim_get_selection_bounds (vim, &iter, &selection);

  /* Move down to the first non-blank line */
  while (gtk_text_iter_starts_line (&iter) &&
         gtk_text_iter_ends_line (&iter))
    if (!gtk_text_iter_forward_line (&iter))
      break;

  /* Find the next blank line */
  while (gtk_text_iter_forward_line (&iter))
    if (gtk_text_iter_starts_line (&iter) &&
        gtk_text_iter_ends_line (&iter))
      break;

  if (has_selection)
    {
      gb_source_vim_select_range (vim, &iter, &selection);
      gb_source_vim_ensure_anchor_selected (vim);
    }
  else
    gtk_text_buffer_select_range (buffer, &iter, &iter);

  vim->priv->target_line_offset = gb_source_vim_get_line_offset (vim);

  gb_source_vim_ensure_scroll (vim);
}

static void
gb_source_vim_move_backward_paragraph (GbSourceVim *vim)
{
  GtkTextBuffer *buffer;
  GtkTextIter iter, selection;
  gboolean has_selection;

  g_assert (GB_IS_SOURCE_VIM (vim));

  buffer = gtk_text_view_get_buffer (vim->priv->text_view);
  has_selection = gb_source_vim_get_selection_bounds (vim, &iter, &selection);

  /* Move up to the first non-blank line */
  while (gtk_text_iter_starts_line (&iter) &&
         gtk_text_iter_ends_line (&iter))
    if (!gtk_text_iter_backward_line (&iter))
      break;

  /* Find the next blank line */
  while (gtk_text_iter_backward_line (&iter))
    if (gtk_text_iter_starts_line (&iter) &&
        gtk_text_iter_ends_line (&iter))
      break;

  if (has_selection)
    {
      if (gtk_text_iter_equal (&iter, &selection))
        gtk_text_iter_forward_char (&selection);
      gb_source_vim_select_range (vim, &iter, &selection);
      gb_source_vim_ensure_anchor_selected (vim);
    }
  else
    gtk_text_buffer_select_range (buffer, &iter, &iter);

  vim->priv->target_line_offset = gb_source_vim_get_line_offset (vim);

  gb_source_vim_ensure_scroll (vim);
}

static void
gb_source_vim_move_down (GbSourceVim *vim)
{
  GbSourceVimPrivate *priv;
  GtkTextBuffer *buffer;
  GtkTextIter iter;
  GtkTextIter selection;
  gboolean has_selection;
  guint line;
  guint offset;

  g_assert (GB_IS_SOURCE_VIM (vim));

  priv = vim->priv;

  buffer = gtk_text_view_get_buffer (priv->text_view);
  has_selection = gb_source_vim_get_selection_bounds (vim, &iter, &selection);
  line = gtk_text_iter_get_line (&iter);
  offset = vim->priv->target_line_offset;

  /*
   * If we have a whole line selected (from say `V`), then we need to swap
   * the cursor and selection. This feels to me like a slight bit of a hack.
   * There may be cause to actually have a selection mode and know the type
   * of selection (line vs individual characters).
   */
  if (is_single_line_selection (&iter, &selection))
    {
      guint target_line;

      if (gtk_text_iter_compare (&iter, &selection) < 0)
        text_iter_swap (&iter, &selection);

      target_line = gtk_text_iter_get_line (&iter) + 1;
      gtk_text_iter_set_line (&iter, target_line);

      if (target_line != gtk_text_iter_get_line (&iter))
        goto select_to_end;

      gb_source_vim_select_range (vim, &iter, &selection);
      gb_source_vim_ensure_anchor_selected (vim);
      goto move_mark;
    }

  if (is_single_char_selection (&iter, &selection))
    {
      if (gtk_text_iter_compare (&iter, &selection) < 0)
        priv->target_line_offset = ++offset;
    }

  gtk_text_buffer_get_iter_at_line (buffer, &iter, line + 1);
  if ((line + 1) == gtk_text_iter_get_line (&iter))
    {
      for (; offset; offset--)
        if (!gtk_text_iter_ends_line (&iter))
          if (!gtk_text_iter_forward_char (&iter))
            break;
      if (has_selection)
        {
          gb_source_vim_select_range (vim, &iter, &selection);
          gb_source_vim_ensure_anchor_selected (vim);
        }
      else
        gtk_text_buffer_select_range (buffer, &iter, &iter);
    }
  else
    {
select_to_end:
      gtk_text_buffer_get_end_iter (buffer, &iter);
      if (has_selection)
        {
          gb_source_vim_select_range (vim, &iter, &selection);
          gb_source_vim_ensure_anchor_selected (vim);
        }
      else
        gtk_text_buffer_select_range (buffer, &iter, &iter);
    }

move_mark:
  gb_source_vim_ensure_scroll (vim);
}

static void
gb_source_vim_move_up (GbSourceVim *vim)
{
  GbSourceVimPrivate *priv;
  GtkTextBuffer *buffer;
  GtkTextIter iter;
  GtkTextIter selection;
  gboolean has_selection;
  guint line;
  guint offset;

  g_assert (GB_IS_SOURCE_VIM (vim));

  priv = vim->priv;

  buffer = gtk_text_view_get_buffer (priv->text_view);
  has_selection = gb_source_vim_get_selection_bounds (vim, &iter, &selection);
  line = gtk_text_iter_get_line (&iter);
  offset = vim->priv->target_line_offset;

  if (line == 0)
    return;

  /*
   * If we have a whole line selected (from say `V`), then we need to swap
   * the cursor and selection. This feels to me like a slight bit of a hack.
   * There may be cause to actually have a selection mode and know the type
   * of selection (line vs individual characters).
   */
  if (is_single_line_selection (&iter, &selection))
    {
      if (gtk_text_iter_compare (&iter, &selection) > 0)
        text_iter_swap (&iter, &selection);
      gtk_text_iter_set_line (&iter, gtk_text_iter_get_line (&iter) - 1);
      gb_source_vim_select_range (vim, &iter, &selection);
      gb_source_vim_ensure_anchor_selected (vim);
      goto move_mark;
    }

  if (is_single_char_selection (&iter, &selection))
    {
      if (gtk_text_iter_compare (&iter, &selection) > 0)
        {
          if (offset)
            --offset;
          priv->target_line_offset = offset;
        }
    }

  gtk_text_buffer_get_iter_at_line (buffer, &iter, line - 1);
  if ((line - 1) == gtk_text_iter_get_line (&iter))
    {
      for (; offset; offset--)
        if (!gtk_text_iter_ends_line (&iter))
          if (!gtk_text_iter_forward_char (&iter))
            break;

      if (has_selection)
        {
          if (gtk_text_iter_equal (&iter, &selection))
            gtk_text_iter_backward_char (&iter);
          gb_source_vim_select_range (vim, &iter, &selection);
          gb_source_vim_ensure_anchor_selected (vim);
        }
      else
        gtk_text_buffer_select_range (buffer, &iter, &iter);
    }

move_mark:
  gb_source_vim_ensure_scroll (vim);
}

static void
gb_source_vim_toggle_case (GbSourceVim             *vim,
                           GtkSourceChangeCaseType  change_type)
{
  GtkTextBuffer *buffer;
  GtkTextIter begin;
  GtkTextIter end;
  gboolean has_selection;
  guint begin_offset;
  guint end_offset;

  g_assert (GB_IS_SOURCE_VIM (vim));

  buffer = gtk_text_view_get_buffer (vim->priv->text_view);
  has_selection = gb_source_vim_get_selection_bounds (vim, &begin, &end);

  if (!GTK_SOURCE_IS_BUFFER (buffer))
    return;

  if (!has_selection)
    {
      gtk_text_iter_forward_char (&end);
      gtk_text_buffer_select_range (buffer, &begin, &end);
    }

  begin_offset = gtk_text_iter_get_offset (&begin);
  end_offset = gtk_text_iter_get_offset (&end);

  gtk_text_buffer_begin_user_action (buffer);
  gtk_source_buffer_change_case (GTK_SOURCE_BUFFER (buffer), change_type,
                                 &begin, &end);
  gtk_text_buffer_end_user_action (buffer);

  gtk_text_buffer_get_iter_at_offset (buffer, &begin, begin_offset);
  gtk_text_buffer_get_iter_at_offset (buffer, &end, end_offset);

  if (gtk_text_iter_compare (&begin, &end) > 0)
    text_iter_swap (&begin, &end);

  if (has_selection)
    gtk_text_buffer_select_range (buffer, &begin, &begin);
  else
    gtk_text_buffer_select_range (buffer, &end, &end);
}

static void
gb_source_vim_delete_selection (GbSourceVim *vim)
{
  GtkTextBuffer *buffer;
  GtkTextIter begin;
  GtkTextIter end;
  GtkClipboard *clipboard;
  gchar *text;

  g_assert (GB_IS_SOURCE_VIM (vim));

  buffer = gtk_text_view_get_buffer (vim->priv->text_view);
  gtk_text_buffer_get_selection_bounds (buffer, &begin, &end);

  /*
   * If there is no selection to delete, try to remove the next character
   * in the line. If there is no next character, delete the last character
   * in the line. It might look like there is no selection if the line
   * was empty.
   */
  if (gtk_text_iter_equal (&begin, &end))
    {
      if (gtk_text_iter_starts_line (&begin) &&
          gtk_text_iter_ends_line (&end) &&
          (0 == gtk_text_iter_get_line_offset (&end)))
        return;
      else if (!gtk_text_iter_ends_line (&end))
        {
          if (!gtk_text_iter_forward_char (&end))
            gtk_text_buffer_get_end_iter (buffer, &end);
        }
      else if (!gtk_text_iter_starts_line (&begin))
        {
          if (!gtk_text_iter_backward_char (&begin))
            return;
        }
      else
        return;
    }

  /*
   * Yank the selection text and apply it to the clipboard.
   */
  text = gtk_text_iter_get_slice (&begin, &end);
  clipboard = gtk_widget_get_clipboard (GTK_WIDGET (vim->priv->text_view),
                                        GDK_SELECTION_CLIPBOARD);
  gtk_clipboard_set_text (clipboard, text, -1);
  g_free (text);

  gtk_text_buffer_begin_user_action (buffer);
  gtk_text_buffer_delete (buffer, &begin, &end);
  gtk_text_buffer_end_user_action (buffer);

  vim->priv->target_line_offset = gb_source_vim_get_line_offset (vim);

  gb_source_vim_ensure_scroll (vim);
}

static gboolean
gb_source_vim_find_char_forward (GbSourceVim *vim,
                                 gchar        c)
{
  GtkTextBuffer *buffer;
  GtkTextIter iter, selection;
  gboolean has_selection;

  g_assert (GB_IS_SOURCE_VIM (vim));

  buffer = gtk_text_view_get_buffer (vim->priv->text_view);
  has_selection = gb_source_vim_get_selection_bounds (vim, &iter, &selection);

  if (gtk_text_iter_compare (&iter, &selection) < 0)
    text_iter_swap (&iter, &selection);

  while (gtk_text_iter_forward_char (&iter))
    if (gtk_text_iter_get_char (&iter) == (gunichar)c)
      break; /* found */
    else if (gtk_text_iter_ends_line (&iter))
      return FALSE; /* not found */

  if (has_selection)
    {
      if (gtk_text_iter_compare (&iter, &selection) > 0)
        gtk_text_iter_forward_char (&iter);
      gb_source_vim_select_range (vim, &iter, &selection);
    }
  else
    gtk_text_buffer_select_range (buffer, &iter, &iter);

  return TRUE;
}

static gboolean
gb_source_vim_find_char_backward (GbSourceVim *vim,
                                  gchar        c)
{
  GtkTextBuffer *buffer;
  GtkTextIter iter, selection;
  gboolean has_selection;

  g_assert (GB_IS_SOURCE_VIM (vim));

  buffer = gtk_text_view_get_buffer (vim->priv->text_view);
  has_selection = gb_source_vim_get_selection_bounds (vim, &iter, &selection);

  if (gtk_text_iter_compare (&iter, &selection) < 0)
    text_iter_swap (&iter, &selection);

  while (gtk_text_iter_backward_char (&iter))
    if (gtk_text_iter_get_char (&iter) == (gunichar)c)
      break; /* found */
    else if (gtk_text_iter_starts_line (&iter))
      return FALSE; /* not found */

  if (has_selection)
    {
      if (gtk_text_iter_compare (&iter, &selection) > 0)
        gtk_text_iter_forward_char (&iter);
      gb_source_vim_select_range (vim, &iter, &selection);
    }
  else
    gtk_text_buffer_select_range (buffer, &iter, &iter);

  return TRUE;
}

static void
gb_source_vim_select_line (GbSourceVim *vim)
{
  GbSourceVimPrivate *priv;
  GtkTextBuffer *buffer;
  GtkTextMark *insert;
  GtkTextIter iter;
  GtkTextIter begin;
  GtkTextIter end;

  g_assert (GB_IS_SOURCE_VIM (vim));

  priv = vim->priv;

  buffer = gtk_text_view_get_buffer (priv->text_view);
  insert = gtk_text_buffer_get_insert (buffer);
  gtk_text_buffer_get_iter_at_mark (buffer, &iter, insert);

  /*
   * Move to the start iter to the beginning of the line.
   */
  gtk_text_iter_assign (&begin, &iter);
  while (!gtk_text_iter_starts_line (&begin))
    if (!gtk_text_iter_backward_char (&begin))
      break;

  /*
   * Move to the end cursor to the end of the line.
   */
  gtk_text_iter_assign (&end, &iter);
  while (!gtk_text_iter_ends_line (&end))
    {
      if (!gtk_text_iter_forward_char (&end))
        {
          /*
           * This is the last line in the buffer, so we need to select the
           * newline before the line instead of the newline after the line.
           */
          gtk_text_iter_backward_char (&begin);
          break;
        }
    }

  /*
   * We actually want to select the \n before the line.
   */
  if (gtk_text_iter_ends_line (&end))
    gtk_text_iter_forward_char (&end);

  gtk_text_buffer_select_range (buffer, &begin, &end);

  gb_source_vim_set_selection_anchor (vim, &begin, &end);

  vim->priv->target_line_offset = 0;
}

static void
gb_source_vim_select_char (GbSourceVim *vim)
{
  GtkTextBuffer *buffer;
  GtkTextIter iter;
  GtkTextIter selection;
  GtkTextIter *target;
  gboolean has_selection;

  g_assert (GB_IS_SOURCE_VIM (vim));

  buffer = gtk_text_view_get_buffer (vim->priv->text_view);
  has_selection = gb_source_vim_get_selection_bounds (vim, &iter, &selection);
  target = has_selection ? &iter : &selection;

  if (!gtk_text_iter_forward_char (target))
    gtk_text_buffer_get_end_iter (buffer, target);

  gb_source_vim_select_range (vim, &iter, &selection);
  gb_source_vim_set_selection_anchor (vim, &iter, &selection);

  vim->priv->target_line_offset = gb_source_vim_get_line_offset (vim);

  gb_source_vim_ensure_scroll (vim);
}

static void
gb_source_vim_apply_motion (GbSourceVim *vim,
                            char         motion,
                            guint        count)
{
  GbSourceVimCommand *cmd;

  cmd = g_hash_table_lookup (gCommands, GINT_TO_POINTER (motion));
  if (!cmd || (cmd->type != GB_SOURCE_VIM_COMMAND_MOVEMENT))
    return;

  if ((cmd->flags & GB_SOURCE_VIM_COMMAND_FLAG_MOTION_LINEWISE))
    gb_source_vim_select_line (vim);
  else
    gb_source_vim_select_char (vim);

  cmd->func (vim, count, '\0');

  if ((cmd->flags & GB_SOURCE_VIM_COMMAND_FLAG_MOTION_EXCLUSIVE))
    {
      GtkTextIter iter, selection;

      gb_source_vim_get_selection_bounds (vim, &iter, &selection);
      if (gtk_text_iter_compare (&iter, &selection) < 0)
        text_iter_swap (&iter, &selection);

      /* From the docs:
       * "If the motion is exclusive and the end of the motion is in column 1,
       *  the end of the motion is moved to the end of the previous line and
       *  the motion becomes inclusive."
       */
      if (gtk_text_iter_get_line_offset (&iter) == 0)
        {
          GtkTextIter tmp;
          guint line;

          gtk_text_iter_backward_char (&iter);

          /* More docs:
           * "If [as above] and the start of the motion was at or before
           *  the first non-blank in the line, the motion becomes linewise."
           */
           tmp = selection;
           line = gtk_text_iter_get_line (&selection);

           gtk_text_iter_backward_word_start (&tmp);
           if (gtk_text_iter_is_start (&tmp) ||
               gtk_text_iter_get_line (&tmp) < line)
             {
               while (!gtk_text_iter_starts_line (&selection))
                 gtk_text_iter_backward_char (&selection);
               while (!gtk_text_iter_starts_line (&iter))
                 gtk_text_iter_forward_char (&iter);
             }
        }
      else
        {
          gtk_text_iter_backward_char (&iter);
        }
      gb_source_vim_select_range (vim, &iter, &selection);
    }
}

static void
gb_source_vim_undo (GbSourceVim *vim)
{
  GtkSourceUndoManager *undo;
  GtkTextBuffer *buffer;
  GtkTextIter iter;

  g_assert (GB_IS_SOURCE_VIM (vim));

  /*
   * We only support GtkSourceView for now.
   */
  buffer = gtk_text_view_get_buffer (vim->priv->text_view);
  if (!GTK_SOURCE_IS_BUFFER (buffer))
    return;

  undo = gtk_source_buffer_get_undo_manager (GTK_SOURCE_BUFFER (buffer));
  if (gtk_source_undo_manager_can_undo (undo))
    gtk_source_undo_manager_undo (undo);

  /*
   * GtkSourceView might preserve the selection. So let's go ahead and
   * clear it manually to the selection-bound mark position.
   */
  if (gb_source_vim_get_selection_bounds (vim, NULL, &iter))
    gtk_text_buffer_select_range (buffer, &iter, &iter);

  vim->priv->target_line_offset = gb_source_vim_get_line_offset (vim);

  gb_source_vim_ensure_scroll (vim);
}

static void
gb_source_vim_redo (GbSourceVim *vim)
{
  GtkSourceUndoManager *undo;
  GtkTextBuffer *buffer;
  GtkTextMark *insert;
  GtkTextIter iter;

  g_assert (GB_IS_SOURCE_VIM (vim));

  /*
   * We only support GtkSourceView for now.
   */
  buffer = gtk_text_view_get_buffer (vim->priv->text_view);
  if (!GTK_SOURCE_IS_BUFFER (buffer))
    return;

  undo = gtk_source_buffer_get_undo_manager (GTK_SOURCE_BUFFER (buffer));
  if (gtk_source_undo_manager_can_redo (undo))
    gtk_source_undo_manager_redo (undo);

  /*
   * GtkSourceView might preserve the selection. So let's go ahead and
   * clear it manually to the insert mark position.
   */
  insert = gtk_text_buffer_get_insert (buffer);
  gtk_text_buffer_get_iter_at_mark (buffer, &iter, insert);
  gtk_text_buffer_select_range (buffer, &iter, &iter);

  vim->priv->target_line_offset = gb_source_vim_get_line_offset (vim);

  gb_source_vim_ensure_scroll (vim);
}

static void
gb_source_vim_join (GbSourceVim *vim)
{
  GtkTextBuffer *buffer;
  GtkTextIter iter;
  GtkTextIter selection;
  gboolean has_selection;

  g_assert (GB_IS_SOURCE_VIM (vim));

  buffer = gtk_text_view_get_buffer (vim->priv->text_view);
  if (!GTK_SOURCE_IS_BUFFER (buffer))
    return;

  has_selection = gb_source_vim_get_selection_bounds (vim, &iter, &selection);

  /* Join with the following line. */
  if (!has_selection)
    gtk_text_iter_forward_line (&selection);

  gtk_source_buffer_join_lines (GTK_SOURCE_BUFFER (buffer), &iter, &selection);
  gtk_text_buffer_select_range (buffer, &iter, &iter);

  vim->priv->target_line_offset = gb_source_vim_get_line_offset (vim);

  gb_source_vim_ensure_scroll (vim);
}

static void
gb_source_vim_insert_nl_before (GbSourceVim *vim)
{
  GtkTextBuffer *buffer;
  GtkTextMark *insert;
  GtkTextIter iter;
  guint line;

  g_assert (GB_IS_SOURCE_VIM (vim));

  buffer = gtk_text_view_get_buffer (vim->priv->text_view);
  insert = gtk_text_buffer_get_insert (buffer);
  gtk_text_buffer_get_iter_at_mark (buffer, &iter, insert);
  line = gtk_text_iter_get_line (&iter);

  /*
   * Insert a newline before the current line.
   */
  gtk_text_buffer_get_iter_at_line (buffer, &iter, line);
  gtk_text_buffer_insert (buffer, &iter, "\n", 1);

  /*
   * Move ourselves back to the line we were one.
   */
  gtk_text_buffer_get_iter_at_line (buffer, &iter, line);

  /*
   * Select this position as the cursor.
   */
  gtk_text_buffer_select_range (buffer, &iter, &iter);

  /*
   * We might need to auto-indent the cursor after the newline.
   */
  gb_source_vim_maybe_auto_indent (vim);

  vim->priv->target_line_offset = gb_source_vim_get_line_offset (vim);

  gb_source_vim_ensure_scroll (vim);
}

static void
gb_source_vim_insert_nl_after (GbSourceVim *vim,
                               gboolean     auto_indent)
{
  GtkTextBuffer *buffer;
  GtkTextMark *insert;
  GtkTextIter iter;

  g_assert (GB_IS_SOURCE_VIM (vim));

  buffer = gtk_text_view_get_buffer (vim->priv->text_view);
  insert = gtk_text_buffer_get_insert (buffer);
  gtk_text_buffer_get_iter_at_mark (buffer, &iter, insert);

  /*
   * Move to the end of the current line and insert a newline.
   */
  while (!gtk_text_iter_ends_line (&iter))
    if (!gtk_text_iter_forward_char (&iter))
      break;
  gtk_text_buffer_insert (buffer, &iter, "\n", 1);

  /*
   * Select this position as the cursor to update insert.
   */
  gtk_text_buffer_select_range (buffer, &iter, &iter);

  /*
   * We might need to auto-indent after the newline.
   */
  if (auto_indent)
    gb_source_vim_maybe_auto_indent (vim);

  vim->priv->target_line_offset = gb_source_vim_get_line_offset (vim);

  gb_source_vim_ensure_scroll (vim);
}

static void
gb_source_vim_delete_to_line_start (GbSourceVim *vim)
{
  GtkTextBuffer *buffer;
  GtkTextMark *insert;
  GtkTextIter begin;
  GtkTextIter end;

  g_return_if_fail (GB_IS_SOURCE_VIM (vim));

  /*
   * Clear any selection so we are left at the cursor position.
   */
  gb_source_vim_clear_selection (vim);

  /*
   * Get everything we need to determine the deletion region.
   */
  buffer = gtk_text_view_get_buffer (vim->priv->text_view);
  insert = gtk_text_buffer_get_insert (buffer);
  gtk_text_buffer_get_iter_at_mark (buffer, &begin, insert);
  gtk_text_iter_assign (&end, &begin);

  /*
   * Move backward to the start of the line. VIM actually moves back to the
   * first non-whitespace character at the beginning of the line rather
   * than just position 0.
   *
   * If we are at the start of a line already, we actually just want to
   * remove the \n.
   */
  if (!gtk_text_iter_starts_line (&begin))
    {
      gb_source_vim_move_line_start (vim, FALSE);

      gtk_text_buffer_get_iter_at_mark (buffer, &begin, insert);

      if (gtk_text_iter_compare (&begin, &end) > 0)
        {
          while (!gtk_text_iter_starts_line (&begin))
            if (!gtk_text_iter_backward_char (&begin))
              break;
        }
    }
  else
    gtk_text_iter_backward_char (&begin);

  gtk_text_buffer_begin_user_action (buffer);
  gtk_text_buffer_delete (buffer, &begin, &end);
  gtk_text_buffer_end_user_action (buffer);

  vim->priv->target_line_offset = gb_source_vim_get_line_offset (vim);
}

static void
gb_source_vim_paste (GbSourceVim *vim)
{
  GtkClipboard *clipboard;
  GtkTextBuffer *buffer;
  GtkTextMark *insert;
  GtkTextIter iter;
  guint line;
  guint offset;
  gchar *text;

  g_return_if_fail (GB_IS_SOURCE_VIM (vim));

  buffer = gtk_text_view_get_buffer (vim->priv->text_view);

  /*
   * Track the current insert location so we can jump back to it.
   */
  insert = gtk_text_buffer_get_insert (buffer);
  gtk_text_buffer_get_iter_at_mark (buffer, &iter, insert);
  line = gtk_text_iter_get_line (&iter);
  offset = gtk_text_iter_get_line_offset (&iter);

  gtk_text_buffer_begin_user_action (buffer);

  /*
   * Fetch the clipboard contents so we can check to see if we are pasting a
   * whole line (which needs to be treated differently).
   */
  clipboard = gtk_widget_get_clipboard (GTK_WIDGET (vim->priv->text_view),
                                        GDK_SELECTION_CLIPBOARD);
  text = gtk_clipboard_wait_for_text (clipboard);

  /*
   * If we are pasting an entire line, we don't want to paste it at the current
   * location. We want to insert a new line after the current line, and then
   * paste it there (so move the insert mark first).
   */
  if (text && g_str_has_suffix (text, "\n"))
    {
      const gchar *tmp;
      gchar *trimmed;

      /*
       * WORKAROUND:
       *
       * This is a hack so that we can continue to use the paste code from
       * within GtkTextBuffer.
       *
       * We needed to keep the trailing \n in the text so that we know when
       * we are selecting whole lines. We also need to insert a new line
       * manually based on the context. Furthermore, we need to remove the
       * trailing line since we already added one.
       *
       * Terribly annoying, but the result is something that feels very nice,
       * just like VIM.
       */

      trimmed = g_strndup (text, strlen (text) - 1);
      gb_source_vim_insert_nl_after (vim, FALSE);
      gtk_clipboard_set_text (clipboard, trimmed, -1);
      g_signal_emit_by_name (vim->priv->text_view, "paste-clipboard");
      gtk_clipboard_set_text (clipboard, text, -1);
      g_free (trimmed);

      /*
       * VIM leaves us on the first non-whitespace character.
       */
      offset = 0;
      for (tmp = text; *tmp; tmp = g_utf8_next_char (tmp))
        {
          gunichar ch;

          ch = g_utf8_get_char (tmp);
          if (g_unichar_isspace (ch))
            {
              offset++;
              continue;
            }
          break;
        }

      line++;
    }
  else
    {
      GtkTextIter tmp;
      GtkTextIter tmp2;

      /*
       * By default, GtkTextBuffer will paste at our current position.
       * While VIM will paste after the current position. Let's advance the
       * buffer a single character on the current line if possible. We switch
       * to insert mode so that we can move past the last character in the
       * buffer. Possibly should consider an alternate design for this.
       */
      gb_source_vim_set_mode (vim, GB_SOURCE_VIM_INSERT);
      gb_source_vim_move_forward (vim);
      g_signal_emit_by_name (vim->priv->text_view, "paste-clipboard");
      gb_source_vim_set_mode (vim, GB_SOURCE_VIM_NORMAL);

      gtk_text_buffer_get_selection_bounds (buffer, &tmp, &tmp2);
      offset = gtk_text_iter_get_line_offset (&tmp);
      if (offset)
        offset--;
    }

  gtk_text_buffer_end_user_action (buffer);

  /*
   * Restore the cursor position.
   */
  gtk_text_buffer_get_iter_at_line (buffer, &iter, line);
  for (; offset; offset--)
    if (gtk_text_iter_ends_line (&iter) || !gtk_text_iter_forward_char (&iter))
      break;
  gtk_text_buffer_select_range (buffer, &iter, &iter);

  vim->priv->target_line_offset = gb_source_vim_get_line_offset (vim);

  g_free (text);
}

static void
gb_source_vim_move_to_end (GbSourceVim *vim)
{
  GtkTextBuffer *buffer;
  GtkTextIter iter;
  GtkTextIter selection;
  gboolean has_selection;

  g_return_if_fail (GB_IS_SOURCE_VIM (vim));

  buffer = gtk_text_view_get_buffer (vim->priv->text_view);
  has_selection = gb_source_vim_get_selection_bounds (vim, &iter, &selection);

  gtk_text_buffer_get_end_iter (buffer, &iter);

  if (has_selection)
    gb_source_vim_select_range (vim, &iter, &selection);
  else
    gtk_text_buffer_select_range (buffer, &iter, &iter);

  vim->priv->target_line_offset = gb_source_vim_get_line_offset (vim);

  gb_source_vim_ensure_scroll (vim);
}

static void
gb_source_vim_set_clipboard_text (GbSourceVim *vim,
                                  const gchar *text)
{
  GtkClipboard *clipboard;

  g_return_if_fail (GB_IS_SOURCE_VIM (vim));
  g_return_if_fail (text);

  clipboard = gtk_widget_get_clipboard (GTK_WIDGET (vim->priv->text_view),
                                        GDK_SELECTION_CLIPBOARD);
  gtk_clipboard_set_text (clipboard, text, -1);
}

static void
gb_source_vim_yank (GbSourceVim *vim)
{
  GtkTextBuffer *buffer;
  GtkTextIter begin;
  GtkTextIter end;
  gchar *text;

  g_assert (GB_IS_SOURCE_VIM (vim));

  /*
   * Get the current textview selection.
   */
  buffer = gtk_text_view_get_buffer (vim->priv->text_view);
  gtk_text_buffer_get_selection_bounds (buffer, &begin, &end);

  /*
   * Copy the selected text.
   */
  text = gtk_text_iter_get_slice (&begin, &end);

  /*
   * We might need to synthesize a trailing \n if this is at the end of the
   * buffer and we are performing a full line selection.
   */
  if (GTK_SOURCE_IS_BUFFER (buffer))
    {
      GtkSourceBuffer *sb = GTK_SOURCE_BUFFER (buffer);
      GtkTextIter line_start;
      GtkTextIter eob;

      gtk_text_buffer_get_end_iter (buffer, &eob);
      gtk_text_buffer_get_iter_at_line (buffer, &line_start,
                                        gtk_text_iter_get_line (&end));

      if (gtk_source_buffer_get_implicit_trailing_newline (sb) &&
          gtk_text_iter_equal (&eob, &end) &&
          (gtk_text_iter_compare (&begin, &line_start) <= 0))
        {
          gchar *tmp = text;

          text = g_strdup_printf ("%s\n", tmp);
          g_free (tmp);
        }
    }

  /*
   * Update the selection clipboard.
   */
  gb_source_vim_set_clipboard_text (vim, text);
  g_free (text);

  /*
   * Move the cursor to the first character that was selected.
   */
  gtk_text_buffer_select_range (buffer, &begin, &begin);

  gb_source_vim_ensure_scroll (vim);
  vim->priv->target_line_offset = gb_source_vim_get_line_offset (vim);
}

gchar *
gb_source_vim_get_current_word (GbSourceVim *vim,
                                GtkTextIter *begin,
                                GtkTextIter *end)
{
  GtkTextBuffer *buffer;
  GtkTextMark *insert;

  g_return_val_if_fail (GB_IS_SOURCE_VIM (vim), NULL);
  g_return_val_if_fail (begin, NULL);
  g_return_val_if_fail (end, NULL);

  buffer = gtk_text_view_get_buffer (vim->priv->text_view);
  insert = gtk_text_buffer_get_insert (buffer);
  gtk_text_buffer_get_iter_at_mark (buffer, begin, insert);

  if (text_iter_forward_vim_word_end (begin))
    {
      gtk_text_iter_assign (end, begin);
      gtk_text_iter_forward_char (end);
      if (text_iter_backward_vim_word (begin))
        return gtk_text_iter_get_slice (begin, end);
    }

  return NULL;
}

static gboolean
gb_source_vim_select_current_word (GbSourceVim *vim,
                                   GtkTextIter *begin,
                                   GtkTextIter *end)
{
  GtkTextBuffer *buffer;
  GtkTextMark *insert;

  g_return_val_if_fail (GB_IS_SOURCE_VIM (vim), FALSE);
  g_return_val_if_fail (begin, FALSE);
  g_return_val_if_fail (end, FALSE);

  buffer = gtk_text_view_get_buffer (vim->priv->text_view);
  insert = gtk_text_buffer_get_insert (buffer);
  gtk_text_buffer_get_iter_at_mark (buffer, begin, insert);

  if (text_iter_forward_vim_word_end (begin))
    {
      gtk_text_iter_assign (end, begin);
      gtk_text_iter_forward_char (end);
      if (text_iter_backward_vim_word (begin))
        return TRUE;
    }

  return FALSE;
}

static void
gb_source_vim_search_cb (GObject      *source,
                         GAsyncResult *result,
                         gpointer      user_data)
{
  GtkSourceSearchContext *search_context = (GtkSourceSearchContext *)source;
  GbSourceVim *vim = user_data;
  GtkTextIter match_begin;
  GtkTextIter match_end;

  g_return_if_fail (GTK_SOURCE_IS_SEARCH_CONTEXT (search_context));
  g_return_if_fail (G_IS_ASYNC_RESULT (result));
  g_return_if_fail (GB_IS_SOURCE_VIM (vim));

  if (gtk_source_search_context_backward_finish (search_context, result,
                                                 &match_begin, &match_end,
                                                 NULL))
    {
      if (vim->priv->text_view)
        {
          GtkTextBuffer *buffer;

          buffer = gtk_text_view_get_buffer (vim->priv->text_view);
          gtk_text_buffer_select_range (buffer, &match_begin, &match_begin);
          gtk_text_view_scroll_to_iter (vim->priv->text_view, &match_end,
                                        0.0, TRUE, 1.0, 0.5);
        }
    }

  g_object_unref (vim);
}

static void
gb_source_vim_reverse_search (GbSourceVim *vim)
{
  GtkTextIter begin;
  GtkTextIter end;

  g_return_if_fail (GB_IS_SOURCE_VIM (vim));

  if (!GTK_SOURCE_IS_VIEW (vim->priv->text_view))
    return;

  gb_source_vim_set_search_direction (vim, GTK_DIR_UP);

  if (gb_source_vim_select_current_word (vim, &begin, &end))
    {
      GtkTextIter start_iter;
      gchar *text;

      text = gtk_text_iter_get_slice (&begin, &end);

      if (gtk_text_iter_compare (&begin, &end) <= 0)
        gtk_text_iter_assign (&start_iter, &begin);
      else
        gtk_text_iter_assign (&start_iter, &end);

      gb_source_vim_set_search_text (vim, text);

      g_object_set (vim->priv->search_settings,
                    "at-word-boundaries", TRUE,
                    "case-sensitive", TRUE,
                    "wrap-around", TRUE,
                    NULL);

      gtk_source_search_context_set_highlight (vim->priv->search_context,
                                               TRUE);

      gtk_source_search_context_backward_async (vim->priv->search_context,
                                                &start_iter,
                                                NULL,
                                                gb_source_vim_search_cb,
                                                g_object_ref (vim));

      g_free (text);
    }
}

static void
gb_source_vim_search (GbSourceVim *vim)
{
  GtkTextIter iter;
  GtkTextIter selection;
  GtkTextIter start_iter;
  gboolean has_selection;
  gchar *text = NULL;

  g_return_if_fail (GB_IS_SOURCE_VIM (vim));

  if (!GTK_SOURCE_IS_VIEW (vim->priv->text_view))
    return;

  gb_source_vim_set_search_direction (vim, GTK_DIR_DOWN);

  has_selection = gb_source_vim_get_selection_bounds (vim, &iter, &selection);

  if (has_selection)
    text = gtk_text_iter_get_slice (&iter, &selection);
  else if (gb_source_vim_select_current_word (vim, &iter, &selection))
    text = gtk_text_iter_get_slice (&iter, &selection);
  else
    return;

  if (gtk_text_iter_compare (&iter, &selection) > 0)
    gtk_text_iter_assign (&start_iter, &iter);
  else
    gtk_text_iter_assign (&start_iter, &selection);

  gb_source_vim_set_search_text (vim, text);

  g_object_set (vim->priv->search_settings,
                "at-word-boundaries", TRUE,
                "case-sensitive", TRUE,
                "wrap-around", TRUE,
                NULL);

  gtk_source_search_context_set_highlight (vim->priv->search_context,
                                           TRUE);

  gtk_source_search_context_forward_async (vim->priv->search_context,
                                           &start_iter,
                                           NULL,
                                           gb_source_vim_search_cb,
                                           g_object_ref (vim));

  g_free (text);
}

static void
gb_source_vim_repeat_search (GbSourceVim      *vim,
                             GtkDirectionType  search_direction)
{
  GtkTextIter iter;
  const gchar *search_text = NULL;

  g_return_if_fail (GB_IS_SOURCE_VIM (vim));

  if (!GTK_SOURCE_IS_VIEW (vim->priv->text_view))
    return;

  search_text = gb_source_vim_get_search_text (vim);

  if (gb_str_empty0 (search_text))
    return;

  gb_source_vim_get_next_char_iter (vim, search_direction, TRUE, &iter);

  g_object_set (vim->priv->search_settings,
                "at-word-boundaries", FALSE,
                "case-sensitive", TRUE,
                "wrap-around", TRUE,
                NULL);

  gtk_source_search_context_set_highlight (vim->priv->search_context, TRUE);

  if (search_direction == GTK_DIR_DOWN)
    gtk_source_search_context_forward_async (vim->priv->search_context,
                                             &iter,
                                             NULL,
                                             gb_source_vim_search_cb,
                                             g_object_ref (vim));
  else
    gtk_source_search_context_backward_async (vim->priv->search_context,
                                              &iter,
                                              NULL,
                                              gb_source_vim_search_cb,
                                              g_object_ref (vim));
}

static void
gb_source_vim_move_to_line_n (GbSourceVim *vim,
                              guint        line)
{
  GtkTextIter iter, selection;
  gboolean has_selection;

  g_assert (GB_IS_SOURCE_VIM (vim));

  has_selection = gb_source_vim_get_selection_bounds (vim, &iter, &selection);

  if (is_single_line_selection (&iter, &selection))
    {
      gtk_text_iter_set_line (&iter, line);

      if (gtk_text_iter_compare (&iter, &selection) > 0)
        gtk_text_iter_forward_line (&iter);
    }
  else
    gtk_text_iter_set_line (&iter, line);

  if (has_selection)
    {
      gb_source_vim_select_range (vim, &iter, &selection);
      gb_source_vim_ensure_anchor_selected (vim);
    }
  else
    {
      gb_source_vim_select_range (vim, &iter, &iter);
    }

  vim->priv->target_line_offset = gb_source_vim_get_line_offset (vim);

  gtk_text_view_scroll_to_iter (vim->priv->text_view, &iter, 0.0, TRUE,
                                0.0, 0.5);
}

static gboolean
reshow_highlight (gpointer data)
{
  GbSourceVim *vim = data;
  GtkSourceView *source_view;

  vim->priv->anim_timeout = 0;

  source_view = GTK_SOURCE_VIEW (vim->priv->text_view);
  gtk_source_view_set_highlight_current_line (source_view, TRUE);

  return G_SOURCE_REMOVE;
}

static void
gb_source_vim_move_to_iter (GbSourceVim *vim,
                            GtkTextIter *iter,
                            gdouble      yalign)
{
  GtkTextBuffer *buffer;
  GtkTextMark *insert;

  g_assert (GB_IS_SOURCE_VIM (vim));
  g_assert (iter);
  g_assert (yalign >= 0.0);
  g_assert (yalign <= 1.0);

  if (GTK_SOURCE_IS_VIEW (vim->priv->text_view))
    {
      GtkSourceView *source_view;

      source_view = GTK_SOURCE_VIEW (vim->priv->text_view);
      if (vim->priv->anim_timeout ||
          gtk_source_view_get_highlight_current_line (source_view))
        {
          if (vim->priv->anim_timeout)
            g_source_remove (vim->priv->anim_timeout);
          gtk_source_view_set_highlight_current_line (source_view, FALSE);
          vim->priv->anim_timeout = g_timeout_add (200, reshow_highlight, vim);
        }
    }

  buffer = gtk_text_view_get_buffer (vim->priv->text_view);
  insert = gtk_text_buffer_get_insert (buffer);

  if (gtk_text_buffer_get_has_selection (buffer))
    {
      gtk_text_buffer_move_mark (buffer, insert, iter);
      gb_source_vim_ensure_anchor_selected (vim);
    }
  else
    gtk_text_buffer_select_range (buffer, iter, iter);

  gtk_text_view_scroll_to_iter (vim->priv->text_view, iter, 0.0,
                                TRUE, 0.5, yalign);
}

static GbSourceVimAdjustedScroll*
gb_source_vim_adjust_scroll (GbSourceVim          *vim,
                             gint                  line,
                             GbSourceVimAlignment  alignment)
{
  GdkRectangle rect;
  gint line_top;
  gint line_bottom;
  gint line_current;
  gint page_lines;
  GtkTextIter iter_top;
  GtkTextIter iter_bottom;
  GtkTextIter iter_current;

  GtkTextBuffer *buffer;
  gfloat min_yalign, max_yalign, yalign;
  GbSourceVimAdjustedScroll *result;

  g_assert (GB_IS_SOURCE_VIM (vim));

  gtk_text_view_get_visible_rect (vim->priv->text_view, &rect);
  gtk_text_view_get_iter_at_location (vim->priv->text_view, &iter_top,
                                      rect.x, rect.y);
  gtk_text_view_get_iter_at_location (vim->priv->text_view, &iter_bottom,
                                      rect.x, rect.y + rect.height);

  buffer = gtk_text_view_get_buffer (vim->priv->text_view);
  gtk_text_buffer_get_selection_bounds (buffer, &iter_current, NULL);

  result = malloc (sizeof (*result));
  line_top = gtk_text_iter_get_line (&iter_top);
  line_bottom = gtk_text_iter_get_line (&iter_bottom);
  line_current = gtk_text_iter_get_line (&iter_current);
  page_lines = line_bottom - line_top;

  if (page_lines == 0) {
    result->line = 0;
    result->yalign = 0.;

    return result;
  }

  min_yalign = MIN (vim->priv->scroll_off / (float) page_lines, 0.5);
  max_yalign = 1.0 - min_yalign;
  yalign = (line - line_top) / (float) page_lines;

  switch (alignment)
    {
    case GB_SOURCE_VIM_ALIGNMENT_NONE:
      /* Only change yalign if necesary, if not, indicate to the caller that
       * is not necesary to adjust scroll
       */
      if (min_yalign > yalign || yalign > max_yalign)
        result->yalign = CLAMP (yalign, min_yalign, max_yalign);
      else
        result->yalign = -1;
      result->line = line;
      break;

    case GB_SOURCE_VIM_ALIGNMENT_KEEP:
      result->yalign = MAX (0.0, (float)(line_current - line_top) /
                                (float)(line_bottom - line_top));
      result->line = line;
      break;

    case GB_SOURCE_VIM_ALIGNMENT_TOP:
      result->yalign = CLAMP (0.0, min_yalign, max_yalign);
      result->line = line + result->yalign * page_lines;
      break;

    case GB_SOURCE_VIM_ALIGNMENT_BOTTOM:
      result->yalign = CLAMP (1.0, min_yalign, max_yalign);
      result->line = MAX(0, line - (1.0 - result->yalign) * page_lines);
      break;

    default:
      g_assert_not_reached ();
    }

  return result;
}

static void
gb_source_vim_move_page (GbSourceVim                 *vim,
                         GbSourceVimPageDirectionType direction)
{
  GdkRectangle rect;
  GtkTextIter iter_top, iter_bottom, iter_current;
  guint offset;
  gint line, line_top, line_bottom, line_current;
  GtkTextBuffer *buffer;
  GbSourceVimAdjustedScroll *adjusted_scroll;

  g_assert (GB_IS_SOURCE_VIM (vim));

  gtk_text_view_get_visible_rect (vim->priv->text_view, &rect);
  gtk_text_view_get_iter_at_location (vim->priv->text_view, &iter_top,
                                      rect.x, rect.y);
  gtk_text_view_get_iter_at_location (vim->priv->text_view, &iter_bottom,
                                      rect.x, rect.y + rect.height);

  buffer = gtk_text_view_get_buffer (vim->priv->text_view);
  gtk_text_buffer_get_selection_bounds (buffer, &iter_current, NULL);

  line_top = gtk_text_iter_get_line (&iter_top);
  line_bottom = gtk_text_iter_get_line (&iter_bottom);
  line_current = gtk_text_iter_get_line (&iter_current);

  /* We can't use gb_soure_vim_ensure_scroll because we want specific aligments */
  switch (direction)
    {
    case GB_SOURCE_VIM_HALF_PAGE_UP:
      line = line_current - (line_bottom - line_top) / 2;
      adjusted_scroll = gb_source_vim_adjust_scroll (vim, line, GB_SOURCE_VIM_ALIGNMENT_KEEP);
      break;
    case GB_SOURCE_VIM_HALF_PAGE_DOWN:
      line = line_current + (line_bottom - line_top) / 2;
      adjusted_scroll = gb_source_vim_adjust_scroll (vim, line, GB_SOURCE_VIM_ALIGNMENT_KEEP);
      break;
    case GB_SOURCE_VIM_PAGE_UP:
      line = gtk_text_iter_get_line (&iter_top);
      adjusted_scroll = gb_source_vim_adjust_scroll (vim, line, GB_SOURCE_VIM_ALIGNMENT_BOTTOM);
      break;
    case GB_SOURCE_VIM_PAGE_DOWN:
      line = MAX (0, gtk_text_iter_get_line (&iter_bottom));
      adjusted_scroll = gb_source_vim_adjust_scroll (vim, line, GB_SOURCE_VIM_ALIGNMENT_TOP);
      break;
    default:
      g_assert_not_reached();
    }

  gtk_text_iter_set_line (&iter_current, adjusted_scroll->line);

  for (offset = vim->priv->target_line_offset; offset; offset--)
    if (gtk_text_iter_ends_line (&iter_current) ||
        !gtk_text_iter_forward_char (&iter_current))
      break;

  gb_source_vim_move_to_iter (vim, &iter_current, adjusted_scroll->yalign);

  g_free (adjusted_scroll);
}

static void
gb_source_vim_indent (GbSourceVim *vim)
{
  GtkSourceView *view;
  GtkTextBuffer *buffer;
  GtkTextIter iter;
  GtkTextIter selection;

  g_assert (GB_IS_SOURCE_VIM (vim));

  if (!GTK_SOURCE_IS_VIEW (vim->priv->text_view))
    return;

  view = GTK_SOURCE_VIEW (vim->priv->text_view);
  buffer = gtk_text_view_get_buffer (vim->priv->text_view);

  if (gtk_text_buffer_get_selection_bounds (buffer, &iter, &selection))
    gtk_source_view_indent_lines (view, &iter, &selection);
}

static void
gb_source_vim_unindent (GbSourceVim *vim)
{
  GtkSourceView *view;
  GtkTextBuffer *buffer;
  GtkTextIter iter;
  GtkTextIter selection;

  g_assert (GB_IS_SOURCE_VIM (vim));

  if (!GTK_SOURCE_IS_VIEW (vim->priv->text_view))
    return;

  view = GTK_SOURCE_VIEW (vim->priv->text_view);
  buffer = gtk_text_view_get_buffer (vim->priv->text_view);

  if (gtk_text_buffer_get_selection_bounds (buffer, &iter, &selection))
    gtk_source_view_unindent_lines (view, &iter, &selection);
}

static void
gb_source_vim_add (GbSourceVim *vim,
                   gint         by_count)
{
  GtkTextBuffer *buffer;
  GtkTextIter iter;
  GtkTextIter selection;
  gchar *endptr = NULL;
  gchar *replace = NULL;
  gchar *slice;
  gint64 value = 0;

  g_assert (vim);

  /*
   * TODO: There are a lot of smarts we can put in here. Guessing the base
   *       comes to mind (hex, octal, etc).
   */

  buffer = gtk_text_view_get_buffer (vim->priv->text_view);
  gtk_text_buffer_get_selection_bounds (buffer, &iter, &selection);

  slice = gtk_text_iter_get_slice (&iter, &selection);
  value = g_ascii_strtoll (slice, &endptr, 10);

  if (((value == G_MAXINT64) || (value == G_MININT64)) && (errno == ERANGE))
    goto cleanup;

  if (!endptr || *endptr)
    goto cleanup;

  value += by_count;

  replace = g_strdup_printf ("%"G_GINT64_FORMAT, value);

  gtk_text_buffer_delete (buffer, &iter, &selection);
  gtk_text_buffer_insert (buffer, &iter, replace, -1);
  gtk_text_iter_backward_char (&iter);
  gtk_text_buffer_select_range (buffer, &iter, &iter);

cleanup:
  g_free (slice);
  g_free (replace);
}

static GbSourceVimPhraseStatus
gb_source_vim_parse_phrase (GbSourceVim       *vim,
                            GbSourceVimPhrase *phrase)
{
  const gchar *str;
  guint count = 0;
  gchar key;
  gchar modifier;
  gint n_scanned;

  g_assert (GB_IS_SOURCE_VIM (vim));
  g_assert (phrase);

  phrase->key = 0;
  phrase->count = 0;
  phrase->modifier = 0;

  str = vim->priv->phrase->str;

  n_scanned = sscanf (str, "%u%c%c", &count, &key, &modifier);

  if (n_scanned == 3)
    {
      phrase->count = count;
      phrase->key = key;
      phrase->modifier = modifier;

      return GB_SOURCE_VIM_PHRASE_SUCCESS;
    }

  if (n_scanned == 2)
    {
      phrase->count = count;
      phrase->key = key;
      phrase->modifier = 0;

      return GB_SOURCE_VIM_PHRASE_SUCCESS;
    }

  /* Special case for "0" command. */
  if ((n_scanned == 1) && (count == 0))
    {
      phrase->key = '0';
      phrase->count = 0;
      phrase->modifier = 0;

      return GB_SOURCE_VIM_PHRASE_SUCCESS;
    }

  if (n_scanned == 1)
    return GB_SOURCE_VIM_PHRASE_NEED_MORE;

  n_scanned = sscanf (str, "%c%u%c", &key, &count, &modifier);

  if (n_scanned == 3)
    {
      phrase->count = count;
      phrase->key = key;
      phrase->modifier = modifier;

      return GB_SOURCE_VIM_PHRASE_SUCCESS;
    }

  /* there's a count following key - the modifier is non-optional then */
  if (n_scanned == 2)
    return GB_SOURCE_VIM_PHRASE_NEED_MORE;

  n_scanned = sscanf (str, "%c%c", &key, &modifier);

  if (n_scanned == 2)
    {
      phrase->count = 0;
      phrase->key = key;
      phrase->modifier = modifier;

      return GB_SOURCE_VIM_PHRASE_SUCCESS;
    }

  if (n_scanned == 1)
    {
      phrase->count = 0;
      phrase->key = key;
      phrase->modifier = 0;

      return GB_SOURCE_VIM_PHRASE_SUCCESS;
    }

  return GB_SOURCE_VIM_PHRASE_FAILED;
}

static gboolean
gb_source_vim_handle_normal (GbSourceVim *vim,
                             GdkEventKey *event)
{
  GbSourceVimCommand *cmd;
  GbSourceVimPhraseStatus status;
  GbSourceVimPhrase phrase;
  GtkTextBuffer *buffer;

  g_assert (GB_IS_SOURCE_VIM (vim));
  g_assert (event);

  buffer = gtk_text_view_get_buffer (vim->priv->text_view);

  switch (event->keyval)
    {
    case GDK_KEY_bracketleft:
      if ((event->state & GDK_CONTROL_MASK) == 0)
        break;
      /* Fall through */
    case GDK_KEY_Escape:
      gb_source_vim_clear_selection (vim);
      gb_source_vim_clear_phrase (vim);
      vim->priv->in_ctrl_w = FALSE;
      return TRUE;

    case GDK_KEY_Page_Up:
    case GDK_KEY_KP_Page_Up:
      gb_source_vim_clear_phrase (vim);
      gb_source_vim_move_page (vim, GB_SOURCE_VIM_PAGE_UP);
      return TRUE;

    case GDK_KEY_Page_Down:
    case GDK_KEY_KP_Page_Down:
      gb_source_vim_clear_phrase (vim);
      gb_source_vim_move_page (vim, GB_SOURCE_VIM_PAGE_DOWN);
      return TRUE;

    case GDK_KEY_KP_Enter:
    case GDK_KEY_Return:
      gb_source_vim_clear_phrase (vim);
      gb_source_vim_move_down (vim);
      return TRUE;

    case GDK_KEY_BackSpace:
      gb_source_vim_clear_phrase (vim);
      if (!vim->priv->phrase->len)
        gb_source_vim_move_backward (vim);
      return TRUE;

    case GDK_KEY_colon:
      if (!vim->priv->phrase->len)
        {
          gb_source_vim_set_mode (vim, GB_SOURCE_VIM_COMMAND);
          return TRUE;
        }
      break;

    case GDK_KEY_KP_Down:
    case GDK_KEY_Down:
      gb_source_vim_clear_phrase (vim);
      gb_source_vim_move_down (vim);
      return TRUE;

    case GDK_KEY_KP_Up:
    case GDK_KEY_Up:
      gb_source_vim_clear_phrase (vim);
      gb_source_vim_move_up (vim);
      return TRUE;

    case GDK_KEY_KP_Left:
    case GDK_KEY_Left:
      gb_source_vim_clear_phrase (vim);
      gb_source_vim_move_backward (vim);
      return TRUE;

    case GDK_KEY_KP_Right:
    case GDK_KEY_Right:
      gb_source_vim_clear_phrase (vim);
      gb_source_vim_move_forward (vim);
      return TRUE;

    case GDK_KEY_a:
    case GDK_KEY_x:
      if ((event->state & GDK_CONTROL_MASK) != 0)
        {
          GtkTextIter begin;
          GtkTextIter end;

          gb_source_vim_clear_phrase (vim);
          gb_source_vim_clear_selection (vim);
          if (gb_source_vim_select_current_word (vim, &begin, &end))
            {
              if (gtk_text_iter_backward_char (&begin) &&
                  ('-' != gtk_text_iter_get_char (&begin)))
                gtk_text_iter_forward_char (&begin);
              gtk_text_buffer_select_range (buffer, &begin, &end);
              gb_source_vim_add (vim, (event->keyval == GDK_KEY_a) ? 1 : -1);
              gb_source_vim_clear_selection (vim);
            }
          return TRUE;
        }
      break;

    case GDK_KEY_b:
      if ((event->state & GDK_CONTROL_MASK) != 0)
        {
          gb_source_vim_clear_phrase (vim);
          gb_source_vim_move_page (vim, GB_SOURCE_VIM_PAGE_UP);
          return TRUE;
        }
      break;

    case GDK_KEY_D:
      /* Special case for <Control><Shift>D, Gtk Inspector. */
      if ((event->state & (GDK_CONTROL_MASK | GDK_SHIFT_MASK)) ==
          (GDK_CONTROL_MASK | GDK_SHIFT_MASK))
          return FALSE;
      break;

    case GDK_KEY_d:
      if ((event->state & GDK_CONTROL_MASK) != 0)
        {
          gb_source_vim_clear_phrase (vim);
          gb_source_vim_move_page (vim, GB_SOURCE_VIM_HALF_PAGE_DOWN);
          return TRUE;
        }
      break;

    case GDK_KEY_f:
      if ((event->state & GDK_CONTROL_MASK) != 0)
        {
          gb_source_vim_clear_phrase (vim);
          gb_source_vim_move_page (vim, GB_SOURCE_VIM_PAGE_DOWN);
          return TRUE;
        }
      break;

    case GDK_KEY_r:
      if ((event->state & GDK_CONTROL_MASK) != 0)
        {
          gb_source_vim_clear_phrase (vim);
          gb_source_vim_redo (vim);
          return TRUE;
        }
      break;

    case GDK_KEY_u:
      if ((event->state & GDK_CONTROL_MASK) != 0)
        {
          gb_source_vim_clear_phrase (vim);
          gb_source_vim_move_page (vim, GB_SOURCE_VIM_HALF_PAGE_UP);
          return TRUE;
        }
      break;

    case GDK_KEY_w:
      if ((event->state & GDK_CONTROL_MASK) != 0)
        {
          gb_source_vim_clear_phrase (vim);
          vim->priv->in_ctrl_w = TRUE;
          return TRUE;
        }
      break;

    default:
      break;
    }

  if (gtk_bindings_activate_event (G_OBJECT (vim->priv->text_view), event))
    return TRUE;

  /*
   * TODO: The GdkEventKey.string field is deprecated, so we will need to
   *       determine how to do this more precisely once we can no longer use
   *       that.
   */

  if (event->string && *event->string)
    {
      g_string_append (vim->priv->phrase, event->string);
      g_object_notify_by_pspec (G_OBJECT (vim), gParamSpecs [PROP_PHRASE]);
    }

  status = gb_source_vim_parse_phrase (vim, &phrase);

  switch (status)
    {
    case GB_SOURCE_VIM_PHRASE_SUCCESS:
      cmd = g_hash_table_lookup (gCommands, GINT_TO_POINTER (phrase.key));
      if (!cmd)
        {
          gb_source_vim_clear_phrase (vim);
          break;
        }

      if (cmd->flags & GB_SOURCE_VIM_COMMAND_FLAG_REQUIRES_MODIFIER &&
          !((cmd->flags & GB_SOURCE_VIM_COMMAND_FLAG_VISUAL) &&
            gtk_text_buffer_get_has_selection (buffer)) &&
          !phrase.modifier)
        break;

      gb_source_vim_clear_phrase (vim);

      cmd->func (vim, phrase.count, phrase.modifier);
      if (cmd->flags & GB_SOURCE_VIM_COMMAND_FLAG_VISUAL)
        gb_source_vim_clear_selection (vim);

      break;

    case GB_SOURCE_VIM_PHRASE_NEED_MORE:
      break;

    default:
    case GB_SOURCE_VIM_PHRASE_FAILED:
      gb_source_vim_clear_phrase (vim);
      break;
    }

  return TRUE;
}

static gboolean
gb_source_vim_handle_insert (GbSourceVim *vim,
                             GdkEventKey *event)
{
  if (!vim->priv->in_replay)
    gb_source_vim_recording_capture (vim, (GdkEvent *)event);

  switch (event->keyval)
    {
    case GDK_KEY_bracketleft:
      if ((event->state & GDK_CONTROL_MASK) == 0)
        break;
      /* Fall through */
    case GDK_KEY_Escape:
      /*
       * First move back onto the last character we entered, and then
       * return to NORMAL mode.
       */
      gb_source_vim_move_backward (vim);
      gb_source_vim_set_mode (vim, GB_SOURCE_VIM_NORMAL);
      return FALSE;

    case GDK_KEY_u:
      /*
       * Delete everything before the cursor upon <Control>U.
       */
      if ((event->state & GDK_CONTROL_MASK) != 0)
        {
          gb_source_vim_delete_to_line_start (vim);
          return TRUE;
        }

      break;

    default:
      break;
    }

  return FALSE;
}

static gboolean
gb_source_vim_handle_command (GbSourceVim *vim,
                              GdkEventKey *event)
{
  /*
   * We typically shouldn't be hitting here, we should be focused in the
   * command_entry widget.
   */

  switch (event->keyval)
    {
    case GDK_KEY_bracketleft:
      if ((event->state & GDK_CONTROL_MASK) == 0)
        break;
      /* Fall through */
    case GDK_KEY_Escape:
      /*
       * Escape back into NORMAL mode.
       */
      gb_source_vim_set_mode (vim, GB_SOURCE_VIM_NORMAL);
      return TRUE;

    default:
      break;
    }

  if (!gtk_bindings_activate_event (G_OBJECT (vim->priv->text_view), event))
    {
      /*
       * TODO: Show visual error because we can't input right now. We shouldn't
       *       even get here though.
       */
    }

  return TRUE;
}

static gboolean
gb_source_vim_handle_ctrl_w (GbSourceVim *vim,
                             GdkEventKey *event)
{
  GbSourceVimSplit split = 0;
  gboolean handled = FALSE;

  g_return_val_if_fail (GB_IS_SOURCE_VIM (vim), FALSE);
  g_return_val_if_fail (event, FALSE);

  vim->priv->in_ctrl_w = FALSE;

  switch (event->keyval)
    {
    case GDK_KEY_S:
    case GDK_KEY_s:
      split = GB_SOURCE_VIM_SPLIT_HORIZONTAL;
      break;

    case GDK_KEY_v:
    case GDK_KEY_V:
      split = GB_SOURCE_VIM_SPLIT_VERTICAL;
      break;

    case GDK_KEY_c:
      split = GB_SOURCE_VIM_SPLIT_CLOSE;
      break;

    case GDK_KEY_n:
      split = GB_SOURCE_VIM_SPLIT_CYCLE_NEXT;
      break;

    case GDK_KEY_p:
      split = GB_SOURCE_VIM_SPLIT_CYCLE_PREVIOUS;
      break;

    case GDK_KEY_w:
      split = GB_SOURCE_VIM_SPLIT_CYCLE_NEXT;
      break;

    default:
      break;
    }

  if (split)
    g_signal_emit (vim, gSignals [SPLIT], 0, split, &handled);

  if (!handled)
    {
      /*
       * TODO: emit message about unhandled split.
       *
       * Vim will emit a message here about the unhandled split. We should
       * probably do the same, but that will require that we plumb messages
       * too. Also a good idea.
       */
    }

  return !!split;
}

static void
gb_source_vim_event_after_cb (GtkTextView *text_view,
                              GdkEventKey *event,
                              GbSourceVim *vim)
{
  g_return_if_fail (GB_IS_SOURCE_VIM (vim));

  if (vim->priv->mode == GB_SOURCE_VIM_INSERT &&
      event->type == GDK_KEY_PRESS)
    gb_source_vim_ensure_scroll (vim);
}

static gboolean
gb_source_vim_key_press_event_cb (GtkTextView *text_view,
                                  GdkEventKey *event,
                                  GbSourceVim *vim)
{
  gboolean ret;

  g_return_val_if_fail (GTK_IS_TEXT_VIEW (text_view), FALSE);
  g_return_val_if_fail (event, FALSE);
  g_return_val_if_fail (GB_IS_SOURCE_VIM (vim), FALSE);

  switch (vim->priv->mode)
    {
    case GB_SOURCE_VIM_NORMAL:
      if (vim->priv->in_ctrl_w)
        ret = gb_source_vim_handle_ctrl_w (vim, event);
      else
        ret = gb_source_vim_handle_normal (vim, event);
      break;

    case GB_SOURCE_VIM_INSERT:
      ret = gb_source_vim_handle_insert (vim, event);
      break;

    case GB_SOURCE_VIM_COMMAND:
      ret = gb_source_vim_handle_command (vim, event);
      break;

    default:
      g_assert_not_reached();
    }

  return ret;
}

static gboolean
gb_source_vim_key_release_event_cb (GtkTextView *text_view,
                                    GdkEventKey *event,
                                    GbSourceVim *vim)
{
  g_return_val_if_fail (GTK_IS_TEXT_VIEW (text_view), FALSE);
  g_return_val_if_fail (event, FALSE);
  g_return_val_if_fail (GB_IS_SOURCE_VIM (vim), FALSE);

  if ((vim->priv->mode == GB_SOURCE_VIM_INSERT) && vim->priv->recording)
    gb_source_vim_recording_capture (vim, (GdkEvent *)event);

  return FALSE;
}

static gboolean
gb_source_vim_focus_in_event_cb (GtkTextView *text_view,
                                 GdkEvent    *event,
                                 GbSourceVim *vim)
{
  g_return_val_if_fail (GTK_IS_TEXT_VIEW (text_view), FALSE);
  g_return_val_if_fail (event, FALSE);
  g_return_val_if_fail (GB_IS_SOURCE_VIM (vim), FALSE);

  if (vim->priv->mode == GB_SOURCE_VIM_COMMAND)
    gb_source_vim_set_mode (vim, GB_SOURCE_VIM_NORMAL);

  return FALSE;
}

static gboolean
gb_source_vim_focus_out_event_cb (GtkTextView *text_view,
                                  GdkEvent    *event,
                                  GbSourceVim *vim)
{
  g_return_val_if_fail (GTK_IS_TEXT_VIEW (text_view), FALSE);
  g_return_val_if_fail (event, FALSE);
  g_return_val_if_fail (GB_IS_SOURCE_VIM (vim), FALSE);

  vim->priv->in_ctrl_w = FALSE;

  return FALSE;
}

static void
gb_source_vim_maybe_adjust_insert (GbSourceVim *vim)
{
  GtkTextBuffer *buffer;
  GtkTextMark *insert;
  GtkTextIter iter;

  g_return_if_fail (GB_IS_SOURCE_VIM (vim));

  buffer = gtk_text_view_get_buffer (vim->priv->text_view);
  insert = gtk_text_buffer_get_insert (buffer);
  gtk_text_buffer_get_iter_at_mark (buffer, &iter, insert);

  if (gtk_text_iter_ends_line (&iter) &&
      !gtk_text_iter_starts_line (&iter) &&
      !gtk_text_buffer_get_has_selection (buffer))
    {
      /*
       * Probably want to add a canary here for dealing with reentrancy.
       */
      if (gtk_text_iter_backward_char (&iter))
        gtk_text_buffer_select_range (buffer, &iter, &iter);
    }
}

static void
gb_source_vim_mark_set_cb (GtkTextBuffer *buffer,
                           GtkTextIter   *iter,
                           GtkTextMark   *mark,
                           GbSourceVim   *vim)
{
  g_return_if_fail (GTK_IS_TEXT_BUFFER (buffer));
  g_return_if_fail (iter);
  g_return_if_fail (GTK_IS_TEXT_MARK (mark));
  g_return_if_fail (GB_IS_SOURCE_VIM (vim));

  if (vim->priv->mode == GB_SOURCE_VIM_INSERT)
    return;

  if (!gtk_widget_has_focus (GTK_WIDGET (vim->priv->text_view)))
    return;

  if (mark != gtk_text_buffer_get_insert (buffer))
    return;

  gb_source_vim_maybe_adjust_insert (vim);
}

static void
gb_source_vim_delete_range_cb (GtkTextBuffer *buffer,
                               GtkTextIter   *begin,
                               GtkTextIter   *end,
                               GbSourceVim   *vim)
{
  GtkTextIter iter;
  GtkTextMark *insert;
  guint line;
  guint end_line;
  guint begin_line;

  g_return_if_fail (GTK_IS_TEXT_BUFFER (buffer));
  g_return_if_fail (begin);
  g_return_if_fail (end);
  g_return_if_fail (GB_IS_SOURCE_VIM (vim));

  /*
   * If we are not the focus widget, then it is not our problem.
   */
  if (!gtk_widget_has_focus (GTK_WIDGET (vim->priv->text_view)))
    return;

  if (vim->priv->mode == GB_SOURCE_VIM_INSERT)
    return;

  /*
   * Replace the cursor if we maybe deleted past the end of the line.
   * This should force the cursor to be on the last character instead of
   * after it.
   */

  insert = gtk_text_buffer_get_insert (buffer);
  gtk_text_buffer_get_iter_at_mark (buffer, &iter, insert);

  line = gtk_text_iter_get_line (&iter);
  begin_line = gtk_text_iter_get_line (begin);
  end_line = gtk_text_iter_get_line (end);

  if (line >= begin_line && line <= end_line)
    {
      if (gtk_text_iter_ends_line (end))
        gb_source_vim_move_line_end (vim);
    }
}

static int
str_compare_qsort (const void *aptr,
                   const void *bptr)
{
  const gchar * const *a = aptr;
  const gchar * const *b = bptr;

  return g_strcmp0 (*a, *b);
}

static void
gb_source_vim_op_sort (GbSourceVim *vim,
                       const gchar *command_text)
{
  GtkTextBuffer *buffer;
  GtkTextMark *insert;
  GtkTextIter begin;
  GtkTextIter end;
  GtkTextIter cursor;
  guint cursor_offset;
  gchar *text;
  gchar **parts;

  g_assert (GB_IS_SOURCE_VIM (vim));

  buffer = gtk_text_view_get_buffer (vim->priv->text_view);
  gtk_text_buffer_get_selection_bounds (buffer, &begin, &end);

  if (gtk_text_iter_equal (&begin, &end))
    return;

  insert = gtk_text_buffer_get_insert (buffer);
  gtk_text_buffer_get_iter_at_mark (buffer, &cursor, insert);
  cursor_offset = gtk_text_iter_get_offset (&cursor);

  if (gtk_text_iter_compare (&begin, &end) > 0)
    text_iter_swap (&begin, &end);

  if (gtk_text_iter_starts_line (&end))
    gtk_text_iter_backward_char (&end);

  text = gtk_text_iter_get_slice (&begin, &end);
  parts = g_strsplit (text, "\n", 0);
  g_free (text);

  qsort (parts, g_strv_length (parts), sizeof (gchar *),
         str_compare_qsort);

  text = g_strjoinv ("\n", parts);
  gtk_text_buffer_delete (buffer, &begin, &end);
  gtk_text_buffer_insert (buffer, &begin, text, -1);
  g_free (text);
  g_strfreev (parts);

  gtk_text_buffer_get_iter_at_offset (buffer, &begin, cursor_offset);
  gtk_text_buffer_select_range (buffer, &begin, &begin);
}

static void
gb_source_vim_connect (GbSourceVim *vim)
{
  GtkTextBuffer *buffer;

  g_return_if_fail (GB_IS_SOURCE_VIM (vim));
  g_return_if_fail (!vim->priv->connected);

  buffer = gtk_text_view_get_buffer (vim->priv->text_view);

  vim->priv->key_press_event_handler =
    g_signal_connect_object (vim->priv->text_view,
                             "key-press-event",
                             G_CALLBACK (gb_source_vim_key_press_event_cb),
                             vim,
                             0);

  vim->priv->event_after_handler =
    g_signal_connect_object (vim->priv->text_view,
                             "event-after",
                             G_CALLBACK (gb_source_vim_event_after_cb),
                             vim,
                             0);

  vim->priv->key_release_event_handler =
    g_signal_connect_object (vim->priv->text_view,
                             "key-release-event",
                             G_CALLBACK (gb_source_vim_key_release_event_cb),
                             vim,
                             0);

  vim->priv->focus_in_event_handler =
    g_signal_connect_object (vim->priv->text_view,
                             "focus-in-event",
                             G_CALLBACK (gb_source_vim_focus_in_event_cb),
                             vim,
                             0);

  vim->priv->focus_out_event_handler =
    g_signal_connect_object (vim->priv->text_view,
                             "focus-out-event",
                             G_CALLBACK (gb_source_vim_focus_out_event_cb),
                             vim,
                             0);

  vim->priv->mark_set_handler =
    g_signal_connect_object (buffer,
                            "mark-set",
                            G_CALLBACK (gb_source_vim_mark_set_cb),
                            vim,
                            G_CONNECT_AFTER);

  vim->priv->delete_range_handler =
    g_signal_connect_object (buffer,
                            "delete-range",
                            G_CALLBACK (gb_source_vim_delete_range_cb),
                            vim,
                            G_CONNECT_AFTER);

  if (GTK_SOURCE_IS_BUFFER (buffer))
    vim->priv->search_context =
      gtk_source_search_context_new (GTK_SOURCE_BUFFER (buffer),
                                     vim->priv->search_settings);

  gb_source_vim_set_mode (vim, GB_SOURCE_VIM_NORMAL);

  vim->priv->connected = TRUE;
}

static void
gb_source_vim_disconnect (GbSourceVim *vim)
{
  g_return_if_fail (GB_IS_SOURCE_VIM (vim));
  g_return_if_fail (vim->priv->connected);

  if (vim->priv->mode == GB_SOURCE_VIM_NORMAL)
    gtk_text_view_set_overwrite (vim->priv->text_view, FALSE);

  g_signal_handler_disconnect (vim->priv->text_view,
                               vim->priv->key_press_event_handler);
  vim->priv->key_press_event_handler = 0;

  g_signal_handler_disconnect (vim->priv->text_view,
                               vim->priv->event_after_handler);
  vim->priv->event_after_handler = 0;

  g_signal_handler_disconnect (vim->priv->text_view,
                               vim->priv->key_release_event_handler);
  vim->priv->key_release_event_handler = 0;

  g_signal_handler_disconnect (vim->priv->text_view,
                               vim->priv->focus_in_event_handler);
  vim->priv->focus_in_event_handler = 0;

  g_signal_handler_disconnect (vim->priv->text_view,
                               vim->priv->focus_out_event_handler);
  vim->priv->focus_out_event_handler = 0;

  g_signal_handler_disconnect (gtk_text_view_get_buffer (vim->priv->text_view),
                               vim->priv->mark_set_handler);
  vim->priv->mark_set_handler = 0;

  g_signal_handler_disconnect (gtk_text_view_get_buffer (vim->priv->text_view),
                               vim->priv->delete_range_handler);
  vim->priv->delete_range_handler = 0;

  g_clear_object (&vim->priv->search_context);

  vim->priv->mode = 0;

  vim->priv->connected = FALSE;
}

gboolean
gb_source_vim_get_enabled (GbSourceVim *vim)
{
  g_return_val_if_fail (GB_IS_SOURCE_VIM (vim), FALSE);

  return vim->priv->enabled;
}

void
gb_source_vim_set_enabled (GbSourceVim *vim,
                           gboolean     enabled)
{
  GbSourceVimPrivate *priv;

  g_return_if_fail (GB_IS_SOURCE_VIM (vim));

  priv = vim->priv;

  if (priv->enabled == enabled)
    return;

  if (enabled)
    {

      gb_source_vim_connect (vim);
      priv->enabled = TRUE;
      gb_source_vim_maybe_adjust_insert (vim);
    }
  else
    {
      gb_source_vim_disconnect (vim);
      priv->enabled = FALSE;
    }

  g_object_notify_by_pspec (G_OBJECT (vim), gParamSpecs [PROP_ENABLED]);
}

const gchar *
gb_source_vim_get_search_text (GbSourceVim *vim)
{
  const gchar *search_text;

  g_return_val_if_fail (GB_IS_SOURCE_VIM (vim), NULL);

  search_text = gtk_source_search_settings_get_search_text (vim->priv->search_settings);

  return search_text;
}

void
gb_source_vim_set_search_text (GbSourceVim     *vim,
                               const char      *search_text)
{
  const gchar *old_search_text;

  old_search_text = gtk_source_search_settings_get_search_text (vim->priv->search_settings);

  if (g_strcmp0 (old_search_text, search_text) == 0)
    return;

  gtk_source_search_settings_set_search_text (vim->priv->search_settings,
                                              search_text);

  g_object_notify_by_pspec (G_OBJECT (vim), gParamSpecs [PROP_SEARCH_TEXT]);
}

GtkDirectionType
gb_source_vim_get_search_direction (GbSourceVim *vim)
{
  g_return_val_if_fail (GB_IS_SOURCE_VIM (vim), GTK_DIR_DOWN);

  return vim->priv->search_direction;
}

void
gb_source_vim_set_search_direction (GbSourceVim      *vim,
                                    GtkDirectionType  search_direction)
{
  if (vim->priv->search_direction == search_direction)
    return;

  vim->priv->search_direction = search_direction;

  g_object_notify_by_pspec (G_OBJECT (vim), gParamSpecs [PROP_SEARCH_DIRECTION]);
}

GtkWidget *
gb_source_vim_get_text_view (GbSourceVim *vim)
{
  g_return_val_if_fail (GB_IS_SOURCE_VIM (vim), NULL);

  return (GtkWidget *)vim->priv->text_view;
}

static void
gb_source_vim_set_text_view (GbSourceVim *vim,
                             GtkTextView *text_view)
{
  GbSourceVimPrivate *priv;

  g_return_if_fail (GB_IS_SOURCE_VIM (vim));
  g_return_if_fail (GTK_IS_TEXT_VIEW (text_view));

  priv = vim->priv;

  if (priv->text_view == text_view)
    return;

  if (priv->text_view)
    {
      if (priv->enabled)
        gb_source_vim_disconnect (vim);
      g_object_remove_weak_pointer (G_OBJECT (priv->text_view),
                                    (gpointer *)&priv->text_view);
      priv->text_view = NULL;
    }

  if (text_view)
    {
      priv->text_view = text_view;
      g_object_add_weak_pointer (G_OBJECT (text_view),
                                 (gpointer *)&priv->text_view);
      if (priv->enabled)
        gb_source_vim_connect (vim);
    }

  g_object_notify_by_pspec (G_OBJECT (vim), gParamSpecs [PROP_TEXT_VIEW]);
}

static void
gb_source_vim_op_syntax (GbSourceVim *vim,
                         const gchar *name)
{
  GtkTextBuffer *buffer;
  gboolean enabled;

  g_assert (GB_IS_SOURCE_VIM (vim));
  g_assert (g_str_has_prefix (name, "syntax "));

  name += strlen ("syntax ");

  buffer = gtk_text_view_get_buffer (vim->priv->text_view);

  if (!GTK_SOURCE_IS_BUFFER (buffer))
    return;

  if (g_strcmp0 (name, "on") == 0)
    enabled = TRUE;
  else if (g_strcmp0 (name, "off") == 0)
    enabled = FALSE;
  else
    return;

  gtk_source_buffer_set_highlight_syntax (GTK_SOURCE_BUFFER (buffer), enabled);
}

static void
gb_source_vim_op_colorscheme (GbSourceVim *vim,
                              const gchar *name)
{
  GtkSourceStyleSchemeManager *manager;
  GtkSourceStyleScheme *scheme;
  GtkTextBuffer *buffer;

  g_assert (GB_IS_SOURCE_VIM (vim));
  g_assert (g_str_has_prefix (name, "colorscheme "));

  name += strlen ("colorscheme ");

  buffer = gtk_text_view_get_buffer (vim->priv->text_view);
  if (!GTK_SOURCE_IS_BUFFER (buffer))
    return;

  manager = gtk_source_style_scheme_manager_get_default ();
  scheme = gtk_source_style_scheme_manager_get_scheme (manager, name);

  if (scheme)
    gtk_source_buffer_set_style_scheme (GTK_SOURCE_BUFFER (buffer), scheme);
}

static gboolean
gb_source_vim_match_is_selected (GbSourceVim *vim,
                                 GtkTextIter *match_begin,
                                 GtkTextIter *match_end)
{
  GtkTextIter sel_begin;
  GtkTextIter sel_end;

  g_return_val_if_fail (GB_IS_SOURCE_VIM (vim), FALSE);
  g_return_val_if_fail (match_begin, FALSE);
  g_return_val_if_fail (match_end, FALSE);

  gb_source_vim_get_selection_bounds (vim, &sel_begin, &sel_end);

  if (gtk_text_iter_compare (&sel_begin, &sel_end) > 0)
    text_iter_swap (&sel_begin, &sel_end);

  return ((gtk_text_iter_compare (&sel_begin, match_begin) <= 0) &&
          (gtk_text_iter_compare (&sel_begin, match_end) < 0) &&
          (gtk_text_iter_compare (&sel_end, match_begin) > 0) &&
          (gtk_text_iter_compare (&sel_end, match_end) >= 0));
}

static void
gb_source_vim_do_search_and_replace (GbSourceVim *vim,
                                     GtkTextIter *begin,
                                     GtkTextIter *end,
                                     const gchar *search_text,
                                     const gchar *replace_text,
                                     gboolean     is_global)
{
  GtkTextBuffer *buffer;
  GtkTextMark *mark;
  GtkTextIter tmp1;
  GtkTextIter tmp2;
  GtkTextIter match_begin;
  GtkTextIter match_end;
  GError *error = NULL;

  g_assert (GB_IS_SOURCE_VIM (vim));
  g_assert (search_text);
  g_assert (replace_text);
  g_assert ((!begin && !end) || (begin && end));

  if (!vim->priv->search_context)
    return;

  buffer = gtk_text_view_get_buffer (vim->priv->text_view);

  if (!begin)
    {
      gtk_text_buffer_get_start_iter (buffer, &tmp1);
      begin = &tmp1;
    }

  if (!end)
    {
      gtk_text_buffer_get_end_iter (buffer, &tmp2);
      end = &tmp2;
    }

  mark = gtk_text_buffer_create_mark (buffer, NULL, end, FALSE);

  gb_source_vim_set_search_text (vim, search_text);
  gtk_source_search_settings_set_case_sensitive (vim->priv->search_settings,
                                                 TRUE);

  while (gtk_source_search_context_forward (vim->priv->search_context, begin,
                                            &match_begin, &match_end))
    {
      if (is_global ||
          gb_source_vim_match_is_selected (vim, &match_begin, &match_end))
        {
          GtkTextMark *mark2;

          mark2 = gtk_text_buffer_create_mark (buffer, NULL, &match_end, FALSE);

          if (!gtk_source_search_context_replace (vim->priv->search_context,
                                                  &match_begin, &match_end,
                                                  replace_text, -1, &error))
            {
              g_warning ("%s", error->message);
              g_clear_error (&error);
              gtk_text_buffer_delete_mark (buffer, mark2);
              break;
            }

          gtk_text_buffer_get_iter_at_mark (buffer, &match_end, mark2);
          gtk_text_buffer_delete_mark (buffer, mark2);
        }

      *begin = match_end;

      gtk_text_buffer_get_iter_at_mark (buffer, end, mark);
    }

  gtk_text_buffer_delete_mark (buffer, mark);
}

static void
gb_source_vim_op_search_and_replace (GbSourceVim *vim,
                                     const gchar *command)
{
  GtkTextBuffer *buffer;
  const gchar *search_begin = NULL;
  const gchar *search_end = NULL;
  const gchar *replace_begin = NULL;
  const gchar *replace_end = NULL;
  gchar *search_text = NULL;
  gchar *replace_text = NULL;
  gunichar separator;

  g_assert (GB_IS_SOURCE_VIM (vim));
  g_assert (g_str_has_prefix (command, "%s") ||
            g_str_has_prefix (command, "s"));

  if (*command == '%')
    command++;
  command++;

  separator = g_utf8_get_char (command);
  if (!separator)
    return;

  search_begin = command = g_utf8_next_char (command);

  for (; *command; command = g_utf8_next_char (command))
    {
      if (*command == '\\')
        {
          command = g_utf8_next_char (command);
          if (!*command)
            return;
          continue;
        }

      if (g_utf8_get_char (command) == separator)
        {
          search_end = command;
          break;
        }
    }

  if (!search_end)
    return;

  replace_begin = command = g_utf8_next_char (command);

  for (; *command; command = g_utf8_next_char (command))
    {
      if (*command == '\\')
        {
          command = g_utf8_next_char (command);
          if (!*command)
            return;
          continue;
        }

      if (g_utf8_get_char (command) == separator)
        {
          replace_end = command;
          break;
        }
    }

  if (!replace_end)
    return;

  command = g_utf8_next_char (command);

  if (*command)
    {
      for (; *command; command++)
        {
          switch (*command)
            {
            case 'g':
              break;

            /* what other options are supported? */
            default:
              break;
            }
        }
    }

  search_text = g_strndup (search_begin, search_end - search_begin);
  replace_text = g_strndup (replace_begin, replace_end - replace_begin);

  buffer = gtk_text_view_get_buffer (vim->priv->text_view);

  if (gtk_text_buffer_get_has_selection (buffer))
    {
      GtkTextIter begin;
      GtkTextIter end;

      gtk_text_buffer_get_selection_bounds (buffer, &begin, &end);

      if (gtk_text_iter_compare (&begin, &end) > 0)
        text_iter_swap (&begin, &end);
      gb_source_vim_do_search_and_replace (vim, &begin, &end, search_text,
                                           replace_text, FALSE);
    }
  else
    gb_source_vim_do_search_and_replace (vim, NULL, NULL, search_text,
                                         replace_text, TRUE);

  g_free (search_text);
  g_free (replace_text);
}

static void
gb_source_vim_op_nohl (GbSourceVim *vim,
                       const gchar *command_text)
{
  if (vim->priv->search_context)
    gtk_source_search_context_set_highlight (vim->priv->search_context, FALSE);
}

static void
gb_source_vim_op_goto_line (GbSourceVim *vim,
                            const gchar *command_text)
{
  if (command_text[0] == '$')
    gb_source_vim_move_to_end (vim);
  else
    {
      int line = atoi (command_text);
      line = line > 0 ? line - 1 : 0;

      gb_source_vim_move_to_line_n (vim, line);
    }
}

static void
gb_source_vim_op_set_pair (GbSourceVim *vim,
                           const gchar *key,
                           const gchar *value)
{
  GtkSourceView *source_view;
  GtkTextBuffer *buffer;

  g_return_if_fail (GB_IS_SOURCE_VIM (vim));
  g_return_if_fail (key);
  g_return_if_fail (value);

  if (!GTK_SOURCE_IS_VIEW (vim->priv->text_view))
    return;

  source_view = GTK_SOURCE_VIEW (vim->priv->text_view);
  buffer = gtk_text_view_get_buffer (vim->priv->text_view);

  if (!GTK_SOURCE_IS_BUFFER (buffer))
    return;

  if (g_str_equal ("ts", key) || g_str_equal ("tabstop", key))
    {
      gint64 v64;

      v64 = g_ascii_strtoll (value, NULL, 10);
      if (((v64 == G_MAXINT64) || (v64 == G_MININT64)) && (errno == ERANGE))
        return;

      gtk_source_view_set_tab_width (source_view, v64);
    }
  else if (g_str_equal ("sw", key) || g_str_equal ("shiftwidth", key))
    {
      gint64 v64;

      v64 = g_ascii_strtoll (value, NULL, 10);
      if (((v64 == G_MAXINT64) || (v64 == G_MININT64)) && (errno == ERANGE))
        return;

      gtk_source_view_set_indent_width (source_view, v64);
    }
  else if (g_str_equal ("so", key) || g_str_equal ("scrolloff", key))
    {
      gint64 v64;

      v64 = g_ascii_strtoll (value, NULL, 10);
      if (((v64 == G_MAXINT64) || (v64 == G_MININT64)) && (errno == ERANGE))
        return;

      vim->priv->scroll_off = (guint)v64;
    }
  else if (g_str_has_prefix (key, "nonu"))
    {
      gtk_source_view_set_show_line_numbers (source_view, FALSE);
    }
  else if (g_str_has_prefix (key, "nu"))
    {
      gtk_source_view_set_show_line_numbers (source_view, TRUE);
    }
  else if (g_str_equal (key, "et"))
    {
      gtk_source_view_set_insert_spaces_instead_of_tabs (source_view, TRUE);
    }
  else if (g_str_equal (key, "noet"))
    {
      gtk_source_view_set_insert_spaces_instead_of_tabs (source_view, FALSE);
    }
  else if (g_str_equal (key, "ft") || g_str_equal (key, "filetype"))
    {
      GtkSourceLanguageManager *manager;
      GtkSourceLanguage *language;

      manager = gtk_source_language_manager_get_default ();
      language = gtk_source_language_manager_get_language (manager, value);
      gtk_source_buffer_set_language (GTK_SOURCE_BUFFER (buffer), language);

      gtk_widget_queue_draw (GTK_WIDGET (vim->priv->text_view));
    }
}

static void
gb_source_vim_op_set (GbSourceVim *vim,
                      const gchar *command_text)
{
  gchar **parts;
  guint i;

  g_return_if_fail (GB_IS_SOURCE_VIM (vim));
  g_return_if_fail (g_str_has_prefix (command_text, "set "));

  command_text += strlen ("set ");

  parts = g_strsplit (command_text, " ", 0);

  for (i = 0; parts [i]; i++)
    {
      gchar **kv;

      kv = g_strsplit (parts [i], "=", 2);
      gb_source_vim_op_set_pair (vim, kv [0], kv [1] ? kv [1] : "");
      g_strfreev (kv);
    }

  g_strfreev (parts);
}

static void
gb_source_vim_op_edit (GbSourceVim *vim,
                       const gchar *command_text)
{
  const gchar *path;
  GFile *file;

  g_return_if_fail (GB_IS_SOURCE_VIM (vim));
  g_return_if_fail (g_str_has_prefix (command_text, "e ") ||
                    g_str_has_prefix (command_text, "edit "));

  path = strstr (command_text, " ") + 1;
  file = g_file_new_for_path (path);
  g_signal_emit (vim, gSignals [SWITCH_TO_FILE], 0, file);
  g_clear_object (&file);
}

static void
gb_source_vim_op_split_horizontal (GbSourceVim *vim,
                                   const gchar *command_text)
{
  gboolean ret = FALSE;

  g_return_if_fail (GB_IS_SOURCE_VIM (vim));

  g_signal_emit (vim, gSignals [SPLIT], 0,
                 GB_SOURCE_VIM_SPLIT_HORIZONTAL, &ret);
}

static void
gb_source_vim_op_split_vertical (GbSourceVim *vim,
                                 const gchar *command_text)
{
  gboolean ret = FALSE;

  g_return_if_fail (GB_IS_SOURCE_VIM (vim));

  g_signal_emit (vim, gSignals [SPLIT], 0,
                 GB_SOURCE_VIM_SPLIT_VERTICAL, &ret);
}

static GbSourceVimOperation
gb_source_vim_parse_operation (const gchar *command_text)
{
  static GRegex *goto_line_regex = NULL;
  GbSourceVimOperation ret = NULL;

  g_return_val_if_fail (command_text, NULL);

  if (!goto_line_regex)
    goto_line_regex = g_regex_new ("^([0-9]+|\\$)$", 0, 0, NULL);

  if (g_str_equal (command_text, "sort"))
    ret = gb_source_vim_op_sort;
  else if (g_str_has_prefix (command_text, "edit ") ||
           g_str_has_prefix (command_text, "e "))
    ret = gb_source_vim_op_edit;
  else if (g_str_equal (command_text, "nohl"))
    ret = gb_source_vim_op_nohl;
  else if (g_str_has_prefix (command_text, "set "))
    ret = gb_source_vim_op_set;
  else if (g_str_has_prefix (command_text, "syntax "))
    ret = gb_source_vim_op_syntax;
  else if (g_str_has_prefix (command_text, "colorscheme "))
    ret = gb_source_vim_op_colorscheme;
  else if (g_str_has_prefix (command_text, "%s"))
    ret = gb_source_vim_op_search_and_replace;
  else if (g_str_equal (command_text, "split") ||
           g_str_equal (command_text, "sp"))
    ret = gb_source_vim_op_split_horizontal;
  else if (g_str_equal (command_text, "vsplit") ||
           g_str_equal (command_text, "vsp"))
    ret = gb_source_vim_op_split_vertical;
  else if (g_regex_match (goto_line_regex, command_text, 0, NULL))
    ret = gb_source_vim_op_goto_line;
  /* XXX: Keep this last */
  else if (g_str_has_prefix (command_text, "s") &&
           !g_ascii_isalnum (command_text[1]))
    ret = gb_source_vim_op_search_and_replace;

  return ret;
}

gboolean
gb_source_vim_is_command (const gchar *command_text)
{
  GbSourceVimOperation func;

  g_return_val_if_fail (command_text, FALSE);

  func = gb_source_vim_parse_operation (command_text);
  if (func)
    return TRUE;

  /*
   * Some other valid commands, that we don't know how to handle.
   * (But they may be handled by EXECUTE_COMMAND signal.
   */
  if (g_str_equal (command_text, "w") ||
      g_str_equal (command_text, "wq") ||
      g_str_equal (command_text, "q") ||
      g_str_equal (command_text, "q!"))
    return TRUE;

  return FALSE;
}

static gboolean
gb_source_vim_real_execute_command (GbSourceVim *vim,
                                    const gchar *command)
{
  GbSourceVimOperation func;
  GtkTextBuffer *buffer;
  gboolean ret = FALSE;
  gchar *copy;

  g_return_val_if_fail (GB_IS_SOURCE_VIM (vim), FALSE);
  g_return_val_if_fail (command, FALSE);

  copy = g_strstrip (g_strdup (command));
  func = gb_source_vim_parse_operation (copy);

  if (func)
    {
      buffer = gtk_text_view_get_buffer (vim->priv->text_view);
      gtk_text_buffer_begin_user_action (buffer);
      func (vim, command);
      gb_source_vim_clear_selection (vim);
      gb_source_vim_set_mode (vim, GB_SOURCE_VIM_NORMAL);
      gtk_text_buffer_end_user_action (buffer);
      ret = TRUE;
    }

  g_free (copy);

  return ret;
}

gboolean
gb_source_vim_execute_command (GbSourceVim *vim,
                               const gchar *command)
{
  gboolean ret = FALSE;

  g_return_val_if_fail (GB_IS_SOURCE_VIM (vim), FALSE);
  g_return_val_if_fail (command, FALSE);

  g_signal_emit (vim, gSignals [EXECUTE_COMMAND], 0, command, &ret);

  return ret;
}

static void
gb_source_vim_finalize (GObject *object)
{
  GbSourceVimPrivate *priv = GB_SOURCE_VIM (object)->priv;

  if (priv->anim_timeout)
    {
      g_source_remove (priv->anim_timeout);
      priv->anim_timeout = 0;
    }

  if (priv->text_view)
    {
      gb_source_vim_disconnect (GB_SOURCE_VIM (object));
      g_object_remove_weak_pointer (G_OBJECT (priv->text_view),
                                    (gpointer *)&priv->text_view);
      priv->text_view = NULL;
    }

  g_clear_object (&priv->search_settings);
  g_clear_object (&priv->vim_settings);

  g_string_free (priv->phrase, TRUE);
  priv->phrase = NULL;

  g_clear_pointer (&priv->captured_events, g_ptr_array_unref);

  G_OBJECT_CLASS (gb_source_vim_parent_class)->finalize (object);
}

static void
gb_source_vim_get_property (GObject    *object,
                            guint       prop_id,
                            GValue     *value,
                            GParamSpec *pspec)
{
  GbSourceVim *vim = GB_SOURCE_VIM (object);

  switch (prop_id)
    {
    case PROP_ENABLED:
      g_value_set_boolean (value, gb_source_vim_get_enabled (vim));
      break;

    case PROP_MODE:
      g_value_set_enum (value, gb_source_vim_get_mode (vim));
      break;

    case PROP_PHRASE:
      g_value_set_string (value, vim->priv->phrase->str);
      break;

    case PROP_SEARCH_TEXT:
      g_value_set_string (value, gb_source_vim_get_search_text (vim));
      break;

    case PROP_SEARCH_DIRECTION:
      g_value_set_enum (value, gb_source_vim_get_search_direction (vim));
      break;

    case PROP_TEXT_VIEW:
      g_value_set_object (value, gb_source_vim_get_text_view (vim));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
gb_source_vim_set_property (GObject      *object,
                            guint         prop_id,
                            const GValue *value,
                            GParamSpec   *pspec)
{
  GbSourceVim *vim = GB_SOURCE_VIM (object);

  switch (prop_id)
    {
    case PROP_ENABLED:
      gb_source_vim_set_enabled (vim, g_value_get_boolean (value));
      break;

    case PROP_SEARCH_TEXT:
      gb_source_vim_set_search_text (vim, g_value_get_string (value));
      break;

    case PROP_SEARCH_DIRECTION:
      gb_source_vim_set_search_direction (vim, g_value_get_enum (value));
      break;

    case PROP_TEXT_VIEW:
      gb_source_vim_set_text_view (vim, g_value_get_object (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
gb_source_vim_cmd_repeat (GbSourceVim *vim,
                          guint        count,
                          gchar        modifier)
{
  GtkSourceCompletion *completion;
  GtkSourceView *source_view;

  g_return_if_fail (GB_IS_SOURCE_VIM (vim));

  if (!GTK_SOURCE_IS_VIEW (vim->priv->text_view) ||
      !vim->priv->recording_trigger ||
      !vim->priv->captured_events->len)
    return;

  source_view = GTK_SOURCE_VIEW (vim->priv->text_view);
  completion = gtk_source_view_get_completion (source_view);

  gtk_source_completion_block_interactive (completion);
  gb_source_vim_recording_replay (vim);
  gtk_source_completion_unblock_interactive (completion);
}

static void
gb_source_vim_begin_search (GbSourceVim      *vim,
                            GtkDirectionType  direction)
{
  GtkTextBuffer *buffer;
  GtkTextIter begin;
  GtkTextIter end;
  gchar *text = NULL;

  g_assert (GB_IS_SOURCE_VIM (vim));

  if (vim->priv->search_context)
    gtk_source_search_context_set_highlight (vim->priv->search_context, FALSE);

  buffer = gtk_text_view_get_buffer (vim->priv->text_view);

  if (gtk_text_buffer_get_has_selection (buffer))
    {
      gtk_text_buffer_get_selection_bounds (buffer, &begin, &end);
      text = gtk_text_iter_get_slice (&begin, &end);
    }

  g_signal_emit (vim, gSignals [BEGIN_SEARCH], 0, direction, text);

  g_free (text);
}

static void
gb_source_vim_cmd_begin_search (GbSourceVim *vim,
                                guint        count,
                                gchar        modifier)
{
  gb_source_vim_begin_search (vim, GTK_DIR_DOWN);
}

static void
gb_source_vim_cmd_begin_search_backward (GbSourceVim *vim,
                                         guint        count,
                                         gchar        modifier)
{
  gb_source_vim_begin_search (vim, GTK_DIR_UP);
}

static void
gb_source_vim_cmd_forward_line_end (GbSourceVim *vim,
                                    guint        count,
                                    gchar        modifier)
{
  g_assert (GB_IS_SOURCE_VIM (vim));

  gb_source_vim_move_line_end (vim);
}

static void
gb_source_vim_cmd_backward_0 (GbSourceVim *vim,
                              guint        count,
                              gchar        modifier)
{
  g_assert (GB_IS_SOURCE_VIM (vim));

  gb_source_vim_move_line0 (vim);
}

static void
gb_source_vim_cmd_backward_start (GbSourceVim *vim,
                                  guint        count,
                                  gchar        modifier)
{
  g_assert (GB_IS_SOURCE_VIM (vim));

  gb_source_vim_move_line_start (vim, FALSE);
}

static void
gb_source_vim_cmd_backward_paragraph (GbSourceVim *vim,
                                      guint        count,
                                      gchar        modifier)
{
  guint i;

  g_assert (GB_IS_SOURCE_VIM (vim));

  count = MAX (1, count);

  for (i = 0; i < count; i++)
    gb_source_vim_move_backward_paragraph (vim);
}

static void
gb_source_vim_cmd_forward_paragraph (GbSourceVim *vim,
                                      guint        count,
                                      gchar        modifier)
{
  guint i;

  g_assert (GB_IS_SOURCE_VIM (vim));

  count = MAX (1, count);

  for (i = 0; i < count; i++)
    gb_source_vim_move_forward_paragraph (vim);
}

static void
gb_source_vim_cmd_match_backward (GbSourceVim *vim,
                                  guint        count,
                                  gchar        modifier)
{
  guint i;

  g_assert (GB_IS_SOURCE_VIM (vim));

  count = MAX (1, count);

  for (i = 0; i < count; i++)
    gb_source_vim_reverse_search (vim);
}

static void
gb_source_vim_cmd_match_forward (GbSourceVim *vim,
                                 guint        count,
                                 gchar        modifier)
{
  guint i;

  g_assert (GB_IS_SOURCE_VIM (vim));

  count = MAX (1, count);

  for (i = 0; i < count; i++)
    gb_source_vim_search (vim);
}

static void
gb_source_vim_cmd_indent (GbSourceVim *vim,
                          guint        count,
                          gchar        modifier)
{
  guint i;

  g_assert (GB_IS_SOURCE_VIM (vim));

  count = MAX (1, count);

  for (i = 0; i < count; i++)
    gb_source_vim_indent (vim);

  gb_source_vim_clear_selection (vim);
}

static void
gb_source_vim_cmd_unindent (GbSourceVim *vim,
                            guint        count,
                            gchar        modifier)
{
  guint i;

  g_assert (GB_IS_SOURCE_VIM (vim));

  count = MAX (1, count);

  for (i = 0; i < count; i++)
    gb_source_vim_unindent (vim);

  gb_source_vim_clear_selection (vim);
}

static void
gb_source_vim_cmd_insert_end (GbSourceVim *vim,
                              guint        count,
                              gchar        modifier)
{
  g_assert (GB_IS_SOURCE_VIM (vim));

  gb_source_vim_set_mode (vim, GB_SOURCE_VIM_INSERT);
  gb_source_vim_recording_begin (vim, 'A', modifier);
  gb_source_vim_clear_selection (vim);
  gb_source_vim_move_line_end (vim);
}

static void
gb_source_vim_cmd_insert_after (GbSourceVim *vim,
                                guint        count,
                                gchar        modifier)
{
  g_assert (GB_IS_SOURCE_VIM (vim));

  gb_source_vim_set_mode (vim, GB_SOURCE_VIM_INSERT);
  gb_source_vim_recording_begin (vim, 'a', modifier);
  gb_source_vim_clear_selection (vim);
  gb_source_vim_move_forward (vim);
}

static void
gb_source_vim_cmd_backward_word (GbSourceVim *vim,
                                 guint        count,
                                 gchar        modifier)
{
  guint i;

  g_assert (GB_IS_SOURCE_VIM (vim));

  count = MAX (1, count);

  for (i = 0; i < count; i++)
    gb_source_vim_move_backward_word (vim);
}

static void
gb_source_vim_cmd_change (GbSourceVim *vim,
                          guint        count,
                          gchar        modifier)
{
  g_assert (GB_IS_SOURCE_VIM (vim));

  if (modifier == 'c')
    {
      gb_source_vim_cmd_delete (vim, count, 'd');
      gb_source_vim_cmd_insert_before_line (vim, 0, '\0');
    }
  else if (modifier != 'd')
    {
      /* cd should do nothing */
      gb_source_vim_cmd_delete (vim, count, modifier);
      gb_source_vim_set_mode (vim, GB_SOURCE_VIM_INSERT);
      gb_source_vim_recording_begin (vim, 'c', modifier);
    }
}

static void
gb_source_vim_cmd_change_to_end (GbSourceVim *vim,
                                 guint        count,
                                 gchar        modifier)
{
  g_assert (GB_IS_SOURCE_VIM (vim));

  gb_source_vim_cmd_delete_to_end (vim, count, '\0');
  gb_source_vim_set_mode (vim, GB_SOURCE_VIM_INSERT);
  gb_source_vim_recording_begin (vim, 'C', modifier);
  gb_source_vim_move_forward (vim);
}

static void
gb_source_vim_cmd_delete (GbSourceVim *vim,
                          guint        count,
                          gchar        modifier)
{
  GtkTextBuffer *buffer;

  g_assert (GB_IS_SOURCE_VIM (vim));

  buffer = gtk_text_view_get_buffer (vim->priv->text_view);

  if (!gtk_text_buffer_get_has_selection (buffer))
    {
      if (modifier == 'd')
        {
          GtkTextMark *insert;
          GtkTextIter end_iter;
          GtkTextIter mark_iter;

          /*
           * WORKAROUND:
           *
           * We need to workaround that we can't "line select" the last line
           * in the buffer with GtkTextBuffer. So instead, we'll just handle
           * that case specially here.
           */
          insert = gtk_text_buffer_get_insert (buffer);
          gtk_text_buffer_get_iter_at_mark (buffer, &mark_iter, insert);
          gtk_text_buffer_get_end_iter (buffer, &end_iter);

          if (gtk_text_iter_equal (&mark_iter, &end_iter))
            {
              gtk_text_iter_backward_char (&mark_iter);
              gb_source_vim_select_range (vim, &mark_iter, &end_iter);
            }
          else
            gb_source_vim_cmd_select_line (vim, count, '\0');
        }
      else
        gb_source_vim_apply_motion (vim, modifier, count);
    }

  gb_source_vim_delete_selection (vim);

  if (modifier == 'd')
    {
      /* Move the cursor to the 0 position */
      gb_source_vim_cmd_backward_0 (vim, 0, 0);
    }
}

static void
gb_source_vim_cmd_delete_to_end (GbSourceVim *vim,
                                 guint        count,
                                 gchar        modifier)
{
  guint i;

  g_assert (GB_IS_SOURCE_VIM (vim));

  count = MAX (1, count);

  gb_source_vim_clear_selection (vim);
  gb_source_vim_select_char (vim);
  gb_source_vim_move_line_end (vim);
  for (i = 1; i < count; i++)
    gb_source_vim_move_down (vim);
  gb_source_vim_delete_selection (vim);
}

static void
gb_source_vim_cmd_forward_word_end (GbSourceVim *vim,
                                    guint        count,
                                    gchar        modifier)
{
  guint i;

  g_assert (GB_IS_SOURCE_VIM (vim));

  count = MAX (1, count);

  for (i = 0; i < count; i++)
    gb_source_vim_move_forward_word_end (vim);
}

static void
gb_source_vim_cmd_find_char_forward (GbSourceVim *vim,
                                     guint        count,
                                     gchar        modifier)
{
  guint i;

  g_assert (GB_IS_SOURCE_VIM (vim));

  count = MAX (1, count);

  gb_source_vim_save_position (vim);
  for (i = 0; i < count; i++)
    if (!gb_source_vim_find_char_forward (vim, modifier))
      {
        gb_source_vim_restore_position (vim);
        return;
      }
}

static void
gb_source_vim_cmd_find_char_backward (GbSourceVim *vim,
                                      guint        count,
                                      gchar        modifier)
{
  guint i;

  g_assert (GB_IS_SOURCE_VIM (vim));

  count = MAX (1, count);

  gb_source_vim_save_position (vim);
  for (i = 0; i < count; i++)
    if (!gb_source_vim_find_char_backward (vim, modifier))
      {
        gb_source_vim_restore_position (vim);
        return;
      }
}

static void
gb_source_vim_cmd_find_char_exclusive_forward (GbSourceVim *vim,
                                               guint        count,
                                               gchar        modifier)
{
  guint i;

  g_assert (GB_IS_SOURCE_VIM (vim));

  count = MAX (1, count);

  gb_source_vim_save_position (vim);
  for (i = 0; i < count; i++)
    if (!gb_source_vim_find_char_forward (vim, modifier))
      {
        gb_source_vim_restore_position (vim);
        return;
      }
  gb_source_vim_move_backward (vim);
}

static void
gb_source_vim_cmd_find_char_exclusive_backward (GbSourceVim *vim,
                                                guint        count,
                                                gchar        modifier)
{
  guint i;

  g_assert (GB_IS_SOURCE_VIM (vim));

  count = MAX (1, count);

  gb_source_vim_save_position (vim);
  for (i = 0; i < count; i++)
    if (!gb_source_vim_find_char_backward (vim, modifier))
      {
        gb_source_vim_restore_position (vim);
        return;
      }
    gb_source_vim_move_forward (vim);
}

static void
gb_source_vim_cmd_g (GbSourceVim *vim,
                     guint        count,
                     gchar        modifier)
{
  g_assert (GB_IS_SOURCE_VIM (vim));

  /*
   * TODO: We have more plumbing todo so we can support commands that are
   *       multiple characters (gU gu g~ and gg are all separate commands).
   *       We can support `gU' on a selection, but not `gUw'.
   */

  switch (modifier)
    {
    case '~':
      /* swap case */
      break;

    case 'u':
      /* lowercase */
      break;

    case 'U':
      /* uppercase */
      break;

    case 'g':
      /* jump to beginning of buffer. */
      gb_source_vim_clear_selection (vim);
      gb_source_vim_move_to_line_n (vim, 0);
      break;

    default:
      break;
    }
}

static void
gb_source_vim_cmd_goto_line (GbSourceVim *vim,
                             guint        count,
                             gchar        modifier)
{
  g_assert (GB_IS_SOURCE_VIM (vim));

  if (count)
    gb_source_vim_move_to_line_n (vim, count - 1);
  else
    gb_source_vim_move_to_end (vim);
}

static void
gb_source_vim_cmd_move_backward (GbSourceVim *vim,
                                 guint        count,
                                 gchar        modifier)
{
  guint i;

  g_assert (GB_IS_SOURCE_VIM (vim));

  count = MAX (1, count);

  for (i = 0; i < count; i++)
    gb_source_vim_move_backward (vim);
}

static void
gb_source_vim_cmd_insert_start (GbSourceVim *vim,
                                guint        count,
                                gchar        modifier)
{
  g_assert (GB_IS_SOURCE_VIM (vim));

  gb_source_vim_set_mode (vim, GB_SOURCE_VIM_INSERT);
  gb_source_vim_recording_begin (vim, 'I', modifier);
  gb_source_vim_clear_selection (vim);
  gb_source_vim_move_line_start (vim, TRUE);
}

static void
gb_source_vim_cmd_insert (GbSourceVim *vim,
                          guint        count,
                          gchar        modifier)
{
  g_assert (GB_IS_SOURCE_VIM (vim));

  gb_source_vim_set_mode (vim, GB_SOURCE_VIM_INSERT);
  gb_source_vim_recording_begin (vim, 'i', modifier);
  gb_source_vim_clear_selection (vim);
}

static void
gb_source_vim_cmd_move_down (GbSourceVim *vim,
                             guint        count,
                             gchar        modifier)
{
  guint i;

  g_assert (GB_IS_SOURCE_VIM (vim));

  count = MAX (1, count);

  for (i = 0; i < count; i++)
    gb_source_vim_move_down (vim);
}

static void
gb_source_vim_cmd_move_up (GbSourceVim *vim,
                           guint        count,
                           gchar        modifier)
{
  guint i;

  g_assert (GB_IS_SOURCE_VIM (vim));

  count = MAX (1, count);

  for (i = 0; i < count; i++)
    gb_source_vim_move_up (vim);
}

static void
gb_source_vim_cmd_move_forward (GbSourceVim *vim,
                                guint        count,
                                gchar        modifier)
{
  guint i;

  g_assert (GB_IS_SOURCE_VIM (vim));

  count = MAX (1, count);

  for (i = 0; i < count; i++)
    gb_source_vim_move_forward (vim);
}

static void
gb_source_vim_cmd_repeat_search_reverse (GbSourceVim *vim,
                                         guint        count,
                                         gchar        modifier)
{
  GtkDirectionType search_direction;
  guint i;

  g_assert (GB_IS_SOURCE_VIM (vim));

  count = MAX (1, count);

  if (vim->priv->search_direction == GTK_DIR_DOWN)
    search_direction = GTK_DIR_UP;
  else if (vim->priv->search_direction == GTK_DIR_UP)
    search_direction = GTK_DIR_DOWN;
  else
    g_assert_not_reached ();

  for (i = 0; i < count; i++)
    gb_source_vim_repeat_search (vim, search_direction);
}

static void
gb_source_vim_cmd_repeat_search (GbSourceVim *vim,
                                 guint        count,
                                 gchar        modifier)
{
  guint i;

  g_assert (GB_IS_SOURCE_VIM (vim));

  count = MAX (1, count);

  for (i = 0; i < count; i++)
    gb_source_vim_repeat_search (vim, vim->priv->search_direction);
}

static void
gb_source_vim_cmd_jump_to_doc (GbSourceVim *vim,
                               guint        count,
                               gchar        modifier)
{
  GtkTextIter begin;
  GtkTextIter end;
  gchar *word;

  g_assert (GB_IS_SOURCE_VIM (vim));

  gb_source_vim_select_current_word (vim, &begin, &end);
  word = gtk_text_iter_get_slice (&begin, &end);
  g_signal_emit (vim, gSignals [JUMP_TO_DOC], 0, word);
  g_free (word);

  gb_source_vim_select_range (vim, &begin, &begin);
}

static void
gb_source_vim_cmd_insert_before_line (GbSourceVim *vim,
                                      guint        count,
                                      gchar        modifier)
{
  g_assert (GB_IS_SOURCE_VIM (vim));

  gb_source_vim_set_mode (vim, GB_SOURCE_VIM_INSERT);
  gb_source_vim_recording_begin (vim, 'O', modifier);
  gb_source_vim_insert_nl_before (vim);
}

static void
gb_source_vim_cmd_insert_after_line (GbSourceVim *vim,
                                     guint        count,
                                     gchar        modifier)
{
  g_assert (GB_IS_SOURCE_VIM (vim));

  gb_source_vim_set_mode (vim, GB_SOURCE_VIM_INSERT);
  gb_source_vim_recording_begin (vim, 'o', modifier);
  gb_source_vim_insert_nl_after (vim, TRUE);
}

static void
gb_source_vim_cmd_paste_after (GbSourceVim *vim,
                               guint        count,
                               gchar        modifier)
{
  guint i;

  g_assert (GB_IS_SOURCE_VIM (vim));

  count = MAX (1, count);

  for (i = 0; i < count; i++)
    gb_source_vim_paste (vim);
}

static void
gb_source_vim_cmd_paste_before (GbSourceVim *vim,
                                guint        count,
                                gchar        modifier)
{
  g_assert (GB_IS_SOURCE_VIM (vim));

  /* TODO: Paste Before intead of after. */
  gb_source_vim_cmd_paste_after (vim, count, modifier);
}

static void
gb_source_vim_cmd_overwrite (GbSourceVim *vim,
                             guint        count,
                             gchar        modifier)
{
  g_assert (GB_IS_SOURCE_VIM (vim));

  gb_source_vim_set_mode (vim, GB_SOURCE_VIM_INSERT);
  gb_source_vim_recording_begin (vim, 'R', modifier);
  gtk_text_view_set_overwrite (vim->priv->text_view, TRUE);
}

static void
gb_source_vim_cmd_replace (GbSourceVim *vim,
                           guint        count,
                           gchar        modifier)
{
  GtkTextBuffer *buffer;
  GtkTextIter begin, end;
  gboolean at_end;

  g_assert (GB_IS_SOURCE_VIM (vim));

  buffer = gtk_text_view_get_buffer (vim->priv->text_view);

  gtk_text_buffer_begin_user_action (buffer);
  gb_source_vim_delete_selection (vim);

  gtk_text_buffer_get_selection_bounds (buffer, &begin, &end);
  gtk_text_iter_forward_char (&begin);
  if (gtk_text_iter_ends_line (&begin))
    at_end = TRUE;
  else
    {
      gtk_text_iter_backward_char (&begin);
      at_end = FALSE;
    }

  gtk_text_buffer_insert (buffer, &begin, &modifier, 1);
  if (at_end)
    gb_source_vim_move_forward (vim);
  else
    gb_source_vim_move_backward (vim);

  gtk_text_buffer_end_user_action (buffer);
}

static void
gb_source_vim_cmd_substitute (GbSourceVim *vim,
                              guint        count,
                              gchar        modifier)
{
  g_assert (GB_IS_SOURCE_VIM (vim));

  gb_source_vim_cmd_change (vim, count, 'l');
}

static void
gb_source_vim_cmd_undo_redo (GbSourceVim *vim,
                             guint        count,
                             gchar        modifier)
{
  GtkTextBuffer *buffer;
  GtkTextIter iter;
  GtkTextIter selection;
  gboolean has_selection;

  g_assert (GB_IS_SOURCE_VIM (vim));

  buffer = gtk_text_view_get_buffer (vim->priv->text_view);
  if (!GTK_SOURCE_IS_BUFFER (buffer))
    return;

  /*
   * TODO: I don't like that we are overloading a command based on there
   *       being a selection or not. Real VIM probably handles this as
   *       selections having a different mode (thereby a deferent command
   *       hashtable lookup).
   */

  has_selection = gb_source_vim_get_selection_bounds (vim, &iter, &selection);

  if (has_selection)
    {
      gb_source_vim_toggle_case (vim, GTK_SOURCE_CHANGE_CASE_UPPER);
      return;
    }

  if (gtk_source_buffer_can_redo (GTK_SOURCE_BUFFER (buffer)))
    gb_source_vim_redo (vim);
  else
    gb_source_vim_undo (vim);
}

static void
gb_source_vim_cmd_undo (GbSourceVim *vim,
                        guint        count,
                        gchar        modifier)
{
  gboolean has_selection;
  GtkTextIter iter;
  GtkTextIter selection;
  guint i;

  g_assert (GB_IS_SOURCE_VIM (vim));

  /*
   * TODO: I don't like that we are overloading a command based on there
   *       being a selection or not. Real VIM probably handles this as
   *       selections having a different mode (thereby a deferent command
   *       hashtable lookup).
   */

  has_selection = gb_source_vim_get_selection_bounds (vim, &iter, &selection);

  if (has_selection)
    {
      gb_source_vim_toggle_case (vim, GTK_SOURCE_CHANGE_CASE_LOWER);
      return;
    }

  count = MAX (1, count);
  for (i = 0; i < count; i++)
    gb_source_vim_undo (vim);
}

static void
gb_source_vim_cmd_select_line (GbSourceVim *vim,
                               guint        count,
                               gchar        modifier)
{
  guint i;

  g_assert (GB_IS_SOURCE_VIM (vim));

  count = MAX (1, count);

  gb_source_vim_select_line (vim);
  for (i = 1; i < count; i++)
    gb_source_vim_move_down (vim);
}

static void
gb_source_vim_cmd_select (GbSourceVim *vim,
                          guint        count,
                          gchar        modifier)
{
  guint i;

  g_assert (GB_IS_SOURCE_VIM (vim));

  count = MAX (1, count);

  gb_source_vim_select_char (vim);
  for (i = 1; i < count; i++)
    gb_source_vim_move_forward (vim);
}

static void
gb_source_vim_cmd_forward_word (GbSourceVim *vim,
                                guint        count,
                                gchar        modifier)
{
  guint i;

  g_assert (GB_IS_SOURCE_VIM (vim));

  count = MAX (1, count);

  for (i = 0; i < count; i++)
    gb_source_vim_move_forward_word (vim);
}

static void
gb_source_vim_cmd_delete_selection (GbSourceVim *vim,
                                    guint        count,
                                    gchar        modifier)
{
  guint i;

  g_assert (GB_IS_SOURCE_VIM (vim));

  count = MAX (1, count);

  for (i = 0; i < count; i++)
    gb_source_vim_delete_selection (vim);
}

static void
gb_source_vim_cmd_yank (GbSourceVim *vim,
                        guint        count,
                        gchar        modifier)
{
  GtkTextBuffer *buffer;

  g_assert (GB_IS_SOURCE_VIM (vim));

  buffer = gtk_text_view_get_buffer (vim->priv->text_view);

  gb_source_vim_save_position (vim);

  if (!gtk_text_buffer_get_has_selection (buffer))
    {
      if (modifier == 'y')
        {
          GtkTextIter iter;
          GtkTextMark *insert;

          insert = gtk_text_buffer_get_insert (buffer);
          gtk_text_buffer_get_iter_at_mark (buffer, &iter, insert);

          if (!gtk_text_iter_forward_to_line_end (&iter))
            {
              GtkTextIter begin = iter;
              gchar *str;
              gchar *line;

              /*
               * WORKAROUND:
               *
               * We are on the last line of the file, so we can't fetch the
               * newline at the end of this line. We will need to synthesize
               * the line with our own trailing\n, so that the yank does not
               * include a prefixing newline.
               */
              gtk_text_iter_set_line_offset (&begin, 0);
              str = gtk_text_iter_get_slice (&begin, &iter);
              line = g_strdup_printf ("%s\n", str);
              gb_source_vim_set_clipboard_text (vim, line);
              g_free (str);
              g_free (line);

              goto finish;
            }

          gb_source_vim_cmd_select_line (vim, count, '\0');
        }
      else
        gb_source_vim_apply_motion (vim, modifier, count);
    }

  gb_source_vim_yank (vim);

finish:
  gb_source_vim_clear_selection (vim);
  gb_source_vim_restore_position (vim);
}

static void
gb_source_vim_cmd_join (GbSourceVim *vim,
                        guint        count,
                        gchar        modifier)
{
  g_assert (GB_IS_SOURCE_VIM (vim));

  gb_source_vim_join (vim);
}

static void
gb_source_vim_cmd_center (GbSourceVim *vim,
                          guint        count,
                          gchar        modifier)
{
  GtkTextBuffer *buffer;
  GtkTextMark *insert;
  GtkTextIter iter;

  g_assert (GB_IS_SOURCE_VIM (vim));

  buffer = gtk_text_view_get_buffer (vim->priv->text_view);
  insert = gtk_text_buffer_get_insert (buffer);
  gtk_text_buffer_get_iter_at_mark (buffer, &iter, insert);

  switch (modifier)
    {
    case 'b':
      gtk_text_view_scroll_to_iter (vim->priv->text_view, &iter, 0.0, TRUE,
                                    0.5, 1.0);
      break;

    case 't':
      gtk_text_view_scroll_to_iter (vim->priv->text_view, &iter, 0.0, TRUE,
                                    0.5, 0.0);
      break;

    case 'z':
      gtk_text_view_scroll_to_iter (vim->priv->text_view, &iter, 0.0, TRUE,
                                    0.5, 0.5);
      break;

    default:
      break;
    }
}

static void
gb_source_vim_cmd_matching_bracket (GbSourceVim *vim,
                                    guint        count,
                                    gchar        modifier)
{
  GtkTextIter iter;
  GtkTextIter selection;

  g_return_if_fail (GB_IS_SOURCE_VIM (vim));

  gb_source_vim_get_selection_bounds (vim, &iter, &selection);

  switch (gtk_text_iter_get_char (&iter))
    {
    case '{':
    case '}':
    case '[':
    case ']':
    case '(':
    case ')':
      gb_source_vim_move_matching_bracket (vim);
      break;

    default:
      break;
    }
}

static void
gb_source_vim_cmd_toggle_case (GbSourceVim *vim,
                               guint        count,
                               gchar        modifier)
{
  GtkTextBuffer *buffer;
  guint i;

  g_assert (GB_IS_SOURCE_VIM (vim));

  buffer = gtk_text_view_get_buffer (vim->priv->text_view);

  if (gtk_text_buffer_get_has_selection (buffer))
    count = 1;
  else
    count = MAX (1, count);

  for (i = 0; i < count; i++)
    gb_source_vim_toggle_case (vim, GTK_SOURCE_CHANGE_CASE_TOGGLE);
}

static void
gb_source_vim_class_register_command (GbSourceVimClass       *klass,
                                      gchar                   key,
                                      GbSourceVimCommandFlags flags,
                                      GbSourceVimCommandType  type,
                                      GbSourceVimCommandFunc  func)
{
  GbSourceVimCommand *cmd;
  gpointer keyptr = GINT_TO_POINTER ((gint)key);

  g_assert (GB_IS_SOURCE_VIM_CLASS (klass));

  /*
   * TODO: It would be neat to have gCommands be a field in the klass. We
   *       could then just chain up to discover the proper command. This
   *       allows for subclasses to override and add new commands.
   *       To do so will probably take registering the GObjectClass
   *       manually to set base_init().
   */

  if (!gCommands)
    gCommands = g_hash_table_new (g_direct_hash, g_direct_equal);

  cmd = g_new0 (GbSourceVimCommand, 1);
  cmd->type = type;
  cmd->key = key;
  cmd->func = func;
  cmd->flags = flags;

  g_hash_table_replace (gCommands, keyptr, cmd);
}

static void
gb_source_vim_class_init (GbSourceVimClass *klass)
{
  GObjectClass *object_class;

  object_class = G_OBJECT_CLASS (klass);
  object_class->finalize = gb_source_vim_finalize;
  object_class->get_property = gb_source_vim_get_property;
  object_class->set_property = gb_source_vim_set_property;

  klass->execute_command = gb_source_vim_real_execute_command;

  gParamSpecs [PROP_ENABLED] =
    g_param_spec_boolean ("enabled",
                          _("Enabled"),
                          _("If the VIM engine is enabled."),
                          FALSE,
                          (G_PARAM_READWRITE |
                           G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class, PROP_ENABLED,
                                   gParamSpecs [PROP_ENABLED]);

  gParamSpecs [PROP_MODE] =
    g_param_spec_enum ("mode",
                       _("Mode"),
                       _("The current mode of the widget."),
                       GB_TYPE_SOURCE_VIM_MODE,
                       GB_SOURCE_VIM_NORMAL,
                       (G_PARAM_READABLE |
                        G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class, PROP_MODE,
                                   gParamSpecs [PROP_MODE]);

  gParamSpecs [PROP_PHRASE] =
    g_param_spec_string ("phrase",
                         _("Phrase"),
                         _("The current phrase input."),
                         NULL,
                         (G_PARAM_READABLE |
                          G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class, PROP_PHRASE,
                                   gParamSpecs [PROP_PHRASE]);

  gParamSpecs [PROP_SEARCH_TEXT] =
    g_param_spec_string ("search-text",
                         _("Search Text"),
                         _("The last text searched for."),
                         NULL,
                         (G_PARAM_READWRITE |
                          G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class, PROP_SEARCH_TEXT,
                                   gParamSpecs [PROP_SEARCH_TEXT]);

  gParamSpecs [PROP_SEARCH_DIRECTION] =
    g_param_spec_enum ("search-direction",
                       _("Search Direction"),
                       _("The direction of the last text searched for."),
                       GTK_TYPE_DIRECTION_TYPE,
                       GTK_DIR_DOWN,
                       (G_PARAM_READWRITE |
                        G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class, PROP_SEARCH_DIRECTION,
                                   gParamSpecs [PROP_SEARCH_DIRECTION]);

  gParamSpecs [PROP_TEXT_VIEW] =
    g_param_spec_object ("text-view",
                         _("Text View"),
                         _("The text view the VIM engine is managing."),
                         GTK_TYPE_TEXT_VIEW,
                         (G_PARAM_READWRITE |
                          G_PARAM_CONSTRUCT_ONLY |
                          G_PARAM_STATIC_STRINGS));
  g_object_class_install_property (object_class, PROP_TEXT_VIEW,
                                   gParamSpecs [PROP_TEXT_VIEW]);

  /**
   * GbSourceVim::begin-search:
   * @search_direction: Direction to search
   * @search_text: (allow none): Optional search text to apply to the search.
   *
   * This signal is emitted when the `/` key is pressed. The consuming code
   * should make their search entry widget visible and set the search text
   * to @search_text if non-%NULL.
   */
  gSignals [BEGIN_SEARCH] =
    g_signal_new ("begin-search",
                  GB_TYPE_SOURCE_VIM,
                  G_SIGNAL_RUN_LAST,
                  G_STRUCT_OFFSET (GbSourceVimClass, begin_search),
                  NULL,
                  NULL,
                  g_cclosure_marshal_generic,
                  G_TYPE_NONE,
                  2,
                  GTK_TYPE_DIRECTION_TYPE,
                  G_TYPE_STRING);

  /**
   * GbSourceVim::command-visibility-toggled:
   * @visible: If the the command entry should be visible.
   *
   * The "command-visibility-toggled" signal is emitted when the command entry
   * should be shown or hidden. The command entry is used to interact with the
   * VIM style command line.
   */
  gSignals [COMMAND_VISIBILITY_TOGGLED] =
    g_signal_new ("command-visibility-toggled",
                  GB_TYPE_SOURCE_VIM,
                  G_SIGNAL_RUN_LAST,
                  G_STRUCT_OFFSET (GbSourceVimClass,
                                   command_visibility_toggled),
                  NULL,
                  NULL,
                  g_cclosure_marshal_VOID__BOOLEAN,
                  G_TYPE_NONE,
                  1,
                  G_TYPE_BOOLEAN);

  gSignals [EXECUTE_COMMAND] =
    g_signal_new ("execute-command",
                  GB_TYPE_SOURCE_VIM,
                  G_SIGNAL_RUN_LAST,
                  G_STRUCT_OFFSET (GbSourceVimClass, execute_command),
                  g_signal_accumulator_true_handled,
                  NULL,
                  g_cclosure_marshal_VOID__STRING,
                  G_TYPE_BOOLEAN,
                  1,
                  G_TYPE_STRING);

  /**
   * GbSourceVim::jump-to-doc:
   * @search_text: keyword to search for.
   *
   * Requests that documentation for @search_text is shown. This is typically
   * performed with SHIFT-K in VIM.
   */
  gSignals [JUMP_TO_DOC] =
    g_signal_new ("jump-to-doc",
                  GB_TYPE_SOURCE_VIM,
                  G_SIGNAL_RUN_LAST,
                  G_STRUCT_OFFSET (GbSourceVimClass, jump_to_doc),
                  NULL,
                  NULL,
                  g_cclosure_marshal_VOID__STRING,
                  G_TYPE_NONE,
                  1,
                  G_TYPE_STRING);

  /**
   * GbSourceVim::split:
   * @type: A #GbSourceVimSplit containing the split type.
   *
   * Requests a split operation on the current buffer.
   */
  gSignals [SPLIT] =
    g_signal_new ("split",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  G_STRUCT_OFFSET (GbSourceVimClass, split),
                  g_signal_accumulator_true_handled,
                  NULL,
                  g_cclosure_marshal_generic,
                  G_TYPE_BOOLEAN,
                  1,
                  GB_TYPE_SOURCE_VIM_SPLIT);

  gSignals [SWITCH_TO_FILE] =
    g_signal_new ("switch-to-file",
                  G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST,
                  G_STRUCT_OFFSET (GbSourceVimClass, switch_to_file),
                  NULL,
                  NULL,
                  g_cclosure_marshal_generic,
                  G_TYPE_NONE,
                  1,
                  G_TYPE_FILE);

  /*
   * Register all of our internal VIM commands. These can be used directly
   * or via phrases.
   */
  gb_source_vim_class_register_command (klass, '.',
                                        GB_SOURCE_VIM_COMMAND_FLAG_NONE,
                                        GB_SOURCE_VIM_COMMAND_CHANGE,
                                        gb_source_vim_cmd_repeat);
  gb_source_vim_class_register_command (klass, '/',
                                        GB_SOURCE_VIM_COMMAND_FLAG_NONE,
                                        GB_SOURCE_VIM_COMMAND_JUMP,
                                        gb_source_vim_cmd_begin_search);
  gb_source_vim_class_register_command (klass, '?',
                                        GB_SOURCE_VIM_COMMAND_FLAG_NONE,
                                        GB_SOURCE_VIM_COMMAND_JUMP,
                                        gb_source_vim_cmd_begin_search_backward);
  gb_source_vim_class_register_command (klass, '$',
                                        GB_SOURCE_VIM_COMMAND_FLAG_NONE,
                                        GB_SOURCE_VIM_COMMAND_MOVEMENT,
                                        gb_source_vim_cmd_forward_line_end);
  gb_source_vim_class_register_command (klass, '0',
                                        GB_SOURCE_VIM_COMMAND_FLAG_MOTION_EXCLUSIVE,
                                        GB_SOURCE_VIM_COMMAND_MOVEMENT,
                                        gb_source_vim_cmd_backward_0);
  gb_source_vim_class_register_command (klass, '^',
                                        GB_SOURCE_VIM_COMMAND_FLAG_MOTION_EXCLUSIVE,
                                        GB_SOURCE_VIM_COMMAND_MOVEMENT,
                                        gb_source_vim_cmd_backward_start);
  gb_source_vim_class_register_command (klass, '}',
                                        GB_SOURCE_VIM_COMMAND_FLAG_MOTION_EXCLUSIVE,
                                        GB_SOURCE_VIM_COMMAND_MOVEMENT,
                                        gb_source_vim_cmd_forward_paragraph);
  gb_source_vim_class_register_command (klass, '{',
                                        GB_SOURCE_VIM_COMMAND_FLAG_MOTION_EXCLUSIVE,
                                        GB_SOURCE_VIM_COMMAND_MOVEMENT,
                                        gb_source_vim_cmd_backward_paragraph);
  gb_source_vim_class_register_command (klass, '#',
                                        GB_SOURCE_VIM_COMMAND_FLAG_NONE,
                                        GB_SOURCE_VIM_COMMAND_JUMP,
                                        gb_source_vim_cmd_match_backward);
  gb_source_vim_class_register_command (klass, '*',
                                        GB_SOURCE_VIM_COMMAND_FLAG_NONE,
                                        GB_SOURCE_VIM_COMMAND_JUMP,
                                        gb_source_vim_cmd_match_forward);
  gb_source_vim_class_register_command (klass, '>',
                                        GB_SOURCE_VIM_COMMAND_FLAG_NONE,
                                        GB_SOURCE_VIM_COMMAND_CHANGE,
                                        gb_source_vim_cmd_indent);
  gb_source_vim_class_register_command (klass, '<',
                                        GB_SOURCE_VIM_COMMAND_FLAG_NONE,
                                        GB_SOURCE_VIM_COMMAND_CHANGE,
                                        gb_source_vim_cmd_unindent);
  gb_source_vim_class_register_command (klass, '%',
                                        GB_SOURCE_VIM_COMMAND_FLAG_NONE,
                                        GB_SOURCE_VIM_COMMAND_JUMP,
                                        gb_source_vim_cmd_matching_bracket);
  gb_source_vim_class_register_command (klass, '~',
                                        GB_SOURCE_VIM_COMMAND_FLAG_NONE,
                                        GB_SOURCE_VIM_COMMAND_CHANGE,
                                        gb_source_vim_cmd_toggle_case);
  gb_source_vim_class_register_command (klass, 'A',
                                        GB_SOURCE_VIM_COMMAND_FLAG_NONE,
                                        GB_SOURCE_VIM_COMMAND_CHANGE,
                                        gb_source_vim_cmd_insert_end);
  gb_source_vim_class_register_command (klass, 'a',
                                        GB_SOURCE_VIM_COMMAND_FLAG_NONE,
                                        GB_SOURCE_VIM_COMMAND_CHANGE,
                                        gb_source_vim_cmd_insert_after);
  gb_source_vim_class_register_command (klass, 'B',
                                        GB_SOURCE_VIM_COMMAND_FLAG_MOTION_EXCLUSIVE,
                                        GB_SOURCE_VIM_COMMAND_MOVEMENT,
                                        gb_source_vim_cmd_backward_word);
  gb_source_vim_class_register_command (klass, 'b',
                                        GB_SOURCE_VIM_COMMAND_FLAG_MOTION_EXCLUSIVE,
                                        GB_SOURCE_VIM_COMMAND_MOVEMENT,
                                        gb_source_vim_cmd_backward_word);
  gb_source_vim_class_register_command (klass, 'c',
                                        GB_SOURCE_VIM_COMMAND_FLAG_REQUIRES_MODIFIER |
                                        GB_SOURCE_VIM_COMMAND_FLAG_VISUAL,
                                        GB_SOURCE_VIM_COMMAND_CHANGE,
                                        gb_source_vim_cmd_change);
  gb_source_vim_class_register_command (klass, 'C',
                                        GB_SOURCE_VIM_COMMAND_FLAG_NONE,
                                        GB_SOURCE_VIM_COMMAND_CHANGE,
                                        gb_source_vim_cmd_change_to_end);
  gb_source_vim_class_register_command (klass, 'd',
                                        GB_SOURCE_VIM_COMMAND_FLAG_REQUIRES_MODIFIER |
                                        GB_SOURCE_VIM_COMMAND_FLAG_VISUAL,
                                        GB_SOURCE_VIM_COMMAND_CHANGE,
                                        gb_source_vim_cmd_delete);
  gb_source_vim_class_register_command (klass, 'D',
                                        GB_SOURCE_VIM_COMMAND_FLAG_NONE,
                                        GB_SOURCE_VIM_COMMAND_CHANGE,
                                        gb_source_vim_cmd_delete_to_end);
  gb_source_vim_class_register_command (klass, 'E',
                                        GB_SOURCE_VIM_COMMAND_FLAG_NONE,
                                        GB_SOURCE_VIM_COMMAND_MOVEMENT,
                                        gb_source_vim_cmd_forward_word_end);
  gb_source_vim_class_register_command (klass, 'e',
                                        GB_SOURCE_VIM_COMMAND_FLAG_NONE,
                                        GB_SOURCE_VIM_COMMAND_MOVEMENT,
                                        gb_source_vim_cmd_forward_word_end);
  gb_source_vim_class_register_command (klass, 'F',
                                        GB_SOURCE_VIM_COMMAND_FLAG_REQUIRES_MODIFIER |
                                        GB_SOURCE_VIM_COMMAND_FLAG_MOTION_EXCLUSIVE,
                                        GB_SOURCE_VIM_COMMAND_MOVEMENT,
                                        gb_source_vim_cmd_find_char_backward);
  gb_source_vim_class_register_command (klass, 'f',
                                        GB_SOURCE_VIM_COMMAND_FLAG_REQUIRES_MODIFIER,
                                        GB_SOURCE_VIM_COMMAND_MOVEMENT,
                                        gb_source_vim_cmd_find_char_forward);
  gb_source_vim_class_register_command (klass, 'G',
                                        GB_SOURCE_VIM_COMMAND_FLAG_MOTION_LINEWISE,
                                        GB_SOURCE_VIM_COMMAND_MOVEMENT,
                                        gb_source_vim_cmd_goto_line);
  gb_source_vim_class_register_command (klass, 'g',
                                        GB_SOURCE_VIM_COMMAND_FLAG_REQUIRES_MODIFIER,
                                        GB_SOURCE_VIM_COMMAND_CHANGE,
                                        gb_source_vim_cmd_g);
  gb_source_vim_class_register_command (klass, 'h',
                                        GB_SOURCE_VIM_COMMAND_FLAG_MOTION_EXCLUSIVE,
                                        GB_SOURCE_VIM_COMMAND_MOVEMENT,
                                        gb_source_vim_cmd_move_backward);
  gb_source_vim_class_register_command (klass, 'I',
                                        GB_SOURCE_VIM_COMMAND_FLAG_NONE,
                                        GB_SOURCE_VIM_COMMAND_CHANGE,
                                        gb_source_vim_cmd_insert_start);
  gb_source_vim_class_register_command (klass, 'i',
                                        GB_SOURCE_VIM_COMMAND_FLAG_NONE,
                                        GB_SOURCE_VIM_COMMAND_CHANGE,
                                        gb_source_vim_cmd_insert);
  gb_source_vim_class_register_command (klass, 'j',
                                        GB_SOURCE_VIM_COMMAND_FLAG_MOTION_LINEWISE,
                                        GB_SOURCE_VIM_COMMAND_MOVEMENT,
                                        gb_source_vim_cmd_move_down);
  gb_source_vim_class_register_command (klass, 'J',
                                        GB_SOURCE_VIM_COMMAND_FLAG_NONE,
                                        GB_SOURCE_VIM_COMMAND_CHANGE,
                                        gb_source_vim_cmd_join);
  gb_source_vim_class_register_command (klass, 'k',
                                        GB_SOURCE_VIM_COMMAND_FLAG_MOTION_LINEWISE,
                                        GB_SOURCE_VIM_COMMAND_MOVEMENT,
                                        gb_source_vim_cmd_move_up);
  gb_source_vim_class_register_command (klass, 'K',
                                        GB_SOURCE_VIM_COMMAND_FLAG_NONE,
                                        GB_SOURCE_VIM_COMMAND_NOOP,
                                        gb_source_vim_cmd_jump_to_doc);
  gb_source_vim_class_register_command (klass, 'l',
                                        GB_SOURCE_VIM_COMMAND_FLAG_MOTION_EXCLUSIVE,
                                        GB_SOURCE_VIM_COMMAND_MOVEMENT,
                                        gb_source_vim_cmd_move_forward);
  gb_source_vim_class_register_command (klass, 'N',
                                        GB_SOURCE_VIM_COMMAND_FLAG_NONE,
                                        GB_SOURCE_VIM_COMMAND_JUMP,
                                        gb_source_vim_cmd_repeat_search_reverse);
  gb_source_vim_class_register_command (klass, 'n',
                                        GB_SOURCE_VIM_COMMAND_FLAG_NONE,
                                        GB_SOURCE_VIM_COMMAND_JUMP,
                                        gb_source_vim_cmd_repeat_search);
  gb_source_vim_class_register_command (klass, 'O',
                                        GB_SOURCE_VIM_COMMAND_FLAG_NONE,
                                        GB_SOURCE_VIM_COMMAND_CHANGE,
                                        gb_source_vim_cmd_insert_before_line);
  gb_source_vim_class_register_command (klass, 'o',
                                        GB_SOURCE_VIM_COMMAND_FLAG_NONE,
                                        GB_SOURCE_VIM_COMMAND_CHANGE,
                                        gb_source_vim_cmd_insert_after_line);
  gb_source_vim_class_register_command (klass, 'P',
                                        GB_SOURCE_VIM_COMMAND_FLAG_NONE,
                                        GB_SOURCE_VIM_COMMAND_CHANGE,
                                        gb_source_vim_cmd_paste_before);
  gb_source_vim_class_register_command (klass, 'p',
                                        GB_SOURCE_VIM_COMMAND_FLAG_NONE,
                                        GB_SOURCE_VIM_COMMAND_CHANGE,
                                        gb_source_vim_cmd_paste_after);
  gb_source_vim_class_register_command (klass, 'R',
                                        GB_SOURCE_VIM_COMMAND_FLAG_NONE,
                                        GB_SOURCE_VIM_COMMAND_CHANGE,
                                        gb_source_vim_cmd_overwrite);
  gb_source_vim_class_register_command (klass, 'r',
                                        GB_SOURCE_VIM_COMMAND_FLAG_REQUIRES_MODIFIER,
                                        GB_SOURCE_VIM_COMMAND_CHANGE,
                                        gb_source_vim_cmd_replace);
  gb_source_vim_class_register_command (klass, 's',
                                        GB_SOURCE_VIM_COMMAND_FLAG_NONE,
                                        GB_SOURCE_VIM_COMMAND_CHANGE,
                                        gb_source_vim_cmd_substitute);
  gb_source_vim_class_register_command (klass, 'T',
                                        GB_SOURCE_VIM_COMMAND_FLAG_REQUIRES_MODIFIER |
                                        GB_SOURCE_VIM_COMMAND_FLAG_MOTION_EXCLUSIVE,
                                        GB_SOURCE_VIM_COMMAND_MOVEMENT,
                                        gb_source_vim_cmd_find_char_exclusive_backward);
  gb_source_vim_class_register_command (klass, 't',
                                        GB_SOURCE_VIM_COMMAND_FLAG_REQUIRES_MODIFIER,
                                        GB_SOURCE_VIM_COMMAND_MOVEMENT,
                                        gb_source_vim_cmd_find_char_exclusive_forward);
  gb_source_vim_class_register_command (klass, 'u',
                                        GB_SOURCE_VIM_COMMAND_FLAG_NONE,
                                        GB_SOURCE_VIM_COMMAND_CHANGE,
                                        gb_source_vim_cmd_undo);
  gb_source_vim_class_register_command (klass, 'U',
                                        GB_SOURCE_VIM_COMMAND_FLAG_NONE,
                                        GB_SOURCE_VIM_COMMAND_CHANGE,
                                        gb_source_vim_cmd_undo_redo);
  gb_source_vim_class_register_command (klass, 'V',
                                        GB_SOURCE_VIM_COMMAND_FLAG_NONE,
                                        GB_SOURCE_VIM_COMMAND_NOOP,
                                        gb_source_vim_cmd_select_line);
  gb_source_vim_class_register_command (klass, 'v',
                                        GB_SOURCE_VIM_COMMAND_FLAG_NONE,
                                        GB_SOURCE_VIM_COMMAND_NOOP,
                                        gb_source_vim_cmd_select);
  gb_source_vim_class_register_command (klass, 'W',
                                        GB_SOURCE_VIM_COMMAND_FLAG_MOTION_EXCLUSIVE,
                                        GB_SOURCE_VIM_COMMAND_MOVEMENT,
                                        gb_source_vim_cmd_forward_word);
  gb_source_vim_class_register_command (klass, 'w',
                                        GB_SOURCE_VIM_COMMAND_FLAG_MOTION_EXCLUSIVE,
                                        GB_SOURCE_VIM_COMMAND_MOVEMENT,
                                        gb_source_vim_cmd_forward_word);
  gb_source_vim_class_register_command (klass, 'x',
                                        GB_SOURCE_VIM_COMMAND_FLAG_VISUAL,
                                        GB_SOURCE_VIM_COMMAND_CHANGE,
                                        gb_source_vim_cmd_delete_selection);
  gb_source_vim_class_register_command (klass, 'y',
                                        GB_SOURCE_VIM_COMMAND_FLAG_REQUIRES_MODIFIER |
                                        GB_SOURCE_VIM_COMMAND_FLAG_VISUAL,
                                        GB_SOURCE_VIM_COMMAND_NOOP,
                                        gb_source_vim_cmd_yank);
  gb_source_vim_class_register_command (klass, 'z',
                                        GB_SOURCE_VIM_COMMAND_FLAG_REQUIRES_MODIFIER,
                                        GB_SOURCE_VIM_COMMAND_NOOP,
                                        gb_source_vim_cmd_center);
}

static void
gb_source_vim_scroll_off_changed (GbSourceVim *vim,
                                  const gchar *key,
                                  GSettings   *settings)
{
  g_return_if_fail (GB_IS_SOURCE_VIM (vim));
  g_return_if_fail (G_IS_SETTINGS (settings));

  vim->priv->scroll_off = g_settings_get_int (settings, "scroll-off");
  if (vim->priv->text_view != NULL)
    gb_source_vim_ensure_scroll (vim);
}

static void
gb_source_vim_init (GbSourceVim *vim)
{
  vim->priv = gb_source_vim_get_instance_private (vim);
  vim->priv->enabled = FALSE;
  vim->priv->mode = 0;
  vim->priv->phrase = g_string_new (NULL);
  vim->priv->search_settings = gtk_source_search_settings_new ();
  vim->priv->search_direction = GTK_DIR_DOWN;
  vim->priv->captured_events =
    g_ptr_array_new_with_free_func ((GDestroyNotify)gdk_event_free);

  vim->priv->vim_settings = g_settings_new ("org.gnome.builder.editor.vim");
  g_signal_connect_object (vim->priv->vim_settings,
                           "changed::scroll-off",
                           G_CALLBACK (gb_source_vim_scroll_off_changed),
                           vim,
                           G_CONNECT_SWAPPED);
  gb_source_vim_scroll_off_changed (vim, NULL, vim->priv->vim_settings);
}

GType
gb_source_vim_mode_get_type (void)
{
  static GType type_id;
  static const GEnumValue values[] = {
    { GB_SOURCE_VIM_NORMAL, "GB_SOURCE_VIM_NORMAL", "NORMAL" },
    { GB_SOURCE_VIM_INSERT, "GB_SOURCE_VIM_INSERT", "INSERT" },
    { GB_SOURCE_VIM_COMMAND, "GB_SOURCE_VIM_COMMAND", "COMMAND" },
    { 0 }
  };

  if (!type_id)
    type_id = g_enum_register_static ("GbSourceVimMode", values);

  return type_id;
}

GType
gb_source_vim_split_get_type (void)
{
  static GType type_id;
  static const GEnumValue values[] = {
    { GB_SOURCE_VIM_SPLIT_HORIZONTAL, "GB_SOURCE_VIM_SPLIT_HORIZONTAL", "HORIZONTAL" },
    { GB_SOURCE_VIM_SPLIT_VERTICAL, "GB_SOURCE_VIM_SPLIT_VERTICAL", "VERTICAL" },
    { GB_SOURCE_VIM_SPLIT_CLOSE, "GB_SOURCE_VIM_SPLIT_CLOSE", "CLOSE" },
    { GB_SOURCE_VIM_SPLIT_CYCLE_NEXT, "GB_SOURCE_VIM_SPLIT_CYCLE_NEXT", "CYCLE_NEXT" },
    { GB_SOURCE_VIM_SPLIT_CYCLE_PREVIOUS, "GB_SOURCE_VIM_SPLIT_CYCLE_PREVIOUS", "CYCLE_PREVIOUS" },
    { 0 }
  };

  if (!type_id)
    type_id = g_enum_register_static ("GbSourceVimSplit", values);

  return type_id;
}
