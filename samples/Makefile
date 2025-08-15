# SPDX-License-Identifier: MIT

include $(src)/scripts/utils.mk

TARGETS :=
TARGETS += read-file
TARGETS += cli-history
TARGETS += clish

sdir := $(obj)/bin

TARGETS := $(patsubst %,$(sdir)/%,$(TARGETS))

all: $(TARGETS)

$(sdir):
	@mkdir -p $(sdir)

$(TARGETS): $(sdir) $(LIBCCLI_STATIC)

$(sdir)/%: $(bdir)/%.o
	$(call do_sample_build,$@,$<)

$(bdir)/%.o: $(bdir)/%.c
	$(Q)$(CC) -o $@ -c $< $(CFLAGS) $(INCLUDES)

clean:
	$(Q)$(call do_clean,$(sdir)/*)
