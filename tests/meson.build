# The application's schema, for the settings the repository falls back to
test_schema = custom_target('test-schema',
  input: schema_file_in,
  output: 'gschemas.compiled',
  command: [find_program('glib-compile-schemas'), '--strict',
            '--targetdir=@OUTDIR@', meson.project_build_root() / 'data'],
)

test_c_args = [
  '-DTEST_SCHEMA_DIR="@0@"'.format(meson.current_build_dir()),
  '-DTEST_SHELL_SCHEMA_DIR="@0@"'.format(meson.project_build_root() / 'data' / 'shell'),
]

test_repository = executable('test-repository',
  ['test-repository.c', '../src/web/exm-repository.c'],
  dependencies: exm_deps,
  c_args: test_c_args,
  include_directories: include_directories('../src'),
)

test('repository', test_repository,
  env: ['G_DEBUG=fatal-criticals'],
  depends: [test_schema],
)

test_package = executable('test-package',
  ['test-package.c', '../src/local/exm-package.c', '../src/web/exm-repository.c'],
  dependencies: exm_deps,
  c_args: test_c_args,
  include_directories: include_directories('../src'),
)

# The isolated test directories have no proxy settings to read
test('package', test_package,
  env: ['G_DEBUG=fatal-criticals', 'GIO_USE_PROXY_RESOLVER=dummy', 'GSETTINGS_BACKEND=memory'],
  depends: [shell_schema],
)
