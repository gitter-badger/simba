#
# @file Makefile
# @version 1.0
#
# @section License
# Copyright (C) 2014-2015, Erik Moqvist
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License as published by the Free Software Foundation; either
# version 2.1 of the License, or (at your option) any later version.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Lesser General Public License for more details.
#
# This file is part of the Simba project.
#

.PHONY: tags

BOARD ?= linux

# List of all tests to build and run
TESTS = $(addprefix tst/kernel/, bits \
                                 event \
                                 fifo \
                                 fs \
                                 log \
                                 prof \
                                 queue \
                                 sem \
                                 setting \
                                 shell \
                                 std \
                                 sys \
                                 thrd \
                                 timer)
TESTS += $(addprefix tst/slib/, crc hash_map)

ifeq ($(BOARD), linux)
    TESTS += $(addprefix tst/slib/, fat16)
endif

# List of all application to build
APPS = $(TESTS)

ifeq ($(ARCH),avr)
    APPS += $(addprefix tst/drivers/,adc cantp ds18b20 exti mcp2515 owi pin uart)
endif

all: $(APPS:%=%.all)

release: $(APPS:%=%.release)

clean: $(APPS:%=%.clean)

run: $(TESTS:%=%.run)

size: $(TESTS:%=%.size)

report:
	for t in $(TESTS) ; do $(MAKE) -C $(basename $$t) report ; echo ; done

test: run
	$(MAKE) report

coverage: $(TESTS:%=%.cov)
	lcov $(TESTS:%=-a %/coverage.info) -o coverage.info
	genhtml coverage.info
	@echo
	@echo "Run 'firefox index.html' to open the coverage report in a web browser."
	@echo

jenkins-coverage: $(TESTS:%=%.jc)

$(APPS:%=%.all):
	$(MAKE) -C $(basename $@) all

$(APPS:%=%.release):
	$(MAKE) -C $(basename $@) release

$(APPS:%=%.clean):
	$(MAKE) -C $(basename $@) clean

$(APPS:%=%.size):
	$(MAKE) -C $(basename $@) size

$(TESTS:%=%.run):
	$(MAKE) -C $(basename $@) run

$(TESTS:%=%.report):
	$(MAKE) -C $(basename $@) report

$(TESTS:%=%.cov):
	$(MAKE) -C $(basename $@) coverage

$(TESTS:%=%.jc):
	$(MAKE) -C $(basename $@) jenkins-coverage

tags:
	echo "Creating tags file .TAGS"
	etags -o .TAGS $$(git ls-files *.[hci] | xargs)

cloc:
	cloc $$(git ls-files | xargs)

pmccabe:
	pmccabe $$(git ls-files "*.[hic]") | sort -n

help:
	@echo "--------------------------------------------------------------------------------"
	@echo "  target                      description"
	@echo "--------------------------------------------------------------------------------"
	@echo "  all                         compile and link the application"
	@echo "  clean                       remove all generated files and folders"
	@echo "  run                         run the application"
	@echo "  report                      print test report"
	@echo "  test                        run + report"
	@echo "  release                     compile with NDEBUG=yes and NPROFILE=yes"
	@echo "  size                        print executable size information"
	@echo "  cloc                        print source code line statistics"
	@echo "  pmccabe                     print source code complexity statistics"
	@echo "  help                        show this help"
	@echo
