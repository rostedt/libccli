# SPDX-License-Identifier: MIT

include $(src)/scripts/utils.mk

bdir:=$(obj)/utest

TARGETS = $(bdir)/ccli-utest

OBJS =
OBJS += ccli-utest.o
OBJS += libccli-utest.o

LIBS += -lcunit				\
	-ldl				\
	-lpthread			\
	$(obj)/lib/libccli.a

OBJS := $(OBJS:%.o=$(bdir)/%.o)
DEPS := $(OBJS:$(bdir)/%.o=$(bdir)/.%.d)

$(bdir):
	@mkdir -p $(bdir)

$(OBJS): | $(bdir)
$(DEPS): | $(bdir)

$(bdir)/ccli-utest: $(OBJS) $(LIBCCLI_STATIC)
	$(Q)$(do_app_build)

$(bdir)/%.o: %.c
	$(Q)$(call do_fpic_compile)

$(DEPS): $(bdir)/.%.d: %.c
	$(Q)$(CC) -M $(CPPFLAGS) $(CFLAGS) $< > $@
	$(Q)$(CC) -M -MT $(bdir)/$*.o $(CPPFLAGS) $(CFLAGS) $< > $@

$(OBJS): $(bdir)/%.o : $(bdir)/.%.d

dep_includes := $(wildcard $(DEPS))

test: $(TARGETS)

clean:
	$(Q)$(call do_clean,$(TARGETS) $(bdir)/*.o $(bdir)/.*.d)
