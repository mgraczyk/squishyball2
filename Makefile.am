## Process this file with automake to produce Makefile.in

AUTOMAKE_OPTIONS = foreign 1.11

EXTRA_DIST = COPYING autogen.sh

bin_PROGRAMS = squishyball
mandir = @MANDIR@
man_MANS = squishyball.1

squishyball_SOURCES = audio.c loader.c main.c mincurses.c tty.c main.h mincurses.h

debug:
	$(MAKE) all CFLAGS="@DEBUG@"

profile:
	$(MAKE) all CFLAGS="@PROFILE@"
