# Private native Windows compiler preparation. No installer is executed.
SOURCE_CLANG_PREFIX ?= $(abspath build/host-tools/llvm-21.1.8)
SOURCE_CLANG_CACHE ?= $(abspath build/downloads/host-tools)

.PHONY: windows-clang
windows-clang:
	python3 -B tools/setup/prepare_clang.py "$(SOURCE_CLANG_PREFIX)" --cache "$(SOURCE_CLANG_CACHE)"
