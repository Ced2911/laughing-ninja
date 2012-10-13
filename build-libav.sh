cd libav;
./configure --enable-cross-compile --cross-prefix=xenon- \
--enable-encoders --disable-altivec --disable-network --disable-shared --enable-static \
--disable-avconv --disable-avprobe --disable-avserver --disable-avfilter --disable-avdevice --disable-pthreads \
--arch=PPC --target-os=none --prefix=/usr/local/xenon/usr/ \
--extra-cflags="-DXENON -m32 -mno-altivec -fno-pic  -fno-pic -mpowerpc64 -mhard-float" \
--extra-ldflags="-DXENON -m32 -mno-altivec -fno-pic  -fno-pic -mpowerpc64 -mhard-float -L/usr/local/xenon/usr/lib -L/usr/local/xenon/xenon/lib/32 -lxenon -lm -T /usr/local/xenon/app.lds" 
# patch it !
# remove werror in makefile
# add #include "libxenon_miss/miss.h" in config.h
make -j4
