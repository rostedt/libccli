# SPDX-License-Identifier: LGPL-2.1

include $(src)/scripts/utils.mk

OBJS =
OBJS += ccli.o
OBJS += line.o
OBJS += history.o
OBJS += complete.o

OBJS := $(OBJS:%.o=$(bdir)/%.o)
DEPS := $(OBJS:$(bdir)/%.o=$(bdir)/.%.d)

$(LIBCCLI_STATIC): $(OBJS)
	$(Q)$(call do_build_static_lib)

$(LIBCCLI_SHARED): $(OBJS)
	$(Q)$(call do_compile_shared_library,$(notdir $(LIBCCLI_SHARED_VERSION)))

$(LIBCCLI_SHARED_VERSION): $(LIBCCLI_SHARED)
	@ln -sf $(<F) $@

$(LIBCCLI_SHARED_SO): $(LIBCCLI_SHARED_VERSION)
	@ln -sf $(<F) $@

libccli.so: $(LIBCCLI_SHARED_SO)

$(bdir)/%.o: %.c
	$(Q)$(call do_fpic_compile)

$(DEPS): $(bdir)/.%.d: %.c
	$(Q)$(CC) -M -MT $(bdir)/$*.o $(CPPFLAGS) $(CFLAGS) $< > $@

$(OBJS): $(bdir)/%.o : $(bdir)/.%.d

$(OBJS): | $(bdir)
$(DEPS): | $(bdir)

clean:
	$(Q)$(call do_clean,$(OBJS) .*.d)

dep_includes := $(wildcard $(DEPS))

ifneq ($(dep_includes),)
  include $(dep_includes)
endif

.PHONY: $(LIBCCLI_SHARED_SO) $(LIBCCLI_STATIC)
