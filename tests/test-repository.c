/*
 * test-repository.c
 *
 * Copyright 2026 Jake
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define G_SETTINGS_ENABLE_BACKEND

#include "exm-config.h"
#include "web/exm-repository.h"

#include <gio/gsettingsbackend.h>

/* Throwaway Ed25519 key and a signature over SIGNED, made with OpenSSL 3 */
#define TEST_KEY "YFE8ykXr/yYORPmP97MV/8VteVwEkDe9FKDEewtcHno="
#define SIGNED "{\"schema_version\":1,\"serial\":100}"
#define SIGNATURE "aoV1Bt7ZwW6YxHo50ABvTYeCtAAosRLFFlLnHQURNF9Px2Ta0rjSb2QYquBTq1RZ4f2HQI+K4gslx59Kq0SxCg=="

#define NOW 1791316800 /* 2026-10-06T20:00:00Z */

static const gchar *INDEX =
    "{\"schema_version\":1,\"serial\":1791316800,"
    "\"expires_at\":\"2026-10-20T20:00:00Z\","
    "\"extensions\":["
    " {\"uuid\":\"dock@x\",\"name\":\"Dash Dock\",\"description\":\"A dock\",\"creator\":\"someone\","
    "  \"homepage\":\"https://gitlab.com/a/b\",\"tier\":\"ego-mirror\","
    "  \"origin\":{\"site\":\"https://extensions.gnome.org\",\"extension_id\":307,"
    "            \"link\":\"https://extensions.gnome.org/extension/307/x/\"},"
    "  \"releases\":[{\"version\":4,\"origin_version_id\":11,\"shell_versions\":[\"50\",\"51\"],"
    "                \"sha256\":\"00\",\"size\":1,\"download\":\"downloads/00.zip\"}]},"
    " {\"uuid\":\"blur@y\",\"name\":\"Blur\",\"description\":\"Blurs things\",\"creator\":\"other\","
    "  \"homepage\":null,\"tier\":\"ego-mirror\",\"origin\":{},\"releases\":[]}"
    "]}";

static GBytes *
bytes (const gchar *text)
{
    return g_bytes_new_static (text, strlen (text));
}

static JsonNode *
parse (const gchar *text)
{
    g_autoptr (JsonParser) parser = json_parser_new ();

    g_assert_true (json_parser_load_from_data (parser, text, -1, NULL));
    return json_node_ref (json_parser_get_root (parser));
}

static JsonNode *
answer (const gchar *url)
{
    g_autoptr (JsonNode) index = parse (INDEX);
    g_autoptr (GError) error = NULL;
    g_autoptr (GBytes) reply = exm_repository_answer (json_node_get_object (index), url, &error);
    g_autoptr (JsonParser) parser = json_parser_new ();

    g_assert_no_error (error);
    g_assert_true (json_parser_load_from_data (parser, g_bytes_get_data (reply, NULL),
                                               g_bytes_get_size (reply), NULL));
    return json_node_ref (json_parser_get_root (parser));
}

static void
test_signature (void)
{
    g_autoptr (GBytes) data = bytes (SIGNED);
    g_autoptr (GBytes) signature = bytes (SIGNATURE "\n");
    g_autoptr (GBytes) tampered = bytes ("{\"schema_version\":1,\"serial\":101}");
    g_autoptr (GBytes) garbage = bytes ("not base64 at all");
    GError *error = NULL;

    g_assert_true (exm_repository_verify_signature (TEST_KEY, data, signature, &error));
    g_assert_no_error (error);

    g_assert_false (exm_repository_verify_signature (TEST_KEY, tampered, signature, &error));
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error (&error);

    g_assert_false (exm_repository_verify_signature ("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=",
                                                     data, signature, &error));
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error (&error);

    g_assert_false (exm_repository_verify_signature (TEST_KEY, data, garbage, &error));
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error (&error);

    g_assert_false (exm_repository_verify_signature ("", data, signature, &error));
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
    g_clear_error (&error);
}

static void
test_index_checks (void)
{
    g_autoptr (GBytes) index = bytes (INDEX);
    g_autoptr (GBytes) newer_schema = bytes ("{\"schema_version\":2,\"serial\":1,\"extensions\":[]}");
    g_autoptr (JsonNode) root = NULL;
    GError *error = NULL;
    gint64 serial = 0;

    root = exm_repository_parse_index (index, NOW, 1791316800, &serial, &error);
    g_assert_no_error (error);
    g_assert_nonnull (root);
    g_assert_cmpint (serial, ==, 1791316800);

    g_assert_null (exm_repository_parse_index (index, NOW + 15 * 24 * 3600, 0, &serial, &error));
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_assert_nonnull (strstr (error->message, "expired"));
    g_clear_error (&error);

    g_assert_null (exm_repository_parse_index (index, NOW, 1791316801, &serial, &error));
    g_assert_nonnull (strstr (error->message, "older"));
    g_clear_error (&error);

    g_assert_null (exm_repository_parse_index (newer_schema, NOW, 0, &serial, &error));
    g_assert_nonnull (strstr (error->message, "Unsupported"));
    g_clear_error (&error);
}

static void
test_browse_and_search (void)
{
    g_autoptr (JsonNode) all = answer ("https://extensions.gnome.org/api/v1/extensions/?ordering=-popularity&page=1&page_size=10&status=3");
    g_autoptr (JsonNode) second = answer ("https://extensions.gnome.org/api/v1/extensions/?page=2&page_size=1");
    g_autoptr (JsonNode) found = answer ("https://extensions.gnome.org/api/v1/extensions/search/DOCK a/?page=1&page_size=10");
    g_autoptr (JsonNode) creator = answer ("https://extensions.gnome.org/api/v1/extensions/search/other/?ordering=name&page_size=100");
    JsonArray *results;
    JsonObject *first;

    g_assert_cmpint (json_object_get_int_member (json_node_get_object (all), "count"), ==, 2);
    results = json_object_get_array_member (json_node_get_object (all), "results");
    first = json_array_get_object_element (results, 0);
    // Ordered by name
    g_assert_cmpstr (json_object_get_string_member (first, "uuid"), ==, "blur@y");
    g_assert_false (json_object_has_member (first, "link"));

    results = json_object_get_array_member (json_node_get_object (second), "results");
    g_assert_cmpuint (json_array_get_length (results), ==, 1);
    g_assert_cmpstr (json_object_get_string_member (json_array_get_object_element (results, 0), "uuid"),
                     ==, "dock@x");

    g_assert_cmpint (json_object_get_int_member (json_node_get_object (found), "count"), ==, 1);
    g_assert_cmpint (json_object_get_int_member (json_node_get_object (creator), "count"), ==, 1);
}

static void
test_detail_and_versions (void)
{
    g_autoptr (JsonNode) detail = answer ("https://extensions.gnome.org/api/v1/extensions/dock@x/");
    g_autoptr (JsonNode) versions = answer ("https://extensions.gnome.org/api/v1/extensions/dock@x/versions/?page=1&page_size=100");
    JsonObject *extension = json_node_get_object (detail);
    JsonObject *release, *shell;

    g_assert_cmpint (json_object_get_int_member (extension, "id"), ==, 307);
    g_assert_cmpstr (json_object_get_string_member (extension, "tier"), ==, "ego-mirror");
    g_assert_cmpstr (json_object_get_string_member (extension, "link"), ==,
                     "https://extensions.gnome.org/extension/307/x/");
    g_assert_cmpstr (json_object_get_string_member (extension, "url"), ==, "https://gitlab.com/a/b");
    g_assert_cmpstr (json_object_get_string_member (json_object_get_object_member (extension, "creator"),
                                                    "username"), ==, "someone");

    release = json_array_get_object_element (json_object_get_array_member (json_node_get_object (versions),
                                                                           "results"), 0);
    g_assert_cmpint (json_object_get_int_member (release, "version"), ==, 4);
    g_assert_cmpint (json_object_get_int_member (release, "status"), ==, 3);
    shell = json_array_get_object_element (json_object_get_array_member (release, "shell_versions"), 1);
    g_assert_cmpint (json_object_get_int_member (shell, "major"), ==, 51);
    g_assert_cmpint (json_object_get_int_member (shell, "minor"), ==, -1);
}

static void
test_unavailable (void)
{
    g_autoptr (JsonNode) index = parse (INDEX);
    JsonObject *object = json_node_get_object (index);
    GError *error = NULL;

    g_assert_null (exm_repository_answer (object, "https://extensions.gnome.org/api/v1/extensions/none@x/", &error));
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
    g_clear_error (&error);

    g_assert_null (exm_repository_answer (object, "https://extensions.gnome.org/comments/all/?pk=307&all=false", &error));
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
    g_clear_error (&error);

    g_assert_null (exm_repository_answer (object, "https://extensions.gnome.org/api/v1/profile/12/", &error));
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
    g_clear_error (&error);
}

static void
test_source_checks (void)
{
    GError *error = NULL;

    g_assert_true (exm_repository_check_url ("https://registry.example/index.json", NULL));
    g_assert_true (exm_repository_check_url ("http://127.0.0.1:8765/index.json", NULL));
    g_assert_true (exm_repository_check_url ("http://[::1]/index.json", NULL));

    g_assert_false (exm_repository_check_url ("http://example.com/index.json", &error));
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error (&error);
    g_assert_false (exm_repository_check_url ("not a url", NULL));
    g_assert_false (exm_repository_check_url ("file:///etc/passwd", NULL));
    g_assert_false (exm_repository_check_url ("https:///index.json", NULL));

    g_assert_true (exm_repository_check_key (TEST_KEY, NULL));
    g_assert_false (exm_repository_check_key ("", &error));
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error (&error);
    // 64 bytes: a signature, not a key
    g_assert_false (exm_repository_check_key (SIGNATURE, NULL));
    g_assert_false (exm_repository_check_key ("YFE8ykXr/yYORPmP97MV/8VteVwEkDe9FKDEewtcHn!=", NULL));
    g_assert_false (exm_repository_check_key (NULL, NULL));
}

static GSettings *
app_settings (void)
{
    g_autoptr (GSettingsSchemaSource) source = NULL;
    g_autoptr (GSettingsSchema) schema = NULL;
    g_autoptr (GSettingsBackend) backend = g_memory_settings_backend_new ();
    GError *error = NULL;

    source = g_settings_schema_source_new_from_directory (TEST_SCHEMA_DIR, NULL, TRUE, &error);
    g_assert_no_error (error);
    schema = g_settings_schema_source_lookup (source, APP_ID, FALSE);
    g_assert_nonnull (schema);

    return g_settings_new_full (schema, backend, NULL);
}

static void
test_own_settings (void)
{
    g_autoptr (GSettings) settings = app_settings ();
    g_autofree gchar *url = NULL;
    g_autofree gchar *key = NULL;
    GError *error = NULL;

    g_unsetenv ("EXM_EXTENSION_REPOSITORY");
    g_unsetenv ("EXM_EXTENSION_REPOSITORY_KEY");

    // A stock Shell has no repository; the application's default is used
    g_assert_cmpint (exm_repository_choose_source (NULL, NULL, settings, &url, &key),
                     ==, EXM_REPOSITORY_ORIGIN_SETTINGS);
    g_assert_cmpstr (url, ==, "https://shell-extension-registry-81c4e5.gitlab.io/index.json");
    g_assert_cmpstr (key, ==, "37rwPeZLAvu/3PO70cbnr6trHwutr7SetPnOkCNm5/g=");
    g_assert_true (exm_repository_check_source (url, key, NULL));
    g_clear_pointer (&url, g_free);
    g_clear_pointer (&key, g_free);

    g_assert_true (exm_repository_store_source (settings, "http://127.0.0.1:8765/index.json", TEST_KEY, &error));
    g_assert_no_error (error);
    exm_repository_choose_source (NULL, NULL, settings, &url, &key);
    g_assert_cmpstr (url, ==, "http://127.0.0.1:8765/index.json");
    g_assert_cmpstr (key, ==, TEST_KEY);
    g_clear_pointer (&url, g_free);
    g_clear_pointer (&key, g_free);

    // Both or neither, and checked like the Shell does
    g_assert_false (exm_repository_store_source (settings, "https://registry.example/index.json", "", &error));
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error (&error);
    g_assert_false (exm_repository_store_source (settings, "", TEST_KEY, &error));
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error (&error);
    g_assert_false (exm_repository_store_source (settings, "http://example.com/index.json", TEST_KEY, &error));
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error (&error);
    g_assert_false (exm_repository_store_source (settings, "https://registry.example/index.json", SIGNATURE, NULL));

    exm_repository_choose_source (NULL, NULL, settings, &url, &key);
    g_assert_cmpstr (url, ==, "http://127.0.0.1:8765/index.json");
    g_clear_pointer (&url, g_free);
    g_clear_pointer (&key, g_free);

    // Empty: extensions.gnome.org
    g_assert_true (exm_repository_store_source (settings, "", "", NULL));
    exm_repository_choose_source (NULL, NULL, settings, &url, &key);
    g_assert_cmpstr (url, ==, "");
    g_assert_cmpstr (key, ==, "");
    g_clear_pointer (&url, g_free);
    g_clear_pointer (&key, g_free);

    // A Shell with a repository decides, even when it uses extensions.gnome.org
    g_assert_true (exm_repository_store_source (settings, "https://registry.example/index.json", TEST_KEY, NULL));
    g_assert_cmpint (exm_repository_choose_source ("", "", settings, &url, &key),
                     ==, EXM_REPOSITORY_ORIGIN_SHELL);
    g_assert_cmpstr (url, ==, "");
    g_clear_pointer (&url, g_free);
    g_clear_pointer (&key, g_free);

    g_setenv ("EXM_EXTENSION_REPOSITORY", "http://127.0.0.1:1/index.json", TRUE);
    g_assert_cmpint (exm_repository_choose_source (NULL, NULL, settings, &url, &key),
                     ==, EXM_REPOSITORY_ORIGIN_ENVIRONMENT);
    g_assert_cmpstr (url, ==, "http://127.0.0.1:1/index.json");
    g_assert_cmpstr (key, ==, "");
    g_unsetenv ("EXM_EXTENSION_REPOSITORY");
}

static void
test_routing (void)
{
    ExmRepository *repository = exm_repository_get_default ();

    g_assert_false (exm_repository_handles (repository, "https://extensions.gnome.org/api/v1/extensions/"));

    g_test_expect_message (NULL, G_LOG_LEVEL_WARNING, "*must be an HTTPS address*");
    exm_repository_set_source (repository, "http://example.com/index.json", TEST_KEY);
    g_test_assert_expected_messages ();
    g_assert_false (exm_repository_is_enabled (repository));

    // A repository without a usable key is refused
    g_test_expect_message (NULL, G_LOG_LEVEL_WARNING, "*base64 Ed25519 key*");
    exm_repository_set_source (repository, "https://registry.example/index.json", "");
    g_test_assert_expected_messages ();
    g_assert_false (exm_repository_is_enabled (repository));

    exm_repository_set_source (repository, "https://registry.example/index.json", TEST_KEY);
    g_assert_true (exm_repository_handles (repository, "https://extensions.gnome.org/api/v1/extensions/"));
    g_assert_true (exm_repository_handles (repository, "https://extensions.gnome.org/comments/all/?pk=1"));
    g_assert_false (exm_repository_handles (repository, "https://extensions.gnome.org/extension-data/icons/a.png"));

    exm_repository_set_source (repository, "", "");
    g_assert_false (exm_repository_is_enabled (repository));
}

int
main (int argc, char *argv[])
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/repository/signature", test_signature);
    g_test_add_func ("/repository/index-checks", test_index_checks);
    g_test_add_func ("/repository/browse-and-search", test_browse_and_search);
    g_test_add_func ("/repository/detail-and-versions", test_detail_and_versions);
    g_test_add_func ("/repository/unavailable", test_unavailable);
    g_test_add_func ("/repository/source-checks", test_source_checks);
    g_test_add_func ("/repository/routing", test_routing);
    g_test_add_func ("/repository/own-settings", test_own_settings);

    return g_test_run ();
}
