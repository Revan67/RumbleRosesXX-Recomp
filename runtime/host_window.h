#pragma once
#include <cstdint>

void StartHostWindow(uint8_t* guestBase);
void UpdateHostFrontBuffer(uint32_t guestAddress, uint32_t width, uint32_t height);
