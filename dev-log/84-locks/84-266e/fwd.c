/* fwd: the exports of libd3d12.so that dredshim passes through unchanged. */
#include <stdint.h>
#define EXPORT __attribute__((visibility("default")))
void *sym(const char *name);

/* Forward an export with up to six integer arguments, as the SysV ABI
 * passes them in registers; none of these takes a float. */
#define FWD(name)                                                       \
	EXPORT long name(uintptr_t a, uintptr_t b, uintptr_t c,         \
			 uintptr_t d, uintptr_t e, uintptr_t f)          \
	{                                                               \
		static long (*fn)(uintptr_t, uintptr_t, uintptr_t,      \
				  uintptr_t, uintptr_t, uintptr_t);     \
		if (!fn)                                                \
			fn = sym(#name);                                \
		return fn(a, b, c, d, e, f);                            \
	}
FWD(D3D12CoreCreateLayeredDevice)
FWD(D3D12CoreGetLayeredDeviceSize)
FWD(D3D12CoreRegisterLayers)
FWD(D3D12CreateRootSignatureDeserializer)
FWD(D3D12CreateVersionedRootSignatureDeserializer)
FWD(D3D12EnableExperimentalFeatures)
FWD(D3D12GetDebugInterface)
FWD(D3D12SerializeRootSignature)
FWD(D3D12SerializeVersionedRootSignature)
