run:
	python3 server.py

seed:
	python3 tools/demo_seed.py

reset:
	./tools/reset.sh

voices:
	cd voice-pack && ./make_voice.sh

check:
	python3 -m py_compile server.py card.py tools/demo_seed.py
	echo "syntax OK"
