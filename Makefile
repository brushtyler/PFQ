# PFQ Makefile (c) 2011-2014 Nicola Bonelli <nicola@pfq.io>
#

MAKE_DIRECTORIES:= kernel user

.PHONY: all clean install

all: $(MAKE_DIRECTORIES)
clean: $(MAKE_DIRECTORIES)
install: $(MAKE_DIRECTORIES)

.PHONY: $(MAKE_DIRECTORIES)

$(MAKE_DIRECTORIES):
		@$(MAKE) --keep-going --directory=$@ $(MAKECMDGOALS)

.PHONY: $(MAKECMDGOALS)
		$(MAKECMDGOALS): $(MAKE_DIRECTORIES)

