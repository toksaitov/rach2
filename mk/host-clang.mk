# Private native Windows compiler preparation, in MSYS2, from the pins of
# tools/setup/llvm21.mk. No installer is executed: 7-Zip reads the files out
# of the installer's archive. The prefix's receipt, clang.sha256, has the
# SHA-256 of the pins, the compiler's version and the SHA-256 of every other
# file of the prefix; a prefix that differs from its receipt is refused.
include tools/setup/llvm21.mk
SOURCE_CLANG_PREFIX ?= $(abspath build/host-tools/llvm-$(LLVM21_VERSION))
SOURCE_CLANG_CACHE ?= $(abspath build/downloads/host-tools)
SOURCE_CLANG_7ZIP ?= /c/Program Files/7-Zip/7z.exe

.PHONY: windows-clang
windows-clang:
	@set -eu; prefix='$(SOURCE_CLANG_PREFIX)'; cache='$(SOURCE_CLANG_CACHE)'; \
	pins=$$(sha256sum -b tools/setup/llvm21.mk); pins=$${pins%% *}; \
	listing() { (cd "$$1" && find . -type f ! -name clang.sha256 | LC_ALL=C sort | \
	    while IFS= read -r f; do sha256sum -b "$${f#./}"; done); }; \
	if [ -e "$$prefix" ]; then \
	    if [ ! -f "$$prefix/clang.sha256" ] || [ -n "$$(find "$$prefix" -type l)" ] || \
	        [ "$$(sed -n 1p "$$prefix/clang.sha256")" != "pins $$pins" ] || \
	        [ "$$(sed 1,2d "$$prefix/clang.sha256")" != "$$(listing "$$prefix")" ]; then \
	        echo "clang preparation: the private Windows compiler changed; use a fresh prefix" >&2; \
	        exit 1; \
	    fi; \
	    echo "clang preparation: unchanged: $$prefix"; exit 0; \
	fi; \
	scratch=; trap '[ -z "$$scratch" ] || rm -rf "$$scratch"; rm -f "$$cache"/*.part' EXIT; \
	mkdir -p "$$cache" "$$(dirname "$$prefix")"; \
	check() { sum=$$(sha256sum -b "$$1"); [ "$${sum%% *}" = "$$2" ] || { \
	    echo "clang preparation: digest mismatch: $$1" >&2; exit 1; }; }; \
	set -- $(LLVM21_PACKAGE) $(LLVM21_SIGNATURE) $(LLVM21_LICENSES); \
	while [ $$# -gt 0 ]; do \
	    if [ ! -f "$$cache/$$1" ]; then \
	        echo "clang preparation: downloading $$2"; \
	        curl -sSfL --retry 3 -o "$$cache/$$1.part" "$$2"; \
	        check "$$cache/$$1.part" "$$3"; mv "$$cache/$$1.part" "$$cache/$$1"; \
	    fi; \
	    check "$$cache/$$1" "$$3"; shift 3; \
	done; \
	scratch=$$(mktemp -d "$$(dirname "$$prefix")/rach2-clang.XXXXXX"); stage=$$scratch/prefix; \
	mkdir "$$stage"; set -- $(LLVM21_PACKAGE); \
	"$(SOURCE_CLANG_7ZIP)" x -y -o"$$(cygpath -m "$$stage")" "$$(cygpath -m "$$cache/$$1")" \
	    bin/clang.exe 'lib/clang/21/include/*' -r > "$$scratch/7z.log" || { \
	    cat "$$scratch/7z.log" >&2; exit 1; }; \
	if [ ! -f "$$stage/bin/clang.exe" ] || [ ! -f "$$stage/lib/clang/21/include/stddef.h" ]; then \
	    echo "clang preparation: the pinned archive did not provide the compiler and headers" >&2; \
	    exit 1; \
	fi; \
	set -- $(LLVM21_LICENSES); \
	while [ $$# -gt 0 ]; do cp "$$cache/$$1" "$$stage/$$1"; shift 3; done; \
	system=$$(cygpath -u "$${SYSTEMROOT:-C:/Windows}")/System32; \
	version=$$(PATH=$$system "$$stage/bin/clang.exe" --version | sed -n 1p); \
	case $$version in "clang version $(LLVM21_VERSION)"*) ;; *) \
	    echo "clang preparation: the extracted compiler has a different version: $$version" >&2; \
	    exit 1 ;; \
	esac; \
	{ echo "pins $$pins"; echo "version $$version"; listing "$$stage"; } > "$$stage/clang.sha256"; \
	mv "$$stage" "$$prefix"; \
	echo "clang preparation: $$version"; \
	echo "clang preparation: receipt: $$prefix/clang.sha256"
