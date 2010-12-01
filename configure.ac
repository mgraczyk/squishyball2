#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

cflags_save="$CFLAGS"
AC_PREREQ(2.57)
AC_INIT(main.c)
AM_INIT_AUTOMAKE(squishyball, 20101201, [vorbis-dev@xiph.org])
AC_CONFIG_FILES([Makefile])
m4_ifdef([AM_SILENT_RULES], [AM_SILENT_RULES([yes])], [AC_SUBST([AM_DEFAULT_VERBOSITY], [1])])

# Checks for programs.
AC_PROG_CC

# Checks for libraries.

PKG_CHECK_MODULES([vorbisfile], [vorbisfile])
PKG_CHECK_MODULES([FLAC], [flac >= 0.8.0])
PKG_CHECK_MODULES([ao], [ao > 1.0.0])
AC_CHECK_LIB([ncurses], [initscr],,[AC_MSG_ERROR([ncurses required!])])
AC_CHECK_LIB([ncurses], [_nc_tinfo_fkeysf], USE_FKEYSF=1, USE_FKEYSF=0)
AC_CHECK_LIB([m], [cos])
AC_CHECK_LIB([pthread], [pthread_create])

# Checks for header files.
AC_HEADER_STDC
AC_CHECK_HEADERS([stdlib.h string.h])
AC_CHECK_HEADERS([vorbis/vorbisfile.h])
AC_CHECK_HEADERS([ao/ao.h])
AC_CHECK_HEADERS([FLAC/stream_decoder.h])

# Checks for typedefs, structures, and compiler characteristics.
AC_C_CONST

if test -z "$GCC"; then
        case $host in 
        *-*-irix*)
                DEBUG="-g -signed" 
                CFLAGS="-O2 -w -signed"
                PROFILE="-p -g3 -O2 -signed"
                ;;
        sparc-sun-solaris*)
                DEBUG="-v -g"
                CFLAGS="-xO4 -fast -w -fsimple -native -xcg92"
                PROFILE="-v -xpg -g -xO4 -fast -native -fsimple -xcg92 -Dsuncc"
                ;;
        *)
                DEBUG="-g"
                CFLAGS="-O"
                PROFILE="-g -p" 
                ;;
        esac
else
        case $host in 
        *-*-linux*)
                DEBUG="-g -Wall -fsigned-char"
                CFLAGS="-O2 -fsigned-char -ffast-math"
                PROFILE="-Wall -W -pg -g -O2 -fsigned-char -ffast-math"
                ;;
        sparc-sun-*)
                DEBUG="-g -Wall -fsigned-char"
                CFLAGS="-O2 -fsigned-char"
                PROFILE="-pg -g -O2 -fsigned-char" 
                ;;
        *-*-darwin*)
                DEBUG="-fno-common -g -Wall -fsigned-char"
                CFLAGS="-fno-common -O2 -Wall -fsigned-char -ffast-math"
                PROFILE="-fno-common -O2 -Wall -pg -g -fsigned-char -ffast-math"
                ;;
        *)
                DEBUG="-g -Wall -fsigned-char"
                CFLAGS="-O2 -fsigned-char -ffast-math"
                PROFILE="-O2 -g -pg -fsigned-char -ffast-math" 
                ;;
        esac
fi

COMMON_FLAGS="$cflags_save $vorbisfile_CFLAGS $ao_CFLAGS $FLAC_CFLAGS -DUSE_FKEYSF=$USE_FKEYSF"
CFLAGS="$CFLAGS -DVERSION='\"$VERSION\"' $COMMON_FLAGS"
DEBUG="$DEBUG -DVERSION='\\\"$VERSION\\\"' $COMMON_FLAGS"
PROFILE="$PROFILE -DVERSION='\\\"$VERSION\\\"' $COMMON_FLAGS"
LIBS="$LIBS $vorbisfile_LIBS $ao_LIBS $FLAC_LIBS"
AC_SUBST(DEBUG)
AC_SUBST(PROFILE)

AC_OUTPUT
