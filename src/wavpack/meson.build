wavpack_dep = dependency('wavpack', version: '>= 4.31', required: get_option('wavpack'))
have_wavpack = wavpack_dep.found()


if have_wavpack
  shared_module('wavpack',
    'wavpack.cc',
    dependencies: [audacious_dep, wavpack_dep, audtag_dep],
    name_prefix: '',
    include_directories: [src_inc],
    install: true,
    install_dir: input_plugin_dir,
  )
endif
