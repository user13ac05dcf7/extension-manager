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

typedef enum
{
    EXM_REPOSITORY_ORIGIN_SHELL,
    EXM_REPOSITORY_ORIGIN_ENVIRONMENT,
    EXM_REPOSITORY_ORIGIN_SETTINGS,
} ExmRepositoryOrigin;

ExmRepository *exm_repository_get_default (void);

ExmRepositoryOrigin
exm_repository_choose_source (const gchar  *shell_url,
                              const gchar  *shell_key,
                              GSettings    *settings,
                              gchar       **url,
                              gchar       **public_key);

gboolean
exm_repository_store_source (GSettings    *settings,
                             const gchar  *url,
                             const gchar  *public_key,
                             GError      **error);

void
exm_repository_set_source (ExmRepository *self,
                           const gchar   *url,
                           const gchar   *public_key);

gboolean
exm_repository_check_url (const gchar  *url,
                          GError      **error);

gboolean
exm_repository_check_key (const gchar  *public_key,
                          GError      **error);

gboolean
exm_repository_check_source (const gchar  *url,
                             const gchar  *public_key,
                             GError      **error);

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

void
exm_repository_lookup_async (ExmRepository       *self,
                             const gchar         *uuid,
                             GCancellable        *cancellable,
                             GAsyncReadyCallback  callback,
                             gpointer             user_data);

JsonNode *
exm_repository_lookup_finish (ExmRepository  *self,
                              GAsyncResult   *result,
                              gchar         **index_url,
                              GError        **error);

gboolean
exm_repository_get_download (JsonObject   *extension,
                             const gchar  *index_url,
                             const gchar  *shell_version,
                             gchar       **url,
                             gchar       **sha256,
                             gint64       *size,
                             GError      **error);

gboolean
exm_repository_is_shell_compatible (JsonArray   *versions,
                                    const gchar *shell_version);

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

JsonObject *
exm_repository_find_package (JsonObject   *index,
                             const gchar  *uuid,
                             GError      **error);

GBytes *
exm_repository_answer (JsonObject   *index,
                       const gchar  *ego_url,
                       GError      **error);

G_END_DECLS
