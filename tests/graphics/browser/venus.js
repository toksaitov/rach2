/* Record native and fallback requests separately; read back only on Venus. */
(async () => {
 if (!navigator.gpu) throw Error('WebGPU is unavailable');
 const result = {requests: {}, venus_passes: 0};
 for (const [name, options] of [['default', {}], ['fallback', {forceFallbackAdapter: true}]]) {
  const adapter = await navigator.gpu.requestAdapter(options);
  if (!adapter) {
   result.requests[name] = {adapter: null, readback: null};
   continue;
  }
  const info = adapter.info;
  const row = {adapter: {vendor: info.vendor, architecture: info.architecture,
   description: info.description, device: info.device, fallback: info.isFallbackAdapter},
   readback: null};
  result.requests[name] = row;
  if (/\bVenus\b/i.test(info.description) && !info.isFallbackAdapter) {
   row.readback = await gpu(adapter);
   if (row.readback.words !== 1024 || row.readback.errors !== 0 ||
       JSON.stringify(row.readback.pixel) !== '[255,128,0,255]')
    throw Error('Venus readback differs: ' + JSON.stringify(row));
   result.venus_passes++;
  }
 }
 return result;
})()
