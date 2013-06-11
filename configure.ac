dnl required version of autoconf
AC_PREREQ([2.60])

AC_INIT([vlc-plugin-gles2],[0.1.0],[bugs@avionic-design.de])

AC_CONFIG_MACRO_DIR([m4])
AC_CONFIG_SRCDIR([src])
AC_CONFIG_HEADERS([config.h])

dnl required version of automake
AM_INIT_AUTOMAKE([1.10])
m4_ifdef([AM_SILENT_RULES], [AM_SILENT_RULES([yes])])

dnl enable mainainer mode by default
AM_MAINTAINER_MODE([enable])

dnl check for tools (compiler etc.)
AC_PROG_CC_C99
AM_PROG_CC_C_O
AM_PROG_LIBTOOL
PKG_PROG_PKG_CONFIG()

PKG_CHECK_MODULES(EGL, [egl >= 1.3])
PKG_CHECK_MODULES(GLES2, [glesv2 >= 2.0])
PKG_CHECK_MODULES(VLC_PLUGIN, [vlc-plugin >= 1.1.0])

dnl set the plugindir where plugins should be installed (for src/Makefile.am)
plugindir="\$(libdir)/vlc/plugins/video_output"
AC_SUBST(plugindir)

dnl set proper LDFLAGS for plugins
VLC_PLUGIN_LDFLAGS='-module -avoid-version -export-symbols-regex ^vlc_entry'
AC_SUBST(VLC_PLUGIN_LDFLAGS)

AC_CONFIG_FILES([Makefile src/Makefile])
AC_OUTPUT
