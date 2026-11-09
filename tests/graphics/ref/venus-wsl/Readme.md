# Linux's Venus references on the testing PC

Debian's results through Venus on the testing PC's WSL, the reference that Rach2's results in the same QEMU profile are compared with. They come from the Debian reference guest of the graphics tests ([its Readme](../../Readme.md#the-reference-guest)) on the `venus` profile, under KVM with 8 CPUs and 8 GiB, over the host's lavapipe with the udmabuf module loaded. The guest's Venus driver is Mesa 26.1.6-1~bpo13+1's, as Rach2's root has it ([Venus](../../../../vendor/gnu/Readme.md#venus)), and every other driver Mesa 25.0.7-2+deb13u1's. The files are the tools' own output, unedited; text is named `.out`, since it keeps the tools' own characters.

## Results

* `vulkaninfo/`, taken on 2026-11-10: `summary.out` is `vulkaninfo --summary` with every driver, which lists Venus (`Virtio-GPU Venus (llvmpipe (LLVM 21.1.8, 256 bits))`, Vulkan 1.4.307, `Mesa 26.1.6-1~bpo13+1`) and the guest's own llvmpipe (`Mesa 25.0.7-2+deb13u1`); `loader.out` is the same with `VK_LOADER_DEBUG=driver`, the ICD files the loader found and the library each names; `venus.out` is the whole `vulkaninfo` of Venus alone, with `VK_DRIVER_FILES` naming its ICD: 164 device extensions, `VK_KHR_load_store_op_none`, `VK_KHR_maintenance5`, `VK_KHR_maintenance6` and `VK_EXT_map_memory_placed` among them, with `memoryMapPlaced` and `memoryUnmapReserve` true, `memoryMapRangePlaced` false and a placed-map alignment of 4 KiB; `install.out` lists the guest's ICD files, the Venus library and the two diversions of 25.0.7's Venus files; `record.out` is the run's record.

The run that took `vulkaninfo/`, from the top of the tree, with the outputs then moved into it and `icd.out` named `install.out`:

    sh tests/graphics/debian-ref/debian-ref.sh run -p venus -r record.out \
        -o summary.out -o loader.out -o venus.out -o icd.out \
        'vulkaninfo --summary > /tmp/summary.out 2>&1;
         VK_LOADER_DEBUG=driver vulkaninfo --summary > /tmp/loader.out 2>&1;
         VK_DRIVER_FILES=/usr/share/vulkan/icd.d/venus_icd.x86_64.json vulkaninfo > /tmp/venus.out 2>&1;
         { ls -l /usr/share/vulkan/icd.d/ /usr/lib/x86_64-linux-gnu/venus/;
           cat /usr/share/vulkan/icd.d/venus_icd.x86_64.json;
           dpkg-divert --list | grep mesa25; } > /tmp/icd.out 2>&1'

The dEQP-VK sample and vkcube's timings on Venus are stored here by the tasks that run them on Rach2, and the Direct3D results in `../d3d-venus-wsl/`, each taken on this guest.

## Updating

A new Venus pin in `vendor/gnu/debian/venus.list` takes every reference here again, on a guest prepared with it, as the Mesa qualification record in the development log lists. A new run replaces a directory whole, and its commit message says what was run and where.
