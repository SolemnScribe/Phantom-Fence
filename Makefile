# Phantom Fence - cross-compile from Linux with MinGW-w64
# (Debian/Ubuntu: apt install g++-mingw-w64-x86-64)
# SPDX-License-Identifier: GPL-3.0-or-later

CXX      := x86_64-w64-mingw32-g++
WINDRES  := x86_64-w64-mingw32-windres
STRIP    := x86_64-w64-mingw32-strip
CXXFLAGS := -Os -Wall -Wextra -Wno-unknown-pragmas -municode -mwindows -static
LIBS     := -lgdi32 -ldwmapi -lshell32 -luser32 -ladvapi32

PhantomFence.exe: src/PhantomFence.cpp src/phantomfence_res.o
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LIBS)
	$(STRIP) -s $@

src/phantomfence_res.o: src/PhantomFence.rc src/phantomfence.ico
	$(WINDRES) -I src src/PhantomFence.rc src/phantomfence_res.o

clean:
	rm -f PhantomFence.exe src/phantomfence_res.o

.PHONY: clean
