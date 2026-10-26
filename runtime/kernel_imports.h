#pragma once

void WaitForGuestThreads();
bool CloseGuestFile(unsigned int handle);
void RegisterGuestThreadClock(unsigned int address);
void UpdateGuestThreadClocks(unsigned char* base, unsigned int tickCount);
