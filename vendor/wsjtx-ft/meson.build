# WSJT-X's FT8/FT4 decoders (Fortran) → skimmer-ft8d, a helper program like
# skimmer-wsprd. VENDOR.md records the subset and why. The upstream files are
# never patched: warnings are silenced here. Without a Fortran compiler (or
# with -Dft8=disabled) nothing here is built and FT8 is simply not there.
ft8_enabled = false
skimmer_ft8d = disabler()
skimmer_ft8code = disabler()
skimmer_ft4code = disabler()

if add_languages('fortran', required : get_option('ft8'), native : false)
  ft8_enabled = true
  ft_args = ['-w', '-O2', '-fno-second-underscore', '-ffree-line-length-none',
             '-fallow-argument-mismatch', '-fno-range-check']
  ft_inc = include_directories('.', 'ft8', 'ft4')

  # Everything the decoders (and the encoders the gate uses) reach. The
  # *_params.f90 / *_testmsg.f90 / pfx.f90 / fftw3.f03 files are only
  # INCLUDEd, never compiled on their own.
  ft_common = files(
    'timer_module.f90', 'shmem.f90', 'crc.f90', 'fftw3mod.f90', 'readwav.f90',
    '77bit/packjt77.f90', 'packjt.f90', 'chkcall.f90', 'fmtmsg.f90',
    'deg2grid.f90', 'grid2deg.f90', 'indexx.f90', 'init_random_seed.f90',
    'four2a.f90', 'db.f90', 'pctile.f90', 'peakup.f90', 'pltanh.f90',
    'polyfit.f90', 'determ.f90', 'shell.f90', 'smo121.f90', 'nuttal_window.f90',
    'bpdecode144.f90', 'ft2/gfsk_pulse.f90',
    'qra/q65/q65_set_list.f90',            # only for stdcall()
    'ft8/baseline.f90', 'ft8/decode174_91.f90', 'ft8/encode174_91.f90',
    'ft8/encode174_91_nocrc.f90', 'ft8/ft8apset.f90', 'ft8/ft8_a7.f90',
    'ft8/ft8_a8d.f90', 'ft8/ft8b.f90', 'ft8/ft8_downsample.f90',
    'ft8/gen_ft8wave.f90', 'ft8/genft8.f90', 'ft8/get_crc14.f90',
    'ft8/chkcrc14a.f90', 'ft8/get_spectrum_baseline.f90',
    'ft8/osd174_91.f90', 'ft8/subtractft8.f90', 'ft8/sync8.f90',
    'ft8/sync8d.f90', 'ft8/twkfreq1.f90',
    'ft4/ft4_baseline.f90', 'ft4/ft4_downsample.f90', 'ft4/gen_ft4wave.f90',
    'ft4/genft4.f90', 'ft4/get_ft4_bitmetrics.f90', 'ft4/getcandidates4.f90',
    'ft4/subtractft4.f90', 'ft4/sync4d.f90',
    'ft8_decode.f90', 'ft4_decode.f90',
    'wsjtx_stubs.c',                       # ours: boost crc + Qt shmem
  )
  ft_lib = static_library('wsjtx-ft', ft_common,
    fortran_args : ft_args, c_args : ['-w'],
    include_directories : ft_inc,
    dependencies : [fftw3f_dep, m_dep],
    override_options : ['warning_level=0'])

  skimmer_ft8d = executable('skimmer-ft8d', 'ft8d_main.f90',
    fortran_args : ft_args, include_directories : ft_inc,
    link_with : ft_lib, dependencies : [fftw3f_dep, m_dep],
    override_options : ['warning_level=0'], install : true)

  # Upstream's ft8code / ft4code (the channel tones of a message): test-only,
  # so the gates synthesize signals without an encoder of our own.
  skimmer_ft8code = executable('skimmer-ft8code', 'ft8/ft8code.f90',
    fortran_args : ft_args, include_directories : ft_inc,
    link_with : ft_lib, dependencies : [fftw3f_dep, m_dep],
    override_options : ['warning_level=0'])
  skimmer_ft4code = executable('skimmer-ft4code', 'ft4/ft4code.f90',
    fortran_args : ft_args, include_directories : ft_inc,
    link_with : ft_lib, dependencies : [fftw3f_dep, m_dep],
    override_options : ['warning_level=0'])
endif
