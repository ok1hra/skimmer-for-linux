# WSJT-X's wsprd, vendored (VENDOR.md): the WSPR decoder skimmer-headless
# runs as a helper program, one 2-minute cycle of one band per start.
# Upstream sources are not modified — warnings are silenced here; the
# Fortran OSD is replaced by osd_stub.c (never called: no -o).
wsprd_common = files('wsprd_utils.c', 'wsprsim_utils.c', 'nhash.c', 'tab.c', 'fano.c')
wsprd_c_args = ['-w', '-ffast-math', '-U_GNU_SOURCE', '-std=gnu11']

skimmer_wsprd = executable('skimmer-wsprd',
  ['wsprd.c', 'jelinek.c', 'osd_stub.c', wsprd_common],
  c_args : wsprd_c_args,
  override_options : ['warning_level=0', 'c_std=gnu11'],
  dependencies : [fftw3f_dep, m_dep],
  install : true,
)

# wsprsim's encoder, for the gate's synthetic WSPR signal
wsprd_enc_lib = static_library('wsprd-enc', wsprd_common,
  c_args : wsprd_c_args,
  override_options : ['warning_level=0', 'c_std=gnu11'],
)
wsprd_enc_inc = include_directories('.')
