# JS8Call-improved's JS8 decoder (C++20) → skimmer-js8d, a helper program like
# skimmer-ft8d. VENDOR.md records the subset, the pin and WIFILT's shim. The
# upstream files are never patched: extract_core.py cuts the Qt worker off
# JS8.cpp (sha256-pinned) and appends the shim's C API. Without a C++20
# compiler or Boost's headers (or with -Djs8=disabled) nothing here is built
# and JS8 is simply not there.
js8_enabled = false
skimmer_js8d = disabler()
skimmer_js8code = disabler()

js8_cpp_ok = add_languages('cpp', required : get_option('js8'), native : false)
if js8_cpp_ok
  cxx = meson.get_compiler('cpp')
  js8_cpp_ok = cxx.has_header('boost/crc.hpp', required : get_option('js8')) and \
               cxx.has_header('boost/multi_index_container.hpp', required : get_option('js8'))
endif
if js8_cpp_ok
  js8_enabled = true
  py = find_program('python3')
  js8_core = custom_target('js8-reference-core',
    input : ['upstream/JS8_Mode/JS8.cpp', 'shim/decoder_wrapper.inc'],
    output : 'js8_reference_core.cpp',
    command : [py, files('extract_core.py'), '@INPUT0@', '@INPUT1@', '@OUTPUT@',
               'b72787f6e6473919872103997ef5dc9847810ff3e7783124a3961a2da2a86bf1'])
  # The shim first: its JS8.h / QDebug / QLoggingCategory / QtGlobal stand in
  # for upstream's Qt-bound ones; upstream's root so "JS8_Include/…",
  # "JS8_Mode/…" and <vendor/Eigen/Dense> resolve as in its tree.
  js8_inc = include_directories('shim', 'upstream', 'upstream/JS8_Mode')
  js8_args = ['-std=c++20', '-w', '-O2']
  js8_lib = static_library('js8-reference', js8_core,
    'upstream/JS8_Mode/FrequencyTracker.cpp', 'shim/qt_shim.cpp',
    cpp_args : js8_args, include_directories : js8_inc,
    dependencies : [fftw3f_dep, threads_dep],
    override_options : ['warning_level=0', 'cpp_std=c++20'])
  skimmer_js8d = executable('skimmer-js8d', 'js8d_main.cpp',
    cpp_args : ['-std=c++20', '-O2'], include_directories : js8_inc,
    link_with : js8_lib, dependencies : [fftw3f_dep, threads_dep],
    override_options : ['cpp_std=c++20'], install : true)
  # Test-only: the channel tones of a frame (upstream's JS8::encode), so the
  # gates synthesize JS8 without an encoder of our own.
  skimmer_js8code = executable('skimmer-js8code', 'js8code_main.cpp',
    cpp_args : ['-std=c++20', '-O2'], include_directories : js8_inc,
    link_with : js8_lib, dependencies : [fftw3f_dep, threads_dep],
    override_options : ['cpp_std=c++20'])
endif
