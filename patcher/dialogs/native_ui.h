#pragma once
#include <array>
#include <functional>
#include <string>
#include <vector>
#include <windows.h>

// File Pilot 0.8.5's UI is its own immediate-mode tree, not Dear ImGui.
// These bindings are specific to the hash-gated picker executable.
class NativePickerUi {
  public:
    struct Callbacks {
        std::function<void()> accept, cancel;
        std::function<void(DWORD)> type;
    };
    void initialize(BYTE *image, HWND *window, Callbacks callbacks);
    void configure(const std::wstring &name, const std::wstring &ok,
                   const std::vector<std::wstring> &types, DWORD index, bool showReadOnly,
                   bool readOnly, bool save);
    int reservedHeight(int clientHeight) const;
    void prepareInput();
    BYTE *render(void(__fastcall *begin)(DWORD, UINT64, UINT64, UINT64), void(__fastcall *end)());
    void setApp(UINT64 app);
    void recordNameGeometry(BYTE *widget);
    bool message(UINT message, WPARAM w, LPARAM l);
    std::wstring name() const;
    bool setName(const std::wstring &name);
    void setOkLabel(const std::wstring &label);
    void setStatus(const std::wstring &text);
    void setType(DWORD index);
    DWORD type() const {
        return typeIndex;
    }
    bool readOnly() const {
        return readOnlyValue;
    }
    bool editing() const {
        return focus == Focus::Name;
    }
    void focusName(bool selectName = false);

  private:
    enum class Focus { Browser, Name, Type, ReadOnly, Accept, Cancel };
    struct View {
        const char *data;
        size_t length;
    };
    struct Signal {
        UINT64 id;
        BYTE button, clicks, modifiers, flags;
        short wheelX, wheelY;
    };
    struct Text {
        UINT64 allocator = 0;
        char *data = nullptr;
        size_t length = 0, capacity = 65536;
        unsigned short anchor = 0, cursor = 0, wordStart = 0, wordEnd = 0;
        UINT64 reserved[5]{};
    } text;
    std::array<char, 65536> buffer{};
    std::array<UINT64, 12> editor{};
    UINT64 font = 0, measureFont = 0;
    BYTE *image = nullptr;
    HWND *window = nullptr;
    Callbacks callbacks;
    Focus focus = Focus::Browser;
    DWORD typeIndex = 1, highlighted = 1;
    bool showReadOnly = false, readOnlyValue = false, dropdown = false;
    wchar_t highSurrogate = 0;
    std::string okLabel, statusText;
    std::vector<std::string> types;
    struct Snapshot {
        std::string bytes;
        unsigned short anchor, cursor;
    };
    std::vector<Snapshot> undo, redo;
    int width = 0, height = 0, footerHeight = 0, unit = 1;
    int menuFirst = 0;
    bool menuMouseMoved = false;
    DWORD nameClickTime = 0;
    POINT nameClickPoint{}, namePressPoint{};
    unsigned nameClickCount = 0, nameClickSelection = 0;
    bool nameMouseDown = false, nameSelectionPending = false;
    bool nameClickExtension = false, nameGeometryReady = false;
    int nameTextLeft = 0;
    double nameTextScroll = 0;
    UINT64 nameTextFont = 0;
    struct Metrics {
        int margin, gap, row, status, height;
    };
    RECT nameRect{}, typeRect{}, readOnlyRect{}, acceptRect{}, cancelRect{}, popupRect{};
    BYTE *context() const;
    BYTE *scope() const;
    Metrics metrics(int clientHeight) const;
    int textWidth(const std::string &value) const;
    BYTE *box(const char *id, int x, int y, int w, int h, DWORD theme, UINT64 flags = 0,
              bool rounded = false, bool centered = false, int padding = 0,
              size_t radiusOffset = 0x1888);
    void caption(BYTE *widget, const std::string &value);
    void label(const char *id, const std::string &text, RECT rectangle, DWORD theme = 7);
    Signal interact(BYTE *widget);
    bool clicked(const Signal &signal) const;
    void focusOutline(const char *id, BYTE *widget, int w, int h);
    void icon(const char *id, DWORD glyph, RECT rectangle, DWORD theme, bool selected = false);
    bool action(const char *id, const std::string &label, RECT rectangle, bool focused);
    void renderType(RECT rectangle);
    void renderReadOnly(RECT rectangle);
    void footer();
    void renderName();
    void renderDropdown();
    void selectFilename(bool extension = false);
    bool extensionAt(POINT point) const;
    size_t nameOffsetAt(int position) const;
    void placeNameCaret(const Signal &signal, BYTE *widget);
    void resetNameClicks();
    void applyNameClickSelection();
    void chooseType(DWORD index);
    void replace(const std::wstring &text, bool remember = true);
    void changed();
    void remember();
    void restore(bool backwards);
    void clipboard(bool cut);
    void paste();
    size_t previous(size_t position) const;
    size_t next(size_t position) const;
    void move(size_t position, bool extend);
    void cycle(bool backwards);
    static std::string encode(const std::wstring &text);
    static std::wstring decode(const char *text, size_t length);
};
