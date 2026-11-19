CXX ?= c++
PKG_CONFIG ?= pkg-config
WINDRES ?= windres
RCC ?= $(shell $(PKG_CONFIG) --variable=libexecdir Qt6Core)/rcc$(if $(filter Windows_NT,$(OS)),.exe)

CPPFLAGS += $(shell $(PKG_CONFIG) --cflags Qt6Widgets)
CXXFLAGS ?= -O1
CXXFLAGS += -std=c++17 -Wall -Wextra -Wpedantic -MMD -MP
LDFLAGS += -Wl,--strip-all
LDLIBS += $(shell $(PKG_CONFIG) --libs Qt6Widgets)

sources := main.cpp editor-controls.cpp workspace.cpp agents.cpp model-catalog.cpp chat.cpp discord-rpc.cpp
objects := $(sources:%.cpp=build/%.o) build/sava-assets.o
link_inputs := $(objects)

ifeq ($(OS),Windows_NT)
TARGET := build/sava.exe
CXXFLAGS += -finput-charset=UTF-8 -fexec-charset=UTF-8
LDFLAGS += -mwindows
LDLIBS += -ldwmapi
link_inputs += build/sava.res
else
TARGET := build/sava
CXXFLAGS += -fPIC
endif

.PHONY: all clean
all: $(TARGET)

$(TARGET): $(link_inputs)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

build:
	mkdir -p build

build/%.o: %.cpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

build/sava-assets.cpp: sava-assets.qrc sava.png | build
	$(RCC) -no-compress -name sava_assets $< -o $@

build/sava-assets.o: build/sava-assets.cpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

build/sava.res: sava.rc sava.ico sava.manifest | build
	$(WINDRES) $< -O coff -o $@

clean:
	rm -rf build

-include $(objects:.o=.d)
