CXX      = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -O2 `pkg-config --cflags gtk4`
LIBS     = `pkg-config --libs gtk4`
TARGET   = diskmon
SRC      = src/main.cpp
OBJ      = $(SRC:.cpp=.o)

PREFIX  = /usr/local
BINDIR  = $(PREFIX)/bin
APPDIR  = /usr/share/applications
ICONDIR = /usr/share/icons/hicolor/256x256/apps
SVGDIR  = /usr/share/icons/hicolor/scalable/apps

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CXX) -o $@ $^ $(LIBS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(TARGET) $(OBJ)

install: all
	@echo "Installing binary..."
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)

	@echo "Installing desktop entry..."
	install -d $(DESTDIR)$(APPDIR)
	install -m 644 diskmon.desktop $(DESTDIR)$(APPDIR)/diskmon.desktop

	@echo "Installing icons (global, so the window/taskbar/about can find them)..."
	install -d $(DESTDIR)$(ICONDIR)
	install -m 644 icons/diskmon.png $(DESTDIR)$(ICONDIR)/diskmon.png
	install -d $(DESTDIR)$(SVGDIR)
	install -m 644 icons/diskmon.svg $(DESTDIR)$(SVGDIR)/diskmon.svg

	@echo "Updating icon cache and desktop database..."
	-gtk-update-icon-cache -f -t /usr/share/icons/hicolor
	-update-desktop-database /usr/share/applications
	@echo "Installation complete."

uninstall:
	@echo "Removing binary..."
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET)

	@echo "Removing desktop entry..."
	rm -f $(DESTDIR)$(APPDIR)/diskmon.desktop

	@echo "Removing icons..."
	rm -f $(DESTDIR)$(ICONDIR)/diskmon.png
	rm -f $(DESTDIR)$(SVGDIR)/diskmon.svg

	@echo "Updating icon cache and desktop database..."
	-gtk-update-icon-cache -f -t /usr/share/icons/hicolor
	-update-desktop-database /usr/share/applications
	@echo "Uninstallation complete."

.PHONY: all clean install uninstall
