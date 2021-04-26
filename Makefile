.PHONY  = all release clean

#CC = clang-3.5
PROG = playlister

CFLAGS_EXTRA = -Og -g -D DEBUG
# -fsanitize=thread
# -fsanitize=address
# -fsanitize=memory
# -fsanitize=undefined

# use pkg-config for getting CFLAGS and LDLIBS
FFMPEG_LIBS=    libavdevice                        \
                libavformat                        \
                libavfilter                        \
                libavcodec                         \
                libswresample                      \
                libswscale                         \
                libavutil

CFLAGS  = $(shell pkg-config --cflags $(FFMPEG_LIBS))
CFLAGS += -std=gnu99 -D_FILE_OFFSET_BITS=64 -I. -Wall -Wextra -Wformat=2 -Wno-format-nonliteral -Wshadow -Wpointer-arith -Wcast-qual -Wmissing-prototypes -Wno-missing-braces -std=gnu99 -D_GNU_SOURCE
CFLAGS += -DUSE_BITSTREAM -DALLOW_PROXY
CFLAGS += -I /usr/include/libxml2
LDLIBS  = -lpthread 
LDLIBS += $(shell pkg-config --libs $(FFMPEG_LIBS))

SOURCES = $(wildcard *.c)
OBJECTS = $(SOURCES:.c=.o)
DEPEND  = $(OBJECTS:.o=.d)

INCLUDES = $(wildcard *.h)

-include $(DEPEND)

release: CFLAGS_EXTRA = -O3

all: $(PROG)

release: $(PROG)

%.o: %.c $(INCLUDES)
	$(CC) -c $(CFLAGS) $(CFLAGS_EXTRA) $(HEADERS) $<

$(PROG): Makefile $(OBJECTS) $(INCLUDES)
	$(CC) -o $(PROG) $(OBJECTS) -lpthread -lcurl -ljson-c $(LDLIBS)

# -ljson
# -pie
# -levent
# -lxml2

clean:
	rm -rf $(PROG) $(OBJECTS) $(DEPEND)
