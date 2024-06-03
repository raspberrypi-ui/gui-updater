bin_PROGRAMS = gui-updater

gui_updater_CFLAGS = \
	-I$(top_srcdir) \
	-DPACKAGE_LIB_DIR=\""$(libdir)"\" \
	-DPACKAGE_DATA_DIR=\""$(datadir)/gui-updater"\" \
	-DPACKAGE_UI_DIR=\""$(datadir)/gui-updater/ui"\" \
	-DPACKAGE_BIN_DIR=\""$(bindir)"\" \
	-DPACKAGE_LOCALE_DIR=\""$(prefix)/$(DATADIRNAME)/locale"\" \
	$(PACKAGE_CFLAGS) \
	$(G_CAST_CHECKS)

gui_updater_SOURCES = updater.c

gui_updater_includedir = $(includedir)

gui_updater_include_HEADERS =

gui_updater_LDFLAGS = $(DYNAMIC_FLAGS)
gui_updater_DEPENDENCIES_EXTRA = $(BUILTIN_PLUGINS)

gui_updater_LDADD = \
		$(BUILTIN_PLUGINS) \
		$(PACKAGE_LIBS) \
		$(X11_LIBS) \
		$(INTLLIBS)

EXTRA_DIST =
