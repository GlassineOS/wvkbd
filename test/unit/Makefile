# Host tests of the input method's parts (no Wayland, no device): make -C test/unit
# (or make check at the top). GLASSINE_PREDICT=path adds a run against the real predictor.
TOP = ../..
B = build
CFLAGS = -std=gnu99 -Wall -Wextra -Wno-unused-parameter -g -O1 -I$(TOP)
CORE = $(TOP)/im.c $(TOP)/predict.c $(TOP)/strip.c $(TOP)/osk.c $(TOP)/hw.c $(TOP)/paper.c $(TOP)/pen.c
HDRS = $(wildcard $(TOP)/*.h)
TESTS = test_core test_predict test_paper test_pen

check: $(addprefix $(B)/, $(TESTS))
	@mkdir -p $(B)/tmp
	@fail=0; for t in $(TESTS); do echo "== $$t"; TMPDIR=$(CURDIR)/$(B)/tmp ./$(B)/$$t $(CURDIR) || fail=1; done; \
	rm -rf $(B)/tmp; exit $$fail

$(B)/%: %.c $(CORE) $(HDRS) t.h
	@mkdir -p $(B)
	$(CC) $(CFLAGS) -o $@ $< $(CORE) -lm

# a hang is found sooner in the test
$(B)/test_paper: CFLAGS += -DHW_HUNG_MS=1500

clean:
	rm -rf $(B)

.PHONY: check clean
