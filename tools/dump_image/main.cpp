// ng2 - Ninja Gaiden II static recompilation
//
// Writes the loaded (decrypted, decompressed) image of a XEX, without running
// it. The build feeds it to XenosRecomp, which finds the shaders in it.
//
// usage: ng2_dump_image <game_dir> <xex file name> <output file>

#include <cstdio>
#include <string>

#include <rex/kernel/init.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/user_module.h>
#include <rex/system/xex_module.h>
#include <rex/system/xmemory.h>

using rex::X_STATUS;

int main(int argc, char** argv) {
  if (argc != 4) {
    std::fprintf(stderr, "usage: %s <game_dir> <xex file name> <output file>\n", argv[0]);
    return 2;
  }

  auto runtime = std::make_unique<rex::Runtime>(argv[1]);
  auto status = runtime->Setup(rex::RuntimeConfig{
      .kernel_init = rex::kernel::InitializeKernel,
      .tool_mode = true,
  });
  if (status != X_STATUS_SUCCESS) {
    std::fprintf(stderr, "runtime setup failed: %08X\n", status);
    return 1;
  }

  status = runtime->LoadXexImage(std::string("game:\\") + argv[2]);
  if (status != X_STATUS_SUCCESS) {
    std::fprintf(stderr, "cannot load %s: %08X\n", argv[2], status);
    return 1;
  }

  auto* xex = runtime->kernel_state()->GetExecutableModule()->xex_module();
  uint32_t base = xex->base_address();
  uint32_t size = xex->image_size();
  const uint8_t* image = runtime->memory()->TranslateVirtual<const uint8_t*>(base);

  FILE* f = std::fopen(argv[3], "wb");
  if (!f || std::fwrite(image, 1, size, f) != size) {
    std::fprintf(stderr, "cannot write %s\n", argv[3]);
    return 1;
  }
  std::fclose(f);
  std::printf("%s: image %08X-%08X written to %s\n", argv[2], base, base + size, argv[3]);
  return 0;
}
