#include "views/schematic_svg_export.h"
#include "view_model/schematic_model.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;

// ── XML-safe text escaping ─────────────────────────────────────────────────
std::string xmlEscape(const std::string& s) {
    std::string r;
    r.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&': r += "&amp;"; break;
            case '<': r += "&lt;";  break;
            case '>': r += "&gt;";  break;
            case '"': r += "&quot;"; break;
            case '\'': r += "&apos;"; break;
            default:   r += c; break;
        }
    }
    return r;
}

// ── 2D primitives + transform helpers ─────────────────────────────────────
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

// Component-local (ox, oy) → world canvas coords given comp pos/rot/mirror.
V2 sc(const SchematicComp& comp, double ox, double oy) {
    double mx = comp.mirrorX ? -ox : ox;
    V2 r = rotateOff({mx, oy}, comp.rotation);
    return { comp.pos.x + r.x, comp.pos.y + r.y };
}

// Pin world position (matches pinCanvasPos in schematic_view.cpp).
V2 pinWorld(const SchematicComp& comp, const PinDef& pin) {
    return sc(comp, pin.offset.x, pin.offset.y);
}

// ── SVG writer: simple buffered emitter with primitive helpers ────────────
struct SvgWriter {
    std::ostringstream out;
    double minx, miny, maxx, maxy;
    bool   bboxInit = false;
    double pad      = 30.0;     // canvas-units padding
    double scale    = 1.0;

    void track(double x, double y) {
        if (!bboxInit) { minx = maxx = x; miny = maxy = y; bboxInit = true; }
        else { if (x<minx)minx=x; if (x>maxx)maxx=x; if (y<miny)miny=y; if (y>maxy)maxy=y; }
    }
    void track(V2 p) { track(p.x, p.y); }

    static std::string col(unsigned r, unsigned g, unsigned b, double a = 1.0) {
        char buf[40];
        if (a >= 0.999) std::snprintf(buf, sizeof(buf), "rgb(%u,%u,%u)", r, g, b);
        else            std::snprintf(buf, sizeof(buf), "rgba(%u,%u,%u,%.2f)", r, g, b, a);
        return buf;
    }

    void line(V2 a, V2 b, const std::string& stroke, double thick) {
        track(a); track(b);
        out << "<line x1=\"" << a.x << "\" y1=\"" << a.y
            << "\" x2=\"" << b.x << "\" y2=\"" << b.y
            << "\" stroke=\"" << stroke << "\" stroke-width=\"" << thick
            << "\" stroke-linecap=\"round\"/>\n";
    }
    void rect(V2 a, V2 b, const std::string& stroke, double thick,
              const std::string& fill = "none") {
        double x0=std::min(a.x,b.x), y0=std::min(a.y,b.y);
        double w =std::abs(b.x-a.x), h=std::abs(b.y-a.y);
        track(x0,y0); track(x0+w,y0+h);
        out << "<rect x=\"" << x0 << "\" y=\"" << y0
            << "\" width=\"" << w << "\" height=\"" << h
            << "\" fill=\"" << fill << "\" stroke=\"" << stroke
            << "\" stroke-width=\"" << thick << "\"/>\n";
    }
    void circleStroke(V2 c, double r, const std::string& stroke, double thick) {
        track(c.x-r,c.y-r); track(c.x+r,c.y+r);
        out << "<circle cx=\"" << c.x << "\" cy=\"" << c.y
            << "\" r=\"" << r << "\" fill=\"none\" stroke=\"" << stroke
            << "\" stroke-width=\"" << thick << "\"/>\n";
    }
    void circleFill(V2 c, double r, const std::string& fill) {
        track(c.x-r,c.y-r); track(c.x+r,c.y+r);
        out << "<circle cx=\"" << c.x << "\" cy=\"" << c.y
            << "\" r=\"" << r << "\" fill=\"" << fill << "\"/>\n";
    }
    // Polyline (for path stroke style polylines)
    void polyline(const std::vector<V2>& pts, const std::string& stroke, double thick,
                  bool closed = false, const std::string& fill = "none") {
        if (pts.empty()) return;
        out << (closed ? "<polygon points=\"" : "<polyline points=\"");
        for (size_t i = 0; i < pts.size(); ++i) {
            if (i) out << " ";
            out << pts[i].x << "," << pts[i].y;
            track(pts[i]);
        }
        out << "\" fill=\"" << fill << "\" stroke=\"" << stroke
            << "\" stroke-width=\"" << thick
            << "\" stroke-linejoin=\"round\" stroke-linecap=\"round\"/>\n";
    }
    void dashLine(V2 a, V2 b, const std::string& stroke, double thick,
                  double dashLen = 5.0, double gapLen = 3.0) {
        track(a); track(b);
        out << "<line x1=\"" << a.x << "\" y1=\"" << a.y
            << "\" x2=\"" << b.x << "\" y2=\"" << b.y
            << "\" stroke=\"" << stroke << "\" stroke-width=\"" << thick
            << "\" stroke-dasharray=\"" << dashLen << "," << gapLen
            << "\" stroke-linecap=\"round\"/>\n";
    }
    void text(V2 p, const std::string& s, const std::string& fill,
              double fontSize = 11.0, const char* anchor = "middle",
              const char* baseline = "central") {
        track(p);
        out << "<text x=\"" << p.x << "\" y=\"" << p.y
            << "\" fill=\"" << fill
            << "\" font-family=\"Consolas, 'Courier New', monospace\""
            << " font-size=\"" << fontSize
            << "\" text-anchor=\"" << anchor
            << "\" dominant-baseline=\"" << baseline
            << "\">" << xmlEscape(s) << "</text>\n";
    }
};

// ── Color palette (matches schematic_view.cpp) ────────────────────────────
const std::string kBgCol     = SvgWriter::col(20, 24, 32);
const std::string kCompCol   = SvgWriter::col(110,155,220);
const std::string kPinCol    = SvgWriter::col(80,200,120);
const std::string kPin1Col   = SvgWriter::col(120,170,255);
const std::string kJuncCol   = SvgWriter::col(80,210,120);
const std::string kWireCol   = SvgWriter::col(200,200, 80);
const std::string kCoreCol   = SvgWriter::col(150,180,230, 0.78);
const std::string kPolDotCol = SvgWriter::col(220,230,255, 0.94);
const std::string kNetCol    = SvgWriter::col(80,230,120);
const std::string kLabelCol  = SvgWriter::col(220,220,220);

// ── Per-component drawing ─────────────────────────────────────────────────
void drawComponent(SvgWriter& w, const SchematicComp& comp, const CompTypeDef& td,
                   const std::vector<int>& wireCountPerPin)
{
    auto p = [&](double x, double y) { return sc(comp, x, y); };
    const std::string& id = comp.typeId;
    const double t = 1.5;  // standard stroke

    // Mirror the editor's drawCompSymbol primitives one-for-one so the SVG
    // matches what the user sees in the canvas.
    if (id == "R") {
        // Body half {24,14}; pins at ±40
        w.line(p(-40,0), p(-14,0), kCompCol, t);
        w.line(p(+14,0), p(+40,0), kCompCol, t);
        w.line(p(-14,-7), p(+14,-7), kCompCol, t);
        w.line(p(+14,-7), p(+14,+7), kCompCol, t);
        w.line(p(+14,+7), p(-14,+7), kCompCol, t);
        w.line(p(-14,+7), p(-14,-7), kCompCol, t);
    }
    else if (id == "C") {
        // Pins at ±20; plates at x=±5; thicker plate strokes
        w.line(p(-20,0), p(-5,0),  kCompCol, t);
        w.line(p(+5,0),  p(+20,0), kCompCol, t);
        w.line(p(-5,-12), p(-5,+12), kCompCol, t * 1.5);
        w.line(p(+5,-12), p(+5,+12), kCompCol, t * 1.5);
    }
    else if (id == "L") {
        w.line(p(-40,0), p(-24,0), kCompCol, t);
        w.line(p(+24,0), p(+40,0), kCompCol, t);
        // Four upward bumps (a from π → 2π so sinf is negative = canvas Y up)
        constexpr int N = 14;
        double bumpCx[4] = {-18.0, -6.0, +6.0, +18.0};
        for (int b = 0; b < 4; ++b) {
            std::vector<V2> arc;
            for (int k = 0; k <= N; ++k) {
                double a = kPi + (double)k / N * kPi;
                arc.push_back(p(bumpCx[b] + 6.0 * std::cos(a), 6.0 * std::sin(a)));
            }
            w.polyline(arc, kCompCol, t);
        }
    }
    else if (id == "V_DC" || id == "V_SQUARE" || id == "V_SIN" || id == "V_STEP") {
        // Editor: circle in canvas-space (NOT rotated), leads via sc(...)
        w.circleStroke({comp.pos.x, comp.pos.y}, 18.0, kCompCol, t);
        w.line(p(-40,0), p(-18,0), kCompCol, t);
        w.line(p(+18,0), p(+40,0), kCompCol, t);
        if (id == "V_DC") {
            // "+" near P pin
            w.line(p(-12,0), p(-6,0), kCompCol, t);
            w.line(p(-9,-3), p(-9,+3), kCompCol, t);
            // "−" near N pin
            w.line(p(+6,0), p(+12,0), kCompCol, t);
        } else if (id == "V_SIN") {
            constexpr int NS = 16;
            std::vector<V2> sn;
            for (int k = 0; k <= NS; ++k) {
                double tt = (double)k / NS;
                sn.push_back({comp.pos.x + (-12.0 + 24.0 * tt),
                              comp.pos.y + (-6.0 * std::sin(2.0 * kPi * tt))});
            }
            w.polyline(sn, kCompCol, t);
        } else if (id == "V_SQUARE") {
            std::vector<V2> sq = {
                {comp.pos.x - 12, comp.pos.y - 5},
                {comp.pos.x -  4, comp.pos.y - 5},
                {comp.pos.x -  4, comp.pos.y + 5},
                {comp.pos.x +  4, comp.pos.y + 5},
                {comp.pos.x +  4, comp.pos.y - 5},
                {comp.pos.x + 12, comp.pos.y - 5}
            };
            w.polyline(sq, kCompCol, t);
        } else { // V_STEP
            std::vector<V2> stp = {
                {comp.pos.x - 12, comp.pos.y + 5},
                {comp.pos.x -  4, comp.pos.y + 5},
                {comp.pos.x -  4, comp.pos.y - 5},
                {comp.pos.x + 12, comp.pos.y - 5}
            };
            w.polyline(stp, kCompCol, t);
        }
    }
    else if (id == "I") {
        w.circleStroke({comp.pos.x, comp.pos.y}, 18.0, kCompCol, t);
        w.line(p(-40,0), p(-18,0), kCompCol, t);
        w.line(p(+18,0), p(+40,0), kCompCol, t);
        w.line(p(+8,0), p(-8,0), kCompCol, t);
        // Filled arrowhead at sc(-8,0) pointing left
        std::vector<V2> arrow = { p(-8,0), p(-4,-4), p(-4,+4) };
        w.polyline(arrow, kCompCol, t, true, kCompCol);
    }
    else if (id == "D") {
        w.line(p(-40,0), p(-12,0), kCompCol, t);
        w.line(p(+12,0), p(+40,0), kCompCol, t);
        std::vector<V2> tri = { p(-12,-10), p(+12,0), p(-12,+10) };
        w.polyline(tri, kCompCol, t, true);
        w.line(p(+12,-12), p(+12,+12), kCompCol, t);
    }
    else if (id == "S") {
        // Mirrors the editor's MOSFET-with-body-diode rendering exactly.
        w.line(p(-20,  0), p(-5,  0), kCompCol, t);                  // G lead
        w.line(p(-20, +20), p(-5, +20), kCompCol, t);                // GRef lead
        w.line(p(-5, -15), p(-5, +20), kCompCol, t);                 // Gate bar
        // Channel 3 segments at x=0
        w.line(p(0, -20), p(0, -10), kCompCol, t);
        w.line(p(0,  -5), p(0,  +5), kCompCol, t);
        w.line(p(0, +10), p(0, +20), kCompCol, t);
        // Horizontal stubs: drain / body / source
        w.line(p(0, -15), p(20, -15), kCompCol, t);
        w.line(p(0,   0), p(20,   0), kCompCol, t);
        w.line(p(0, +15), p(20, +15), kCompCol, t);
        // D / S vertical
        w.line(p(20, -15), p(20, -25), kCompCol, t);
        w.line(p(20,   0), p(20, +25), kCompCol, t);
        // D / S horizontal
        w.line(p(20, -20), p(33, -20), kCompCol, t);
        w.line(p(20, +20), p(33, +20), kCompCol, t);
        // Outer body-diode bar (gap at -5..+5)
        w.line(p(33, -20), p(33,  -5), kCompCol, t);
        w.line(p(33,  +5), p(33, +20), kCompCol, t);
        // D / S pin leads
        w.line(p(20, -40), p(20, -25), kCompCol, t);
        w.line(p(20, +25), p(20, +40), kCompCol, t);
        // Body arrow (left-pointing filled triangle, matches AddTriangleFilled)
        std::vector<V2> bArr = { p(0,0), p(10,-5), p(10,+5) };
        w.polyline(bArr, kCompCol, t, true, kCompCol);
        // Body-diode: cathode bar + filled UP-pointing triangle. Tip at
        // (33,-5), base from (28,+5) to (38,+5). The editor uses
        // AddTriangleFilled so the SVG must be a filled polygon, not a stroke.
        w.line(p(28, -5), p(38, -5), kCompCol, t);
        std::vector<V2> bdTri = { p(33, -5), p(28, +5), p(38, +5) };
        w.polyline(bdTri, kCompCol, t, true, kCompCol);
    }
    else if (id == "GND") {
        // Stem + 3 bars; default rotation 0: stem points DOWN (canvas Y+)
        w.line(p(0,0), p(0,8), kPinCol, t);
        w.line(p(-5, 8),  p(+5, 8),  kPinCol, t);
        w.line(p(-3,12),  p(+3,12),  kPinCol, t);
        w.line(p(-1,16),  p(+1,16),  kPinCol, t);
    }
    else if (id == "JUNC") {
        // Drawn separately below as a special dot.
        // (Body intentionally empty here.)
    }
    else if (id == "NETLABEL") {
        // Compact flag pentagon -- pin at (-20,0)
        std::vector<V2> flag = {
            p(-20,0), p(-16,-7), p(-4,-7), p(-4,+7), p(-16,+7)
        };
        w.polyline(flag, kNetCol, t, true);
        if (!comp.paramValues.empty())
            w.text(p(+10,0), comp.paramValues[0], kNetCol, 11.0, "middle", "central");
    }
    else if (id == "TX") {
        // Two sets of bumps (primary on left, secondary on right) facing each other,
        // with two vertical core lines between them.
        w.line(p(-5,-18), p(-5,+18), kCoreCol, t * 0.8);
        w.line(p(+5,-18), p(+5,+18), kCoreCol, t * 0.8);
        w.line(p(-40,-20), p(-22,-18), kCompCol, t);
        w.line(p(-40,+20), p(-22,+18), kCompCol, t);
        w.line(p(+40,-20), p(+22,-18), kCompCol, t);
        w.line(p(+40,+20), p(+22,+18), kCompCol, t);
        constexpr int NC = 12;
        double primY[3] = {-12.0, 0.0, +12.0};
        for (int b = 0; b < 3; ++b) {
            double yc = primY[b];
            std::vector<V2> arcL, arcR;
            for (int k = 0; k <= NC; ++k) {
                double a = -kPi/2.0 - (double)k / NC * kPi;
                arcL.push_back(p(-22.0 + 6.0 * std::cos(a), yc + 6.0 * std::sin(a)));
            }
            for (int k = 0; k <= NC; ++k) {
                double a = -kPi/2.0 + (double)k / NC * kPi;
                arcR.push_back(p(+22.0 + 6.0 * std::cos(a), yc + 6.0 * std::sin(a)));
            }
            w.polyline(arcL, kCompCol, t);
            w.polyline(arcR, kCompCol, t);
        }
        w.circleFill(p(-28,-22), 3.5, kPolDotCol);
        w.circleFill(p(+28,-22), 3.5, kPolDotCol);
        if (comp.paramValues.size() >= 2) {
            std::string ratio = comp.paramValues[0] + ":" + comp.paramValues[1];
            w.text(p(0, 0), ratio, kCompCol, 10.0);
        }
    }
    else if (id == "TX3") {
        // Three secondaries on the right; fall back to a simplified TX-like body.
        w.line(p(-5,-30), p(-5,+30), kCoreCol, t * 0.8);
        w.line(p(+5,-30), p(+5,+30), kCoreCol, t * 0.8);
        // Primary bumps (left)
        constexpr int NC = 10;
        double primY[3] = {-20.0, 0.0, +20.0};
        for (int b = 0; b < 3; ++b) {
            std::vector<V2> arc;
            for (int k = 0; k <= NC; ++k) {
                double a = -kPi/2.0 - (double)k / NC * kPi;
                arc.push_back(p(-22.0 + 6.0 * std::cos(a), primY[b] + 6.0 * std::sin(a)));
            }
            w.polyline(arc, kCompCol, t);
        }
        // Three secondaries on the right (one per pair)
        double secY[3] = {-30.0, -10.0, +10.0};
        for (int s = 0; s < 3; ++s) {
            for (int b = 0; b < 2; ++b) {
                std::vector<V2> arc;
                for (int k = 0; k <= NC; ++k) {
                    double a = -kPi/2.0 + (double)k / NC * kPi;
                    arc.push_back(p(+22.0 + 6.0 * std::cos(a),
                                    secY[s] + 5.0 + b * 10.0 + 6.0 * std::sin(a)));
                }
                w.polyline(arc, kCompCol, t);
            }
        }
        // Leads
        w.line(p(-40,-20), p(-22,-20), kCompCol, t);
        w.line(p(-40,+20), p(-22,+20), kCompCol, t);
        for (int s = 0; s < 3; ++s) {
            double y = -30.0 + s * 20.0;
            w.line(p(+40,y),     p(+22,y),     kCompCol, t);
            w.line(p(+40,y+20),  p(+22,y+20),  kCompCol, t);
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
            w.polyline(arc, kCompCol, t);
        }
        w.circleFill(p(+10, -22), 2.5, kPolDotCol);
        if (comp.paramValues.size() >= 3) {
            std::string lbl = "n=" + comp.paramValues[2];
            w.text(p(+18, 0), lbl, SvgWriter::col(180, 210, 255), 10.0, "middle", "central");
        }
    }
    else if (id == "TX_CORE") {
        // Just a tiny tag; the actual core dashes are drawn between TX_WINDs.
        w.text(p(0, 0), "[CORE]", SvgWriter::col(170, 200, 230), 10.0);
    }
    else {
        // Unknown / unhandled type: draw bounding rect with type label.
        double bx = td.bodyHalfSize.x;
        double by = td.bodyHalfSize.y;
        if (comp.rotation % 2 == 1) std::swap(bx, by);
        w.rect({comp.pos.x - bx, comp.pos.y - by},
               {comp.pos.x + bx, comp.pos.y + by}, kCompCol, t);
        w.text({comp.pos.x, comp.pos.y}, id, kCompCol, 10.0);
    }

    // ── Common: instance label + first param ─────────────────────────────
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

    // ── Pin dots ─────────────────────────────────────────────────────────
    if (id != "JUNC") {
        for (size_t pi = 0; pi < td.pins.size(); ++pi) {
            V2 pp = pinWorld(comp, td.pins[pi]);
            int wc = (pi < wireCountPerPin.size()) ? wireCountPerPin[pi] : 0;
            if (wc == 1) w.circleFill(pp, 2.5, kPin1Col);
            else         w.circleFill(pp, 4.0, kPinCol);
        }
    }
}

// Walk TX_CORE → TX_WIND members and emit the inter-winding dash pattern.
void drawTxCoreDashes(SvgWriter& w, const SchematicComp& core, const SchematicModel& sch) {
    if (core.paramValues.empty()) return;
    const std::string& grp = core.paramValues[0];
    std::vector<const SchematicComp*> winds;
    for (const auto& c : sch.comps())
        if (c.typeId == "TX_WIND" && !c.paramValues.empty() && c.paramValues[0] == grp)
            winds.push_back(&c);
    if (winds.size() < 2) return;
    auto windOff = [](const SchematicComp& wc, double ox, double oy) {
        return sc(wc, ox, oy);
    };
    std::vector<V2> mids;
    for (const auto* wc : winds) {
        V2 top = windOff(*wc, +14.0, -24.0);
        V2 bot = windOff(*wc, +14.0, +24.0);
        V2 mid = windOff(*wc, +14.0,   0.0);
        w.dashLine(top, bot, kCoreCol, 1.3, 5.0, 3.0);
        mids.push_back(mid);
    }
    for (size_t i = 1; i < mids.size(); ++i)
        w.dashLine(mids[i-1], mids[i], kCoreCol, 1.0, 4.0, 3.0);
}

} // anonymous

// ── Public entry points ───────────────────────────────────────────────────
bool exportSchematicToSvg(const SchematicModel& sch, std::string& outSvg, double scale)
{
    if (sch.comps().empty()) { outSvg.clear(); return false; }

    SvgWriter w;
    w.scale = scale;

    // Build per-pin wire count (for conditional pin-dot rendering).
    auto pinKey = [](int compId, int pi) -> int64_t {
        return (static_cast<int64_t>(compId) << 16) | static_cast<int64_t>(pi);
    };
    std::unordered_map<int64_t, int> wireCount;
    for (const auto& wire : sch.wires()) {
        wireCount[pinKey(wire.fromCompId, wire.fromPinIdx)]++;
        wireCount[pinKey(wire.toCompId,   wire.toPinIdx  )]++;
    }

    // Buffer wires to draw first (under components).
    std::ostringstream wireBuf;
    {
        SvgWriter wireW; // discardable; reuse track for bbox via primitive emission below.
        // Use the main writer directly so bbox is consistent.
    }

    // ── Wires ─────────────────────────────────────────────────────────────
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
            w.line(prev, cur, kWireCol, 1.5);
            prev = cur;
        }
        V2 last = pinWorld(*cb, tb->pins[wire.toPinIdx]);
        w.line(prev, last, kWireCol, 1.5);

        if (!wire.netName.empty()) {
            V2 mid = { (prev.x + last.x) * 0.5, (prev.y + last.y) * 0.5 - 8.0 };
            w.text(mid, wire.netName, SvgWriter::col(120, 220, 255), 10.0);
        }
    }

    // ── Components ────────────────────────────────────────────────────────
    for (const auto& comp : sch.comps()) {
        const CompTypeDef* td = SchematicModel::findCompType(comp.typeId);
        if (!td) continue;

        // Per-pin wire counts for this comp
        std::vector<int> pwc(td->pins.size(), 0);
        for (size_t pi = 0; pi < td->pins.size(); ++pi) {
            auto it = wireCount.find(pinKey(comp.id, (int)pi));
            if (it != wireCount.end()) pwc[pi] = it->second;
        }
        drawComponent(w, comp, *td, pwc);

        // JUNC dot
        if (comp.typeId == "JUNC")
            w.circleFill({comp.pos.x, comp.pos.y}, 5.0, kJuncCol);

        // TX_CORE inter-winding dashes
        if (comp.typeId == "TX_CORE")
            drawTxCoreDashes(w, comp, sch);
    }

    // ── Build final document ──────────────────────────────────────────────
    if (!w.bboxInit) { outSvg.clear(); return false; }
    double x0 = w.minx - w.pad, y0 = w.miny - w.pad;
    double x1 = w.maxx + w.pad, y1 = w.maxy + w.pad;
    double width  = x1 - x0;
    double height = y1 - y0;

    std::ostringstream final;
    final << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
          << "<svg xmlns=\"http://www.w3.org/2000/svg\" version=\"1.1\""
          << " width=\""  << (width  * scale) << "\""
          << " height=\"" << (height * scale) << "\""
          << " viewBox=\"" << x0 << " " << y0 << " " << width << " " << height << "\">\n";
    final << "<rect x=\"" << x0 << "\" y=\"" << y0
          << "\" width=\"" << width << "\" height=\"" << height
          << "\" fill=\"" << kBgCol << "\"/>\n";
    final << w.out.str();
    final << "</svg>\n";
    outSvg = final.str();
    return true;
}

bool exportSchematicToSvgFile(const SchematicModel& sch, const std::string& path, double scale)
{
    std::string svg;
    if (!exportSchematicToSvg(sch, svg, scale)) return false;
    std::ofstream f(path, std::ios::binary);
    if (!f.is_open()) return false;
    f.write(svg.data(), static_cast<std::streamsize>(svg.size()));
    return f.good();
}
