#include "native_ui.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <string_view>
#include <imm.h>
#include <windowsx.h>

namespace {
template <typename T> T &at(BYTE *p, size_t offset) {
    return *reinterpret_cast<T *>(p + offset);
}
template <typename T> T fn(BYTE *base, size_t rva) {
    return reinterpret_cast<T>(base + rva);
}
// Native style stacks: count, then four payload words and a one-shot flag.
constexpr size_t stacks[] = {0x2e0,  0x348,  0x4d0,  0x598,  0x660,  0x6c8,  0x730,  0x8b8,
                             0x980,  0xa48,  0xb10,  0xb78,  0xbe0,  0xca8,  0xd70,  0xdd8,
                             0xe40,  0xea8,  0xf10,  0xf78,  0xfe0,  0x1048, 0x1110, 0x1178,
                             0x11e0, 0x1248, 0x12b0, 0x1318, 0x1380, 0x13e8, 0x1450, 0x14b8,
                             0x1520, 0x1588, 0x15f0, 0x1658, 0x16c0, 0x1728, 0x1790, 0x17f8};
struct Styles {
    BYTE *ui;
    // Native helpers consume one-shot entries and overwrite their slots.
    // Restoring only counts resurrects overwritten browser styles.
    std::array<BYTE, 0x1860 - 0x2e0> saved;
    explicit Styles(BYTE *p) : ui(p) {
        memcpy(saved.data(), ui + 0x2e0, saved.size());
    }
    ~Styles() {
        memcpy(ui + 0x2e0, saved.data(), saved.size());
    }
    void defaults() {
        for (auto stack : stacks) {
            at<DWORD>(ui, stack) = 1;
            at<DWORD>(ui, stack + 24) = 0;
        }
    }
    void pushOnce(size_t stack, DWORD a, DWORD b = 0, DWORD c = 0, DWORD d = 0) {
        push(stack, a, b, c, d, true);
    }
    void pushPersistent(size_t stack, DWORD a, DWORD b = 0, DWORD c = 0, DWORD d = 0) {
        push(stack, a, b, c, d, false);
    }
  private:
    void push(size_t stack, DWORD a, DWORD b, DWORD c, DWORD d, bool once) {
        auto &count = at<DWORD>(ui, stack);
        auto entry = ui + stack + 8 + size_t(count++) * 24;
        at<DWORD>(entry, 0) = a;
        at<DWORD>(entry, 4) = b;
        at<DWORD>(entry, 8) = c;
        at<DWORD>(entry, 12) = d;
        at<DWORD>(entry, 16) = once ? 1 : 0;
    }
};
constexpr DWORD One = 0x3f800000, Half = 0x3f000000;
constexpr UINT64 Absolute = (UINT64(1) << 43) | (UINT64(1) << 44);
constexpr UINT64 Fill = 1, Outline = 2, TextFill = 0x10, Hover = 0x2000, Press = 0x4000,
                 Selected = 0x10000, Immediate = 0x40000, Clip = 0x200000,
                 LeftMouse = 0x400000, RightMouse = 0x800000;
constexpr DWORD FieldRole = 7, HintRole = 12, SurfaceRole = 26, DropdownRole = 30,
                ArrowRole = 31, MenuCaptionRole = 32, ActionRole = 34,
                MenuRowRole = 68, ToggleRole = 78;
constexpr UINT64 NameId = 0x46705069636b4e6d;
constexpr UINT64 NameContentsId = [] {
    UINT64 id = NameId;
    for (char c : std::string_view("ScrollContents"))
        id = id * 33 + c;
    return id;
}();
DWORD bits(float value) {
    DWORD result;
    memcpy(&result, &value, sizeof(result));
    return result;
}
UINT64 pickerId(const char *key) {
    UINT64 id = 0x4650494c4f54;
    while (*key)
        id = id * 33 + static_cast<signed char>(*key++);
    return id;
}
struct Parent {
    BYTE *scope, *previous;
    Parent(BYTE *s, BYTE *p) : scope(s), previous(at<BYTE *>(s, 0x10)) {
        at<BYTE *>(scope, 0x10) = p;
    }
    ~Parent() {
        at<BYTE *>(scope, 0x10) = previous;
    }
};
struct ButtonOptions {
    const char *key;
    size_t keyLength;
    UINT64 id;
    DWORD icon, pad;
    const char *label;
    size_t labelLength;
    float alignX, alignY;
    UINT64 flags;
    float scale, rotation;
    int offsetX, offsetY;
    DWORD font, pad2;
    BYTE **widget, **iconWidget;
};
bool inside(RECT r, POINT p) {
    return PtInRect(&r, p) != FALSE;
}
} // namespace

std::string NativePickerUi::encode(const std::wstring &s) {
    int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s.data(), (int)s.size(), nullptr, 0,
                                nullptr, nullptr);
    std::string out(n, '\0');
    if (n)
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s.data(), (int)s.size(), out.data(), n,
                            nullptr, nullptr);
    return out;
}
std::wstring NativePickerUi::decode(const char *s, size_t n) {
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, (int)n, nullptr, 0);
    std::wstring out(count, L'\0');
    if (count)
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, (int)n, out.data(), count);
    return out;
}
void NativePickerUi::initialize(BYTE *base, HWND *hwnd, Callbacks cb) {
    image = base;
    window = hwnd;
    callbacks = std::move(cb);
    text.data = buffer.data();
}
void NativePickerUi::configure(const std::wstring &name, const std::wstring &ok,
                               const std::vector<std::wstring> &labels, DWORD index, bool show,
                               bool readOnly, bool save) {
    if (!setName(name))
        setStatus(L"The filename is too long for FilePilot's text editor.");
    setOkLabel(ok);
    types.clear();
    for (auto &label : labels)
        types.push_back(encode(label));
    if (types.empty())
        types.push_back("All files (*.*)");
    showReadOnly = show;
    readOnlyValue = readOnly;
    setType(index);
    if (save)
        focusName(true);
}
BYTE *NativePickerUi::context() const {
    return at<BYTE *>(image, 0x249278);
}
BYTE *NativePickerUi::scope() const {
    auto ui = context();
    auto count = at<DWORD>(ui, 0xec);
    return ui + (count ? 0xf0 + size_t(count - 1) * 0x20 : 0x1f0);
}
BYTE *NativePickerUi::box(const char *id, int x, int y, int w, int h, DWORD theme, UINT64 flags,
                          bool rounded, bool centered, int padding, size_t radiusOffset) {
    auto ui = context();
    Styles styles(ui);
    styles.pushOnce(0x348, theme);
    styles.pushOnce(0xb10, (DWORD)x);
    styles.pushOnce(0xb78, (DWORD)y);
    styles.pushOnce(0xbe0, 0, (DWORD)std::max(0, w), One);
    styles.pushOnce(0xca8, 0, (DWORD)std::max(0, h), One);
    styles.pushOnce(0x730, padding, padding, 0, 0);
    styles.pushOnce(0x8b8, 0, Half);
    styles.pushOnce(0x980, centered ? Half : 0, Half);
    styles.pushOnce(0x598, 0);
    DWORD radius = rounded ? at<DWORD>(ui, radiusOffset) : 0;
    styles.pushOnce(0x11e0, radius, radius, radius, radius);
    styles.pushOnce(0x1248, at<DWORD>(ui, 0x1894));
    auto widget =
        fn<BYTE *(__fastcall *)(UINT64, UINT64)>(image, 0x1ca3b0)(pickerId(id), Absolute | flags);
    auto previous = at<BYTE *>(scope(), 0x10);
    fn<void(__fastcall *)(BYTE *, BYTE *)>(image, 0x1ce850)(scope(), widget);
    at<BYTE *>(scope(), 0x10) = previous;
    at<BYTE *>(widget, 0x1d8) = nullptr;
    return widget;
}
void NativePickerUi::setApp(UINT64 app) {
    font = at<UINT64>((BYTE *)app, 0x458);
    measureFont = at<UINT64>((BYTE *)app, 0x4b0);
}
void NativePickerUi::recordNameGeometry(BYTE *widget) {
    if (!editing() || at<UINT64>(widget, 8) != NameContentsId)
        return;
    // Capture the geometry and animated scroll that actually painted the
    // filename. The next mouse message must hit that visible text position.
    nameTextLeft = at<int>(widget, 0x290) + at<int>(widget, 0x68);
    nameTextScroll = at<double>(widget, 0x208);
    nameGeometryReady = true;
}
NativePickerUi::Metrics NativePickerUi::metrics(int clientHeight) const {
    float fontSize = font ? at<float>((BYTE *)font, 0x1c) : 12.0f;
    auto scaled = [fontSize](float factor) {
        return std::max(1, (int)std::lround(fontSize * factor));
    };
    Metrics m{scaled(1), scaled(2.0f / 3), scaled(2.25f), scaled(1.5f), 0};
    m.height = 2 * m.margin + 2 * m.gap + 2 * m.row + m.status;
    // Compact the actual row geometry together with the reservation in short windows.
    int available = std::max(0, clientHeight - scaled(3));
    if (m.height > available) {
        int original = m.height;
        m.margin = MulDiv(m.margin, available, original);
        m.gap = MulDiv(m.gap, available, original);
        m.status = MulDiv(m.status, available, original);
        m.row = std::max(0, (available - 2 * m.margin - 2 * m.gap - m.status) / 2);
        m.height = 2 * m.margin + 2 * m.gap + 2 * m.row + m.status;
    }
    return m;
}
int NativePickerUi::reservedHeight(int clientHeight) const {
    return metrics(clientHeight).height;
}
int NativePickerUi::textWidth(const std::string &value) const {
    if (!measureFont || value.empty())
        return 0;
    View input{value.data(), value.size()}, measured{};
    auto size = fn<UINT64(__fastcall *)(UINT64, View *, int, View *)>(image, 0x1b9d10)(
        measureFont, &input, std::numeric_limits<int>::max(), &measured);
    return (int)(DWORD)size;
}
void NativePickerUi::prepareInput() {
    // A type menu overlays the file list. Raise the native input floor so the
    // rows behind it cannot claim the same mouse press first.
    auto ui = context();
    if (dropdown) {
        if (at<DWORD>(ui, 0xd8) > 0) {
            // An existing native popup owns the frame; yield to it rather than
            // putting a second interactive menu over its controls.
            dropdown = false;
            changed();
        } else
            at<DWORD>(ui, 0xd8) = 1;
    }
}
BYTE *NativePickerUi::render(void(__fastcall *begin)(DWORD, UINT64, UINT64, UINT64),
                            void(__fastcall *end)()) {
    auto ui = context();
    // Eight root slots are available. Measurement roots have skip=1; the
    // additional picker root uses skip=0 and survives until native rendering.
    DWORD footerIndex = at<DWORD>(ui, 0xec);
    if (footerIndex >= 8)
        return nullptr;
    Styles styles(ui);
    styles.defaults();
    // Footer controls stay on the browser input layer. Only the popup gets
    // layer 1, so its rows can overlap the filename without click-through.
    styles.pushPersistent(0x1790, 0);
    // Role zero is a green diagnostic/default colour, not a normal text style.
    styles.pushPersistent(0x348, FieldRole);
    styles.pushPersistent(0x980, 0, Half);
    width = at<int>(ui, 8);
    height = at<int>(ui, 12);
    float fontSize = font ? at<float>((BYTE *)font, 0x1c) : 12.0f;
    styles.pushPersistent(0x17f8, bits(fontSize));
    styles.pushPersistent(0x2e0, at<DWORD>(ui, 0x1934));
    // Use File Pilot's font/DPI scale, including its user font-size setting.
    unit = std::max(1, (int)(fontSize * 256.0f / 12.0f));
    footerHeight = reservedHeight(height);
    begin(0, 0, 0, 0);
    auto root = at<BYTE *>(scope(), 8);
    auto footerRoot = root;
    at<RECT>(root, 0x68) = {};
    at<int>(root, 0x48) = 0;
    footer();
    end();
    if (dropdown) {
        if (at<DWORD>(ui, 0xec) < 8) {
            styles.pushPersistent(0x1790, 1);
            // begin's first argument is measurement-skip, not a drawing layer.
            // A separate interactive root puts the menu above all footer nodes
            // despite the native renderer's reverse sibling traversal.
            begin(0, 0, 0, 0);
            root = at<BYTE *>(scope(), 8);
            at<RECT>(root, 0x68) = {};
            at<int>(root, 0x48) = 0;
            renderDropdown();
            end();
        } else {
            dropdown = false;
            changed();
        }
    }
    // Return the paint root without moving native slots: widgets cache their
    // root index. The runtime draws this root before the browser render pass,
    // including native context menus embedded inside the browser tree.
    return footerRoot;
}
void NativePickerUi::caption(BYTE *widget, const std::string &value) {
    // The native widget copies the StringView into its own frame arena.
    View copied{};
    fn<View *(__fastcall *)(View *, void *, const char *, size_t)>(image, 0x212000)(
        &copied, at<void *>(context(), 0), value.data(), value.size());
    at<View>(widget, 0x158) = copied;
}
void NativePickerUi::label(const char *id, const std::string &value, RECT r, DWORD theme) {
    caption(box(id, r.left, r.top, r.right - r.left, r.bottom - r.top, theme, TextFill | Clip),
            value);
}
NativePickerUi::Signal NativePickerUi::interact(BYTE *widget) {
    Signal event{};
    fn<Signal *(__fastcall *)(Signal *, BYTE *, DWORD)>(image, 0x1d9b20)(&event, widget, 0);
    return event;
}
bool NativePickerUi::clicked(const Signal &event) const {
    auto config = at<BYTE *>(image, 0x248f78);
    auto settings = at<BYTE *>(config, 0x48);
    BYTE click = at<int>(settings, 0x840) ? 2 : 4;
    return event.button == 0 && (event.flags & click) != 0;
}
void NativePickerUi::focusOutline(const char *id, BYTE *widget, int w, int h) {
    Parent parent(scope(), widget);
    std::string key = std::string(id) + "Focus";
    // Native field palette's active outline; it does not press the action body.
    box(key.c_str(), 0, 0, w, h, FieldRole, Outline | Selected | Immediate, true);
}
void NativePickerUi::icon(const char *id, DWORD glyph, RECT r, DWORD theme, bool selected) {
    auto ui = context();
    Styles styles(ui);
    // The native helper creates both a container and a glyph. Keep their role
    // alive until the complete subtree has been constructed.
    styles.pushPersistent(0x348, theme);
    styles.pushPersistent(0x4d0, selected ? (DWORD)(Selected | Immediate) : 0);
    styles.pushOnce(0xb10, r.left);
    styles.pushOnce(0xb78, r.top);
    styles.pushOnce(0xbe0, 0, r.right - r.left, One);
    styles.pushOnce(0xca8, 0, r.bottom - r.top, One);
    styles.pushOnce(0x730, 0, 0, 0, 0);
    styles.pushOnce(0x8b8, Half, Half);
    styles.pushPersistent(0x980, Half, Half);
    ButtonOptions options{};
    options.id = pickerId(id);
    options.icon = glyph;
    options.alignX = 0.5f;
    options.alignY = 0.5f;
    options.scale = 1.0f;
    options.flags = Absolute | Clip;
    options.font = at<DWORD>(ui, 0x1938);
    Signal event{};
    fn<Signal *(__fastcall *)(Signal *, ButtonOptions *)>(image, 0x1d90e0)(&event, &options);
}
bool NativePickerUi::action(const char *id, const std::string &value, RECT r, bool focused) {
    int w = r.right - r.left, h = r.bottom - r.top;
    // Same single-node body/caption recipe as FilePilot's ButtonOK/ButtonCancel.
    auto widget = box(id, r.left, r.top, w, h, ActionRole,
                      Fill | Outline | TextFill | Hover | Press | LeftMouse | Clip,
                      true, true);
    caption(widget, value);
    if (focused)
        focusOutline(id, widget, w, h);
    return clicked(interact(widget));
}
void NativePickerUi::renderType(RECT r) {
    int w = r.right - r.left, h = r.bottom - r.top, inset = MulDiv(8, unit, 256);
    auto widget = box("PickerType", r.left, r.top, w, h, DropdownRole,
                      Fill | Outline | Hover | Press | LeftMouse | Clip, true);
    {
        Parent parent(scope(), widget);
        label("PickerTypeValue", types[typeIndex - 1], {inset, 0, std::max(inset, w - h), h});
        icon("PickerTypeArrow", 0xe045, {std::max(0, w - h), 0, w, h}, ArrowRole);
    }
    if (focus == Focus::Type)
        focusOutline("PickerType", widget, w, h);
    if (clicked(interact(widget))) {
        focus = Focus::Type;
        dropdown = !dropdown;
        highlighted = typeIndex;
    }
}
void NativePickerUi::renderReadOnly(RECT r) {
    int w = r.right - r.left, h = r.bottom - r.top;
    auto widget = box("PickerReadOnly", r.left, r.top, w, h, FieldRole,
                      LeftMouse | Hover | Press | Clip);
    {
        Parent parent(scope(), widget);
        int size = std::min(h, MulDiv(16, unit, 256)), y = (h - size) / 2;
        // Native toggle in 1c7bd0: role 78, outlined box, checked glyph e01c.
        auto indicator = box("PickerReadOnlyBox", 0, y, size, size, ToggleRole,
                             Fill | Outline | Immediate | (readOnlyValue ? Selected : 0),
                             true, false, 0, 0x1884);
        if (readOnlyValue) {
            Parent checkParent(scope(), indicator);
            icon("PickerReadOnlyCheck", 0xe01c, {0, 0, size, size}, ToggleRole, true);
        }
        label("PickerReadOnlyCaption", "Open as read-only",
              {size + MulDiv(6, unit, 256), 0, w, h});
    }
    if (focus == Focus::ReadOnly)
        focusOutline("PickerReadOnly", widget, w, h);
    if (clicked(interact(widget))) {
        readOnlyValue = !readOnlyValue;
        focus = Focus::ReadOnly;
    }
}
void NativePickerUi::footer() {
    auto s = [this](int v) { return MulDiv(v, unit, 256); };
    int top = height - footerHeight;
    auto m = metrics(height);
    auto footer = box("PickerFooter", 0, top, width, footerHeight, SurfaceRole, Fill | Clip);
    Parent parent(scope(), footer);
    // Palette 29 is FilePilot's panel separator surface.
    box("PickerFooterSeparator", 0, 0, width, std::max(1, at<int>(context(), 0x1894)), 29, Fill);
    int left = m.margin, right = std::max(left, width - m.margin);
    int labelWidth = std::max(textWidth("File name"), textWidth("File type")) + m.gap;
    int field = std::min(right, left + labelWidth), row = m.row, gap = m.gap;
    int actionWidth = std::max(s(78), std::max(textWidth(okLabel), textWidth("Cancel")) + s(24));
    actionWidth = std::min(actionWidth, std::max(0, (right - field - 2 * gap) / 3));
    int nameY = m.margin, typeY = nameY + row + gap, statusY = typeY + row + gap;
    nameRect = {field, top + nameY, right, top + nameY + row};
    typeRect = {field, top + typeY, std::max(field, right - 2 * actionWidth - 2 * gap),
                top + typeY + row};
    acceptRect = {right - 2 * actionWidth - gap, top + typeY,
                  right - actionWidth - gap, top + typeY + row};
    cancelRect = {right - actionWidth, top + typeY, right, top + typeY + row};
    int readOnlyWidth = std::min(right - field, textWidth("Open as read-only") + s(24));
    readOnlyRect = {field, top + statusY, field + readOnlyWidth, top + statusY + m.status};
    label("PickerNameLabel", "File name", {left, nameY, field - gap, nameY + row});
    label("PickerTypeLabel", "File type", {left, typeY, field - gap, typeY + row});
    renderName();
    RECT local = typeRect;
    OffsetRect(&local, 0, -top);
    renderType(local);
    local = acceptRect;
    OffsetRect(&local, 0, -top);
    if (action("PickerAccept", okLabel, local, focus == Focus::Accept))
        callbacks.accept();
    local = cancelRect;
    OffsetRect(&local, 0, -top);
    if (action("PickerCancel", "Cancel", local, focus == Focus::Cancel))
        callbacks.cancel();
    if (showReadOnly) {
        local = readOnlyRect;
        OffsetRect(&local, 0, -top);
        renderReadOnly(local);
    }
    int statusLeft = showReadOnly ? field + readOnlyWidth + gap : field;
    label("PickerStatus", statusText, {std::min(statusLeft, right), statusY, right,
                                       statusY + m.status}, HintRole);
}
void NativePickerUi::renderName() {
    int w = nameRect.right - nameRect.left, h = nameRect.bottom - nameRect.top;
    int x = nameRect.left, y = nameRect.top - (height - footerHeight);
    int inset = MulDiv(8, unit, 256);
    if (!editing()) {
        nameGeometryReady = false;
        auto input = box("PickerNameFrame", x, y, w, h, FieldRole, Fill | Outline | Clip, true);
        Parent parent(scope(), input);
        label("PickerNameValue",
              text.length ? std::string(text.data, text.length) : "File name or path",
              {inset, 0, std::max(inset, w - inset), h}, text.length ? FieldRole : HintRole);
        return;
    }
    // Apply before building the selection rectangles, then again after native
    // mouse handling. While a multi-click is held, native drag updates must
    // not collapse our basename/extension/full-name range on a later frame.
    applyNameClickSelection();
    auto ui = context();
    auto graphics = at<BYTE *>(image, 0x249280);
    auto fonts = at<UINT64 *>(graphics, 0x3c8);
    nameTextFont = fonts[at<DWORD>(ui, 0x1934)];
    Styles styles(ui);
    styles.pushOnce(0xbe0, 0, w, One);
    styles.pushOnce(0xca8, 0, h, One);
    styles.pushOnce(0xb10, x);
    styles.pushOnce(0xb78, y);
    styles.pushOnce(0x730, inset, inset, 0, 0);
    // Native modal field: c10003, with positioning and clipping for this footer.
    UINT64 flags = Absolute | Fill | Outline | Selected | LeftMouse | RightMouse | Clip;
    styles.pushOnce(0x4d0, (DWORD)flags, (DWORD)(flags >> 32));
    styles.pushOnce(0x8b8, 0, Half);
    styles.pushPersistent(0x980, 0, Half);
    DWORD radius = at<DWORD>(ui, 0x1888);
    styles.pushOnce(0x11e0, radius, radius, radius, radius);
    styles.pushOnce(0x1248, at<DWORD>(ui, 0x1894));
    // Same seven-colour sequence used by File Pilot's own filter text editor.
    fn<void(__fastcall *)(DWORD, DWORD, DWORD, DWORD, DWORD, DWORD)>(image, 0x1d4c60)(7, 14, 13, 10,
                                                                                      11, 12);
    std::array<UINT64, 9> options{};
    const char *hint = "File name or path";
    options[0] = (UINT64)hint;
    options[1] = strlen(hint);
    options[2] = 0x800;
    reinterpret_cast<DWORD *>(options.data())[11] = at<DWORD>(ui, 0x1934);
    // Stable independent ID, shared by rendering and mouse selection.
    auto widget = fn<BYTE *(__fastcall *)(UINT64, UINT64 *, Text *, UINT64 *)>(image, 0x1dd2f0)(
        NameId, editor.data(), &text, options.data());
    Signal signal{};
    // Mouse hit testing and drag/double-click selection are native. The font
    // argument is supplied by the picker via the active app global below.
    if (font) {
        fn<Signal *(__fastcall *)(Signal *, UINT64, BYTE *, UINT64 *, Text *)>(image, 0x1d3600)(
            &signal, font, widget, editor.data(), &text);
        if (signal.button == 0 && (signal.flags & 3))
            focus = Focus::Name;
        placeNameCaret(signal, widget);
    }
    applyNameClickSelection();
    nameSelectionPending = false;
}
void NativePickerUi::renderDropdown() {
    int row = MulDiv(28, unit, 256), margin = MulDiv(4, unit, 256);
    int rows = std::min<int>((int)types.size(),
                             std::max<int>(1, (typeRect.top - MulDiv(42, unit, 256)) / row));
    // Keep a stable viewport when hover changes the highlighted row.
    if ((int)highlighted - 1 < menuFirst)
        menuFirst = (int)highlighted - 1;
    if ((int)highlighted > menuFirst + rows)
        menuFirst = (int)highlighted - rows;
    menuFirst = std::clamp(menuFirst, 0, (int)types.size() - rows);
    int first = menuFirst;
    int menuHeight = rows * row + 2 * margin;
    popupRect = {typeRect.left, typeRect.top - menuHeight - margin, typeRect.right,
                 typeRect.top - margin};
    auto popup = box("PickerTypesMenu", popupRect.left, popupRect.top,
                     popupRect.right - popupRect.left, menuHeight, SurfaceRole,
                     Fill | Outline | Clip, true);
    Parent parent(scope(), popup);
    DWORD hovered = 0;
    for (int i = 0; i < rows; ++i) {
        RECT r{margin, margin + i * row, popupRect.right - popupRect.left - margin,
               margin + (i + 1) * row};
        std::string id = "PickerTypeItem" + std::to_string(first + i);
        bool selected = highlighted == (DWORD)(first + i + 1);
        auto widget = box(id.c_str(), r.left, r.top, r.right - r.left, row, MenuRowRole,
                          Fill | Immediate | LeftMouse | Clip | (selected ? Selected : 0),
                          true, false, 0, 0x1884);
        {
            Parent rowParent(scope(), widget);
            std::string captionId = id + "Caption";
            label(captionId.c_str(), types[first + i],
                  {MulDiv(8, unit, 256), 0, std::max(0L, r.right - r.left - MulDiv(8, unit, 256)),
                   row}, MenuCaptionRole);
        }
        auto event = interact(widget);
        if (menuMouseMoved && (event.flags & 8))
            hovered = first + i + 1;
        if (clicked(event)) {
            chooseType(first + i + 1);
            dropdown = false;
        }
    }
    if (hovered && hovered != highlighted) {
        highlighted = hovered;
        changed();
    }
    menuMouseMoved = false;
}
std::wstring NativePickerUi::name() const {
    return decode(text.data, text.length);
}
bool NativePickerUi::setName(const std::wstring &name) {
    auto bytes = encode(name);
    if ((!name.empty() && bytes.empty()) || bytes.size() > 65532)
        return false;
    resetNameClicks();
    memcpy(buffer.data(), bytes.data(), bytes.size());
    buffer[bytes.size()] = 0;
    text.length = bytes.size();
    text.cursor = text.anchor = (unsigned short)bytes.size();
    undo.clear();
    redo.clear();
    changed();
    return true;
}
void NativePickerUi::setOkLabel(const std::wstring &value) {
    okLabel = encode(value);
}
void NativePickerUi::setStatus(const std::wstring &value) {
    statusText = encode(value);
}
void NativePickerUi::setType(DWORD index) {
    typeIndex = highlighted = std::max<DWORD>(1, std::min<DWORD>(index, (DWORD)types.size()));
}
void NativePickerUi::chooseType(DWORD index) {
    if (index == typeIndex)
        return;
    setType(index);
    callbacks.type(typeIndex);
}
void NativePickerUi::focusName(bool selectName) {
    resetNameClicks();
    focus = Focus::Name;
    dropdown = false;
    if (selectName)
        selectFilename();
    changed();
}
void NativePickerUi::selectFilename(bool extension) {
    // The editor stores UTF-8 byte offsets. ASCII delimiters cannot split
    // a codepoint; retain any directory prefix and the final extension.
    std::string_view value(text.data, text.length);
    auto separator = value.find_last_of("\\/");
    size_t start = separator == value.npos ? 0 : separator + 1;
    auto dot = value.find_last_of('.');
    size_t end = dot != value.npos && dot > start && dot + 1 < value.size()
                     ? dot : value.size();
    if (extension && end < value.size()) {
        start = end + 1;
        end = value.size();
    }
    text.anchor = text.wordStart = (unsigned short)start;
    text.cursor = text.wordEnd = (unsigned short)end;
}
bool NativePickerUi::extensionAt(POINT point) const {
    std::string_view value(text.data, text.length);
    auto separator = value.find_last_of("\\/");
    size_t start = separator == value.npos ? 0 : separator + 1;
    auto dot = value.find_last_of('.');
    if (dot == value.npos || dot <= start || dot + 1 >= value.size())
        return false;
    UINT64 fieldFont = nameTextFont ? nameTextFont : measureFont;
    if (!fieldFont)
        return false;
    auto prefixWidth = [&](size_t length) {
        View prefix{text.data, length}, measured{};
        return (int)(DWORD)fn<UINT64(__fastcall *)(UINT64, View *, int, View *)>(
            image, 0x1b9d10)(fieldFont, &prefix, std::numeric_limits<int>::max(), &measured);
    };
    int left = nameGeometryReady ? nameTextLeft : nameRect.left + MulDiv(8, unit, 256);
    int scroll = nameGeometryReady ? (int)std::lround(nameTextScroll) : 0;
    int position = point.x - left + scroll;
    // Include the dot's hit area, but select only the extension text. Empty
    // space after the rendered filename keeps the usual basename selection.
    return position >= prefixWidth(dot) && position < prefixWidth(value.size());
}
size_t NativePickerUi::nameOffsetAt(int position) const {
    // Match the font and cumulative advance used by the filename renderer,
    // choosing the closest painted caret boundary. UTF-8 codepoints must not
    // be split, and measuring each glyph once keeps long filenames linear.
    if (position <= 0)
        return 0;
    UINT64 fieldFont = nameTextFont ? nameTextFont : measureFont;
    if (!fieldFont)
        return text.cursor;
    float advance = 0;
    size_t offset = 0;
    while (offset < text.length) {
        DWORD codepoint = 0;
        auto bytes = fn<size_t(__fastcall *)(const char *, size_t, DWORD *)>(image, 0x1dec20)(
            text.data + offset, text.length - offset, &codepoint);
        if (!bytes || bytes > text.length - offset)
            break;
        auto glyph = fn<BYTE *(__fastcall *)(UINT64, DWORD)>(image, 0x10c6b0)(fieldFont, codepoint);
        float nextAdvance = advance + at<float>(glyph, 0x10);
        float midpoint = (std::ceil(advance) + std::ceil(nextAdvance)) / 2;
        if ((float)position < midpoint)
            return offset;
        advance = nextAdvance;
        offset += bytes;
    }
    return text.length;
}
void NativePickerUi::placeNameCaret(const Signal &signal, BYTE *widget) {
    // Keep native capture/hit testing and multi-click handling. 1d3600's
    // single-click/drag calculation uses widget+290, ignoring its left
    // padding, whereas the editor paints inside ScrollContents. Recompute
    // only that selection path using the visible text's actual origin.
    if (signal.button != 0 || signal.clicks != 1 || !(signal.flags & 1))
        return;
    auto ui = context();
    auto input = at<BYTE *>(ui, 0x10);
    int left = nameGeometryReady ? nameTextLeft
                                : at<int>(widget, 0x290) + at<int>(widget, 0x68);
    int fieldLeft = at<int>(widget, 0x290), fieldRight = at<int>(widget, 0x298);
    auto contents = fn<BYTE *(__fastcall *)(UINT64)>(image, 0x1cd210)(NameContentsId);
    int scroll = (int)std::lround(at<double>(contents, 0x208));
    int press = std::clamp(at<int>(ui, 0x80), fieldLeft, fieldRight) - left +
                at<int>(ui, 0x88);
    int current = std::clamp(at<int>(input, 0), fieldLeft, fieldRight) - left + scroll;
    auto anchor = (unsigned short)nameOffsetAt(press);
    auto cursor = (unsigned short)nameOffsetAt(current);
    if (text.anchor != anchor || text.cursor != cursor) {
        text.anchor = anchor;
        text.cursor = cursor;
        changed();
    }
}
void NativePickerUi::resetNameClicks() {
    nameClickCount = nameClickSelection = 0;
    nameMouseDown = nameSelectionPending = false;
    nameClickExtension = false;
}
void NativePickerUi::applyNameClickSelection() {
    if (nameClickSelection < 2 || (!nameMouseDown && !nameSelectionPending))
        return;
    auto anchor = text.anchor, cursor = text.cursor;
    if (nameClickSelection == 2)
        selectFilename(nameClickExtension);
    else {
        text.anchor = text.wordStart = 0;
        text.cursor = text.wordEnd = (unsigned short)text.length;
    }
    if (anchor != text.anchor || cursor != text.cursor)
        changed();
}
void NativePickerUi::changed() {
    if (!image)
        return;
    auto frequency = at<LONGLONG>(image, 0x249248), epoch = at<LONGLONG>(image, 0x249250);
    LARGE_INTEGER now{};
    if (frequency > 0 && QueryPerformanceCounter(&now))
        editor[1] = (now.QuadPart - epoch) * 1000000 / frequency;
    if (window && *window)
        InvalidateRect(*window, nullptr, FALSE);
}
void NativePickerUi::remember() {
    undo.push_back({std::string(text.data, text.length), text.anchor, text.cursor});
    if (undo.size() > 32)
        undo.erase(undo.begin());
    redo.clear();
}
void NativePickerUi::restore(bool backwards) {
    auto &source = backwards ? undo : redo;
    auto &target = backwards ? redo : undo;
    if (source.empty())
        return;
    target.push_back({std::string(text.data, text.length), text.anchor, text.cursor});
    auto state = std::move(source.back());
    source.pop_back();
    memcpy(buffer.data(), state.bytes.data(), state.bytes.size());
    text.length = state.bytes.size();
    buffer[text.length] = 0;
    text.anchor = state.anchor;
    text.cursor = state.cursor;
    changed();
}
size_t NativePickerUi::previous(size_t p) const {
    if (p)
        --p;
    while (p && (BYTE(buffer[p]) & 0xc0) == 0x80)
        --p;
    return p;
}
size_t NativePickerUi::next(size_t p) const {
    if (p < text.length)
        ++p;
    while (p < text.length && (BYTE(buffer[p]) & 0xc0) == 0x80)
        ++p;
    return p;
}
void NativePickerUi::move(size_t p, bool extend) {
    text.cursor = (unsigned short)p;
    if (!extend)
        text.anchor = text.cursor;
    changed();
}
void NativePickerUi::replace(const std::wstring &value, bool save) {
    auto bytes = encode(value);
    if (!value.empty() && bytes.empty())
        return;
    if (value.find_first_of(L"\r\n\t") != value.npos)
        return;
    size_t a = std::min(text.anchor, text.cursor), b = std::max(text.anchor, text.cursor);
    size_t count = text.length - (b - a) + bytes.size();
    if (count > 65532)
        return;
    if (save)
        remember();
    memmove(buffer.data() + a + bytes.size(), buffer.data() + b, text.length - b);
    memcpy(buffer.data() + a, bytes.data(), bytes.size());
    text.length = count;
    buffer[count] = 0;
    text.cursor = text.anchor = (unsigned short)(a + bytes.size());
    changed();
}
void NativePickerUi::clipboard(bool cut) {
    size_t a = std::min(text.anchor, text.cursor), b = std::max(text.anchor, text.cursor);
    if (a == b || !OpenClipboard(*window))
        return;
    auto value = decode(buffer.data() + a, b - a);
    auto memory = GlobalAlloc(GMEM_MOVEABLE, (value.size() + 1) * sizeof(wchar_t));
    bool copied = false;
    if (memory) {
        if (auto data = GlobalLock(memory)) {
            memcpy(data, value.c_str(), (value.size() + 1) * sizeof(wchar_t));
            GlobalUnlock(memory);
            if (EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, memory))
                copied = true;
        }
        if (!copied)
            GlobalFree(memory);
    }
    CloseClipboard();
    if (cut && copied)
        replace(L"");
}
void NativePickerUi::paste() {
    if (!OpenClipboard(*window))
        return;
    std::wstring value;
    if (auto memory = GetClipboardData(CF_UNICODETEXT)) {
        if (auto data = (wchar_t *)GlobalLock(memory)) {
            size_t capacity = GlobalSize(memory) / sizeof(wchar_t), length = 0;
            while (length < capacity && data[length])
                ++length;
            if (length < capacity)
                value.assign(data, length);
            GlobalUnlock(memory);
        }
    }
    CloseClipboard();
    if (!value.empty())
        replace(value);
}
void NativePickerUi::cycle(bool backwards) {
    std::vector<Focus> order = {Focus::Browser, Focus::Name, Focus::Type};
    if (showReadOnly)
        order.push_back(Focus::ReadOnly);
    order.push_back(Focus::Accept);
    order.push_back(Focus::Cancel);
    auto i = std::find(order.begin(), order.end(), focus) - order.begin();
    focus = order[(i + order.size() + (backwards ? -1 : 1)) % order.size()];
    if (focus == Focus::Name)
        focusName(true);
    dropdown = false;
    changed();
}
bool NativePickerUi::message(UINT msg, WPARAM w, LPARAM l) {
    // FilePilot's native rename/input popups raise the UI input floor above
    // the browser. Their editor owns Enter, Escape, Tab, characters and IME;
    // footer shortcuts must not submit/cancel the picker behind that popup.
    bool keyboard = (msg >= WM_KEYFIRST && msg <= WM_KEYLAST) ||
                    msg == WM_IME_STARTCOMPOSITION || msg == WM_IME_ENDCOMPOSITION ||
                    msg == WM_IME_COMPOSITION;
    if (keyboard && !dropdown) {
        auto ui = context();
        if (ui && at<DWORD>(ui, 0xd8) > 0)
            return false;
    }
    if (keyboard && msg != WM_KEYUP && msg != WM_SYSKEYUP) {
        // Complete a queued mouse selection before a character/key edits it.
        applyNameClickSelection();
        resetNameClicks();
    }
    if (msg == WM_CANCELMODE || msg == WM_KILLFOCUS ||
        (msg == WM_CAPTURECHANGED && nameMouseDown))
        resetNameClicks();
    if (msg == WM_MOUSEMOVE) {
        POINT point{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        if (nameMouseDown &&
            (std::abs(point.x - namePressPoint.x) > GetSystemMetrics(SM_CXDRAG) / 2 ||
             std::abs(point.y - namePressPoint.y) > GetSystemMetrics(SM_CYDRAG) / 2))
            resetNameClicks(); // Keep FilePilot's native drag selection.
        // A stationary pointer must not undo an arrow-key menu selection.
        menuMouseMoved = dropdown;
        if (dropdown)
            changed();
        return false;
    }
    if (msg == WM_LBUTTONUP && nameMouseDown) {
        nameMouseDown = false;
        nameSelectionPending = nameClickSelection >= 2;
        changed();
        return false;
    }
    if (msg == WM_MOUSEWHEEL && dropdown) {
        int step = GET_WHEEL_DELTA_WPARAM(w) > 0 ? -1 : 1;
        highlighted = (DWORD)std::clamp<int>((int)highlighted + step, 1, (int)types.size());
        changed();
        return true;
    }
    if (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK ||
        msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN) {
        POINT point{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        if (dropdown) {
            resetNameClicks();
            highSurrogate = 0;
            if (inside(popupRect, point)) {
                // Overlay rows take precedence over footer rectangles below.
                focus = Focus::Type;
                changed();
                return false;
            }
            // Dismissal consumes the press, like a native popup. It must not
            // activate a footer/browser control hidden behind the menu.
            dropdown = false;
            changed();
            return true;
        }
        auto ui = context();
        bool nativePopup = ui && at<DWORD>(ui, 0xd8) > 0;
        if (!nativePopup && inside(nameRect, point) &&
            (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK)) {
            // The native Signal click counter is tied to capture/hover IDs.
            // Count presses at the HWND instead, including CS_DBLCLKS windows.
            DWORD time = (DWORD)GetMessageTime();
            bool consecutive = nameClickCount > 0 && nameClickCount < 3 &&
                time - nameClickTime <= GetDoubleClickTime() &&
                std::abs(point.x - nameClickPoint.x) * 2 <= GetSystemMetrics(SM_CXDOUBLECLK) &&
                std::abs(point.y - nameClickPoint.y) * 2 <= GetSystemMetrics(SM_CYDOUBLECLK);
            nameClickCount = consecutive ? nameClickCount + 1 : 1;
            if (nameClickCount == 2)
                nameClickExtension = extensionAt(point);
            else if (nameClickCount == 1)
                nameClickExtension = false;
            nameClickTime = time;
            nameClickPoint = namePressPoint = point;
            nameMouseDown = true;
            nameClickSelection = nameClickCount;
            nameSelectionPending = nameClickCount >= 2;
        } else
            resetNameClicks();
        if (nativePopup)
            return false;
        if (inside(nameRect, point))
            focus = Focus::Name;
        else if (inside(typeRect, point))
            focus = Focus::Type;
        else if (inside(acceptRect, point))
            focus = Focus::Accept;
        else if (inside(cancelRect, point))
            focus = Focus::Cancel;
        else if (showReadOnly && inside(readOnlyRect, point))
            focus = Focus::ReadOnly;
        else if (!dropdown || !inside(popupRect, point)) {
            focus = Focus::Browser;
            dropdown = false;
        }
        highSurrogate = 0;
        changed();
        return false; // Native widgets receive mouse input.
    }
    if (msg == WM_KEYDOWN) {
        bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
        if (w == VK_TAB) {
            cycle(shift);
            return true;
        }
        if (w == VK_ESCAPE) {
            if (dropdown) {
                dropdown = false;
                changed();
            } else
                callbacks.cancel();
            return true;
        }
        if (ctrl && w == 'L') {
            focus = Focus::Browser;
            dropdown = false;
            return false;
        }
        if (w == VK_F4 && !ctrl) {
            focus = Focus::Type;
            dropdown = !dropdown;
            highlighted = typeIndex;
            changed();
            return true;
        }
        if (focus == Focus::Type) {
            if (w == VK_UP || w == VK_DOWN || w == VK_HOME || w == VK_END) {
                DWORD index = dropdown ? highlighted : typeIndex;
                index = w == VK_HOME  ? 1
                        : w == VK_END ? (DWORD)types.size()
                        : w == VK_UP  ? std::max<DWORD>(1, index - 1)
                                      : std::min<DWORD>((DWORD)types.size(), index + 1);
                if (dropdown)
                    highlighted = index;
                else
                    chooseType(index);
                changed();
                return true;
            }
            if (w == VK_RETURN || w == VK_SPACE) {
                if (dropdown) {
                    chooseType(highlighted);
                    dropdown = false;
                } else {
                    dropdown = true;
                    highlighted = typeIndex;
                }
                changed();
                return true;
            }
        }
        if (w == VK_RETURN && focus != Focus::Browser) {
            if (focus == Focus::Cancel)
                callbacks.cancel();
            else
                callbacks.accept();
            return true;
        }
        if (w == VK_SPACE && focus == Focus::ReadOnly) {
            readOnlyValue = !readOnlyValue;
            changed();
            return true;
        }
        if (w == VK_SPACE && (focus == Focus::Accept || focus == Focus::Cancel)) {
            if (focus == Focus::Accept)
                callbacks.accept();
            else
                callbacks.cancel();
            return true;
        }
        if (!editing())
            return focus != Focus::Browser;
        if (ctrl) {
            if (w == 'A') {
                text.anchor = 0;
                text.cursor = (unsigned short)text.length;
                changed();
                return true;
            }
            if (w == 'C' || w == 'X') {
                clipboard(w == 'X');
                return true;
            }
            if (w == 'V') {
                paste();
                return true;
            }
            if (w == 'Z' || w == 'Y') {
                restore(w == 'Z' && !shift);
                return true;
            }
        }
        if (w == VK_HOME || w == VK_END) {
            move(w == VK_HOME ? 0 : text.length, shift);
            return true;
        }
        if (w == VK_LEFT || w == VK_RIGHT) {
            size_t pos = text.cursor;
            if (!shift && text.anchor != text.cursor)
                pos = w == VK_LEFT ? std::min(text.anchor, text.cursor)
                                   : std::max(text.anchor, text.cursor);
            else {
                pos = w == VK_LEFT ? previous(pos) : next(pos);
                if (ctrl)
                    while (pos && pos < text.length && buffer[pos] != ' ' && buffer[pos] != '\\' &&
                           buffer[pos] != '/')
                        pos = w == VK_LEFT ? previous(pos) : next(pos);
            }
            move(pos, shift);
            return true;
        }
        if (w == VK_BACK || w == VK_DELETE) {
            if (text.anchor == text.cursor)
                text.anchor =
                    (unsigned short)(w == VK_BACK ? previous(text.cursor) : next(text.cursor));
            if (text.anchor != text.cursor)
                replace(L"");
            return true;
        }
        return true;
    }
    if ((msg == WM_CHAR || msg == WM_SYSCHAR) && focus != Focus::Browser) {
        if (!editing() || w < 32 || w == 127)
            return true;
        wchar_t c = (wchar_t)w;
        if (c >= 0xd800 && c <= 0xdbff) {
            highSurrogate = c;
            return true;
        }
        std::wstring value;
        if (c >= 0xdc00 && c <= 0xdfff) {
            if (!highSurrogate)
                return true;
            value += highSurrogate;
        }
        highSurrogate = 0;
        value += c;
        replace(value);
        return true;
    }
    if (msg == WM_IME_COMPOSITION && editing() && (l & GCS_RESULTSTR)) {
        if (auto ime = ImmGetContext(*window)) {
            LONG bytes = ImmGetCompositionStringW(ime, GCS_RESULTSTR, nullptr, 0);
            if (bytes > 0 && bytes < 131072) {
                std::wstring value(bytes / sizeof(wchar_t), L'\0');
                ImmGetCompositionStringW(ime, GCS_RESULTSTR, value.data(), bytes);
                replace(value);
            }
            ImmReleaseContext(*window, ime);
        }
        return true;
    }
    if (msg == WM_IME_STARTCOMPOSITION && editing()) {
        if (auto ime = ImmGetContext(*window)) {
            COMPOSITIONFORM form{};
            form.dwStyle = CFS_POINT;
            form.ptCurrentPos = {nameRect.left, nameRect.bottom};
            ImmSetCompositionWindow(ime, &form);
            ImmReleaseContext(*window, ime);
        }
    }
    return (msg == WM_KEYUP || msg == WM_SYSKEYUP) && focus != Focus::Browser;
}
