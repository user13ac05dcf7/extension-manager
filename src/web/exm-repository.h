/*
 * exm-repository.h
 *
 * Copyright 2026 Jake
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
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <gio/gio.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

#define EXM_TYPE_REPOSITORY (exm_repository_get_type())

G_DECLARE_FINAL_TYPE (ExmRepository, exm_repository, EXM, REPOSITORY, GObject)

ExmRepository *exm_repository_get_default (void);

void
exm_repository_set_url (ExmRepository *self,
                        const gchar   *url);

gboolean
exm_repository_is_enabled (ExmRepository *self);

gboolean
exm_repository_handles (ExmRepository *self,
                        const gchar   *ego_url);

void
exm_repository_request_async (ExmRepository       *self,
                              const gchar         *ego_url,
                              GCancellable        *cancellable,
                              GAsyncReadyCallback  callback,
                              gpointer             user_data);

GBytes *
exm_repository_request_finish (ExmRepository  *self,
                               GAsyncResult   *result,
                               GError        **error);

/* The steps below are separate so they can be tested without a network. */

gboolean
exm_repository_verify_signature (const gchar  *public_key,
                                 GBytes       *data,
                                 GBytes       *signature,
                                 GError      **error);

JsonNode *
exm_repository_parse_index (GBytes   *bytes,
                            gint64    now,
                            gint64    last_serial,
                            gint64   *serial,
                            GError  **error);

GBytes *
exm_repository_answer (JsonObject   *index,
                       const gchar  *ego_url,
                       GError      **error);

G_END_DECLS
