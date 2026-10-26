# llvm21.mk: the pinned official LLVM 21.1.8 release for Windows, which
# mk/host-clang.mk prepares as the native source checker. The installer's
# detached signature was verified against LLVM's published release keys,
# LLVM21_KEY_SOURCE with its SHA-256, and was made by LLVM21_SIGNER.
LLVM21_VERSION = 21.1.8
LLVM21_SIGNER = B6C8F98282B944E3B0D5C2530FC3042E345AD05D
LLVM21_KEY_SOURCE = https://releases.llvm.org/release-keys.asc \
    f38bd19b7f2d22cafa755841e6ce9375b9a2e6d41f7b974e4953d6b084839001

# Each download as FILE URL SHA256: the installer, its signature, and the
# LLVM and Clang licenses, which the prefix keeps beside the compiler.
LLVM21_RELEASE = https://github.com/llvm/llvm-project/releases/download/llvmorg-$(LLVM21_VERSION)
LLVM21_SOURCE = https://raw.githubusercontent.com/llvm/llvm-project/llvmorg-$(LLVM21_VERSION)
LLVM21_PACKAGE = LLVM-21.1.8-win64.exe $(LLVM21_RELEASE)/LLVM-21.1.8-win64.exe \
    7a5386c26497db1691f320121e5b113364dd0274b98e55f15f4dbc00c0450113
LLVM21_SIGNATURE = LLVM-21.1.8-win64.exe.sig $(LLVM21_RELEASE)/LLVM-21.1.8-win64.exe.sig \
    0efb8c5ee76a8ed0645d5bec7beb0faf5cf39b2dd590f8b4b99b900f583df92c
LLVM21_LICENSES = \
    llvm21-LICENSE.TXT $(LLVM21_SOURCE)/llvm/LICENSE.TXT \
    8d85c1057d742e597985c7d4e6320b015a9139385cff4cbae06ffc0ebe89afee \
    clang21-LICENSE.TXT $(LLVM21_SOURCE)/clang/LICENSE.TXT \
    ebcd9bbf783a73d05c53ba4d586b8d5813dcdf3bbec50265860ccc885e606f47
