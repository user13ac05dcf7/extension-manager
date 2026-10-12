# The application's schema, for the settings the repository falls back to
test_schema = custom_target('test-schema',
  input: schema_file_in,
  output: 'gschemas.compiled',
  command: [find_program('glib-compile-schemas'), '--strict',
            '--targetdir=@OUTDIR@', meson.project_build_root() / 'data'],
)

test_repository = executable('test-repository',
  ['test-repository.c', '../src/web/exm-repository.c'],
  dependencies: exm_deps,
  c_args: ['-DTEST_SCHEMA_DIR="@0@"'.format(meson.current_build_dir())],
  include_directories: include_directories('../src'),
)

test('repository', test_repository,
  env: ['G_DEBUG=fatal-criticals'],
  depends: [test_schema],
)
