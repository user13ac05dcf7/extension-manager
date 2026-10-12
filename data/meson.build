desktop_file_in_config = configuration_data()
desktop_file_in_config.set('app_id', app_id)
desktop_file_in = configure_file(
          input: 'com.mattjakeman.ExtensionManager.desktop.in.in',
         output: '@0@.desktop.in'.format(app_id),
  configuration: desktop_file_in_config,
)

desktop_file = i18n.merge_file(
  input: desktop_file_in,
  output: '@0@.desktop'.format(app_id),
  type: 'desktop',
  po_dir: '../po',
  install: true,
  install_dir: join_paths(get_option('datadir'), 'applications')
)

desktop_utils = find_program('desktop-file-validate', required: false)
if desktop_utils.found()
  test('Validate desktop file', desktop_utils,
    args: [desktop_file]
  )
endif

appconf = configuration_data()
appconf.set('app_id', app_id)
appstream_file_in = configure_file(
          input: 'com.mattjakeman.ExtensionManager.metainfo.xml.in.in',
         output: 'com.mattjakeman.ExtensionManager.metainfo.xml.in',
  configuration: appconf,
)

appstream_file = i18n.merge_file(
  input: appstream_file_in,
  output: '@0@.metainfo.xml'.format(app_id),
  po_dir: '../po',
  install: true,
  install_dir: join_paths(get_option('datadir'), 'metainfo')
)

appstreamcli = find_program('appstreamcli', required: false)
if appstreamcli.found()
  test('Validate appstream file', appstreamcli,
    args: ['validate', '--no-net', '--explain', appstream_file]
  )
endif

schemaconf = configuration_data()
schemaconf.set('app_id', app_id)
schemaconf.set('app_version', meson.project_version())
schema_file_in = configure_file(
          input: 'com.mattjakeman.ExtensionManager.gschema.xml.in',
         output: '@0@.gschema.xml'.format(app_id),
  configuration: schemaconf,
)

install_data(schema_file_in,
  install_dir: join_paths(get_option('datadir'), 'glib-2.0/schemas')
)

compile_schemas = find_program('glib-compile-schemas', required: false)
if compile_schemas.found()
  test('Validate schema file', compile_schemas,
    args: ['--strict', '--dry-run', meson.current_source_dir()]
  )
endif

search_provider_conf = configuration_data()
search_provider_conf.set('app_id', app_id)
search_provider_conf.set('app_id_path', '/' + app_id.replace('.', '/'))
configure_file(
  input: 'com.mattjakeman.ExtensionManager.search-provider.ini.in',
  output: '@0@.search-provider.ini'.format(app_id),
  configuration: search_provider_conf,
  install_dir: join_paths(get_option('datadir'), 'gnome-shell', 'search-providers')
)

dbus_service_conf = configuration_data()
dbus_service_conf.set('app_id', app_id)
dbus_service_conf.set('bindir', join_paths(get_option('prefix'), get_option('bindir')))
configure_file(
  input: 'com.mattjakeman.ExtensionManager.service.in',
  output: '@0@.service'.format(app_id),
  configuration: dbus_service_conf,
  install_dir: join_paths(get_option('datadir'), 'dbus-1', 'services')
)

subdir('icons')
subdir('shell')
