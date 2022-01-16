# SPDX-License-Identifier: LGPL-2.1

include $(src)/scripts/utils.mk


# This Makefile and manpage XSL files were taken from libtracefs
# and modified for libccli.

MAN3_TXT= \
	$(wildcard libccli-*.txt) \
	libccli.txt

MAN1_TEXT= \
	$(wildcard libccli-*.txt.1)

MAN_TXT = $(MAN3_TXT)
_MAN_XML=$(patsubst %.txt,%.xml,$(MAN_TXT))
_MAN_HTML=$(patsubst %.txt,%.html,$(MAN_TXT))
_DOC_MAN3=$(patsubst %.txt,%.m,$(MAN3_TXT))

MAN_XML=$(addprefix $(OUTPUT),$(_MAN_XML))
MAN_HTML=$(addprefix $(OUTPUT),$(_MAN_HTML))
DOC_MAN3=$(addprefix $(OUTPUT),$(_DOC_MAN3))

_MAN1_XML=$(patsubst %.txt.1,%.xml,$(MAN1_TEXT))
_MAN1_HTML=$(patsubst %.txt.1,%.html,$(MAN1_TEXT))
_DOC_MAN1=$(patsubst %.txt.1,%.m,$(MAN1_TEXT))

MAN1_XML=$(addprefix $(OUTPUT),$(_MAN1_XML))
MAN1_HTML=$(addprefix $(OUTPUT),$(_MAN1_HTML))
DOC_MAN1=$(addprefix $(OUTPUT),$(_DOC_MAN1))


# Make the path relative to DESTDIR, not prefix
ifndef DESTDIR
prefix?=$(HOME)
endif
bindir?=$(prefix)/bin
htmldir?=$(prefix)/share/doc/libccli-doc
pdfdir?=$(prefix)/share/doc/libccli-doc
mandir?=$(prefix)/share/man
man3dir=$(mandir)/man3
man1dir=$(mandir)/man1

ASCIIDOC=asciidoc
ASCIIDOC_EXTRA = --unsafe -f asciidoc.conf
ASCIIDOC_HTML = xhtml11
MANPAGE_XSL = manpage-normal.xsl
XMLTO_EXTRA =
INSTALL?=install
RM ?= rm -f

ifdef USE_ASCIIDOCTOR
ASCIIDOC = asciidoctor
ASCIIDOC_EXTRA = -a compat-mode
ASCIIDOC_EXTRA += -I. -rasciidoctor-extensions
ASCIIDOC_EXTRA += -a mansource="libccli" -a manmanual="libccli Manual"
ASCIIDOC_HTML = xhtml5
endif

ASCIIDOC_INSTALLED := $(shell command -v $(ASCIIDOC) 2> /dev/null)
ifndef ASCIIDOC_INSTALLED
	missing_tools += $(ASCIIDOC)
endif

XMLTO=xmlto
XMLTO_INSTALLED := $(shell command -v $(XMLTO) 2> /dev/null)
ifndef XMLTO_INSTALLED
	missing_tools += $(XMLTO)
endif

#
# For asciidoc ...
#	-7.1.2,	no extra settings are needed.
#	8.0-,	set ASCIIDOC8.
#

#
# For docbook-xsl ...
#	-1.68.1,	set ASCIIDOC_NO_ROFF? (based on changelog from 1.73.0)
#	1.69.0,		no extra settings are needed?
#	1.69.1-1.71.0,	set DOCBOOK_SUPPRESS_SP?
#	1.71.1,		no extra settings are needed?
#	1.72.0,		set DOCBOOK_XSL_172.
#	1.73.0-,	set ASCIIDOC_NO_ROFF
#

#
# If you had been using DOCBOOK_XSL_172 in an attempt to get rid
# of 'the ".ft C" problem' in your generated manpages, and you
# instead ended up with weird characters around callouts, try
# using ASCIIDOC_NO_ROFF instead (it works fine with ASCIIDOC8).
#

ifdef ASCIIDOC8
ASCIIDOC_EXTRA += -a asciidoc7compatible
endif
ifdef DOCBOOK_XSL_172
ASCIIDOC_EXTRA += -a libccli-asciidoc-no-roff
MANPAGE_XSL = manpage-1.72.xsl
else
	ifdef ASCIIDOC_NO_ROFF
	# docbook-xsl after 1.72 needs the regular XSL, but will not
	# pass-thru raw roff codes from asciidoc.conf, so turn them off.
	ASCIIDOC_EXTRA += -a libccli-asciidoc-no-roff
	endif
endif
ifdef MAN_BOLD_LITERAL
XMLTO_EXTRA += -m manpage-bold-literal.xsl
endif
ifdef DOCBOOK_SUPPRESS_SP
XMLTO_EXTRA += -m manpage-suppress-sp.xsl
endif

SHELL_PATH ?= $(SHELL)
# Shell quote;
SHELL_PATH_SQ = $(subst ','\'',$(SHELL_PATH))

DESTDIR ?=
DESTDIR_SQ = '$(subst ','\'',$(DESTDIR))'

export DESTDIR DESTDIR_SQ

QUIET_SUBDIR0  = +$(MAKE) -C # space to separate -C and subdir
QUIET_SUBDIR1  =

ifneq ($(findstring $(MAKEFLAGS),w),w)
PRINT_DIR = --no-print-directory
else # "make -w"
NO_SUBDIR = :
endif

ifneq ($(findstring $(MAKEFLAGS),s),s)
ifneq ($(V),1)
	QUIET_ASCIIDOC	= @echo '  ASCIIDOC '$@;
	QUIET_XMLTO	= @echo '  XMLTO    '$@;
	QUIET_SUBDIR0	= +@subdir=
	QUIET_SUBDIR1	= ;$(NO_SUBDIR) \
			   echo '  SUBDIR   ' $$subdir; \
			  $(MAKE) $(PRINT_DIR) -C $$subdir
	export V
endif
endif

all: check-man-tools html man

man: man3 man1
man3: $(DOC_MAN3)
man1: $(DOC_MAN1)

html: $(MAN_HTML) $(MAN1_HTML)

$(MAN_HTML) $(MAN1_HTML) $(DOC_MAN3) $(DOC_MAN1): asciidoc.conf

install: check-man-tools install-man install-html

check-man-tools:
ifdef missing_tools
	$(error "You need to install $(missing_tools) for man pages")
endif

install-%.3: $(OUTPUT)%.3
	$(Q)$(call do_install,$<,$(man3dir),644);

install-%.1: $(OUTPUT)%.1
	$(Q)$(call do_install,$<,$(man1dir),644);

do-install-man: man $(addprefix install-,$(wildcard $(OUTPUT)*.3)) $(addprefix install-,$(wildcard $(OUTPUT)*.1))

install-man: man
	$(Q)$(MAKE) -C . do-install-man

install-%.txt: $(OUTPUT)%.html
	$(Q)$(call do_install,$<,$(htmldir),644);

install-%.txt.1: $(OUTPUT)%.html
	$(Q)$(call do_install,$<,$(htmldir),644);

do-install-html: html $(addprefix install-,$(wildcard *.txt)) $(addprefix install-,$(wildcard *.txt.1))

install-html: html do-install-html

uninstall: uninstall-man uninstall-html

uninstall-man:
	$(Q)$(RM) $(addprefix $(DESTDIR)$(man3dir)/,$(DOC_MAN3)) $(addprefix $(DESTDIR)$(man1dir)/,$(DOC_MAN1))

uninstall-html:
	$(Q)$(RM) $(addprefix $(DESTDIR)$(htmldir)/,$(MAN_HTML)) $(addprefix $(DESTDIR)$(htmldir)/,$(MAN1_HTML))

ifdef missing_tools
  DO_INSTALL_MAN = $(warning Please install $(missing_tools) to have the man pages installed)
else
  DO_INSTALL_MAN = do-install-man
endif

CLEAN_FILES =					\
	$(MAN_XML) $(addsuffix +,$(MAN_XML))	\
	$(MAN_HTML) $(addsuffix +,$(MAN_HTML))	\
	$(filter-out $(MAN1_TEXT),$(wildcard *.1)) \
	$(DOC_MAN3) *.3 *.m

clean:
	$(Q) $(RM) $(CLEAN_FILES)

ifdef USE_ASCIIDOCTOR
$(OUTPUT)%.3 : $(OUTPUT)%.txt
	$(QUIET_ASCIIDOC)$(RM) $@+ $@ && \
	$(ASCIIDOC) -b manpage -d manpage \
		$(ASCIIDOC_EXTRA) -alibccli_version=$(LIBCCLI_VERSION) -o $@+ $< && \
	mv $@+ $@
$(OUTPUT)%.1 : $(OUTPUT)%.txt.1
	$(QUIET_ASCIIDOC)$(RM) $@+ $@ && \
	$(ASCIIDOC) -b manpage -d manpage \
		$(ASCIIDOC_EXTRA) -alibccli_version=$(LIBCCLI_VERSION) -o $@+ $< && \
	mv $@+ $@
endif

$(OUTPUT)%.m : $(OUTPUT)%.xml
	$(QUIET_XMLTO)$(RM) $@ && \
	$(XMLTO) -o $(OUTPUT). -m $(MANPAGE_XSL) $(XMLTO_EXTRA) man $<; \
	touch $@

$(OUTPUT)%.xml : %.txt
	$(QUIET_ASCIIDOC)$(RM) $@+ $@ && \
	$(ASCIIDOC) -b docbook -d manpage \
		$(ASCIIDOC_EXTRA) -alibccli_version=$(LIBCCLI_VERSION) -o $@+ $< && \
	mv $@+ $@

$(OUTPUT)%.xml : %.txt.1
	$(QUIET_ASCIIDOC)$(RM) $@+ $@ && \
	$(ASCIIDOC) -b docbook -d manpage \
		$(ASCIIDOC_EXTRA) -alibccli_version=$(LIBCCLI_VERSION) -o $@+ $< && \
	mv $@+ $@

$(MAN_HTML): $(OUTPUT)%.html : %.txt
	$(QUIET_ASCIIDOC)$(RM) $@+ $@ && \
	$(ASCIIDOC) -b $(ASCIIDOC_HTML) -d manpage \
		$(ASCIIDOC_EXTRA) -alibccli_version=$(LIBCCLI_VERSION) -o $@+ $< && \
	mv $@+ $@

$(MAN1_HTML): $(OUTPUT)%.html : %.txt.1
	$(QUIET_ASCIIDOC)$(RM) $@+ $@ && \
	$(ASCIIDOC) -b $(ASCIIDOC_HTML) -d manpage \
		$(ASCIIDOC_EXTRA) -alibccli_version=$(LIBCCLI_VERSION) -o $@+ $< && \
	mv $@+ $@
