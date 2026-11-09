.PHONY: run seed reset check test

run:
	python3 server.py

seed:
	python3 tools/demo_seed.py

reset:
	./tools/reset.sh

check:
	python3 -m py_compile server.py card.py tools/demo_seed.py tests/test_server.py

test:
	python3 -m unittest discover -s tests -v
