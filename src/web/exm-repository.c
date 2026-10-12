/*
 * exm-repository.c
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
 * A static extension catalogue (index.json, schema version 1) used in place
 * of extensions.gnome.org.
 *
 * The index URL and its public key come from GNOME Shell when it uses such a
 * repository itself, and from the application's settings otherwise. The index
 * is only trusted after its detached Ed25519 signature (index.json.sig)
 * verifies against that key, it has not expired, and its serial is not lower
 * than one seen before. Requests for the extensions.gnome.org API are then
 * answered from the index in the same JSON shape, so the rest of the
 * application stays unchanged.
 *
 * A Shell with its own repository downloads and checks the packages on install
 * and update. On any other Shell, releases mirrored from extensions.gnome.org
 * are installed by the Shell from there, and the others are downloaded by
 * Extension Manager (see exm-package.c) and checked against the size and
 * SHA-256 that the signed index gives.
 */

#include "exm-repository.h"

#include <glib/gi18n.h>
#include <gnutls/abstract.h>
#include <gnutls/gnutls.h>
#include <libsoup/soup.h>

#define EGO_API "https://extensions.gnome.org/api/v1/"
#define EGO_EXTENSIONS EGO_API "extensions/"
#define EGO_COMMENTS "https://extensions.gnome.org/comments/"

#define MAX_INDEX_SIZE (32 * 1024 * 1024)
#define MAX_PACKAGE_SIZE (16 * 1024 * 1024)
#define INDEX_REFRESH_SECONDS (10 * 60)

struct _ExmRepository
{
    GObject parent_instance;

    SoupSession *session;
    gchar *url;
    gchar *key;
    guint generation;

    JsonNode *index;
    gint64 fetched_at;

    gboolean fetching;
    GPtrArray *waiting;
};

G_DEFINE_FINAL_TYPE (ExmRepository, exm_repository, G_TYPE_OBJECT)

ExmRepository *
exm_repository_get_default (void)
{
    static ExmRepository *instance = NULL;

    if (instance == NULL)
        instance = g_object_new (EXM_TYPE_REPOSITORY, NULL);

    return instance;
}

gboolean
exm_repository_check_url (const gchar  *url,
                          GError      **error)
{
    g_autoptr (GUri) uri = url ? g_uri_parse (url, G_URI_FLAGS_NONE, NULL) : NULL;
    const gchar *scheme, *host;

    if (uri != NULL)
    {
        scheme = g_uri_get_scheme (uri);
        host = g_uri_get_host (uri);

        if (g_strcmp0 (scheme, "https") == 0 && host != NULL && *host != '\0')
            return TRUE;

        // Plain HTTP only on the loopback interface, for tests
        if (g_strcmp0 (scheme, "http") == 0 &&
            (g_strcmp0 (host, "127.0.0.1") == 0 ||
             g_strcmp0 (host, "::1") == 0 ||
             g_strcmp0 (host, "localhost") == 0))
            return TRUE;
    }

    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                         _("The repository URL must be an HTTPS address"));
    return FALSE;
}

gboolean
exm_repository_check_key (const gchar  *public_key,
                          GError      **error)
{
    g_autofree guchar *key = NULL;
    gsize length = 0;

    // g_base64_decode () accepts any input, so check the alphabet first
    if (public_key != NULL && strlen (public_key) == 44 &&
        strspn (public_key, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=") == 44)
        key = g_base64_decode (public_key, &length);

    if (length != 32)
    {
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                             _("The public key must be a base64 Ed25519 key (32 bytes)"));
        return FALSE;
    }

    return TRUE;
}

/**
 * exm_repository_check_source:
 * @url: (nullable): the index URL, empty for extensions.gnome.org
 * @public_key: (nullable): base64 of the raw Ed25519 key that signs the index
 *
 * Returns: whether both are usable, or both are empty
 */
gboolean
exm_repository_check_source (const gchar  *url,
                             const gchar  *public_key,
                             GError      **error)
{
    gboolean has_url = url != NULL && *url != '\0';
    gboolean has_key = public_key != NULL && *public_key != '\0';

    if (!has_url && !has_key)
        return TRUE;

    if (!has_url || !has_key)
    {
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                             _("Set both the index URL and its public key, or neither"));
        return FALSE;
    }

    return exm_repository_check_url (url, error) &&
           exm_repository_check_key (public_key, error);
}

/**
 * exm_repository_choose_source:
 * @shell_url: (nullable): GNOME Shell's ExtensionRepository property, or
 *   %NULL if the Shell has none
 * @shell_key: (nullable): GNOME Shell's ExtensionRepositoryKey property
 * @settings: (nullable): the application settings
 * @url: (out): the index URL, empty for extensions.gnome.org
 * @public_key: (out): the key that signs the index
 *
 * A Shell that uses a repository itself decides, so what is shown is what it
 * installs. Otherwise EXM_EXTENSION_REPOSITORY and
 * EXM_EXTENSION_REPOSITORY_KEY set one for testing, and the application's own
 * settings are used.
 *
 * Returns: where the repository comes from
 */
ExmRepositoryOrigin
exm_repository_choose_source (const gchar  *shell_url,
                              const gchar  *shell_key,
                              GSettings    *settings,
                              gchar       **url,
                              gchar       **public_key)
{
    if (shell_url != NULL && shell_key != NULL)
    {
        *url = g_strdup (shell_url);
        *public_key = g_strdup (shell_key);
        return EXM_REPOSITORY_ORIGIN_SHELL;
    }

    if (g_getenv ("EXM_EXTENSION_REPOSITORY") != NULL)
    {
        *url = g_strdup (g_getenv ("EXM_EXTENSION_REPOSITORY"));
        *public_key = g_strdup (g_getenv ("EXM_EXTENSION_REPOSITORY_KEY") ?: "");
        return EXM_REPOSITORY_ORIGIN_ENVIRONMENT;
    }

    *url = settings ? g_settings_get_string (settings, "extension-repository") : g_strdup ("");
    *public_key = settings ? g_settings_get_string (settings, "extension-repository-key") : g_strdup ("");
    return EXM_REPOSITORY_ORIGIN_SETTINGS;
}

/**
 * exm_repository_store_source:
 * @settings: the application settings
 * @url: (nullable): the index URL, or empty for extensions.gnome.org
 * @public_key: (nullable): the key that signs the index, or empty
 *
 * Checks both and writes them together, so the repository is never seen with
 * the key of another one.
 */
gboolean
exm_repository_store_source (GSettings    *settings,
                             const gchar  *url,
                             const gchar  *public_key,
                             GError      **error)
{
    g_autoptr (GSettingsSchema) schema = NULL;
    g_autoptr (GSettingsBackend) backend = NULL;
    g_autoptr (GSettings) batch = NULL;
    g_autofree gchar *path = NULL;

    if (!exm_repository_check_source (url, public_key, error))
        return FALSE;

    // A separate object, as delay-apply mode stays on once set
    g_object_get (settings,
                  "settings-schema", &schema,
                  "backend", &backend,
                  "path", &path,
                  NULL);
    batch = g_settings_new_full (schema, backend, path);

    g_settings_delay (batch);
    g_settings_set_string (batch, "extension-repository", url ? url : "");
    g_settings_set_string (batch, "extension-repository-key", public_key ? public_key : "");
    g_settings_apply (batch);

    return TRUE;
}

/**
 * exm_repository_set_source:
 * @url: (nullable): the index URL, or %NULL for extensions.gnome.org
 * @public_key: (nullable): base64 of the raw Ed25519 key that signs the index
 *
 * A repository without a usable key is refused, so extensions.gnome.org is
 * used instead.
 */
void
exm_repository_set_source (ExmRepository *self,
                           const gchar   *url,
                           const gchar   *public_key)
{
    g_autoptr (GError) error = NULL;

    if (url != NULL && *url == '\0')
        url = NULL;

    if (url != NULL &&
        (!exm_repository_check_url (url, &error) ||
         !exm_repository_check_key (public_key, &error)))
    {
        g_warning ("Ignoring extension repository %s: %s", url, error->message);
        url = NULL;
    }

    if (url == NULL)
        public_key = NULL;

    if (g_strcmp0 (self->url, url) == 0 && g_strcmp0 (self->key, public_key) == 0)
        return;

    g_free (self->url);
    g_free (self->key);
    self->url = g_strdup (url);
    self->key = g_strdup (public_key);
    self->generation++;
    g_clear_pointer (&self->index, json_node_unref);

    if (url != NULL)
        g_info ("Using extension repository %s", url);
}

gboolean
exm_repository_is_enabled (ExmRepository *self)
{
    return self->url != NULL;
}

gboolean
exm_repository_handles (ExmRepository *self,
                        const gchar   *ego_url)
{
    return exm_repository_is_enabled (self) &&
           (g_str_has_prefix (ego_url, EGO_API) ||
            g_str_has_prefix (ego_url, EGO_COMMENTS));
}

gboolean
exm_repository_verify_signature (const gchar  *public_key,
                                 GBytes       *data,
                                 GBytes       *signature,
                                 GError      **error)
{
    g_autofree guchar *key = NULL;
    g_autofree guchar *sig = NULL;
    g_autofree gchar *sig_text = NULL;
    gsize key_len = 0, sig_len = 0, data_len;
    gnutls_pubkey_t pubkey;
    gnutls_datum_t key_datum, data_datum, sig_datum;
    int ret;

    if (public_key == NULL || *public_key == '\0')
    {
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                             _("No key is set for the extension repository"));
        return FALSE;
    }

    key = g_base64_decode (public_key, &key_len);
    sig_text = g_strstrip (g_strndup (g_bytes_get_data (signature, NULL),
                                      g_bytes_get_size (signature)));
    sig = g_base64_decode (sig_text, &sig_len);

    if (key_len != 32 || sig_len != 64)
    {
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                             _("The extension repository signature is malformed"));
        return FALSE;
    }

    key_datum = (gnutls_datum_t) { key, 32 };
    data_datum.data = (guchar *) g_bytes_get_data (data, &data_len);
    data_datum.size = data_len;
    sig_datum = (gnutls_datum_t) { sig, 64 };

    gnutls_pubkey_init (&pubkey);
    ret = gnutls_pubkey_import_ecc_raw (pubkey, GNUTLS_ECC_CURVE_ED25519, &key_datum, NULL);
    if (ret >= 0)
        ret = gnutls_pubkey_verify_data2 (pubkey, GNUTLS_SIGN_EDDSA_ED25519, 0,
                                          &data_datum, &sig_datum);
    gnutls_pubkey_deinit (pubkey);

    if (ret < 0)
    {
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                             _("The extension repository signature does not match"));
        return FALSE;
    }

    return TRUE;
}

static gboolean
member_is_int (JsonObject  *object,
               const gchar *name)
{
    JsonNode *node = json_object_get_member (object, name);

    return node != NULL && JSON_NODE_HOLDS_VALUE (node) &&
           json_node_get_value_type (node) == G_TYPE_INT64;
}

static const gchar *
member_string (JsonObject  *object,
               const gchar *name)
{
    JsonNode *node = json_object_get_member (object, name);

    if (node == NULL || !JSON_NODE_HOLDS_VALUE (node) ||
        json_node_get_value_type (node) != G_TYPE_STRING)
        return NULL;

    return json_node_get_string (node);
}

static JsonArray *
member_array (JsonObject  *object,
              const gchar *name)
{
    JsonNode *node = json_object_get_member (object, name);

    return node != NULL && JSON_NODE_HOLDS_ARRAY (node) ? json_node_get_array (node) : NULL;
}

/**
 * exm_repository_parse_index:
 * @bytes: the verified index.json
 * @now: the current time, in seconds since the epoch
 * @last_serial: the highest serial seen for this repository, or 0
 * @serial: (out): the serial of this index
 *
 * Returns: (transfer full): the index root, or %NULL if it is unusable
 */
JsonNode *
exm_repository_parse_index (GBytes   *bytes,
                            gint64    now,
                            gint64    last_serial,
                            gint64   *serial,
                            GError  **error)
{
    g_autoptr (JsonParser) parser = json_parser_new ();
    g_autoptr (GDateTime) expires = NULL;
    JsonObject *root;
    const gchar *expires_at;
    gsize size;
    gconstpointer data = g_bytes_get_data (bytes, &size);

    if (!json_parser_load_from_data (parser, data, size, error))
        return NULL;

    if (!JSON_NODE_HOLDS_OBJECT (json_parser_get_root (parser)))
        goto unsupported;

    root = json_node_get_object (json_parser_get_root (parser));

    if (!member_is_int (root, "schema_version") ||
        json_object_get_int_member (root, "schema_version") != 1 ||
        !member_is_int (root, "serial") ||
        member_array (root, "extensions") == NULL)
        goto unsupported;

    expires_at = member_string (root, "expires_at");
    if (expires_at != NULL)
        expires = g_date_time_new_from_iso8601 (expires_at, NULL);

    if (expires == NULL || g_date_time_to_unix (expires) <= now)
    {
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                             _("The extension repository index has expired"));
        return NULL;
    }

    *serial = json_object_get_int_member (root, "serial");
    if (*serial < last_serial)
    {
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                             _("The extension repository index is older than one already seen"));
        return NULL;
    }

    return json_node_ref (json_parser_get_root (parser));

unsupported:
    g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                         _("Unsupported extension repository index"));
    return NULL;
}

/* Serials are kept per repository, so a replayed older index is refused. */

static gchar *
serials_path (void)
{
    return g_build_filename (g_get_user_data_dir (), "extension-manager",
                             "repository-serials.ini", NULL);
}

static gint64
load_serial (const gchar *url)
{
    g_autoptr (GKeyFile) file = g_key_file_new ();
    g_autofree gchar *path = serials_path ();
    g_autofree gchar *key = g_compute_checksum_for_string (G_CHECKSUM_SHA256, url, -1);

    if (!g_key_file_load_from_file (file, path, G_KEY_FILE_NONE, NULL))
        return 0;

    return g_key_file_get_int64 (file, "serials", key, NULL);
}

static void
store_serial (const gchar *url,
              gint64       serial)
{
    g_autoptr (GKeyFile) file = g_key_file_new ();
    g_autofree gchar *path = serials_path ();
    g_autofree gchar *dir = g_path_get_dirname (path);
    g_autofree gchar *key = g_compute_checksum_for_string (G_CHECKSUM_SHA256, url, -1);
    g_autoptr (GError) error = NULL;

    g_key_file_load_from_file (file, path, G_KEY_FILE_NONE, NULL);
    if (g_key_file_get_int64 (file, "serials", key, NULL) == serial)
        return;

    g_key_file_set_int64 (file, "serials", key, serial);
    g_mkdir_with_parents (dir, 0700);

    if (!g_key_file_save_to_file (file, path, &error))
        g_warning ("Could not store the extension repository serial: %s", error->message);
}

/* Answers for extensions.gnome.org API requests */

static gboolean
matches_terms (JsonObject  *extension,
               gchar      **terms)
{
    g_autofree gchar *text = NULL;
    g_autofree gchar *folded = NULL;

    if (terms == NULL || terms[0] == NULL)
        return TRUE;

    text = g_strjoin ("\n",
                      member_string (extension, "name") ?: "",
                      member_string (extension, "description") ?: "",
                      member_string (extension, "creator") ?: "",
                      member_string (extension, "uuid") ?: "",
                      NULL);
    folded = g_utf8_casefold (text, -1);

    for (gchar **term = terms; *term != NULL; term++)
    {
        if (**term != '\0' && strstr (folded, *term) == NULL)
            return FALSE;
    }

    return TRUE;
}

static gint
compare_names (gconstpointer a,
               gconstpointer b)
{
    JsonObject *first = *(JsonObject **) a;
    JsonObject *second = *(JsonObject **) b;
    g_autofree gchar *first_name = g_utf8_casefold (member_string (first, "name") ?: "", -1);
    g_autofree gchar *second_name = g_utf8_casefold (member_string (second, "name") ?: "", -1);

    return g_utf8_collate (first_name, second_name);
}

static gboolean
is_mirrored (JsonObject *extension)
{
    return g_strcmp0 (member_string (extension, "tier"), "ego-mirror") == 0;
}

/*
 * When an extension is listed both as mirrored from extensions.gnome.org and
 * in another tier, only the mirrored entry is used, so a package from the
 * repository can never take the place of the reviewed one.
 */
static JsonObject *
find_extension (JsonObject  *index,
                const gchar *uuid)
{
    JsonArray *extensions = member_array (index, "extensions");
    JsonObject *found = NULL;

    for (guint i = 0; i < json_array_get_length (extensions); i++)
    {
        JsonNode *node = json_array_get_element (extensions, i);
        JsonObject *extension;

        if (!JSON_NODE_HOLDS_OBJECT (node))
            continue;

        extension = json_node_get_object (node);
        if (g_strcmp0 (member_string (extension, "uuid"), uuid) != 0)
            continue;

        if (is_mirrored (extension))
            return extension;

        if (found == NULL)
            found = extension;
    }

    return found;
}

/**
 * exm_repository_find_package:
 * @index: a verified index
 * @uuid: the extension to install
 *
 * Returns: (transfer none): the entry to install @uuid from
 */
JsonObject *
exm_repository_find_package (JsonObject   *index,
                             const gchar  *uuid,
                             GError      **error)
{
    JsonObject *extension = find_extension (index, uuid);

    if (extension == NULL)
    {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                     _("%s is not in the extension repository"), uuid);
        return NULL;
    }

    return extension;
}

/**
 * exm_repository_is_shell_compatible:
 * @versions: (nullable): shell versions as strings, e.g. ["49", "50"]
 * @shell_version: the running GNOME Shell version, e.g. "50.1"
 *
 * Only the major version counts, as for GNOME Shell itself.
 */
gboolean
exm_repository_is_shell_compatible (JsonArray   *versions,
                                    const gchar *shell_version)
{
    gint64 major = shell_version ? g_ascii_strtoll (shell_version, NULL, 10) : 0;

    if (major <= 0)
        return FALSE;

    for (guint i = 0; versions != NULL && i < json_array_get_length (versions); i++)
    {
        JsonNode *node = json_array_get_element (versions, i);

        if (JSON_NODE_HOLDS_VALUE (node) &&
            json_node_get_value_type (node) == G_TYPE_STRING &&
            g_ascii_strtoll (json_node_get_string (node), NULL, 10) == major)
            return TRUE;
    }

    return FALSE;
}

/**
 * exm_repository_get_download:
 * @extension: an entry of a verified index
 * @index_url: the URL of that index; downloads are relative to it
 * @shell_version: the running GNOME Shell version
 * @url: (out): where to download the package
 * @sha256: (out): its SHA-256, in lowercase hexadecimal
 * @size: (out): its size in bytes
 *
 * Picks the newest release that supports @shell_version.
 */
gboolean
exm_repository_get_download (JsonObject   *extension,
                             const gchar  *index_url,
                             const gchar  *shell_version,
                             gchar       **url,
                             gchar       **sha256,
                             gint64       *size,
                             GError      **error)
{
    JsonArray *releases = member_array (extension, "releases");
    const gchar *uuid = member_string (extension, "uuid");
    JsonObject *best = NULL;
    const gchar *download, *digest;
    g_autofree gchar *resolved = NULL;

    for (guint i = 0; releases != NULL && i < json_array_get_length (releases); i++)
    {
        JsonNode *node = json_array_get_element (releases, i);
        JsonObject *release;

        if (!JSON_NODE_HOLDS_OBJECT (node))
            continue;

        release = json_node_get_object (node);
        if (!member_is_int (release, "version") ||
            !exm_repository_is_shell_compatible (member_array (release, "shell_versions"), shell_version))
            continue;

        if (best == NULL ||
            json_object_get_int_member (release, "version") > json_object_get_int_member (best, "version"))
            best = release;
    }

    if (best == NULL)
    {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                     _("%s has no release for GNOME Shell %s"), uuid, shell_version);
        return FALSE;
    }

    download = member_string (best, "download");
    digest = member_string (best, "sha256");

    if (download == NULL || digest == NULL ||
        strlen (digest) != 64 || strspn (digest, "0123456789abcdefABCDEF") != 64 ||
        !member_is_int (best, "size") ||
        json_object_get_int_member (best, "size") <= 0 ||
        json_object_get_int_member (best, "size") > MAX_PACKAGE_SIZE)
    {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                     _("The extension repository has no usable download for %s"), uuid);
        return FALSE;
    }

    resolved = g_uri_resolve_relative (index_url, download, G_URI_FLAGS_NONE, error);
    if (resolved == NULL || !exm_repository_check_url (resolved, error))
        return FALSE;

    *url = g_steal_pointer (&resolved);
    *sha256 = g_ascii_strdown (digest, -1);
    *size = json_object_get_int_member (best, "size");

    return TRUE;
}

static void
add_optional_string (JsonBuilder *builder,
                     const gchar *name,
                     const gchar *value)
{
    if (value == NULL)
        return;

    json_builder_set_member_name (builder, name);
    json_builder_add_string_value (builder, value);
}

static const gchar *
origin_link (JsonObject *extension)
{
    JsonNode *origin = json_object_get_member (extension, "origin");

    if (origin == NULL || !JSON_NODE_HOLDS_OBJECT (origin))
        return NULL;

    return member_string (json_node_get_object (origin), "link");
}

static void
build_extension (JsonBuilder *builder,
                 JsonObject  *extension)
{
    JsonNode *origin = json_object_get_member (extension, "origin");
    const gchar *creator = member_string (extension, "creator");
    gint64 id = 0;

    if (origin != NULL && JSON_NODE_HOLDS_OBJECT (origin) &&
        member_is_int (json_node_get_object (origin), "extension_id"))
        id = json_object_get_int_member (json_node_get_object (origin), "extension_id");

    json_builder_begin_object (builder);

    json_builder_set_member_name (builder, "id");
    json_builder_add_int_value (builder, CLAMP (id, 0, G_MAXUINT));
    add_optional_string (builder, "uuid", member_string (extension, "uuid"));
    add_optional_string (builder, "name", member_string (extension, "name"));
    add_optional_string (builder, "description", member_string (extension, "description"));
    add_optional_string (builder, "url", member_string (extension, "homepage"));
    add_optional_string (builder, "link", origin_link (extension));
    add_optional_string (builder, "tier", member_string (extension, "tier"));

    if (creator != NULL && *creator != '\0')
    {
        json_builder_set_member_name (builder, "creator");
        json_builder_begin_object (builder);
        add_optional_string (builder, "username", creator);
        add_optional_string (builder, "display_name", creator);
        json_builder_end_object (builder);
    }

    json_builder_end_object (builder);
}

static void
build_shell_versions (JsonBuilder *builder,
                      JsonArray   *versions)
{
    json_builder_set_member_name (builder, "shell_versions");
    json_builder_begin_array (builder);

    for (guint i = 0; versions != NULL && i < json_array_get_length (versions); i++)
    {
        JsonNode *node = json_array_get_element (versions, i);
        g_auto (GStrv) parts = NULL;
        gint64 major;

        if (!JSON_NODE_HOLDS_VALUE (node) || json_node_get_value_type (node) != G_TYPE_STRING)
            continue;

        parts = g_strsplit (json_node_get_string (node), ".", 3);
        major = g_ascii_strtoll (parts[0], NULL, 10);
        if (major <= 0)
            continue;

        json_builder_begin_object (builder);
        json_builder_set_member_name (builder, "major");
        json_builder_add_int_value (builder, major);
        json_builder_set_member_name (builder, "minor");
        json_builder_add_int_value (builder, parts[1] ? g_ascii_strtoll (parts[1], NULL, 10) : -1);
        json_builder_set_member_name (builder, "point");
        json_builder_add_int_value (builder, -1);
        json_builder_end_object (builder);
    }

    json_builder_end_array (builder);
}

static gint64
query_int (GHashTable  *params,
           const gchar *name,
           gint64       fallback,
           gint64       maximum)
{
    const gchar *value = params ? g_hash_table_lookup (params, name) : NULL;
    gint64 number = value ? g_ascii_strtoll (value, NULL, 10) : fallback;

    return CLAMP (number, 1, maximum);
}

static void
begin_page (JsonBuilder *builder,
            guint        count,
            gchar       *next)
{
    json_builder_begin_object (builder);
    json_builder_set_member_name (builder, "count");
    json_builder_add_int_value (builder, count);
    json_builder_set_member_name (builder, "next");
    if (next != NULL)
        json_builder_add_string_value (builder, next);
    else
        json_builder_add_null_value (builder);
    json_builder_set_member_name (builder, "previous");
    json_builder_add_null_value (builder);
    json_builder_set_member_name (builder, "results");
    json_builder_begin_array (builder);
    g_free (next);
}

static void
end_page (JsonBuilder *builder)
{
    json_builder_end_array (builder);
    json_builder_end_object (builder);
}

static void
answer_list (JsonBuilder *builder,
             JsonObject  *index,
             const gchar *query,
             GHashTable  *params)
{
    JsonArray *extensions = member_array (index, "extensions");
    g_autoptr (GPtrArray) matches = g_ptr_array_new ();
    g_autofree gchar *folded = query ? g_utf8_casefold (query, -1) : NULL;
    g_auto (GStrv) terms = folded ? g_strsplit_set (folded, " \t", -1) : NULL;
    gint64 page = query_int (params, "page", 1, G_MAXINT);
    gint64 page_size = query_int (params, "page_size", 10, 100);
    guint start;

    for (guint i = 0; i < json_array_get_length (extensions); i++)
    {
        JsonNode *node = json_array_get_element (extensions, i);

        if (JSON_NODE_HOLDS_OBJECT (node) && matches_terms (json_node_get_object (node), terms))
            g_ptr_array_add (matches, json_node_get_object (node));
    }

    // The index carries no download counts or dates, so every ordering is by name
    g_ptr_array_sort (matches, compare_names);

    start = MIN ((page - 1) * page_size, matches->len);
    begin_page (builder, matches->len, NULL);

    for (guint i = start; i < matches->len && i < start + page_size; i++)
        build_extension (builder, g_ptr_array_index (matches, i));

    end_page (builder);
}

static void
answer_versions (JsonBuilder *builder,
                 JsonObject  *extension,
                 GHashTable  *params)
{
    JsonArray *releases = member_array (extension, "releases");
    const gchar *uuid = member_string (extension, "uuid");
    gint64 page = query_int (params, "page", 1, G_MAXINT);
    gint64 page_size = query_int (params, "page_size", 100, 100);
    guint count = releases ? json_array_get_length (releases) : 0;
    guint start = MIN ((page - 1) * page_size, count);
    gchar *next = NULL;

    if (start + page_size < count)
        next = g_strdup_printf (EGO_EXTENSIONS "%s/versions/?page=%" G_GINT64_FORMAT "&page_size=%" G_GINT64_FORMAT,
                                uuid, page + 1, page_size);

    begin_page (builder, count, next);

    for (guint i = start; i < count && i < start + page_size; i++)
    {
        JsonNode *node = json_array_get_element (releases, i);
        JsonObject *release;

        if (!JSON_NODE_HOLDS_OBJECT (node))
            continue;

        release = json_node_get_object (node);
        if (!member_is_int (release, "version"))
            continue;

        json_builder_begin_object (builder);
        add_optional_string (builder, "extension", uuid);
        json_builder_set_member_name (builder, "version");
        json_builder_add_int_value (builder, json_object_get_int_member (release, "version"));
        // Mirrored releases are active on extensions.gnome.org
        json_builder_set_member_name (builder, "status");
        json_builder_add_int_value (builder, 3);
        build_shell_versions (builder, member_array (release, "shell_versions"));
        json_builder_set_member_name (builder, "session_modes");
        json_builder_begin_array (builder);
        json_builder_end_array (builder);
        add_optional_string (builder, "link", origin_link (extension));
        json_builder_end_object (builder);
    }

    end_page (builder);
}

/**
 * exm_repository_answer:
 * @index: a verified index
 * @ego_url: an extensions.gnome.org API URL as the providers request it
 *
 * Returns: (transfer full): the answer in the extensions.gnome.org API shape
 */
GBytes *
exm_repository_answer (JsonObject   *index,
                       const gchar  *ego_url,
                       GError      **error)
{
    g_autoptr (JsonBuilder) builder = json_builder_new ();
    g_autoptr (JsonGenerator) generator = json_generator_new ();
    g_autoptr (JsonNode) root = NULL;
    g_autoptr (GHashTable) params = NULL;
    g_autofree gchar *path = NULL;
    gchar *query;
    gchar *text;
    gsize length;

    if (!g_str_has_prefix (ego_url, EGO_EXTENSIONS))
    {
        // Comments, ratings and profiles exist only on extensions.gnome.org
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                             _("Not available from the extension repository"));
        return NULL;
    }

    path = g_strdup (ego_url + strlen (EGO_EXTENSIONS));
    query = strchr (path, '?');
    if (query != NULL)
    {
        *query = '\0';
        params = g_uri_parse_params (query + 1, -1, "&", G_URI_PARAMS_NONE, NULL);
    }

    if (*path == '\0')
    {
        answer_list (builder, index, NULL, params);
    }
    else if (g_str_has_prefix (path, "search/"))
    {
        g_autofree gchar *term = g_strndup (path + strlen ("search/"),
                                            strlen (path) - strlen ("search/") -
                                            (g_str_has_suffix (path, "/") ? 1 : 0));
        g_autofree gchar *unescaped = g_uri_unescape_string (term, NULL);

        answer_list (builder, index, unescaped ? unescaped : term, params);
    }
    else
    {
        g_auto (GStrv) parts = g_strsplit (path, "/", 3);
        JsonObject *extension = find_extension (index, parts[0]);

        if (extension == NULL)
        {
            g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                         _("%s is not in the extension repository"), parts[0]);
            return NULL;
        }

        if (parts[1] != NULL && g_strcmp0 (parts[1], "versions") == 0)
            answer_versions (builder, extension, params);
        else
            build_extension (builder, extension);
    }

    root = json_builder_get_root (builder);
    json_generator_set_root (generator, root);
    text = json_generator_to_data (generator, &length);

    return g_bytes_new_take (text, length);
}

/* Fetching */

typedef struct
{
    ExmRepository *self;
    guint generation;
    SoupMessage *msg;
    GBytes *index;
} FetchData;

static void start_fetch (ExmRepository *self);

static void
fetch_data_free (FetchData *data)
{
    g_object_unref (data->self);
    g_clear_object (&data->msg);
    g_clear_pointer (&data->index, g_bytes_unref);
    g_free (data);
}

typedef struct
{
    JsonNode *extension;
    gchar *index_url;
} Lookup;

static void
lookup_free (Lookup *lookup)
{
    g_clear_pointer (&lookup->extension, json_node_unref);
    g_free (lookup->index_url);
    g_free (lookup);
}

static void
answer_lookup (ExmRepository *self,
               GTask         *task)
{
    GError *error = NULL;
    JsonObject *extension = exm_repository_find_package (json_node_get_object (self->index),
                                                         g_task_get_task_data (task), &error);
    Lookup *lookup;

    if (extension == NULL)
    {
        g_task_return_error (task, error);
        return;
    }

    lookup = g_new0 (Lookup, 1);
    lookup->extension = json_node_init_object (json_node_alloc (), extension);
    lookup->index_url = g_strdup (self->url);
    g_task_return_pointer (task, lookup, (GDestroyNotify) lookup_free);
}

static void
answer_task (ExmRepository *self,
             GTask         *task)
{
    GError *error = NULL;
    GBytes *bytes;

    if (g_task_get_source_tag (task) == exm_repository_lookup_async)
    {
        answer_lookup (self, task);
        return;
    }

    bytes = exm_repository_answer (json_node_get_object (self->index),
                                   g_task_get_task_data (task), &error);

    if (bytes == NULL)
        g_task_return_error (task, error);
    else
        g_task_return_pointer (task, bytes, (GDestroyNotify) g_bytes_unref);
}

static void
finish_fetch (FetchData *data,
              GError    *error)
{
    ExmRepository *self = data->self;
    g_autoptr (GPtrArray) waiting = NULL;

    self->fetching = FALSE;

    if (data->generation != self->generation)
    {
        g_clear_error (&error);

        // The repository changed while fetching; start again for the new one
        if (self->url != NULL)
        {
            start_fetch (self);
            fetch_data_free (data);
            return;
        }

        error = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                     _("The extension repository was removed"));
    }

    waiting = g_steal_pointer (&self->waiting);
    self->waiting = g_ptr_array_new_with_free_func (g_object_unref);

    for (guint i = 0; i < waiting->len; i++)
    {
        GTask *task = g_ptr_array_index (waiting, i);

        if (error != NULL)
            g_task_return_error (task, g_error_copy (error));
        else
            answer_task (self, task);
    }

    if (error != NULL && self->url != NULL)
        g_warning ("Extension repository %s: %s", self->url, error->message);

    g_clear_error (&error);
    fetch_data_free (data);
}

static GBytes *
read_response (SoupSession   *session,
               SoupMessage   *msg,
               GAsyncResult  *res,
               GError       **error)
{
    GBytes *bytes = soup_session_send_and_read_finish (session, res, error);

    if (bytes == NULL)
        return NULL;

    if (soup_message_get_status (msg) != SOUP_STATUS_OK)
    {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                     _("The extension repository answered %u %s"),
                     soup_message_get_status (msg),
                     soup_message_get_reason_phrase (msg) ?: "");
        g_bytes_unref (bytes);
        return NULL;
    }

    if (g_bytes_get_size (bytes) > MAX_INDEX_SIZE)
    {
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_MESSAGE_TOO_LARGE,
                             _("The extension repository index is too large"));
        g_bytes_unref (bytes);
        return NULL;
    }

    return bytes;
}

static void
on_signature (SoupSession  *session,
              GAsyncResult *res,
              FetchData    *data)
{
    ExmRepository *self = data->self;
    g_autoptr (GBytes) signature = NULL;
    JsonNode *index;
    GError *error = NULL;
    gint64 serial;

    signature = read_response (session, data->msg, res, &error);
    if (signature == NULL)
    {
        finish_fetch (data, error);
        return;
    }

    if (data->generation == self->generation)
    {
        if (!exm_repository_verify_signature (self->key, data->index, signature, &error))
        {
            finish_fetch (data, error);
            return;
        }

        index = exm_repository_parse_index (data->index, g_get_real_time () / G_USEC_PER_SEC,
                                            load_serial (self->url), &serial, &error);
        if (index == NULL)
        {
            finish_fetch (data, error);
            return;
        }

        store_serial (self->url, serial);
        g_clear_pointer (&self->index, json_node_unref);
        self->index = index;
        self->fetched_at = g_get_monotonic_time ();
    }

    finish_fetch (data, NULL);
}

static void
on_index (SoupSession  *session,
          GAsyncResult *res,
          FetchData    *data)
{
    g_autofree gchar *signature_url = NULL;
    GError *error = NULL;

    data->index = read_response (session, data->msg, res, &error);
    if (data->index == NULL)
    {
        finish_fetch (data, error);
        return;
    }

    signature_url = g_strconcat (data->self->url, ".sig", NULL);
    g_clear_object (&data->msg);
    data->msg = soup_message_new (SOUP_METHOD_GET, signature_url);
    soup_session_send_and_read_async (session, data->msg, G_PRIORITY_DEFAULT, NULL,
                                      (GAsyncReadyCallback) on_signature, data);
}

static void
start_fetch (ExmRepository *self)
{
    FetchData *data = g_new0 (FetchData, 1);

    data->self = g_object_ref (self);
    data->generation = self->generation;
    data->msg = soup_message_new (SOUP_METHOD_GET, self->url);

    if (data->msg == NULL)
    {
        finish_fetch (data, g_error_new (G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                                         _("Invalid extension repository URL: %s"), self->url));
        return;
    }

    self->fetching = TRUE;
    soup_session_send_and_read_async (self->session, data->msg, G_PRIORITY_DEFAULT, NULL,
                                      (GAsyncReadyCallback) on_index, data);
}

static void
queue_task (ExmRepository *self,
            GTask         *task)
{
    if (self->index != NULL &&
        g_get_monotonic_time () - self->fetched_at < INDEX_REFRESH_SECONDS * G_USEC_PER_SEC)
    {
        answer_task (self, task);
        g_object_unref (task);
        return;
    }

    g_ptr_array_add (self->waiting, task);

    if (!self->fetching)
        start_fetch (self);
}

void
exm_repository_request_async (ExmRepository       *self,
                              const gchar         *ego_url,
                              GCancellable        *cancellable,
                              GAsyncReadyCallback  callback,
                              gpointer             user_data)
{
    GTask *task = g_task_new (self, cancellable, callback, user_data);

    g_task_set_task_data (task, g_strdup (ego_url), g_free);
    queue_task (self, task);
}

GBytes *
exm_repository_request_finish (ExmRepository  *self,
                               GAsyncResult   *result,
                               GError        **error)
{
    g_return_val_if_fail (g_task_is_valid (result, self), NULL);

    return g_task_propagate_pointer (G_TASK (result), error);
}

/**
 * exm_repository_lookup_async:
 * @uuid: the extension to install
 *
 * Finds the entry to install @uuid from in the verified index.
 */
void
exm_repository_lookup_async (ExmRepository       *self,
                             const gchar         *uuid,
                             GCancellable        *cancellable,
                             GAsyncReadyCallback  callback,
                             gpointer             user_data)
{
    GTask *task = g_task_new (self, cancellable, callback, user_data);

    g_task_set_source_tag (task, exm_repository_lookup_async);
    g_task_set_task_data (task, g_strdup (uuid), g_free);
    queue_task (self, task);
}

/**
 * exm_repository_lookup_finish:
 * @index_url: (out) (optional): the URL of the index the entry is from
 *
 * Returns: (transfer full): the extension's entry in the index
 */
JsonNode *
exm_repository_lookup_finish (ExmRepository  *self,
                              GAsyncResult   *result,
                              gchar         **index_url,
                              GError        **error)
{
    Lookup *lookup;
    JsonNode *extension;

    g_return_val_if_fail (g_task_is_valid (result, self), NULL);

    lookup = g_task_propagate_pointer (G_TASK (result), error);
    if (lookup == NULL)
        return NULL;

    extension = g_steal_pointer (&lookup->extension);
    if (index_url)
        *index_url = g_steal_pointer (&lookup->index_url);
    lookup_free (lookup);

    return extension;
}

static void
exm_repository_finalize (GObject *object)
{
    ExmRepository *self = EXM_REPOSITORY (object);

    g_clear_object (&self->session);
    g_clear_pointer (&self->url, g_free);
    g_clear_pointer (&self->key, g_free);
    g_clear_pointer (&self->index, json_node_unref);
    g_clear_pointer (&self->waiting, g_ptr_array_unref);

    G_OBJECT_CLASS (exm_repository_parent_class)->finalize (object);
}

static void
exm_repository_class_init (ExmRepositoryClass *klass)
{
    G_OBJECT_CLASS (klass)->finalize = exm_repository_finalize;
}

static void
exm_repository_init (ExmRepository *self)
{
    self->session = soup_session_new_with_options ("timeout", 30, NULL);
    self->waiting = g_ptr_array_new_with_free_func (g_object_unref);
}
