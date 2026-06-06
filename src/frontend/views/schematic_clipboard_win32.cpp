#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#pragma comment(lib, "gdiplus.lib")

#include "views/schematic_clipboard_win32.h"
#include "view_model/schematic_model.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

using namespace Gdiplus;

namespace {

constexpr double kPi = 3.14159265358979323846;

// ── 2D primitives + transforms ─────────────────────────────────────────────
struct V2 { double x, y; };

V2 rotateOff(V2 v, int rot) {
    rot = ((rot % 4) + 4) % 4;
    switch (rot) {
        case 0: return v;
        case 1: return { -v.y,  v.x };
        case 2: return { -v.x, -v.y };
        default:return {  v.y, -v.x };
    }
}

V2 sc(const SchematicComp& comp, double ox, double oy) {
    double mx = comp.mirrorX ? -ox : ox;
    V2 r = rotateOff({mx, oy}, comp.rotation);
    return { comp.pos.x + r.x, comp.pos.y + r.y };
}

V2 pinWorld(const SchematicComp& comp, const PinDef& pin) {
    return sc(comp, pin.offset.x, pin.offset.y);
}

// ── Painter wrapping a GDI+ Graphics with canvas→pixel projection ─────────
class GdiPainter {
public:
    GdiPainter(Graphics& g, double scale, double offX, double offY)
        : g_(g), scale_(scale), offX_(offX), offY_(offY) {}

    PointF map(V2 c) const {
        return PointF(static_cast<REAL>((c.x - offX_) * scale_),
                      static_cast<REAL>((c.y - offY_) * scale_));
    }

    void line(V2 a, V2 b, const Color& col, double thick) {
        Pen pen(col, static_cast<REAL>(thick * scale_));
        pen.SetStartCap(LineCapRound);
        pen.SetEndCap(LineCapRound);
        g_.DrawLine(&pen, map(a), map(b));
    }

    void dashLine(V2 a, V2 b, const Color& col, double thick,
                  double dashLen, double gapLen) {
        Pen pen(col, static_cast<REAL>(thick * scale_));
        REAL dashes[2] = { static_cast<REAL>(dashLen), static_cast<REAL>(gapLen) };
        pen.SetDashPattern(dashes, 2);
        g_.DrawLine(&pen, map(a), map(b));
    }

    void circleStroke(V2 c, double r, const Color& col, double thick) {
        Pen pen(col, static_cast<REAL>(thick * scale_));
        REAL rs = static_cast<REAL>(r * scale_);
        PointF p = map(c);
        g_.DrawEllipse(&pen, p.X - rs, p.Y - rs, 2 * rs, 2 * rs);
    }

    void circleFill(V2 c, double r, const Color& col) {
        SolidBrush br(col);
        REAL rs = static_cast<REAL>(r * scale_);
        PointF p = map(c);
        g_.FillEllipse(&br, p.X - rs, p.Y - rs, 2 * rs, 2 * rs);
    }

    void polylineStroke(const std::vector<V2>& pts, const Color& col,
                        double thick, bool closed) {
        if (pts.size() < 2) return;
        Pen pen(col, static_cast<REAL>(thick * scale_));
        pen.SetStartCap(LineCapRound);
        pen.SetEndCap(LineCapRound);
        pen.SetLineJoin(LineJoinRound);
        std::vector<PointF> mp;
        mp.reserve(pts.size());
        for (auto& pt : pts) mp.push_back(map(pt));
        if (closed) g_.DrawPolygon(&pen, mp.data(), static_cast<INT>(mp.size()));
        else        g_.DrawLines  (&pen, mp.data(), static_cast<INT>(mp.size()));
    }

    void polygonFill(const std::vector<V2>& pts, const Color& col) {
        if (pts.size() < 3) return;
        SolidBrush br(col);
        std::vector<PointF> mp;
        mp.reserve(pts.size());
        for (auto& pt : pts) mp.push_back(map(pt));
        g_.FillPolygon(&br, mp.data(), static_cast<INT>(mp.size()));
    }

    void text(V2 p, const std::string& s, const Color& col, double size,
              bool centerH = true, bool centerV = true) {
        if (s.empty()) return;
        // FontFamily::operator= is private, so we can't reassign. Construct
        // each candidate, then use the first one that's available; fall back
        // to GDI+'s built-in generic monospace if neither is installed.
        FontFamily consolas(L"Consolas");
        FontFamily courier(L"Courier New");
        const FontFamily* ff = consolas.IsAvailable() ? &consolas
                              : courier.IsAvailable() ? &courier
                              : FontFamily::GenericMonospace();
        Font font(ff, static_cast<REAL>(size * scale_), FontStyleRegular, UnitPixel);
        SolidBrush br(col);
        StringFormat fmt;
        fmt.SetAlignment(centerH ? StringAlignmentCenter : StringAlignmentNear);
        fmt.SetLineAlignment(centerV ? StringAlignmentCenter : StringAlignmentNear);
        // UTF-8 → UTF-16 for GDI+
        int needed = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
        std::wstring w(needed, L'\0');
        if (needed > 0)
            MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], needed);
        // null-terminator slips into needed; trim
        if (!w.empty() && w.back() == L'\0') w.pop_back();
        PointF mp = map(p);
        g_.DrawString(w.c_str(), -1, &font, mp, &fmt, &br);
    }

private:
    Graphics& g_;
    double scale_, offX_, offY_;
};

// ── Color palette (matches editor) ─────────────────────────────────────────
const Color kBgCol     (255, 20, 24, 32);
const Color kCompCol   (255, 110,155,220);
const Color kPinCol    (255, 80,200,120);
const Color kPin1Col   (255, 120,170,255);
const Color kJuncCol   (255, 80,210,120);
const Color kWireCol   (255, 200,200, 80);
const Color kCoreCol   (200, 150,180,230);
const Color kPolDotCol (240, 220,230,255);
const Color kNetCol    (255, 80,230,120);
const Color kLabelCol  (255, 220,220,220);

// ── Per-component drawing — mirrors the editor's drawCompSymbol ───────────
void drawComponent(GdiPainter& w, const SchematicComp& comp, const CompTypeDef& td,
                   const std::vector<int>& wireCountPerPin)
{
    auto p = [&](double x, double y) { return sc(comp, x, y); };
    const std::string& id = comp.typeId;
    const double t = 1.5;

    if (id == "R") {
        w.line(p(-40,0), p(-14,0), kCompCol, t);
        w.line(p(+14,0), p(+40,0), kCompCol, t);
        w.line(p(-14,-7), p(+14,-7), kCompCol, t);
        w.line(p(+14,-7), p(+14,+7), kCompCol, t);
        w.line(p(+14,+7), p(-14,+7), kCompCol, t);
        w.line(p(-14,+7), p(-14,-7), kCompCol, t);
    }
    else if (id == "C") {
        w.line(p(-20,0), p(-5,0),  kCompCol, t);
        w.line(p(+5,0),  p(+20,0), kCompCol, t);
        w.line(p(-5,-12), p(-5,+12), kCompCol, t * 1.5);
        w.line(p(+5,-12), p(+5,+12), kCompCol, t * 1.5);
    }
    else if (id == "L") {
        w.line(p(-40,0), p(-24,0), kCompCol, t);
        w.line(p(+24,0), p(+40,0), kCompCol, t);
        constexpr int N = 14;
        double bumpCx[4] = {-18.0, -6.0, +6.0, +18.0};
        for (int b = 0; b < 4; ++b) {
            std::vector<V2> arc;
            for (int k = 0; k <= N; ++k) {
                double a = kPi + (double)k / N * kPi;
                arc.push_back(p(bumpCx[b] + 6.0 * std::cos(a), 6.0 * std::sin(a)));
            }
            w.polylineStroke(arc, kCompCol, t, false);
        }
    }
    else if (id == "V_DC" || id == "V_SQUARE" || id == "V_SIN" || id == "V_STEP") {
        w.circleStroke({comp.pos.x, comp.pos.y}, 18.0, kCompCol, t);
        w.line(p(-40,0), p(-18,0), kCompCol, t);
        w.line(p(+18,0), p(+40,0), kCompCol, t);
        if (id == "V_DC") {
            w.line(p(-12,0), p(-6,0), kCompCol, t);
            w.line(p(-9,-3), p(-9,+3), kCompCol, t);
            w.line(p(+6,0), p(+12,0), kCompCol, t);
        } else if (id == "V_SIN") {
            constexpr int NS = 16;
            std::vector<V2> sn;
            for (int k = 0; k <= NS; ++k) {
                double tt = (double)k / NS;
                sn.push_back({comp.pos.x + (-12.0 + 24.0 * tt),
                              comp.pos.y + (-6.0 * std::sin(2.0 * kPi * tt))});
            }
            w.polylineStroke(sn, kCompCol, t, false);
        } else if (id == "V_SQUARE") {
            std::vector<V2> sq = {
                {comp.pos.x - 12, comp.pos.y - 5},
                {comp.pos.x -  4, comp.pos.y - 5},
                {comp.pos.x -  4, comp.pos.y + 5},
                {comp.pos.x +  4, comp.pos.y + 5},
                {comp.pos.x +  4, comp.pos.y - 5},
                {comp.pos.x + 12, comp.pos.y - 5}
            };
            w.polylineStroke(sq, kCompCol, t, false);
        } else {
            std::vector<V2> stp = {
                {comp.pos.x - 12, comp.pos.y + 5},
                {comp.pos.x -  4, comp.pos.y + 5},
                {comp.pos.x -  4, comp.pos.y - 5},
                {comp.pos.x + 12, comp.pos.y - 5}
            };
            w.polylineStroke(stp, kCompCol, t, false);
        }
    }
    else if (id == "I") {
        w.circleStroke({comp.pos.x, comp.pos.y}, 18.0, kCompCol, t);
        w.line(p(-40,0), p(-18,0), kCompCol, t);
        w.line(p(+18,0), p(+40,0), kCompCol, t);
        w.line(p(+8,0), p(-8,0), kCompCol, t);
        std::vector<V2> arr = { p(-8,0), p(-4,-4), p(-4,+4) };
        w.polygonFill(arr, kCompCol);
    }
    else if (id == "D") {
        w.line(p(-40,0), p(-12,0), kCompCol, t);
        w.line(p(+12,0), p(+40,0), kCompCol, t);
        std::vector<V2> tri = { p(-12,-10), p(+12,0), p(-12,+10) };
        w.polylineStroke(tri, kCompCol, t, true);
        w.line(p(+12,-12), p(+12,+12), kCompCol, t);
    }
    else if (id == "S") {
        w.line(p(-20,  0), p(-5,  0), kCompCol, t);
        w.line(p(-20, +20), p(-5, +20), kCompCol, t);
        w.line(p(-5, -15), p(-5, +20), kCompCol, t);
        w.line(p(0, -20), p(0, -10), kCompCol, t);
        w.line(p(0,  -5), p(0,  +5), kCompCol, t);
        w.line(p(0, +10), p(0, +20), kCompCol, t);
        w.line(p(0, -15), p(20, -15), kCompCol, t);
        w.line(p(0,   0), p(20,   0), kCompCol, t);
        w.line(p(0, +15), p(20, +15), kCompCol, t);
        w.line(p(20, -15), p(20, -25), kCompCol, t);
        w.line(p(20,   0), p(20, +25), kCompCol, t);
        w.line(p(20, -20), p(33, -20), kCompCol, t);
        w.line(p(20, +20), p(33, +20), kCompCol, t);
        w.line(p(33, -20), p(33,  -5), kCompCol, t);
        w.line(p(33,  +5), p(33, +20), kCompCol, t);
        w.line(p(20, -40), p(20, -25), kCompCol, t);
        w.line(p(20, +25), p(20, +40), kCompCol, t);
        std::vector<V2> bArr = { p(0,0), p(10,-5), p(10,+5) };
        w.polygonFill(bArr, kCompCol);
        // Body-diode: cathode bar + filled UP-pointing triangle (matches editor's
        // AddTriangleFilled). Tip is at (33,-5); base from (28,+5) to (38,+5).
        w.line(p(28, -5), p(38, -5), kCompCol, t);
        std::vector<V2> bdTri = { p(33, -5), p(28, +5), p(38, +5) };
        w.polygonFill(bdTri, kCompCol);
    }
    else if (id == "GND") {
        w.line(p(0,0), p(0,8), kPinCol, t);
        w.line(p(-5, 8),  p(+5, 8),  kPinCol, t);
        w.line(p(-3,12),  p(+3,12),  kPinCol, t);
        w.line(p(-1,16),  p(+1,16),  kPinCol, t);
    }
    else if (id == "JUNC") {
        // drawn separately as a dot below
    }
    else if (id == "NETLABEL") {
        std::vector<V2> flag = {
            p(-20,0), p(-16,-7), p(-4,-7), p(-4,+7), p(-16,+7)
        };
        w.polylineStroke(flag, kNetCol, t, true);
        if (!comp.paramValues.empty())
            w.text(p(+10,0), comp.paramValues[0], kNetCol, 11.0);
    }
    else if (id == "TX") {
        w.line(p(-5,-18), p(-5,+18), kCoreCol, t * 0.8);
        w.line(p(+5,-18), p(+5,+18), kCoreCol, t * 0.8);
        w.line(p(-40,-20), p(-22,-18), kCompCol, t);
        w.line(p(-40,+20), p(-22,+18), kCompCol, t);
        w.line(p(+40,-20), p(+22,-18), kCompCol, t);
        w.line(p(+40,+20), p(+22,+18), kCompCol, t);
        constexpr int NC = 12;
        double primY[3] = {-12.0, 0.0, +12.0};
        for (int b = 0; b < 3; ++b) {
            std::vector<V2> arcL, arcR;
            for (int k = 0; k <= NC; ++k) {
                double a = -kPi/2.0 - (double)k / NC * kPi;
                arcL.push_back(p(-22.0 + 6.0 * std::cos(a), primY[b] + 6.0 * std::sin(a)));
            }
            for (int k = 0; k <= NC; ++k) {
                double a = -kPi/2.0 + (double)k / NC * kPi;
                arcR.push_back(p(+22.0 + 6.0 * std::cos(a), primY[b] + 6.0 * std::sin(a)));
            }
            w.polylineStroke(arcL, kCompCol, t, false);
            w.polylineStroke(arcR, kCompCol, t, false);
        }
        w.circleFill(p(-28,-22), 3.5, kPolDotCol);
        w.circleFill(p(+28,-22), 3.5, kPolDotCol);
        if (comp.paramValues.size() >= 2) {
            std::string ratio = comp.paramValues[0] + ":" + comp.paramValues[1];
            w.text(p(0, 0), ratio, kCompCol, 10.0);
        }
    }
    else if (id == "TX_WIND") {
        w.line(p(0,-40), p(0,-24), kCompCol, t);
        w.line(p(0,+24), p(0,+40), kCompCol, t);
        constexpr int NSEG = 12;
        double bCy[4] = {-18, -6, +6, +18};
        for (int b = 0; b < 4; ++b) {
            std::vector<V2> arc;
            for (int k = 0; k <= NSEG; ++k) {
                double a = -kPi/2.0 + (double)k / NSEG * kPi;
                arc.push_back(p(6.0 * std::cos(a), bCy[b] + 6.0 * std::sin(a)));
            }
            w.polylineStroke(arc, kCompCol, t, false);
        }
        w.circleFill(p(+10, -22), 2.5, kPolDotCol);
    }
    else {
        // Fallback: bounding rect with type label.
        double bx = td.bodyHalfSize.x;
        double by = td.bodyHalfSize.y;
        if (comp.rotation % 2 == 1) std::swap(bx, by);
        std::vector<V2> rect = {
            {comp.pos.x - bx, comp.pos.y - by},
            {comp.pos.x + bx, comp.pos.y - by},
            {comp.pos.x + bx, comp.pos.y + by},
            {comp.pos.x - bx, comp.pos.y + by}
        };
        w.polylineStroke(rect, kCompCol, t, true);
        w.text({comp.pos.x, comp.pos.y}, id, kCompCol, 10.0);
    }

    // Instance label + value above body
    if (id != "JUNC" && id != "GND" && id != "NETLABEL" &&
        id != "TX_WIND" && id != "TX_CORE")
    {
        std::string lbl = comp.instanceName;
        if (!comp.paramValues.empty() && id != "S")
            lbl += "  " + comp.paramValues[0];
        double labelY = comp.pos.y - td.bodyHalfSize.y - 6.0;
        if (comp.rotation % 2 == 1) labelY = comp.pos.y - td.bodyHalfSize.x - 6.0;
        w.text({comp.pos.x, labelY}, lbl, kLabelCol, 11.0);
    }

    // Pin dots (1-wire = small blue, otherwise green)
    if (id != "JUNC") {
        for (size_t pi = 0; pi < td.pins.size(); ++pi) {
            V2 pp = pinWorld(comp, td.pins[pi]);
            int wc = (pi < wireCountPerPin.size()) ? wireCountPerPin[pi] : 0;
            if (wc == 1) w.circleFill(pp, 2.5, kPin1Col);
            else         w.circleFill(pp, 4.0, kPinCol);
        }
    }
}

void drawTxCoreDashes(GdiPainter& w, const SchematicComp& core, const SchematicModel& sch) {
    if (core.paramValues.empty()) return;
    const std::string& grp = core.paramValues[0];
    std::vector<const SchematicComp*> winds;
    for (const auto& c : sch.comps())
        if (c.typeId == "TX_WIND" && !c.paramValues.empty() && c.paramValues[0] == grp)
            winds.push_back(&c);
    if (winds.size() < 2) return;
    std::vector<V2> mids;
    for (const auto* wc : winds) {
        V2 top = sc(*wc, +14.0, -24.0);
        V2 bot = sc(*wc, +14.0, +24.0);
        V2 mid = sc(*wc, +14.0,   0.0);
        w.dashLine(top, bot, kCoreCol, 1.3, 5.0, 3.0);
        mids.push_back(mid);
    }
    for (size_t i = 1; i < mids.size(); ++i)
        w.dashLine(mids[i-1], mids[i], kCoreCol, 1.0, 4.0, 3.0);
}

// Compute bounding box of all visible primitives by walking the schematic.
bool computeBbox(const SchematicModel& sch, double& minx, double& miny,
                 double& maxx, double& maxy)
{
    bool any = false;
    auto track = [&](double x, double y) {
        if (!any) { minx = maxx = x; miny = maxy = y; any = true; }
        else { minx = std::min(minx,x); maxx = std::max(maxx,x);
               miny = std::min(miny,y); maxy = std::max(maxy,y); }
    };
    for (const auto& comp : sch.comps()) {
        const CompTypeDef* td = SchematicModel::findCompType(comp.typeId);
        if (!td) continue;
        // Outer extent: pin offsets plus padding
        for (const auto& pin : td->pins) {
            V2 q = pinWorld(comp, pin);
            track(q.x, q.y);
        }
        double bx = td->bodyHalfSize.x, by = td->bodyHalfSize.y;
        if (comp.rotation % 2 == 1) std::swap(bx, by);
        track(comp.pos.x - bx, comp.pos.y - by - 12);  // include label space
        track(comp.pos.x + bx, comp.pos.y + by);
    }
    for (const auto& wire : sch.wires()) {
        for (const auto& wp : wire.waypoints) track(wp.x, wp.y);
    }
    return any;
}

// Place an image on the Windows clipboard. Sets BOTH CF_DIB (universal,
// expected by Discord/Slack/ChatGPT/Word/PowerPoint paste handlers) and
// CF_BITMAP (legacy GDI consumers). The DIB blob is bottom-up with alpha
// forced to 255 so apps that ignore alpha still show the right colors.
//
// `topdownBits` points at the DIB-section pixel data (top-down, BGRA, 32bpp).
// On success the system owns the resources passed via SetClipboardData --
// caller must NOT free them. On failure the caller is responsible for
// the HBITMAP and the function frees its own GlobalAlloc.
bool placeBitmapOnClipboard(HWND owner, HBITMAP hbm,
                            int width, int height, const void* topdownBits) {
    const size_t rowBytes  = static_cast<size_t>(width) * 4u;
    const size_t pixelSize = rowBytes * static_cast<size_t>(height);
    const size_t blobSize  = sizeof(BITMAPINFOHEADER) + pixelSize;

    HGLOBAL hDib = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, blobSize);
    if (!hDib) return false;

    void* mem = GlobalLock(hDib);
    if (!mem) { GlobalFree(hDib); return false; }

    BITMAPINFOHEADER* hdr = static_cast<BITMAPINFOHEADER*>(mem);
    hdr->biSize        = sizeof(BITMAPINFOHEADER);
    hdr->biWidth       = width;
    hdr->biHeight      = height;            // POSITIVE = bottom-up (CF_DIB standard)
    hdr->biPlanes      = 1;
    hdr->biBitCount    = 32;
    hdr->biCompression = BI_RGB;
    hdr->biSizeImage   = static_cast<DWORD>(pixelSize);

    // Flip rows top-down → bottom-up while copying, and force alpha = 255 so
    // chat apps that interpret the alpha channel still see opaque pixels.
    BYTE* dst = static_cast<BYTE*>(mem) + sizeof(BITMAPINFOHEADER);
    const BYTE* src = static_cast<const BYTE*>(topdownBits);
    for (int y = 0; y < height; ++y) {
        BYTE* dstRow = dst + static_cast<size_t>(y) * rowBytes;
        const BYTE* srcRow = src + static_cast<size_t>(height - 1 - y) * rowBytes;
        for (int x = 0; x < width; ++x) {
            dstRow[x*4 + 0] = srcRow[x*4 + 0];  // B
            dstRow[x*4 + 1] = srcRow[x*4 + 1];  // G
            dstRow[x*4 + 2] = srcRow[x*4 + 2];  // R
            dstRow[x*4 + 3] = 255;              // A (override)
        }
    }
    GlobalUnlock(hDib);

    // Some clipboard owners hold it briefly; retry a few times.
    bool opened = false;
    for (int i = 0; i < 5; ++i) {
        if (OpenClipboard(owner)) { opened = true; break; }
        Sleep(20);
    }
    if (!opened) { GlobalFree(hDib); return false; }

    EmptyClipboard();
    HANDLE rDib = SetClipboardData(CF_DIB, hDib);
    HANDLE rBmp = SetClipboardData(CF_BITMAP, hbm);
    CloseClipboard();

    if (!rDib) GlobalFree(hDib);
    return rDib != nullptr || rBmp != nullptr;
}

} // anonymous

bool copySchematicImageToClipboard(const SchematicModel& sch, double scale)
{
    if (sch.comps().empty()) return false;

    double minx, miny, maxx, maxy;
    if (!computeBbox(sch, minx, miny, maxx, maxy)) return false;
    const double pad = 30.0;
    minx -= pad; miny -= pad; maxx += pad; maxy += pad;
    const double cw = maxx - minx;
    const double ch = maxy - miny;
    if (cw <= 1 || ch <= 1) return false;

    int width  = std::max(8, (int)std::round(cw * scale));
    int height = std::max(8, (int)std::round(ch * scale));

    // GDI+ session — minimal scope.
    GdiplusStartupInput gin;
    ULONG_PTR token = 0;
    if (GdiplusStartup(&token, &gin, nullptr) != Ok) return false;

    // Top-down 32-bit DIB section so GDI+ + clipboard both work.
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = width;
    bmi.bmiHeader.biHeight      = -height;          // top-down
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    HDC screenDC = GetDC(nullptr);
    void* bits = nullptr;
    HBITMAP hbm = CreateDIBSection(screenDC, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!hbm) {
        ReleaseDC(nullptr, screenDC);
        GdiplusShutdown(token);
        return false;
    }
    HDC memDC = CreateCompatibleDC(screenDC);
    HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, hbm);

    // Build wire-count map
    std::vector<int> dummy;
    auto pinKey = [](int compId, int pi) -> int64_t {
        return (static_cast<int64_t>(compId) << 16) | static_cast<int64_t>(pi);
    };
    // Render via a scoped Graphics so its destructor flushes before we transfer.
    {
        Graphics g(memDC);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintAntiAlias);

        // Background
        SolidBrush bg(kBgCol);
        g.FillRectangle(&bg, 0, 0, width, height);

        GdiPainter painter(g, scale, minx, miny);

        // Wires first (under components)
        for (const auto& wire : sch.wires()) {
            const SchematicComp* ca = sch.findComp(wire.fromCompId);
            const SchematicComp* cb = sch.findComp(wire.toCompId);
            if (!ca || !cb) continue;
            const CompTypeDef* ta = SchematicModel::findCompType(ca->typeId);
            const CompTypeDef* tb = SchematicModel::findCompType(cb->typeId);
            if (!ta || !tb) continue;
            if (wire.fromPinIdx >= (int)ta->pins.size()) continue;
            if (wire.toPinIdx   >= (int)tb->pins.size()) continue;

            V2 prev = pinWorld(*ca, ta->pins[wire.fromPinIdx]);
            for (const auto& wp : wire.waypoints) {
                V2 cur = { wp.x, wp.y };
                painter.line(prev, cur, kWireCol, 1.5);
                prev = cur;
            }
            V2 last = pinWorld(*cb, tb->pins[wire.toPinIdx]);
            painter.line(prev, last, kWireCol, 1.5);

            if (!wire.netName.empty()) {
                V2 mid = { (prev.x + last.x) * 0.5, (prev.y + last.y) * 0.5 - 8.0 };
                painter.text(mid, wire.netName, Color(220, 120,220,255), 10.0);
            }
        }

        // Pin wire counts
        std::vector<int> wireCount;  // flat by (compId<<16|pi); just rebuild map per comp:
        std::vector<std::pair<int64_t,int>> wcMap;
        for (const auto& wire : sch.wires()) {
            wcMap.push_back({pinKey(wire.fromCompId, wire.fromPinIdx), 1});
            wcMap.push_back({pinKey(wire.toCompId,   wire.toPinIdx),   1});
        }
        std::sort(wcMap.begin(), wcMap.end(),
                  [](auto& a, auto& b){ return a.first < b.first; });

        // Components
        for (const auto& comp : sch.comps()) {
            const CompTypeDef* td = SchematicModel::findCompType(comp.typeId);
            if (!td) continue;
            std::vector<int> pwc(td->pins.size(), 0);
            for (size_t pi = 0; pi < td->pins.size(); ++pi) {
                int64_t key = pinKey(comp.id, (int)pi);
                int cnt = 0;
                for (auto& [k,v] : wcMap) if (k == key) cnt++;
                pwc[pi] = cnt;
            }
            drawComponent(painter, comp, *td, pwc);

            if (comp.typeId == "JUNC")
                painter.circleFill({comp.pos.x, comp.pos.y}, 5.0, kJuncCol);
            if (comp.typeId == "TX_CORE")
                drawTxCoreDashes(painter, comp, sch);
        }
    }
    GdiFlush();

    // Detach bitmap from the memory DC and give it to the clipboard. The
    // CF_DIB blob is built from the DIB section's `bits` (top-down BGRA);
    // placeBitmapOnClipboard flips it to bottom-up and forces alpha=255.
    SelectObject(memDC, oldBmp);
    DeleteDC(memDC);
    ReleaseDC(nullptr, screenDC);

    bool ok = placeBitmapOnClipboard(nullptr, hbm, width, height, bits);
    if (!ok) DeleteObject(hbm);

    GdiplusShutdown(token);
    return ok;
}

#endif // _WIN32
