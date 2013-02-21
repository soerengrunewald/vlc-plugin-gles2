plugin_LTLIBRARIES = libgles2_plugin.la

libgles2_plugin_la_SOURCES = gles2.c
libgles2_plugin_la_CFLAGS = \
	$(VLC_PLUGIN_CFLAGS) \
	$(GLES2_CFLAGS) \
	$(EGL_CFLAGS) \
	-DMODULE_STRING=\"gles2\"

libgles2_plugin_la_LIBADD = \
	$(VLC_PLUGIN_LIBS) \
	$(GLES2_LIBS) \
	$(EGL_LIBS)
libgles2_plugin_la_LDFLAGS = \
	$(VLC_PLUGIN_LDFLAGS)

libgles2_plugin_la_LIBTOOLFLAGS = --tag=disable-static

#noinst_HEADERS = gles2.h
