/* gb-document-manager.h
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

#ifndef GB_DOCUMENT_MANAGER_H
#define GB_DOCUMENT_MANAGER_H

#include <gio/gio.h>

#include "gb-document.h"

G_BEGIN_DECLS

#define GB_TYPE_DOCUMENT_MANAGER            (gb_document_manager_get_type())
#define GB_DOCUMENT_MANAGER(obj)            (G_TYPE_CHECK_INSTANCE_CAST ((obj), GB_TYPE_DOCUMENT_MANAGER, GbDocumentManager))
#define GB_DOCUMENT_MANAGER_CONST(obj)      (G_TYPE_CHECK_INSTANCE_CAST ((obj), GB_TYPE_DOCUMENT_MANAGER, GbDocumentManager const))
#define GB_DOCUMENT_MANAGER_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST ((klass),  GB_TYPE_DOCUMENT_MANAGER, GbDocumentManagerClass))
#define GB_IS_DOCUMENT_MANAGER(obj)         (G_TYPE_CHECK_INSTANCE_TYPE ((obj), GB_TYPE_DOCUMENT_MANAGER))
#define GB_IS_DOCUMENT_MANAGER_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((klass),  GB_TYPE_DOCUMENT_MANAGER))
#define GB_DOCUMENT_MANAGER_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS ((obj),  GB_TYPE_DOCUMENT_MANAGER, GbDocumentManagerClass))

typedef struct _GbDocumentManager        GbDocumentManager;
typedef struct _GbDocumentManagerClass   GbDocumentManagerClass;
typedef struct _GbDocumentManagerPrivate GbDocumentManagerPrivate;

struct _GbDocumentManager
{
  GObject parent;

  /*< private >*/
  GbDocumentManagerPrivate *priv;
};

struct _GbDocumentManagerClass
{
  GObjectClass parent;

  void (*document_added)           (GbDocumentManager *manager,
                                     GbDocument        *document);
  void (*document_removed)          (GbDocumentManager *manager,
                                     GbDocument        *document);
  void (*document_modified_changed) (GbDocumentManager *manager,
                                     GbDocument        *document);
};

GType              gb_document_manager_get_type              (void);
GbDocumentManager *gb_document_manager_new                   (void);
void               gb_document_manager_add                   (GbDocumentManager *manager,
                                                              GbDocument        *document);
void               gb_document_manager_remove                (GbDocumentManager *manager,
                                                              GbDocument        *document);
GList             *gb_document_manager_get_documents         (GbDocumentManager *manager);
GList             *gb_document_manager_get_unsaved_documents (GbDocumentManager *manager);
guint              gb_document_manager_get_count             (GbDocumentManager *manager);
GbDocument        *gb_document_manager_find_with_file        (GbDocumentManager *manager,
                                                              GFile             *file);
GbDocument        *gb_document_manager_find_with_type        (GbDocumentManager *manager,
                                                              GType              type);

G_END_DECLS

#endif /* GB_DOCUMENT_MANAGER_H */
