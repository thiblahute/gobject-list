project('gobject-list', 'c',
  version : '1.0',
  meson_version : '>= 0.47',
  default_options : [ 'warning_level=1',
                      'buildtype=debugoptimized' ])

cc = meson.get_compiler('c')
unwind_dep = dependency('libunwind', required: false)
gobject_dep = dependency('gobject-2.0', fallback: ['glib', 'libgobject_dep'])
gst_dep = dependency('gstreamer-1.0', fallback: ['gstreamer', 'gst_dep'])
dl_dep = cc.find_library('dl', required : false)

cdata = configuration_data()
cdata.set('HAVE_LIBUNWIND', 1)
configure_file(output : 'config.h', configuration : cdata)

library('gobject-list', 'gobject-list.c', dependencies: [unwind_dep, gobject_dep, dl_dep, gst_dep])