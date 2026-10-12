/*
 * exm-package.c
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

/*
 * Installs packages from an extension repository on a GNOME Shell that only
 * installs from extensions.gnome.org itself.
 *
 * The package must have the size and SHA-256 that the signed index gives. It
 * is unpacked into a new directory next to the extensions directory, refusing
 * absolute paths, "..", links and special files, and limiting the number of
 * files and the unpacked size; its metadata.json must name the expected UUID
 * and the running Shell version. The directory is then renamed into place.
 *
 * GNOME Shell only loads extensions at login. A new extension is renamed into
 * ~/.local/share/gnome-shell/extensions and added to enabled-extensions; an
 * update of an installed one goes to extension-updates, which the Shell
 * applies at the next login.
 */

#define G_SETTINGS_ENABLE_BACKEND

#include "exm-package.h"

#include "exm-config.h"
#include "../web/exm-repository.h"

#include <archive.h>
#include <archive_entry.h>
#include <errno.h>
#include <fcntl.h>
#include <gio/gsettingsbackend.h>
#include <glib/gi18n.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <libsoup/soup.h>
#include <unistd.h>

#define MAX_ENTRIES 4096
#define MAX_UNPACKED_SIZE (64 * 1024 * 1024)

static gboolean
in_flatpak (void)
{
    return g_file_test ("/.flatpak-info", G_FILE_TEST_EXISTS);
}

/**
 * exm_package_get_shell_dir:
 *
 * Returns: (transfer full): the directory GNOME Shell keeps user extensions
 *   in, ~/.local/share/gnome-shell
 */
gchar *
exm_package_get_shell_dir (void)
{
    const gchar *host_data_dir = g_getenv ("HOST_XDG_DATA_HOME");

    // In a sandbox, the user data directory is the application's own
    if (in_flatpak ())
    {
        if (host_data_dir != NULL && *host_data_dir != '\0')
            return g_build_filename (host_data_dir, "gnome-shell", NULL);

        return g_build_filename (g_get_home_dir (), ".local", "share", "gnome-shell", NULL);
    }

    return g_build_filename (g_get_user_data_dir (), "gnome-shell", NULL);
}

/**
 * exm_package_get_shell_settings:
 *
 * Returns: (transfer full): GNOME Shell's settings, for enabled-extensions
 *   and disabled-extensions
 */
GSettings *
exm_package_get_shell_settings (GError **error)
{
    GSettingsSchemaSource *source = g_settings_schema_source_get_default ();
    g_autoptr (GSettingsSchemaSource) own_source = NULL;
    g_autoptr (GSettingsSchema) schema = NULL;

    if (source != NULL)
        schema = g_settings_schema_source_lookup (source, "org.gnome.shell", TRUE);

    // A sandbox has no GNOME Shell schemas; a copy of the two keys used here
    // is installed with the application
    if (schema == NULL)
    {
        own_source = g_settings_schema_source_new_from_directory (SHELL_SCHEMA_DIR, NULL, TRUE, error);
        if (own_source == NULL)
            return NULL;

        schema = g_settings_schema_source_lookup (own_source, "org.gnome.shell", FALSE);
    }

    if (schema == NULL)
    {
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                             _("GNOME Shell's settings are not available"));
        return NULL;
    }

    // Without access to dconf, a sandbox keeps its settings to itself
    if (in_flatpak () &&
        g_strcmp0 (G_OBJECT_TYPE_NAME (g_settings_backend_get_default ()), "DConfSettingsBackend") != 0)
    {
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                             _("Extension Manager has no access to GNOME Shell's settings"));
        return NULL;
    }

    return g_settings_new_full (schema, NULL, NULL);
}

/**
 * exm_package_enable:
 * @shell_settings: GNOME Shell's settings
 * @uuid: an installed extension
 *
 * Adds @uuid to enabled-extensions and removes it from disabled-extensions.
 * GNOME Shell's EnableExtension method refuses extensions it has not loaded,
 * so this is how a newly installed extension starts at the next login.
 */
gboolean
exm_package_enable (GSettings    *shell_settings,
                    const gchar  *uuid,
                    GError      **error)
{
    g_auto (GStrv) enabled = g_settings_get_strv (shell_settings, "enabled-extensions");
    g_auto (GStrv) disabled = g_settings_get_strv (shell_settings, "disabled-extensions");
    g_autoptr (GStrvBuilder) new_enabled = g_strv_builder_new ();
    g_autoptr (GStrvBuilder) new_disabled = g_strv_builder_new ();
    g_auto (GStrv) enabled_list = NULL;
    g_auto (GStrv) disabled_list = NULL;

    g_strv_builder_addv (new_enabled, (const gchar **) enabled);
    if (!g_strv_contains ((const gchar * const *) enabled, uuid))
        g_strv_builder_add (new_enabled, uuid);

    for (gchar **item = disabled; *item != NULL; item++)
    {
        if (g_strcmp0 (*item, uuid) != 0)
            g_strv_builder_add (new_disabled, *item);
    }

    enabled_list = g_strv_builder_end (new_enabled);
    disabled_list = g_strv_builder_end (new_disabled);

    if (!g_settings_set_strv (shell_settings, "disabled-extensions", (const gchar * const *) disabled_list) ||
        !g_settings_set_strv (shell_settings, "enabled-extensions", (const gchar * const *) enabled_list))
    {
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                             _("GNOME Shell's list of enabled extensions cannot be changed"));
        return FALSE;
    }

    g_settings_sync ();

    return TRUE;
}

/**
 * exm_package_check:
 * @data: the downloaded package
 * @sha256: its SHA-256 from the signed index
 * @size: its size from the signed index
 */
gboolean
exm_package_check (GBytes       *data,
                   const gchar  *sha256,
                   gint64        size,
                   GError      **error)
{
    g_autofree gchar *digest = NULL;

    if (size < 0 || g_bytes_get_size (data) != (gsize) size)
    {
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                             _("The package does not have the size the extension repository gives"));
        return FALSE;
    }

    digest = g_compute_checksum_for_bytes (G_CHECKSUM_SHA256, data);
    if (sha256 == NULL || g_ascii_strcasecmp (digest, sha256) != 0)
    {
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                             _("The package does not match the checksum the extension repository gives"));
        return FALSE;
    }

    return TRUE;
}

/* Unpacking */

static gboolean
is_safe_uuid (const gchar *uuid)
{
    gsize length = uuid ? strlen (uuid) : 0;

    return length > 0 && length <= 255 && uuid[0] != '.' &&
           strspn (uuid, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789@._-") == length;
}

static gboolean
set_errno_error (GError      **error,
                 const gchar  *path)
{
    int saved = errno;

    g_set_error (error, G_IO_ERROR, g_io_error_from_errno (saved),
                 "%s: %s", path, g_strerror (saved));
    return FALSE;
}

static gboolean
set_archive_error (GError         **error,
                   struct archive  *archive)
{
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                 _("The package is not a usable ZIP archive: %s"),
                 archive_error_string (archive) ?: _("unknown error"));
    return FALSE;
}

static gboolean
remove_tree (const gchar  *path,
             GError      **error)
{
    GStatBuf info;

    if (g_lstat (path, &info) != 0)
        return errno == ENOENT ? TRUE : set_errno_error (error, path);

    // Links are removed, never followed
    if (S_ISDIR (info.st_mode))
    {
        g_autoptr (GDir) dir = g_dir_open (path, 0, error);
        const gchar *name;

        if (dir == NULL)
            return FALSE;

        while ((name = g_dir_read_name (dir)) != NULL)
        {
            g_autofree gchar *child = g_build_filename (path, name, NULL);

            if (!remove_tree (child, error))
                return FALSE;
        }

        if (g_rmdir (path) != 0)
            return set_errno_error (error, path);
    }
    else if (g_unlink (path) != 0)
    {
        return set_errno_error (error, path);
    }

    return TRUE;
}

/*
 * Returns the entry's path relative to the extension directory, "" for the
 * directory itself, or NULL if it would end up outside of it.
 */
static gchar *
entry_path (struct archive_entry  *entry,
            GError               **error)
{
    const gchar *name = archive_entry_pathname_utf8 (entry);
    g_autoptr (GStrvBuilder) builder = g_strv_builder_new ();
    g_auto (GStrv) parts = NULL;
    g_auto (GStrv) components = NULL;

    if (name == NULL)
        name = archive_entry_pathname (entry);

    if (name == NULL || name[0] == '/' || strchr (name, '\\') != NULL)
        goto unsafe;

    parts = g_strsplit (name, "/", -1);
    for (gchar **part = parts; *part != NULL; part++)
    {
        if (**part == '\0' || g_strcmp0 (*part, ".") == 0)
            continue;

        if (g_strcmp0 (*part, "..") == 0)
            goto unsafe;

        g_strv_builder_add (builder, *part);
    }

    components = g_strv_builder_end (builder);
    return g_strjoinv ("/", components);

unsafe:
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                 _("The package contains an unsafe path: %s"), name ? name : "");
    return NULL;
}

static gboolean
write_entry (struct archive  *archive,
             const gchar     *target,
             gint64          *unpacked,
             GError         **error)
{
    gchar buffer[16384];
    la_ssize_t length;
    int fd;

    fd = g_open (target, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (fd < 0)
        return set_errno_error (error, target);

    while ((length = archive_read_data (archive, buffer, sizeof buffer)) > 0)
    {
        gchar *pos = buffer;

        *unpacked += length;
        if (*unpacked > MAX_UNPACKED_SIZE)
        {
            g_close (fd, NULL);
            g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                 _("The package is too large when unpacked"));
            return FALSE;
        }

        while (length > 0)
        {
            gssize written = write (fd, pos, length);

            if (written < 0 && errno == EINTR)
                continue;

            if (written < 0)
            {
                set_errno_error (error, target);
                g_close (fd, NULL);
                return FALSE;
            }

            pos += written;
            length -= written;
        }
    }

    if (length < 0)
    {
        g_close (fd, NULL);
        return set_archive_error (error, archive);
    }

    return g_close (fd, error);
}

static gboolean
make_dirs (const gchar  *path,
           GError      **error)
{
    if (g_mkdir_with_parents (path, 0755) != 0)
        return set_errno_error (error, path);

    return TRUE;
}

static gboolean
extract (GBytes       *zip,
         const gchar  *dir,
         GError      **error)
{
    struct archive *archive = archive_read_new ();
    struct archive_entry *entry;
    gconstpointer data;
    gsize length;
    gint64 unpacked = 0;
    guint entries = 0;
    gboolean success = FALSE;
    int ret;

    data = g_bytes_get_data (zip, &length);

    // Only ZIP, whatever the data looks like
    archive_read_support_format_zip (archive);

    if (archive_read_open_memory (archive, data, length) != ARCHIVE_OK)
    {
        set_archive_error (error, archive);
        goto out;
    }

    while ((ret = archive_read_next_header (archive, &entry)) == ARCHIVE_OK)
    {
        g_autofree gchar *path = NULL;
        g_autofree gchar *target = NULL;
        g_autofree gchar *parent = NULL;
        mode_t type = archive_entry_filetype (entry);

        if (++entries > MAX_ENTRIES)
        {
            g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                 _("The package contains too many files"));
            goto out;
        }

        if (archive_entry_hardlink (entry) != NULL ||
            archive_entry_symlink (entry) != NULL ||
            (type != AE_IFREG && type != AE_IFDIR))
        {
            g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                         _("The package contains a link or special file: %s"),
                         archive_entry_pathname (entry) ?: "");
            goto out;
        }

        path = entry_path (entry, error);
        if (path == NULL)
            goto out;

        if (*path == '\0')
        {
            if (type == AE_IFDIR)
                continue;

            g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                 _("The package contains a file without a name"));
            goto out;
        }

        // Nothing unpacked here is a link, so these stay inside dir
        target = g_build_filename (dir, path, NULL);

        if (type == AE_IFDIR)
        {
            if (!make_dirs (target, error))
                goto out;
            continue;
        }

        parent = g_path_get_dirname (target);
        if (!make_dirs (parent, error) ||
            !write_entry (archive, target, &unpacked, error))
            goto out;
    }

    if (ret != ARCHIVE_EOF)
    {
        set_archive_error (error, archive);
        goto out;
    }

    success = TRUE;

out:
    archive_read_free (archive);
    return success;
}

static gboolean
check_metadata (const gchar  *dir,
                const gchar  *uuid,
                const gchar  *shell_version,
                GError      **error)
{
    g_autofree gchar *path = g_build_filename (dir, "metadata.json", NULL);
    g_autoptr (JsonParser) parser = json_parser_new ();
    JsonObject *metadata;
    JsonNode *node;

    if (!g_file_test (path, G_FILE_TEST_IS_REGULAR) ||
        !json_parser_load_from_file (parser, path, NULL) ||
        !JSON_NODE_HOLDS_OBJECT (json_parser_get_root (parser)))
    {
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                             _("The package has no readable metadata.json"));
        return FALSE;
    }

    metadata = json_node_get_object (json_parser_get_root (parser));

    node = json_object_get_member (metadata, "uuid");
    if (node == NULL || !JSON_NODE_HOLDS_VALUE (node) ||
        json_node_get_value_type (node) != G_TYPE_STRING ||
        g_strcmp0 (json_node_get_string (node), uuid) != 0)
    {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                     _("The package is not the extension %s"), uuid);
        return FALSE;
    }

    node = json_object_get_member (metadata, "shell-version");
    if (node == NULL || !JSON_NODE_HOLDS_ARRAY (node) ||
        !exm_repository_is_shell_compatible (json_node_get_array (node), shell_version))
    {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                     _("The package does not support GNOME Shell %s"), shell_version);
        return FALSE;
    }

    return TRUE;
}

static gboolean
move_into_place (const gchar  *dir,
                 const gchar  *uuid,
                 const gchar  *shell_dir,
                 gboolean     *staged,
                 GError      **error)
{
    g_autofree gchar *extensions = g_build_filename (shell_dir, "extensions", NULL);
    g_autofree gchar *installed = g_build_filename (extensions, uuid, NULL);
    GStatBuf info;

    if (g_lstat (installed, &info) == 0)
    {
        // GNOME Shell replaces the installed extension with this at login
        g_autofree gchar *updates = g_build_filename (shell_dir, "extension-updates", NULL);
        g_autofree gchar *update = g_build_filename (updates, uuid, NULL);

        if (!make_dirs (updates, error) ||
            !remove_tree (update, error))
            return FALSE;

        if (g_rename (dir, update) != 0)
            return set_errno_error (error, update);

        *staged = TRUE;
        return TRUE;
    }

    if (!make_dirs (extensions, error))
        return FALSE;

    if (g_rename (dir, installed) != 0)
        return set_errno_error (error, installed);

    *staged = FALSE;
    return TRUE;
}

/**
 * exm_package_unpack:
 * @zip: a package checked with exm_package_check()
 * @uuid: the extension it is for
 * @shell_version: the running GNOME Shell version
 * @shell_dir: GNOME Shell's user directory, see exm_package_get_shell_dir()
 * @staged: (out): whether the package waits in extension-updates for the
 *   next login, as an older version is installed
 */
gboolean
exm_package_unpack (GBytes       *zip,
                    const gchar  *uuid,
                    const gchar  *shell_version,
                    const gchar  *shell_dir,
                    gboolean     *staged,
                    GError      **error)
{
    g_autofree gchar *dir = NULL;

    if (!is_safe_uuid (uuid))
    {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                     _("Not a valid extension UUID: %s"), uuid ? uuid : "");
        return FALSE;
    }

    if (!make_dirs (shell_dir, error))
        return FALSE;

    // Next to the extensions directory, so the rename into it is atomic, but
    // not in it, so GNOME Shell never loads a half unpacked extension
    dir = g_build_filename (shell_dir, ".extension-manager-XXXXXX", NULL);
    if (g_mkdtemp_full (dir, 0755) == NULL)
        return set_errno_error (error, dir);

    if (!extract (zip, dir, error) ||
        !check_metadata (dir, uuid, shell_version, error) ||
        !move_into_place (dir, uuid, shell_dir, staged, error))
    {
        remove_tree (dir, NULL);
        return FALSE;
    }

    return TRUE;
}

/* Downloading */

typedef struct
{
    gchar *uuid;
    gchar *url;
    gchar *sha256;
    gint64 size;
    gchar *shell_version;
    gchar *shell_dir;
} InstallData;

static void
install_data_free (InstallData *data)
{
    g_free (data->uuid);
    g_free (data->url);
    g_free (data->sha256);
    g_free (data->shell_version);
    g_free (data->shell_dir);
    g_free (data);
}

static GBytes *
download (const gchar   *url,
          gint64         size,
          GCancellable  *cancellable,
          GError       **error)
{
    g_autoptr (SoupSession) session = soup_session_new_with_options ("timeout", 60, NULL);
    g_autoptr (SoupMessage) msg = soup_message_new (SOUP_METHOD_GET, url);
    g_autoptr (GInputStream) stream = NULL;
    g_autofree gchar *buffer = NULL;
    gsize length = 0;

    if (msg == NULL)
    {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                     _("Invalid download URL: %s"), url);
        return NULL;
    }

    stream = soup_session_send (session, msg, cancellable, error);
    if (stream == NULL)
        return NULL;

    if (soup_message_get_status (msg) != SOUP_STATUS_OK)
    {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                     _("The extension repository answered %u %s"),
                     soup_message_get_status (msg),
                     soup_message_get_reason_phrase (msg) ?: "");
        return NULL;
    }

    // One byte more than the index gives shows a package that is too large
    buffer = g_malloc (size + 1);
    if (!g_input_stream_read_all (stream, buffer, size + 1, &length, cancellable, error))
        return NULL;

    return g_bytes_new_take (g_steal_pointer (&buffer), length);
}

static void
install_thread (GTask        *task,
                gpointer      source_object G_GNUC_UNUSED,
                InstallData  *data,
                GCancellable *cancellable)
{
    g_autoptr (GBytes) package = NULL;
    GError *error = NULL;
    gboolean staged = FALSE;

    package = download (data->url, data->size, cancellable, &error);

    if (package == NULL ||
        !exm_package_check (package, data->sha256, data->size, &error) ||
        !exm_package_unpack (package, data->uuid, data->shell_version, data->shell_dir,
                             &staged, &error))
    {
        g_task_return_error (task, error);
        return;
    }

    g_task_return_int (task, staged);
}

/**
 * exm_package_install_async:
 * @uuid: the extension
 * @url: the package, from exm_repository_get_download()
 * @sha256: its SHA-256
 * @size: its size
 * @shell_version: the running GNOME Shell version
 * @shell_dir: GNOME Shell's user directory, see exm_package_get_shell_dir()
 *
 * Downloads, checks and unpacks a package. It does not enable the extension;
 * see exm_package_enable().
 */
void
exm_package_install_async (const gchar         *uuid,
                           const gchar         *url,
                           const gchar         *sha256,
                           gint64               size,
                           const gchar         *shell_version,
                           const gchar         *shell_dir,
                           GCancellable        *cancellable,
                           GAsyncReadyCallback  callback,
                           gpointer             user_data)
{
    GTask *task = g_task_new (NULL, cancellable, callback, user_data);
    InstallData *data = g_new0 (InstallData, 1);

    data->uuid = g_strdup (uuid);
    data->url = g_strdup (url);
    data->sha256 = g_strdup (sha256);
    data->size = size;
    data->shell_version = g_strdup (shell_version);
    data->shell_dir = g_strdup (shell_dir);

    g_task_set_source_tag (task, exm_package_install_async);
    g_task_set_task_data (task, data, (GDestroyNotify) install_data_free);
    g_task_run_in_thread (task, (GTaskThreadFunc) install_thread);
    g_object_unref (task);
}

gboolean
exm_package_install_finish (GAsyncResult  *result,
                            gboolean      *staged,
                            GError       **error)
{
    gssize ret;

    g_return_val_if_fail (g_task_is_valid (result, NULL), FALSE);

    ret = g_task_propagate_int (G_TASK (result), error);
    if (ret < 0)
        return FALSE;

    if (staged)
        *staged = (ret != 0);

    return TRUE;
}
