# GNOME Shell's schema is missing in a sandbox; see src/local/exm-package.c
shell_schema = custom_target('shell-schema',
  input: 'org.gnome.shell.gschema.xml',
  output: 'gschemas.compiled',
  command: [find_program('glib-compile-schemas'), '--strict',
            '--targetdir=@OUTDIR@', '@CURRENT_SOURCE_DIR@'],
  install: true,
  install_dir: shell_schema_dir,
)
