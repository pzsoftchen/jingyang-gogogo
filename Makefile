.PHONY: run seed reset check test build icons voices

run:
	$(MAKE) -C server run

seed:
	$(MAKE) -C server seed

reset:
	$(MAKE) -C server reset

check:
	$(MAKE) -C server check

test:
	$(MAKE) -C firmware test
	$(MAKE) -C server test

build:
	$(MAKE) -C firmware build

icons:
	$(MAKE) -C firmware icons

voices:
	$(MAKE) -C firmware voices
