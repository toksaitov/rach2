/* dredshim: a libd3d12.so that forwards to WSL's and turns on D3D12's
 * device-removed extended data (DRED) for Mesa's d3d12 driver, names every
 * resource and heap it creates, and on device removal writes the page
 * fault's address, the allocations at it and the GPU's last operations. */
#define COBJMACROS
#define CINTERFACE
#define INITGUID
#include <wsl/winadapter.h>
#include <directx/d3d12.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define EXPORT __attribute__((visibility("default")))
#define REAL "/usr/lib/wsl/lib/libd3d12.so"

static void *real;
static FILE *out;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned long seq;

static void
say(const char *fmt, ...)
{
	struct timespec ts;
	struct tm tm;
	va_list ap;

	clock_gettime(CLOCK_REALTIME, &ts);
	gmtime_r(&ts.tv_sec, &tm);
	pthread_mutex_lock(&lock);
	fprintf(out, "%02d:%02d:%02d.%03ldZ dred[%d]: ", tm.tm_hour, tm.tm_min,
		tm.tm_sec, ts.tv_nsec / 1000000, (int)getpid());
	va_start(ap, fmt);
	vfprintf(out, fmt, ap);
	va_end(ap);
	fputc('\n', out);
	fflush(out);
	pthread_mutex_unlock(&lock);
}

void *
sym(const char *name)
{
	if (!real) {
		const char *p = getenv("DRED_SHIM_OUT");

		out = p ? fopen(p, "a") : NULL;
		if (!out)
			out = stderr;
		real = dlopen(REAL, RTLD_NOW | RTLD_LOCAL);
		if (!real) {
			fprintf(stderr, "dredshim: %s\n", dlerror());
			abort();
		}
	}
	return dlsym(real, name);
}



static void
name_object(ID3D12Object *o, const char *kind, const char *detail)
{
	char buf[160];
	unsigned long n;

	pthread_mutex_lock(&lock);
	n = ++seq;
	pthread_mutex_unlock(&lock);
	snprintf(buf, sizeof buf, "%s#%lu %s", kind, n, detail);
	ID3D12Object_SetPrivateData(o, &WKPDID_D3DDebugObjectName,
				    (UINT)strlen(buf), buf);
}

static void
name_resource(void **ppv, const D3D12_RESOURCE_DESC *d, const char *how)
{
	char buf[128];

	if (!ppv || !*ppv || !d)
		return;
	snprintf(buf, sizeof buf, "%s dim%d %llux%ux%u mips%u fmt%d flags%#x",
		 how, (int)d->Dimension, (unsigned long long)d->Width,
		 d->Height, d->DepthOrArraySize, d->MipLevels, (int)d->Format,
		 (unsigned)d->Flags);
	/* Every interface the device returns derives from ID3D12Object. */
	name_object(*ppv, "res", buf);
}

static HRESULT (*o_ccr)(ID3D12Device10 *, const D3D12_HEAP_PROPERTIES *,
			D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC *,
			D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE *,
			REFIID, void **);
static HRESULT
h_ccr(ID3D12Device10 *t, const D3D12_HEAP_PROPERTIES *hp, D3D12_HEAP_FLAGS hf,
      const D3D12_RESOURCE_DESC *d, D3D12_RESOURCE_STATES s,
      const D3D12_CLEAR_VALUE *cv, REFIID riid, void **ppv)
{
	HRESULT hr = o_ccr(t, hp, hf, d, s, cv, riid, ppv);

	if (SUCCEEDED(hr))
		name_resource(ppv, d, "committed");
	return hr;
}

static HRESULT (*o_cpr)(ID3D12Device10 *, ID3D12Heap *, UINT64,
			const D3D12_RESOURCE_DESC *, D3D12_RESOURCE_STATES,
			const D3D12_CLEAR_VALUE *, REFIID, void **);
static HRESULT
h_cpr(ID3D12Device10 *t, ID3D12Heap *h, UINT64 off,
      const D3D12_RESOURCE_DESC *d, D3D12_RESOURCE_STATES s,
      const D3D12_CLEAR_VALUE *cv, REFIID riid, void **ppv)
{
	HRESULT hr = o_cpr(t, h, off, d, s, cv, riid, ppv);

	if (SUCCEEDED(hr))
		name_resource(ppv, d, "placed");
	return hr;
}

static HRESULT (*o_ccr3)(ID3D12Device10 *, const D3D12_HEAP_PROPERTIES *,
			 D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC1 *,
			 D3D12_BARRIER_LAYOUT, const D3D12_CLEAR_VALUE *,
			 ID3D12ProtectedResourceSession *, UINT32,
			 const DXGI_FORMAT *, REFIID, void **);
static HRESULT
h_ccr3(ID3D12Device10 *t, const D3D12_HEAP_PROPERTIES *hp, D3D12_HEAP_FLAGS hf,
       const D3D12_RESOURCE_DESC1 *d, D3D12_BARRIER_LAYOUT l,
       const D3D12_CLEAR_VALUE *cv, ID3D12ProtectedResourceSession *ps,
       UINT32 nf, const DXGI_FORMAT *f, REFIID riid, void **ppv)
{
	HRESULT hr = o_ccr3(t, hp, hf, d, l, cv, ps, nf, f, riid, ppv);

	/* DESC1 begins with the fields of DESC. */
	if (SUCCEEDED(hr))
		name_resource(ppv, (const D3D12_RESOURCE_DESC *)d,
			      "committed3");
	return hr;
}

static HRESULT (*o_cpr2)(ID3D12Device10 *, ID3D12Heap *, UINT64,
			 const D3D12_RESOURCE_DESC1 *, D3D12_BARRIER_LAYOUT,
			 const D3D12_CLEAR_VALUE *, UINT32,
			 const DXGI_FORMAT *, REFIID, void **);
static HRESULT
h_cpr2(ID3D12Device10 *t, ID3D12Heap *h, UINT64 off,
       const D3D12_RESOURCE_DESC1 *d, D3D12_BARRIER_LAYOUT l,
       const D3D12_CLEAR_VALUE *cv, UINT32 nf, const DXGI_FORMAT *f,
       REFIID riid, void **ppv)
{
	HRESULT hr = o_cpr2(t, h, off, d, l, cv, nf, f, riid, ppv);

	if (SUCCEEDED(hr))
		name_resource(ppv, (const D3D12_RESOURCE_DESC *)d, "placed2");
	return hr;
}

static HRESULT (*o_ch)(ID3D12Device10 *, const D3D12_HEAP_DESC *, REFIID,
		       void **);
static HRESULT
h_ch(ID3D12Device10 *t, const D3D12_HEAP_DESC *d, REFIID riid, void **ppv)
{
	HRESULT hr = o_ch(t, d, riid, ppv);
	char buf[64];

	if (SUCCEEDED(hr) && ppv && *ppv) {
		snprintf(buf, sizeof buf, "%llu bytes flags%#x",
			 (unsigned long long)d->SizeInBytes, (unsigned)d->Flags);
		name_object(*ppv, "heap", buf);
	}
	return hr;
}

static HRESULT (*o_cqh)(ID3D12Device10 *, const D3D12_QUERY_HEAP_DESC *,
			REFIID, void **);
static HRESULT
h_cqh(ID3D12Device10 *t, const D3D12_QUERY_HEAP_DESC *d, REFIID riid,
      void **ppv)
{
	HRESULT hr = o_cqh(t, d, riid, ppv);
	char buf[64];

	if (SUCCEEDED(hr) && ppv && *ppv) {
		snprintf(buf, sizeof buf, "type%d count%u", (int)d->Type,
			 d->Count);
		name_object(*ppv, "queryheap", buf);
	}
	return hr;
}

/** Replace slot I of vtable VT with FN and keep the old entry in *OLD. */
static void
patch(void **vt, int i, void *fn, void **old)
{
	uintptr_t pg = (uintptr_t)&vt[i] & ~(uintptr_t)4095;

	if (vt[i] == fn)
		return;
	mprotect((void *)pg, 8192, PROT_READ | PROT_WRITE);
	*old = vt[i];
	vt[i] = fn;
}

static const char *
op_name(int op)
{
	switch (op) {
#include "ops.inc"
	}
	return "?";
}

static void
allocs(const char *what, const D3D12_DRED_ALLOCATION_NODE1 *a)
{
	int i = 0;

	for (; a && i < 200; a = a->pNext, i++)
		say("  %s alloc type%d obj %p name %s", what,
		    (int)a->AllocationType, (void *)a->pObject,
		    a->ObjectNameA ? a->ObjectNameA : "-");
}

static void
dump(ID3D12Device *dev, HRESULT why)
{
	ID3D12DeviceRemovedExtendedData2 *d2 = NULL;
	ID3D12DeviceRemovedExtendedData1 *d1 = NULL;
	D3D12_DRED_PAGE_FAULT_OUTPUT2 pf2;
	D3D12_DRED_PAGE_FAULT_OUTPUT1 pf1;
	D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 bc;
	const D3D12_AUTO_BREADCRUMB_NODE1 *n;
	HRESULT hr;
	int k = 0;

	say("device %p removed: reason %#x, %lu objects named", (void *)dev,
	    (unsigned)why, seq);
	if (SUCCEEDED(ID3D12Device_QueryInterface(dev,
			&IID_ID3D12DeviceRemovedExtendedData2, (void **)&d2))) {
		memset(&pf2, 0, sizeof pf2);
		hr = ID3D12DeviceRemovedExtendedData2_GetPageFaultAllocationOutput2(d2, &pf2);
		say("page fault output: hr %#x va %#llx flags %#x state %d",
		    (unsigned)hr, (unsigned long long)pf2.PageFaultVA,
		    (unsigned)pf2.PageFaultFlags,
		    (int)ID3D12DeviceRemovedExtendedData2_GetDeviceState(d2));
		if (SUCCEEDED(hr)) {
			allocs("existing", pf2.pHeadExistingAllocationNode);
			allocs("freed", pf2.pHeadRecentFreedAllocationNode);
		}
		d1 = (ID3D12DeviceRemovedExtendedData1 *)d2;
	} else if (SUCCEEDED(ID3D12Device_QueryInterface(dev,
			&IID_ID3D12DeviceRemovedExtendedData1, (void **)&d1))) {
		memset(&pf1, 0, sizeof pf1);
		hr = ID3D12DeviceRemovedExtendedData1_GetPageFaultAllocationOutput1(d1, &pf1);
		say("page fault output1: hr %#x va %#llx", (unsigned)hr,
		    (unsigned long long)pf1.PageFaultVA);
		if (SUCCEEDED(hr)) {
			allocs("existing", pf1.pHeadExistingAllocationNode);
			allocs("freed", pf1.pHeadRecentFreedAllocationNode);
		}
	} else {
		say("no ID3D12DeviceRemovedExtendedData1 interface");
		return;
	}
	memset(&bc, 0, sizeof bc);
	hr = ID3D12DeviceRemovedExtendedData1_GetAutoBreadcrumbsOutput1(d1, &bc);
	say("breadcrumbs: hr %#x", (unsigned)hr);
	if (FAILED(hr))
		return;
	for (n = bc.pHeadAutoBreadcrumbNode; n && k < 4096; n = n->pNext, k++) {
		UINT last = n->pLastBreadcrumbValue ? *n->pLastBreadcrumbValue : 0;
		UINT i, lo, hi;

		/* A list that ran whole is not where the fault was. */
		if (last >= n->BreadcrumbCount && !getenv("DRED_SHIM_ALL"))
			continue;
		say(" list %p queue %p (%s/%s) ops %u done %u contexts %u",
		    (void *)n->pCommandList, (void *)n->pCommandQueue,
		    n->pCommandListDebugNameA ? n->pCommandListDebugNameA : "-",
		    n->pCommandQueueDebugNameA ? n->pCommandQueueDebugNameA : "-",
		    n->BreadcrumbCount, last, n->BreadcrumbContextsCount);
		lo = last > 24 ? last - 24 : 0;
		hi = last + 8 < n->BreadcrumbCount ? last + 8 : n->BreadcrumbCount;
		for (i = lo; i < hi; i++)
			say("  %s op %u %s", i == last ? ">>" : "  ", i,
			    op_name((int)n->pCommandHistory[i]));
	}
	say("breadcrumb lists walked: %d", k);
}

static void *
watch(void *arg)
{
	ID3D12Device *dev = arg;
	struct timespec ts = { 0, 20 * 1000 * 1000 };
	HRESULT hr;
	int ticks = 0;

	while (SUCCEEDED(hr = ID3D12Device_GetDeviceRemovedReason(dev))) {
		nanosleep(&ts, NULL);
		/* A self-test: remove the device a second after it is made. */
		if (++ticks == 50 && getenv("DRED_SHIM_TEST_REMOVE")) {
			ID3D12Device5 *d5;

			if (SUCCEEDED(ID3D12Device_QueryInterface(dev,
					&IID_ID3D12Device5, (void **)&d5)))
				ID3D12Device5_RemoveDevice(d5);
		}
	}
	dump(dev, hr);
	return NULL;
}

static void
hook_device(void **ppv)
{
	ID3D12Device10 *d10 = NULL;
	ID3D12Device *dev = *ppv;
	pthread_t t;
	void **vt;

	if (SUCCEEDED(ID3D12Device_QueryInterface(dev, &IID_ID3D12Device10,
						  (void **)&d10))) {
		vt = *(void ***)d10;
		say("device %p (as Device10 %p): naming resources", (void *)dev,
		    (void *)d10);
		patch(vt, 27, (void *)h_ccr, (void **)&o_ccr);
		patch(vt, 28, (void *)h_ch, (void **)&o_ch);
		patch(vt, 29, (void *)h_cpr, (void **)&o_cpr);
		patch(vt, 39, (void *)h_cqh, (void **)&o_cqh);
		patch(vt, 76, (void *)h_ccr3, (void **)&o_ccr3);
		patch(vt, 77, (void *)h_cpr2, (void **)&o_cpr2);
		if (*(void ***)dev != vt)
			say("device's own vtable %p differs from Device10's %p",
			    *(void **)dev, (void *)vt);
		ID3D12Device10_Release(d10);
	}
	ID3D12Device_AddRef(dev);
	pthread_create(&t, NULL, watch, dev);
	pthread_detach(t);
}

static HRESULT (*o_fcd)(ID3D12DeviceFactory *, IUnknown *, D3D_FEATURE_LEVEL,
			REFIID, void **);
static HRESULT
h_fcd(ID3D12DeviceFactory *f, IUnknown *a, D3D_FEATURE_LEVEL fl, REFIID riid,
      void **ppv)
{
	HRESULT hr = o_fcd(f, a, fl, riid, ppv);

	say("factory CreateDevice level %#x: %#x", (unsigned)fl, (unsigned)hr);
	if (SUCCEEDED(hr) && ppv && *ppv)
		hook_device(ppv);
	return hr;
}

static void
enable_dred(ID3D12DeviceRemovedExtendedDataSettings1 *s, const char *how)
{
	ID3D12DeviceRemovedExtendedDataSettings1_SetAutoBreadcrumbsEnablement(s,
		D3D12_DRED_ENABLEMENT_FORCED_ON);
	ID3D12DeviceRemovedExtendedDataSettings1_SetPageFaultEnablement(s,
		D3D12_DRED_ENABLEMENT_FORCED_ON);
	ID3D12DeviceRemovedExtendedDataSettings1_SetBreadcrumbContextEnablement(s,
		D3D12_DRED_ENABLEMENT_FORCED_ON);
	ID3D12DeviceRemovedExtendedDataSettings1_Release(s);
	say("dred forced on through %s", how);
}

EXPORT HRESULT
D3D12GetInterface(REFCLSID clsid, REFIID riid, void **ppv)
{
	static HRESULT (*fn)(REFCLSID, REFIID, void **);
	ID3D12DeviceRemovedExtendedDataSettings1 *s = NULL;
	ID3D12DeviceFactory *f;
	HRESULT hr;

	if (!fn)
		fn = sym("D3D12GetInterface");
	hr = fn(clsid, riid, ppv);
	if (FAILED(hr) || !ppv || !*ppv ||
	    memcmp(clsid, &CLSID_D3D12DeviceFactory, sizeof(GUID)) != 0)
		return hr;
	f = *ppv;
	if (SUCCEEDED(ID3D12DeviceFactory_GetConfigurationInterface(f,
			&CLSID_D3D12DeviceRemovedExtendedData,
			&IID_ID3D12DeviceRemovedExtendedDataSettings1,
			(void **)&s)))
		enable_dred(s, "the device factory");
	else
		say("factory gives no DRED settings");
	patch(*(void ***)f, 9, (void *)h_fcd, (void **)&o_fcd);
	return hr;
}

EXPORT HRESULT
D3D12CreateDevice(IUnknown *a, D3D_FEATURE_LEVEL fl, REFIID riid, void **ppv)
{
	static HRESULT (*fn)(IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **);
	static HRESULT (*gdi)(REFIID, void **);
	ID3D12DeviceRemovedExtendedDataSettings1 *s = NULL;
	HRESULT hr;

	if (!fn) {
		fn = sym("D3D12CreateDevice");
		gdi = sym("D3D12GetDebugInterface");
	}
	if (SUCCEEDED(gdi(&IID_ID3D12DeviceRemovedExtendedDataSettings1,
			  (void **)&s)))
		enable_dred(s, "D3D12GetDebugInterface");
	hr = fn(a, fl, riid, ppv);
	say("D3D12CreateDevice level %#x: %#x", (unsigned)fl, (unsigned)hr);
	if (SUCCEEDED(hr) && ppv && *ppv)
		hook_device(ppv);
	return hr;
}
