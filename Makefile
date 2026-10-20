CXXFLAGS = -shared -fPIC --no-gnu-unique -std=c++2b -O2 -Wall
PKGS = pixman-1 libdrm hyprland libinput libudev wayland-server xkbcommon

all: hypr-xshape.so

hypr-xshape.so: main.cpp
	$(CXX) $(CXXFLAGS) main.cpp -o $@ $(shell pkg-config --cflags $(PKGS)) -Iinclude -lxcb -lxcb-shape

clean:
	rm -f hypr-xshape.so

.PHONY: all clean
