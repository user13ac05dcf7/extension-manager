/*
 * exm-package.h
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

G_BEGIN_DECLS

gchar *
exm_package_get_shell_dir (void);

GSettings *
exm_package_get_shell_settings (gboolean  *schema_copy,
                                GError   **error);

void
exm_package_install_async (const gchar         *uuid,
                           const gchar         *url,
                           const gchar         *sha256,
                           gint64               size,
                           const gchar         *shell_version,
                           const gchar         *shell_dir,
                           GCancellable        *cancellable,
                           GAsyncReadyCallback  callback,
                           gpointer             user_data);

gboolean
exm_package_install_finish (GAsyncResult  *result,
                            gboolean      *staged,
                            GError       **error);

gboolean
exm_package_enable (GSettings    *shell_settings,
                    gboolean      schema_copy,
                    const gchar  *uuid,
                    GError      **error);

/* The steps below are separate so they can be tested without a network. */

gboolean
exm_package_check (GBytes       *data,
                   const gchar  *sha256,
                   gint64        size,
                   GError      **error);

gboolean
exm_package_unpack (GBytes       *zip,
                    const gchar  *uuid,
                    const gchar  *shell_version,
                    const gchar  *shell_dir,
                    gboolean     *staged,
                    GError      **error);

G_END_DECLS
