test_repository = executable('test-repository',
  ['test-repository.c', '../src/web/exm-repository.c'],
  dependencies: exm_deps,
  include_directories: include_directories('../src'),
)

test('repository', test_repository,
  env: ['G_DEBUG=fatal-criticals'],
)
