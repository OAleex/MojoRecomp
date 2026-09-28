#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

extern std::atomic<uint32_t> g_xexHeaderBase;
extern std::atomic<uint32_t> g_keTimeStampBundle;

uint32_t PublishXexHeaders(const uint8_t* xexFile, size_t xexFileSize);
void ResolveXexDataImports(const uint8_t* xexFile);
uint32_t XexHeaderField(uint32_t headerBase, uint32_t key);
uint32_t XexTitleId();
