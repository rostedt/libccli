# SPDX-License-Identifier: MIT

include $(src)/scripts/utils.mk

OBJS =
OBJS += ccli.o
OBJS += line.o
OBJS += history.o
OBJS += commands.o
OBJS += complete.o
OBJS += file.o
OBJS += utils.o
OBJS += cache.o
OBJS += alias.o

OBJS := $(OBJS:%.o=$(bdir)/%.o)
DEPS := $(OBJS:$(bdir)/%.o=$(bdir)/.%.d)

$(LIBCCLI_STATIC): $(bdir)/libccli.o
	$(Q)$(call do_build_static_lib)

$(LIBCCLI_SHARED): $(OBJS)
	$(Q)$(call do_compile_shared_library,$(notdir $(LIBCCLI_SHARED_VERSION)))

$(bdir)/libccli-global.o: $(OBJS)
	$(Q)ld -r -o $@ $^

find_local = \
	 $(shell for f in a `nm $^ | grep ' T ' | cut -d' ' -f3`;  do \
	  if [ "$${f#ccli_}" = "$$f" ]; then echo --localize-symbol $$f; fi; done)

$(bdir)/libccli.o: $(bdir)/libccli-global.o
	$(Q)objcopy $(call find_local) $< $@

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
