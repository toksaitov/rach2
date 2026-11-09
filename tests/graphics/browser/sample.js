/* Read a submitted sample's center pixel without changing its shaders. */
window.sample = {frames: 0};
window.addEventListener('error', e => { sample.error = String(e.error || e.message); });
window.addEventListener('unhandledrejection', e => { sample.error = String(e.reason); });
{
 const request = GPU.prototype.requestAdapter;
 GPU.prototype.requestAdapter = function(options) {
  return request.call(this, {...options, forceFallbackAdapter: true});
 };
 const configure = GPUCanvasContext.prototype.configure;
 let device, texture, format, busy = false;
 GPUCanvasContext.prototype.configure = function(options) {
  device = options.device; format = options.format;
  return configure.call(this, {...options, usage: (options.usage || GPUTextureUsage.RENDER_ATTACHMENT) | GPUTextureUsage.COPY_SRC});
 };
 const current = GPUCanvasContext.prototype.getCurrentTexture;
 GPUCanvasContext.prototype.getCurrentTexture = function() {
  texture = current.call(this); return texture;
 };
 const submit = GPUQueue.prototype.submit;
 GPUQueue.prototype.submit = function(commands) {
  submit.call(this, commands);
  if (!texture || busy || sample.frames >= 3) return;
  busy = true;
  const pixels = device.createBuffer({size: 256, usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ});
  const encoder = device.createCommandEncoder();
  encoder.copyTextureToBuffer({texture, origin: [Math.floor(texture.width / 2), Math.floor(texture.height / 2)]},
                             {buffer: pixels, bytesPerRow: 256}, [1, 1]);
  submit.call(this, [encoder.finish()]);
  pixels.mapAsync(GPUMapMode.READ).then(() => {
   let data = Array.from(new Uint8Array(pixels.getMappedRange()).slice(0, 4));
   if (format.startsWith('bgra')) data = [data[2], data[1], data[0], data[3]];
   sample.pixel = data; sample.frames++; pixels.unmap(); pixels.destroy(); busy = false;
  }).catch(e => {sample.error = String(e);});
 };
}
