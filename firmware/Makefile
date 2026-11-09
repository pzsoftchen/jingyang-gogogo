.PHONY: build test icons voices

build:
	./tools/build.sh all

test:
	./tools/test_firmware.sh

icons:
	python3 tools/gen_icons.py

voices:
	cd voice-pack && ./make_voice.sh
