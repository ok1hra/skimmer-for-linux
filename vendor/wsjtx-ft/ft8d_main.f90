! skimmer-ft8d -- decode FT8 or FT4 WAV files with WSJT-X's own decoders.
!
! OURS, not WSJT-X's (see VENDOR.md): it stands in for jt9 + multimode_decoder,
! which would pull in JT65/JT9/Q65/FST4 and Qt's shared memory. It drives
! ft8_decode / ft4_decode exactly as jt9 does when it reads a file from disk
! (jt9.f90: FT8 in three passes, nzhsym 41, 47, 50; FT4 once) and prints
! jt9's own lines, so its output can be compared with a stock jt9 run.
!
! Usage: skimmer-ft8d -8|-4 [-d DEPTH] [-L HZ] [-H HZ] [-f HZ] FILE.wav ...
!   FILE = 12000 Hz mono 16-bit; YYMMDD_HHMMSS.wav gives the UTC like jt9.
!   Files are decoded in order in one process, so a later cycle sees the
!   earlier ones (FT8's a7 table, the callsign hash table) as jt9 would.
! Output: HHMMSS snr dt freq ~|+  message  [aN]   (jt9's format)
!
! Copyright (C) 2026 skimmer-for-linux authors. GPL-3.0-or-later.

module ft8d_out
  use ft8_decode
  use ft4_decode
  implicit none
  integer :: nutc_cur = 0
  integer :: ndecodes = 0
contains
  subroutine ft8_out(this,sync,snr,dt,freq,decoded,nap,qual)
    class(ft8_decoder), intent(inout) :: this
    real, intent(in) :: sync
    integer, intent(in) :: snr
    real, intent(in) :: dt
    real, intent(in) :: freq
    character(len=37), intent(in) :: decoded
    integer, intent(in) :: nap
    real, intent(in) :: qual
    call put(snr,dt,freq,decoded,nap,qual,'~')
  end subroutine ft8_out

  subroutine ft4_out(this,sync,snr,dt,freq,decoded,nap,qual)
    class(ft4_decoder), intent(inout) :: this
    real, intent(in) :: sync
    integer, intent(in) :: snr
    real, intent(in) :: dt
    real, intent(in) :: freq
    character(len=37), intent(in) :: decoded
    integer, intent(in) :: nap
    real, intent(in) :: qual
    call put(snr,dt,freq,decoded,nap,qual,'+')
  end subroutine ft4_out

  ! decoder.f90's ft8_decoded / ft4_decoded: the same line, the same '?'
  subroutine put(snr,dt,freq,decoded,nap,qual,cmode)
    integer, intent(in) :: snr, nap
    real, intent(in) :: dt, freq, qual
    character(len=37), intent(in) :: decoded
    character(len=1), intent(in) :: cmode
    character(len=2) :: annot
    character(len=37) :: decoded0
    decoded0=decoded
    annot='  '
    if(nap.ne.0) then
       write(annot,'(a1,i1)') 'a',nap
       if(qual.lt.0.17) decoded0(37:37)='?'
    endif
    write(*,1001) nutc_cur,snr,dt,nint(freq),cmode,decoded0,annot
1001 format(i6.6,i4,f5.1,i5,1x,a1,1x,1x,a37,1x,a2)
    call flush(6)
    ndecodes=ndecodes+1
  end subroutine put
end module ft8d_out

program skimmer_ft8d
  use ft8d_out
  use readwav
  implicit none
  integer, parameter :: NP8=15*12000, NP4=21*3456
  type(ft8_decoder) :: d8
  type(ft4_decoder) :: d4
  type(wav_header) :: wav
  integer*2 :: id2(NP8), id2a(NP8)
  integer :: mode=8, ndepth=2, nfa=200, nfb=3500, nfqso=1500
  integer :: iarg, nargs, i1, nearly, npts, ios
  character(len=500) :: arg
  character(len=12) :: mycall='            ', hiscall='            '
  character(len=6) :: hisgrid='      '
  logical :: ltry_a8, newdat
  logical*1 :: ldiskdat

  nargs=command_argument_count()
  if(nargs.lt.1) call usage()
  iarg=1
  do while(iarg.le.nargs)
     call get_command_argument(iarg,arg)
     select case(trim(arg))
     case('-8'); mode=8
     case('-4'); mode=4
     case('-d'); iarg=iarg+1; call num(iarg,ndepth)
     case('-L'); iarg=iarg+1; call num(iarg,nfa)
     case('-H'); iarg=iarg+1; call num(iarg,nfb)
     case('-f'); iarg=iarg+1; call num(iarg,nfqso)
     case('-h','--help'); call usage()
     case default
        exit
     end select
     iarg=iarg+1
  enddo
  if(iarg.gt.nargs .or. ndepth.lt.1 .or. ndepth.gt.3 .or. nfa.ge.nfb) call usage()
  ldiskdat=.true.

  do while(iarg.le.nargs)
     call get_command_argument(iarg,arg)
     iarg=iarg+1
     call wav%read(trim(arg))
     if(wav%audio_format%sample_rate.ne.12000 .or.                       &
          wav%audio_format%num_channels.ne.1 .or.                        &
          wav%audio_format%bits_per_sample.ne.16) then
        write(0,*) 'skimmer-ft8d: not 12000 Hz mono 16-bit: ',trim(arg)
        close(unit=wav%lun)
        stop 2
     endif
     ! jt9: the UTC from the name (YYMMDD_HHMMSS.wav or _HHMM.wav)
     nutc_cur=0
     i1=index(arg,'.wav')
     if(i1.lt.1) i1=index(arg,'.WAV')
     if(i1.ge.7) then
        if(arg(i1-5:i1-5).eq.'_') then
           read(arg(i1-4:i1-1),*,iostat=ios) nutc_cur
        else
           read(arg(i1-6:i1-1),*,iostat=ios) nutc_cur
        endif
        if(ios.ne.0) nutc_cur=0
     endif
     npts=NP8
     if(mode.eq.4) npts=NP4
     id2=0
     read(unit=wav%lun,iostat=ios) id2(1:min(npts,wav%data_size/2))
     close(unit=wav%lun)

     if(mode.eq.8) then
        ! jt9.f90 from disk: the early passes at 41 and 47 half-symbols, then 50
        ltry_a8=.false.
        newdat=.true.     ! jt9: params%newdat, which the decoder clears
        nearly=41
        id2a=0
        id2a(1:nearly*3456)=id2(1:nearly*3456)
        call d8%decode(ft8_out,id2a,0,nfqso,nfqso,newdat,nutc_cur,nfa,nfb, &
             nearly,ndepth,0.0,0,.false.,.false.,ltry_a8,.false.,75,       &
             mycall,hiscall,hisgrid,ldiskdat)
        nearly=47
        id2a(1:nearly*3456)=id2(1:nearly*3456)
        call d8%decode(ft8_out,id2a,0,nfqso,nfqso,newdat,nutc_cur,nfa,nfb, &
             nearly,ndepth,0.0,0,.false.,.false.,ltry_a8,.false.,75,       &
             mycall,hiscall,hisgrid,ldiskdat)
        id2a(nearly*3456+1:50*3456)=id2(nearly*3456+1:50*3456)
        call d8%decode(ft8_out,id2a,0,nfqso,nfqso,newdat,nutc_cur,nfa,nfb, &
             50,ndepth,0.0,0,.false.,.false.,ltry_a8,.false.,75,           &
             mycall,hiscall,hisgrid,ldiskdat)
     else
        call d4%decode(ft4_out,id2(1:NP4),0,nfqso,nfa,nfb,ndepth,.false.,0, &
             mycall,hiscall)
     endif
     write(*,'(a)') '<DecodeFinished>'
     call flush(6)
  enddo

contains
  subroutine num(i,n)
    integer, intent(in) :: i
    integer, intent(out) :: n
    character(len=32) :: a
    integer :: st
    call get_command_argument(i,a)
    read(a,*,iostat=st) n
    if(st.ne.0) call usage()
  end subroutine num

  subroutine usage()
    write(0,'(a)') 'Usage: skimmer-ft8d -8|-4 [-d 1..3] [-L HZ] [-H HZ] [-f HZ] FILE.wav ...'
    write(0,'(a)') '  12000 Hz mono 16-bit WAV; YYMMDD_HHMMSS.wav names the UTC (as jt9).'
    stop 2
  end subroutine usage
end program skimmer_ft8d
