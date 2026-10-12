/*
 * test-package.c
 *
 * Copyright 2026 Jake
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define G_SETTINGS_ENABLE_BACKEND

#include "local/exm-package.h"
#include "web/exm-repository.h"

#include <archive.h>
#include <archive_entry.h>
#include <gio/gsettingsbackend.h>
#include <glib/gstdio.h>
#include <gnutls/abstract.h>
#include <gnutls/gnutls.h>
#include <libsoup/soup.h>

/* Throwaway Ed25519 key (raw private and public halves), made with OpenSSL 3 */
#define PRIVATE_KEY "hLTOoHtODKmODvhfgpj7GwfziwkshGA+srjpg7rWlw4="
#define PUBLIC_KEY "ACFZJKxDh9zq97aZ2cF5AZxZxYmceP4Sd1XJSekQbus="

/* openssl pkeyutl -sign -rawin -inkey key.pem -in message.txt, over MESSAGE */
#define MESSAGE "Extension Manager test fixture"
#define MESSAGE_SIGNATURE "Gx5G+qMGFZqIcup3Zw1tzHEUksmka5Nu3z+OfEF5OolhNvqyUPioX7fT7oyAtaMHhqAtTSzSgItZPjKyM4t5Dw=="

#define UUID "fixture@exm.test"
#define SHELL_VERSION "50.1"
#define METADATA "{\"uuid\":\"" UUID "\",\"name\":\"Fixture\",\"shell-version\":[\"49\",\"50\"]}"

typedef struct
{
    const gchar *path;
    const gchar *content;
    mode_t type;
    const gchar *link;
} Entry;

#define FILE_ENTRY(path, content) { path, content, AE_IFREG, NULL }
#define DIR_ENTRY(path) { path, NULL, AE_IFDIR, NULL }
#define LINK_ENTRY(path, target) { path, NULL, AE_IFLNK, target }
#define END_ENTRY { NULL, NULL, 0, NULL }

static GBytes *
make_archive (const Entry *entries,
              gboolean     zip)
{
    struct archive *archive = archive_write_new ();
    gsize capacity = 1024 * 1024;
    gchar *buffer = g_malloc (capacity);
    size_t used = 0;

    if (zip)
        archive_write_set_format_zip (archive);
    else
        archive_write_set_format_pax_restricted (archive);

    g_assert_cmpint (archive_write_open_memory (archive, buffer, capacity, &used), ==, ARCHIVE_OK);

    for (const Entry *e = entries; e->path != NULL; e++)
    {
        struct archive_entry *entry = archive_entry_new ();
        gsize length = e->content ? strlen (e->content) : 0;
        mode_t type = e->type;

        archive_entry_set_pathname (entry, e->path);
        archive_entry_set_filetype (entry, type);
        archive_entry_set_perm (entry, type == AE_IFDIR ? 0755 : 0644);
        if (e->link)
            archive_entry_set_symlink (entry, e->link);
        archive_entry_set_size (entry, length);

        g_assert_cmpint (archive_write_header (archive, entry), ==, ARCHIVE_OK);
        if (length > 0)
            g_assert_cmpint (archive_write_data (archive, e->content, length), ==, (la_ssize_t) length);

        archive_entry_free (entry);
    }

    g_assert_cmpint (archive_write_close (archive), ==, ARCHIVE_OK);
    archive_write_free (archive);

    return g_bytes_new_take (g_realloc (buffer, used), used);
}

static GBytes *
good_package (void)
{
    static const Entry entries[] = {
        FILE_ENTRY ("metadata.json", METADATA),
        FILE_ENTRY ("extension.js", "export default class {}"),
        DIR_ENTRY ("schemas/"),
        FILE_ENTRY ("schemas/org.example.gschema.xml", "<schemalist/>"),
        END_ENTRY
    };

    return make_archive (entries, TRUE);
}

static gchar *
sign (GBytes *data)
{
    g_autofree guchar *private_key = NULL;
    g_autofree guchar *public_key = NULL;
    gsize private_len, public_len, data_len;
    gnutls_privkey_t key;
    gnutls_datum_t k, x, d, signature;
    gchar *text;

    private_key = g_base64_decode (PRIVATE_KEY, &private_len);
    public_key = g_base64_decode (PUBLIC_KEY, &public_len);
    k = (gnutls_datum_t) { private_key, private_len };
    x = (gnutls_datum_t) { public_key, public_len };
    d.data = (guchar *) g_bytes_get_data (data, &data_len);
    d.size = data_len;

    g_assert_cmpint (gnutls_privkey_init (&key), ==, 0);
    g_assert_cmpint (gnutls_privkey_import_ecc_raw (key, GNUTLS_ECC_CURVE_ED25519, &x, NULL, &k), ==, 0);
    g_assert_cmpint (gnutls_privkey_sign_data2 (key, GNUTLS_SIGN_EDDSA_ED25519, 0, &d, &signature), ==, 0);

    text = g_base64_encode (signature.data, signature.size);
    gnutls_free (signature.data);
    gnutls_privkey_deinit (key);

    return text;
}

static gchar *
shell_dir (void)
{
    // G_TEST_OPTION_ISOLATE_DIRS gives every test its own
    return g_build_filename (g_get_user_data_dir (), "gnome-shell", NULL);
}

static gchar *
read_file (const gchar *first,
           ...)
{
    g_autofree gchar *path = NULL;
    gchar *contents = NULL;
    va_list args;

    va_start (args, first);
    path = g_build_filename_valist (first, &args);
    va_end (args);

    if (!g_file_get_contents (path, &contents, NULL, NULL))
        return NULL;

    return contents;
}

static guint
count_children (const gchar *path)
{
    g_autoptr (GDir) dir = g_dir_open (path, 0, NULL);
    guint count = 0;

    while (dir != NULL && g_dir_read_name (dir) != NULL)
        count++;

    return count;
}

static void
test_signer (void)
{
    g_autoptr (GBytes) message = g_bytes_new_static (MESSAGE, strlen (MESSAGE));
    g_autoptr (GBytes) signature = g_bytes_new_static (MESSAGE_SIGNATURE, strlen (MESSAGE_SIGNATURE));
    g_autofree gchar *ours = sign (message);

    // Ed25519 is deterministic, so the fixture signer matches OpenSSL
    g_assert_cmpstr (ours, ==, MESSAGE_SIGNATURE);
    g_assert_true (exm_repository_verify_signature (PUBLIC_KEY, message, signature, NULL));
}

static void
test_check (void)
{
    g_autoptr (GBytes) package = good_package ();
    g_autofree gchar *digest = g_compute_checksum_for_bytes (G_CHECKSUM_SHA256, package);
    g_autofree gchar *upper = g_ascii_strup (digest, -1);
    gint64 size = g_bytes_get_size (package);
    GError *error = NULL;

    g_assert_true (exm_package_check (package, digest, size, &error));
    g_assert_no_error (error);
    g_assert_true (exm_package_check (package, upper, size, NULL));

    g_assert_false (exm_package_check (package, digest, size + 1, &error));
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error (&error);

    g_assert_false (exm_package_check (package, "0000000000000000000000000000000000000000000000000000000000000000",
                                       size, &error));
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error (&error);
}

static void
test_unpack (void)
{
    g_autoptr (GBytes) package = good_package ();
    g_autofree gchar *dir = shell_dir ();
    g_autofree gchar *contents = NULL;
    GError *error = NULL;
    gboolean staged = TRUE;

    g_assert_true (exm_package_unpack (package, UUID, SHELL_VERSION, dir, &staged, &error));
    g_assert_no_error (error);
    g_assert_false (staged);

    contents = read_file (dir, "extensions", UUID, "metadata.json", NULL);
    g_assert_cmpstr (contents, ==, METADATA);
    g_clear_pointer (&contents, g_free);
    contents = read_file (dir, "extensions", UUID, "schemas", "org.example.gschema.xml", NULL);
    g_assert_cmpstr (contents, ==, "<schemalist/>");
    g_clear_pointer (&contents, g_free);

    // Installed already: the update waits for GNOME Shell to apply it at login
    g_assert_true (exm_package_unpack (package, UUID, SHELL_VERSION, dir, &staged, &error));
    g_assert_no_error (error);
    g_assert_true (staged);
    contents = read_file (dir, "extension-updates", UUID, "extension.js", NULL);
    g_assert_cmpstr (contents, ==, "export default class {}");
    g_clear_pointer (&contents, g_free);

    // A newer update replaces one that is waiting
    g_assert_true (exm_package_unpack (package, UUID, SHELL_VERSION, dir, &staged, &error));
    g_assert_no_error (error);
    g_assert_true (staged);

    // Nothing is left behind next to the extensions
    g_assert_cmpuint (count_children (dir), ==, 2);
}

static void
assert_refused (const Entry *entries,
                gboolean     zip,
                const gchar *uuid,
                const gchar *message)
{
    g_autoptr (GBytes) package = make_archive (entries, zip);
    g_autofree gchar *dir = shell_dir ();
    g_autofree gchar *outside = g_build_filename (g_get_user_data_dir (), "evil.js", NULL);
    g_autoptr (GError) error = NULL;
    gboolean staged;

    g_assert_false (exm_package_unpack (package, uuid, SHELL_VERSION, dir, &staged, &error));
    g_assert_nonnull (error);
    if (message != NULL && strstr (error->message, message) == NULL)
        g_error ("Unexpected error: %s", error->message);

    g_assert_cmpuint (count_children (dir), ==, 0);
    g_assert_false (g_file_test (outside, G_FILE_TEST_EXISTS));
    g_assert_false (g_file_test ("/tmp/evil.js", G_FILE_TEST_EXISTS));
}

static void
test_unsafe (void)
{
    const Entry parent[] = { FILE_ENTRY ("metadata.json", METADATA), FILE_ENTRY ("../../evil.js", "x"), END_ENTRY };
    const Entry nested[] = { FILE_ENTRY ("metadata.json", METADATA), FILE_ENTRY ("a/../../../evil.js", "x"), END_ENTRY };
    const Entry absolute[] = { FILE_ENTRY ("metadata.json", METADATA), FILE_ENTRY ("/tmp/evil.js", "x"), END_ENTRY };
    const Entry backslash[] = { FILE_ENTRY ("metadata.json", METADATA), FILE_ENTRY ("..\\evil.js", "x"), END_ENTRY };
    const Entry symlink[] = { FILE_ENTRY ("metadata.json", METADATA), LINK_ENTRY ("evil.js", "/etc/passwd"), END_ENTRY };
    const Entry duplicate[] = { FILE_ENTRY ("metadata.json", METADATA), FILE_ENTRY ("metadata.json", METADATA), END_ENTRY };
    const Entry no_metadata[] = { FILE_ENTRY ("extension.js", "x"), END_ENTRY };
    const Entry other_uuid[] = { FILE_ENTRY ("metadata.json", "{\"uuid\":\"other@x\",\"shell-version\":[\"50\"]}"), END_ENTRY };
    const Entry other_shell[] = { FILE_ENTRY ("metadata.json", "{\"uuid\":\"" UUID "\",\"shell-version\":[\"48\",\"51\"]}"), END_ENTRY };
    const Entry good[] = { FILE_ENTRY ("metadata.json", METADATA), END_ENTRY };

    assert_refused (parent, TRUE, UUID, "unsafe path");
    assert_refused (nested, TRUE, UUID, "unsafe path");
    assert_refused (absolute, TRUE, UUID, "unsafe path");
    assert_refused (backslash, TRUE, UUID, "unsafe path");
    assert_refused (symlink, TRUE, UUID, "link or special file");
    assert_refused (duplicate, TRUE, UUID, NULL);
    assert_refused (no_metadata, TRUE, UUID, "metadata.json");
    assert_refused (other_uuid, TRUE, UUID, "not the extension");
    assert_refused (other_shell, TRUE, UUID, "does not support GNOME Shell 50.1");
    // Only ZIP archives are read
    assert_refused (good, FALSE, UUID, "ZIP");
    // The UUID becomes a directory name
    assert_refused (good, TRUE, "../" UUID, "UUID");
    assert_refused (good, TRUE, "..", "UUID");
}

static GSettings *
shell_settings (void)
{
    g_autoptr (GSettingsSchemaSource) source = NULL;
    g_autoptr (GSettingsSchema) schema = NULL;
    g_autoptr (GSettingsBackend) backend = g_memory_settings_backend_new ();
    GError *error = NULL;

    // The copy of GNOME Shell's two keys that the application installs
    source = g_settings_schema_source_new_from_directory (TEST_SHELL_SCHEMA_DIR, NULL, TRUE, &error);
    g_assert_no_error (error);
    schema = g_settings_schema_source_lookup (source, "org.gnome.shell", FALSE);
    g_assert_nonnull (schema);

    return g_settings_new_full (schema, backend, NULL);
}

static void
test_enable (void)
{
    g_autoptr (GSettings) settings = shell_settings ();
    const gchar *enabled[] = { "a@x", NULL };
    const gchar *disabled[] = { UUID, "b@x", NULL };
    g_auto (GStrv) now_enabled = NULL;
    g_auto (GStrv) now_disabled = NULL;
    GError *error = NULL;

    g_settings_set_strv (settings, "enabled-extensions", enabled);
    g_settings_set_strv (settings, "disabled-extensions", disabled);

    g_assert_true (exm_package_enable (settings, UUID, &error));
    g_assert_no_error (error);
    g_assert_true (exm_package_enable (settings, UUID, &error));

    now_enabled = g_settings_get_strv (settings, "enabled-extensions");
    now_disabled = g_settings_get_strv (settings, "disabled-extensions");
    g_assert_cmpuint (g_strv_length (now_enabled), ==, 2);
    g_assert_cmpstr (now_enabled[0], ==, "a@x");
    g_assert_cmpstr (now_enabled[1], ==, UUID);
    g_assert_cmpuint (g_strv_length (now_disabled), ==, 1);
    g_assert_cmpstr (now_disabled[0], ==, "b@x");
}

/* A signed repository on the loopback interface */

static void
serve (SoupServer        *server G_GNUC_UNUSED,
       SoupServerMessage *msg,
       const char        *path,
       GHashTable        *query G_GNUC_UNUSED,
       GHashTable        *files)
{
    GBytes *bytes = g_hash_table_lookup (files, path);

    if (bytes == NULL)
    {
        soup_server_message_set_status (msg, SOUP_STATUS_NOT_FOUND, NULL);
        return;
    }

    soup_server_message_set_status (msg, SOUP_STATUS_OK, NULL);
    soup_server_message_set_response (msg, "application/octet-stream", SOUP_MEMORY_COPY,
                                      g_bytes_get_data (bytes, NULL), g_bytes_get_size (bytes));
}

static gchar *
release (const gchar *download,
         GBytes      *package)
{
    g_autofree gchar *digest = g_compute_checksum_for_bytes (G_CHECKSUM_SHA256, package);

    return g_strdup_printf ("{\"version\":1,\"shell_versions\":[\"50\"],\"license\":\"GPL-2.0-or-later\","
                            "\"license_source\":\"metadata\",\"sha256\":\"%s\",\"size\":%" G_GSIZE_FORMAT ","
                            "\"download\":\"%s\"}",
                            digest, g_bytes_get_size (package), download);
}

static void
store_result (GObject      *source G_GNUC_UNUSED,
              GAsyncResult *result,
              gpointer      user_data)
{
    *(GAsyncResult **) user_data = g_object_ref (result);
}

static GAsyncResult *
wait_for (GAsyncResult **result)
{
    while (*result == NULL)
        g_main_context_iteration (NULL, TRUE);

    return *result;
}

static JsonNode *
lookup (const gchar  *uuid,
        gchar       **index_url,
        GError      **error)
{
    ExmRepository *repository = exm_repository_get_default ();
    g_autoptr (GAsyncResult) result = NULL;

    exm_repository_lookup_async (repository, uuid, NULL, store_result, &result);
    return exm_repository_lookup_finish (repository, wait_for (&result), index_url, error);
}

static gboolean
install (const gchar  *uuid,
         gboolean     *staged,
         GError      **error)
{
    g_autoptr (JsonNode) entry = NULL;
    g_autoptr (GAsyncResult) result = NULL;
    g_autofree gchar *index_url = NULL;
    g_autofree gchar *url = NULL;
    g_autofree gchar *sha256 = NULL;
    g_autofree gchar *dir = shell_dir ();
    gint64 size;

    entry = lookup (uuid, &index_url, error);
    if (entry == NULL)
        return FALSE;

    g_assert_cmpstr (json_object_get_string_member (json_node_get_object (entry), "tier"), ==, "reviewed");

    if (!exm_repository_get_download (json_node_get_object (entry), index_url, SHELL_VERSION,
                                      &url, &sha256, &size, error))
        return FALSE;

    exm_package_install_async (uuid, url, sha256, size, SHELL_VERSION, dir, NULL, store_result, &result);
    return exm_package_install_finish (wait_for (&result), staged, error);
}

static void
test_install_from_repository (void)
{
    g_autoptr (GHashTable) files = g_hash_table_new_full (g_str_hash, g_str_equal, NULL,
                                                          (GDestroyNotify) g_bytes_unref);
    g_autoptr (SoupServer) server = soup_server_new (NULL, NULL);
    g_autoptr (GBytes) package = good_package ();
    g_autoptr (GBytes) tampered = NULL;
    g_autoptr (GDateTime) expires = g_date_time_new_now_utc ();
    g_autoptr (GDateTime) tomorrow = g_date_time_add_days (expires, 1);
    g_autoptr (JsonNode) mirrored = NULL;
    g_autofree gchar *expires_at = g_date_time_format_iso8601 (tomorrow);
    g_autofree gchar *good_release = release ("downloads/fixture.zip", package);
    g_autofree gchar *tampered_release = release ("downloads/tampered.zip", package);
    g_autofree gchar *index = NULL;
    g_autoptr (GBytes) index_bytes = NULL;
    g_autofree gchar *signature = NULL;
    g_autofree gchar *index_url = NULL;
    g_autofree gchar *dir = shell_dir ();
    g_autofree gchar *contents = NULL;
    GSList *uris;
    GError *error = NULL;
    gboolean staged = TRUE;
    gchar *bytes;

    // The same size, so only the checksum tells them apart
    bytes = g_memdup2 (g_bytes_get_data (package, NULL), g_bytes_get_size (package));
    bytes[g_bytes_get_size (package) / 2] ^= 0x55;
    tampered = g_bytes_new_take (bytes, g_bytes_get_size (package));

    index = g_strdup_printf ("{\"schema_version\":1,\"serial\":%" G_GINT64_FORMAT ",\"expires_at\":\"%s\","
                             "\"extensions\":["
                             " {\"uuid\":\"" UUID "\",\"name\":\"Fixture\",\"tier\":\"reviewed\",\"releases\":[%s]},"
                             " {\"uuid\":\"tampered@exm.test\",\"tier\":\"reviewed\",\"releases\":[%s]},"
                             " {\"uuid\":\"dock@exm.test\",\"tier\":\"reviewed\",\"releases\":[%s]},"
                             " {\"uuid\":\"dock@exm.test\",\"tier\":\"ego-mirror\",\"origin\":{\"extension_id\":1},"
                             "  \"releases\":[{\"version\":1,\"origin_version_id\":2,\"shell_versions\":[\"50\"]}]}"
                             "]}",
                             g_date_time_to_unix (expires), expires_at,
                             good_release, tampered_release, good_release);
    index_bytes = g_bytes_new (index, strlen (index));
    signature = sign (index_bytes);

    g_hash_table_insert (files, "/index.json", g_bytes_ref (index_bytes));
    g_hash_table_insert (files, "/index.json.sig", g_bytes_new (signature, strlen (signature)));
    g_hash_table_insert (files, "/downloads/fixture.zip", g_bytes_ref (package));
    g_hash_table_insert (files, "/downloads/tampered.zip", g_bytes_ref (tampered));

    soup_server_add_handler (server, NULL, (SoupServerCallback) serve, files, NULL);
    g_assert_true (soup_server_listen_local (server, 0, SOUP_SERVER_LISTEN_IPV4_ONLY, &error));
    g_assert_no_error (error);

    uris = soup_server_get_uris (server);
    index_url = g_strdup_printf ("http://127.0.0.1:%d/index.json", g_uri_get_port (uris->data));
    g_slist_free_full (uris, (GDestroyNotify) g_uri_unref);

    exm_repository_set_source (exm_repository_get_default (), index_url, PUBLIC_KEY);

    g_assert_true (install (UUID, &staged, &error));
    g_assert_no_error (error);
    g_assert_false (staged);
    contents = read_file (dir, "extensions", UUID, "metadata.json", NULL);
    g_assert_cmpstr (contents, ==, METADATA);

    g_assert_true (install (UUID, &staged, &error));
    g_assert_no_error (error);
    g_assert_true (staged);

    g_assert_false (install ("tampered@exm.test", &staged, &error));
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_assert_nonnull (strstr (error->message, "checksum"));
    g_clear_error (&error);
    // extensions and extension-updates, and no unpacked leftovers
    g_assert_cmpuint (count_children (dir), ==, 2);

    // Listed in both tiers: only the mirror of extensions.gnome.org is used
    mirrored = lookup ("dock@exm.test", NULL, &error);
    g_assert_no_error (error);
    g_assert_cmpstr (json_object_get_string_member (json_node_get_object (mirrored), "tier"), ==, "ego-mirror");

    // A signature by another key is refused
    exm_repository_set_source (exm_repository_get_default (), index_url,
                               "YFE8ykXr/yYORPmP97MV/8VteVwEkDe9FKDEewtcHno=");
    g_test_expect_message (NULL, G_LOG_LEVEL_WARNING, "*signature does not match*");
    g_assert_null (lookup (UUID, NULL, &error));
    g_test_assert_expected_messages ();
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error (&error);

    exm_repository_set_source (exm_repository_get_default (), "", "");
}

int
main (int argc, char *argv[])
{
    g_test_init (&argc, &argv, G_TEST_OPTION_ISOLATE_DIRS, NULL);

    g_test_add_func ("/package/signer", test_signer);
    g_test_add_func ("/package/check", test_check);
    g_test_add_func ("/package/unpack", test_unpack);
    g_test_add_func ("/package/unsafe", test_unsafe);
    g_test_add_func ("/package/enable", test_enable);
    g_test_add_func ("/package/install-from-repository", test_install_from_repository);

    return g_test_run ();
}
