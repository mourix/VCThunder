/* shaderc.c -- compile gpu_shader.hlsl to DXBC with Microsoft's D3DCompiler_47.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * Windows only; `make shaders` runs it. Writes <outdir>/<entry>.dxbc for every
 * entry in renderer/win32/gpu_shaders.def, at optimisation level 3, and
 * prints the compiler DLL's version.
 * tools/embed-dxbc.py turns the blobs into renderer/win32/gpu_shader_dxbc.h.
 *
 * The renderer ships the result instead of compiling at run time because a
 * d3dcompiler_47 is not always Microsoft's: Wine's and Proton's are a different
 * HLSL compiler, and it miscompiles this shader.
 */
#define COBJMACROS
#include <windows.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <stdlib.h>

static const struct { const char *entry, *target; } SHADERS[] = {
#define VGL_SHADER(entry, target) { #entry, target },
#include "gpu_shaders.def"
#undef VGL_SHADER
};
#define NSHADERS (int)(sizeof SHADERS / sizeof SHADERS[0])

/* The version resource's FileVersion string, as Explorer shows it: the numeric
 * VS_FIXEDFILEINFO of a system DLL reads 6.2 to this unmanifested tool. */
static int print_compiler_version(void)
{
    char path[MAX_PATH], key[64];
    HMODULE m = GetModuleHandleA("d3dcompiler_47.dll");
    DWORD handle, size;
    WORD *lang;
    char *info, *ver;
    UINT len;

    if (!m || !GetModuleFileNameA(m, path, sizeof path))
        return 0;
    size = GetFileVersionInfoSizeA(path, &handle);
    info = size ? malloc(size) : NULL;
    if (!info || !GetFileVersionInfoA(path, 0, size, info) ||
        !VerQueryValueA(info, "\\VarFileInfo\\Translation", (void **)&lang, &len) ||
        len < 4) {
        free(info);
        return 0;
    }
    snprintf(key, sizeof key, "\\StringFileInfo\\%04x%04x\\FileVersion",
             lang[0], lang[1]);
    if (!VerQueryValueA(info, key, (void **)&ver, &len) || !len) {
        free(info);
        return 0;
    }
    printf("d3dcompiler_47 %s\n", ver);
    free(info);
    return 1;
}

int main(int argc, char **argv)
{
    FILE *f;
    char *src;
    long n;
    int i;

    if (argc != 3) {
        fprintf(stderr, "usage: shaderc <gpu_shader.hlsl> <outdir>\n");
        return 2;
    }
    f = fopen(argv[1], "rb");
    if (!f || fseek(f, 0, SEEK_END) || (n = ftell(f)) <= 0 || fseek(f, 0, SEEK_SET)) {
        fprintf(stderr, "shaderc: cannot read %s\n", argv[1]);
        return 1;
    }
    src = malloc((size_t)n);
    if (!src || fread(src, 1, (size_t)n, f) != (size_t)n) {
        fprintf(stderr, "shaderc: cannot read %s\n", argv[1]);
        return 1;
    }
    fclose(f);

    for (i = 0; i < NSHADERS; i++) {
        ID3DBlob *code = NULL, *errors = NULL;
        char out[MAX_PATH];
        /* The source's repository path, whatever directory this runs in. */
        HRESULT hr = D3DCompile(src, (SIZE_T)n, "renderer/win32/gpu_shader.hlsl",
                                NULL, NULL, SHADERS[i].entry, SHADERS[i].target,
                                D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
        if (FAILED(hr)) {
            fprintf(stderr, "shaderc: %s/%s failed (0x%08lx): %s\n",
                    SHADERS[i].entry, SHADERS[i].target, (unsigned long)hr,
                    errors ? (const char *)ID3D10Blob_GetBufferPointer(errors) : "no detail");
            return 1;
        }
        snprintf(out, sizeof out, "%s/%s.dxbc", argv[2], SHADERS[i].entry);
        f = fopen(out, "wb");
        if (!f || fwrite(ID3D10Blob_GetBufferPointer(code), 1,
                         ID3D10Blob_GetBufferSize(code), f) != ID3D10Blob_GetBufferSize(code) ||
            fclose(f)) {
            fprintf(stderr, "shaderc: cannot write %s\n", out);
            return 1;
        }
        ID3D10Blob_Release(code);
        if (errors)
            ID3D10Blob_Release(errors);
    }
    free(src);
    if (!print_compiler_version()) {
        fprintf(stderr, "shaderc: cannot read d3dcompiler_47's version\n");
        return 1;
    }
    return 0;
}
