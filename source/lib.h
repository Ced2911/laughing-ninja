#pragma once

#ifdef __cplusplus 
extern "C" {
}
#endif

void xenon_caps_init(const char * filename);
void xenon_caps_set_codec(int codecid);
void xenon_caps_set_bitrate(int bitrate);
void xenon_caps_set_hw_thread(int t);
void xenon_caps_start();
void xenon_caps_end();


#ifdef __cplusplus 
}
#endif
