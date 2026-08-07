SKIA=/home/cubicool/dev/skia

SKIA_LIBS = \
	-Wl,--start-group \
		${SKIA}/out/Release/libsvg.a \
		${SKIA}/out/Release/libskia.a \
		${SKIA}/out/Release/libskresources.a \
		${SKIA}/out/Release/libskshaper.a \
		${SKIA}/out/Release/libskunicode_core.a \
		${SKIA}/out/Release/libskunicode_icu.a \
		${SKIA}/out/Release/libharfbuzz.a \
		${SKIA}/out/Release/libicu.a \
	-Wl,--end-group

SKIA_FLAGS = -I${SKIA} -L${SKIA}/out/Release -std=c++20 -O2

all: slughorn-benchmark-skia slughorn-benchmark-cairo

slughorn-benchmark-skia: slughorn-benchmark-skia.cpp
	g++ $(<) ${SKIA_FLAGS} ${SKIA_LIBS} \
		$(shell pkg-config --libs fontconfig freetype2) \
		-lGL -lX11 -lpthread -ldl \
		-o $(@)

slughorn-benchmark-cairo: slughorn-benchmark-cairo.cpp
	g++ $(<) $(shell pkg-config --cflags --libs cairo librsvg-2.0) -o $(@)
