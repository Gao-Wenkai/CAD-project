// LargeHWView.cpp: CAD View Implementation
// Rendering engine (AutoCAD-style dark background/grid/crosshair)
// Interaction state machine (simulates AutoCAD command-line prompt flow)

#include "pch.h"
#include "framework.h"
#ifndef SHARED_HANDLERS
#include "LargeHW.h"
#endif

#include "LargeHWDoc.h"
#include "LargeHWView.h"
#include "Dimension.h"
#include "MainFrm.h"
#include <map>
#include <algorithm>
#include <vector>

#include <afxdlgs.h>
#include <cfloat>
#include <climits>
#include <cstdlib>
#include <cwctype>

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

// Helper: closest point on segment to a given point (world coords)
static CPoint ClosestPointOnSegment(const CPoint& s, const CPoint& e, const CPoint& p)
{
    double vx = (double)(e.x - s.x), vy = (double)(e.y - s.y);
    double wx = (double)(p.x - s.x), wy = (double)(p.y - s.y);
    double denom = vx*vx + vy*vy;
    if (denom <= 1e-9) return s;
    double t = (vx*wx + vy*wy) / denom;
    if (t < 0) t = 0; if (t > 1) t = 1;
    return CPoint((int)round(s.x + t*vx), (int)round(s.y + t*vy));
}

// Helper: split line/polyline entities at pairwise intersections and replace originals with pieces
// Internal helper: create temporary splits for angle-dim; returns created IDs and stores originals as clones
static void CreateTemporarySplits(CLargeHWDoc* pDoc, std::vector<int>& outNewIDs)
{
    if (!pDoc) return;
    // collect segments: each entry holds endpoints and owner info
    struct Seg { CPoint a, b; CEntity* owner; int segIndex; bool ownerIsPolyline; };
    std::vector<Seg> segs;
    // snapshot entities list to avoid mutation during iteration
    auto ents = pDoc->GetEntities();
    for (auto* e : ents) {
        if (auto ln = dynamic_cast<CLineEntity*>(e)) {
            segs.push_back({ln->m_ptStart, ln->m_ptEnd, e, -1, false});
        } else if (auto pl = dynamic_cast<CPolylineEntity*>(e)) {
            int n = (int)pl->m_vertices.size();
            for (int i = 0; i < n - 1; ++i) {
                segs.push_back({pl->m_vertices[i], pl->m_vertices[i+1], e, i, true});
            }
        }
    }

    if (segs.size() < 2) return;

    // for each segment, collect intersection parameters t along 0..1
    std::vector<std::vector<double>> interT(segs.size());
    auto add_inter = [&](int i, double t){ if (t >= 0.0 && t <= 1.0) interT[i].push_back(t); };

    for (size_t i = 0; i < segs.size(); ++i) {
        for (size_t j = i+1; j < segs.size(); ++j) {
            // skip if same owner (we don't split within same polyline by self-intersection here)
            if (segs[i].owner == segs[j].owner) continue;
            // compute intersection of segments i and j
            double x1 = segs[i].a.x, y1 = segs[i].a.y, x2 = segs[i].b.x, y2 = segs[i].b.y;
            double x3 = segs[j].a.x, y3 = segs[j].a.y, x4 = segs[j].b.x, y4 = segs[j].b.y;
            double den = (x1 - x2)*(y3 - y4) - (y1 - y2)*(x3 - x4);
            if (fabs(den) < 1e-6) continue;
            double px = ((x1*y2 - y1*x2)*(x3 - x4) - (x1 - x2)*(x3*y4 - y3*x4)) / den;
            double py = ((x1*y2 - y1*x2)*(y3 - y4) - (y1 - y2)*(x3*y4 - y3*x4)) / den;
            // compute parameter t on each segment
            double ti = (fabs(x2 - x1) > fabs(y2 - y1)) ? ((px - x1) / (x2 - x1)) : ((py - y1) / (y2 - y1));
            double tj = (fabs(x4 - x3) > fabs(y4 - y3)) ? ((px - x3) / (x4 - x3)) : ((py - y3) / (y4 - y3));
            if (ti > 1e-4 && ti < 1.0-1e-4 && tj > 1e-4 && tj < 1.0-1e-4) {
                add_inter((int)i, max(0.0, min(1.0, ti)));
                add_inter((int)j, max(0.0, min(1.0, tj)));
            }
        }
    }

    // prepare map owner -> created segments
    std::map<CEntity*, std::vector<std::pair<CPoint,CPoint>>> createdMap;

    for (size_t i = 0; i < segs.size(); ++i) {
        auto& seg = segs[i];
        auto& ts = interT[i];
        // Always create temp entities for polyline segments (break at vertices),
        // and for line segments that have intersections (break at intersection points).
        // For standalone lines with no intersections, keep the original visible.
        if (!seg.ownerIsPolyline && ts.empty()) continue;

        // include endpoints 0 and 1
        ts.push_back(0.0); ts.push_back(1.0);
        sort(ts.begin(), ts.end());
        // unique with tolerance
        std::vector<double> uniq;
        for (double t : ts) {
            if (uniq.empty() || fabs(t - uniq.back()) > 1e-6) uniq.push_back(t);
        }
        for (size_t k = 0; k + 1 < uniq.size(); ++k) {
            double t0 = uniq[k], t1 = uniq[k+1];
            if (t1 - t0 < 1e-6) continue;
            // compute piece endpoints
            CPoint p0((int)floor(seg.a.x + (seg.b.x - seg.a.x) * t0 + 0.5), (int)floor(seg.a.y + (seg.b.y - seg.a.y) * t0 + 0.5));
            CPoint p1((int)floor(seg.a.x + (seg.b.x - seg.a.x) * t1 + 0.5), (int)floor(seg.a.y + (seg.b.y - seg.a.y) * t1 + 0.5));
            if (Distance(p0, p1) <= 1e-6) continue;
            createdMap[seg.owner].push_back({p0, p1});
        }
    }

    // Add created pieces as temporary segments; inherit visual properties from originals
    for (auto& kv : createdMap) {
        CEntity* orig = kv.first;
        for (auto& pr : kv.second) {
            CLineEntity* ln = new CLineEntity(pr.first, pr.second);
            pDoc->AddEntity(ln);
            // Override after AddEntity (which sets current doc defaults)
            ln->m_color = orig->m_color;
            ln->m_nLineStyle = orig->m_nLineStyle;
            ln->m_nLineWidth = orig->m_nLineWidth;
            outNewIDs.push_back(ln->m_nID);
        }
    }
}

// Restore temporary splits: remove created segments
static void RestoreTemporarySplits(CLargeHWDoc* pDoc, const std::vector<int>& newIDs)
{
    if (!pDoc) return;
    for (int id : newIDs) pDoc->RemoveEntity(id, false);
}

namespace
{
CStringA WideToUtf8(const CString& text)
{
    int nChars = text.GetLength();
    if (nChars == 0) return CStringA();

    int nBytes = ::WideCharToMultiByte(CP_UTF8, 0, text, nChars, nullptr, 0, nullptr, nullptr);
    if (nBytes <= 0) return CStringA();

    CStringA utf8;
    LPSTR pBuffer = utf8.GetBuffer(nBytes);
    ::WideCharToMultiByte(CP_UTF8, 0, text, nChars, pBuffer, nBytes, nullptr, nullptr);
    utf8.ReleaseBuffer(nBytes);
    return utf8;
}

bool DecodeTextBytes(const std::vector<BYTE>& bytes, CString& text)
{
    text.Empty();
    if (bytes.empty()) return true;

    if (bytes.size() >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE) {
        int nChars = (int)((bytes.size() - 2) / sizeof(wchar_t));
        text = CString((LPCWSTR)(bytes.data() + 2), nChars);
        return true;
    }

    int nOffset = 0;
    UINT nCodePage = CP_UTF8;
    if (bytes.size() >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF) {
        nOffset = 3;
    }

    int nSize = (int)bytes.size() - nOffset;
    if (nSize <= 0) return true;

    int nChars = ::MultiByteToWideChar(nCodePage, MB_ERR_INVALID_CHARS,
                                       (LPCCH)bytes.data() + nOffset, nSize,
                                       nullptr, 0);
    if (nChars <= 0) {
        nCodePage = CP_ACP;
        nChars = ::MultiByteToWideChar(nCodePage, 0,
                                       (LPCCH)bytes.data(), (int)bytes.size(),
                                       nullptr, 0);
        nOffset = 0;
        nSize = (int)bytes.size();
    }

    if (nChars <= 0) return false;

    LPWSTR pBuffer = text.GetBuffer(nChars);
    ::MultiByteToWideChar(nCodePage, 0, (LPCCH)bytes.data() + nOffset, nSize, pBuffer, nChars);
    text.ReleaseBuffer(nChars);
    return true;
}

bool LoadTextFile(const CString& path, CString& text)
{
    CFile file;
    if (!file.Open(path, CFile::modeRead | CFile::shareDenyWrite))
        return false;

    ULONGLONG nLen64 = file.GetLength();
    if (nLen64 > INT_MAX) {
        file.Close();
        return false;
    }

    std::vector<BYTE> bytes((size_t)nLen64);
    if (!bytes.empty())
        file.Read(bytes.data(), (UINT)bytes.size());
    file.Close();

    return DecodeTextBytes(bytes, text);
}

int ScriptRound(double value)
{
    return (int)(value >= 0.0 ? floor(value + 0.5) : ceil(value - 0.5));
}

CString NormalizeScriptWord(const CString& input)
{
    CString word = input;
    word.Trim();
    while (!word.IsEmpty() && (word[0] == L'_' || word[0] == L'.' || word[0] == L'-'))
        word = word.Mid(1);
    word.MakeUpper();
    return word;
}

bool TryParseDoubleStrict(const CString& input, double& value)
{
    CString s = input;
    s.Trim();
    if (s.IsEmpty())
        return false;

    wchar_t* pEnd = nullptr;
    value = wcstod(s, &pEnd);
    while (pEnd && *pEnd != L'\0' && iswspace(*pEnd))
        ++pEnd;
    return pEnd && *pEnd == L'\0';
}

bool TryParseScriptPoint(const CString& input, CPoint ref, CPoint& point, double coordinateScale = 1.0)
{
    CString s = input;
    s.Trim();
    if (s.IsEmpty())
        return false;

    bool bRelative = false;
    if (s[0] == L'@') {
        bRelative = true;
        s = s.Mid(1);
        s.Trim();
    }

    int nLt = s.Find(L'<');
    if (nLt > 0) {
        double dist = 0.0;
        double angleDeg = 0.0;
        if (!TryParseDoubleStrict(s.Left(nLt), dist) ||
            !TryParseDoubleStrict(s.Mid(nLt + 1), angleDeg)) {
            return false;
        }

        double angleRad = angleDeg * M_PI / 180.0;
        point = CPoint(ref.x + ScriptRound(dist * coordinateScale * cos(angleRad)),
                       ref.y + ScriptRound(dist * coordinateScale * sin(angleRad)));
        return true;
    }

    std::vector<CString> parts;
    int nStart = 0;
    while (nStart <= s.GetLength()) {
        int nComma = s.Find(L',', nStart);
        if (nComma < 0) {
            parts.push_back(s.Mid(nStart));
            break;
        }
        parts.push_back(s.Mid(nStart, nComma - nStart));
        nStart = nComma + 1;
    }

    if (parts.size() < 2)
        return false;

    double x = 0.0;
    double y = 0.0;
    if (!TryParseDoubleStrict(parts[0], x) ||
        !TryParseDoubleStrict(parts[1], y)) {
        return false;
    }

    int ix = ScriptRound(x * coordinateScale);
    int iy = ScriptRound(y * coordinateScale);
    point = bRelative ? CPoint(ref.x + ix, ref.y + iy) : CPoint(ix, iy);
    return true;
}

double DetermineScriptCoordinateScale(const CString& text)
{
    bool hasFractionalCoordinate = false;
    double maxAbs = 0.0;

    const wchar_t* p = text.GetString();
    while (*p) {
        while (*p && !iswdigit(*p) && *p != L'+' && *p != L'-' && *p != L'.')
            ++p;
        if (!*p)
            break;

        const wchar_t* start = p;
        wchar_t* end = nullptr;
        double value = wcstod(start, &end);
        if (end == start) {
            ++p;
            continue;
        }

        bool hasDecimal = false;
        for (const wchar_t* q = start; q < end; ++q) {
            if (*q == L'.') {
                hasDecimal = true;
                break;
            }
        }
        if (hasDecimal)
            hasFractionalCoordinate = true;

        maxAbs = max(maxAbs, fabs(value));
        p = end;
    }

    if (!hasFractionalCoordinate)
        return 1.0;

    double scale = 1000.0;
    while (scale > 1.0 && maxAbs * scale > (double)INT_MAX / 4.0)
        scale /= 10.0;
    return max(1.0, scale);
}

bool TryParseOnOff(const CString& input, bool& value)
{
    CString arg = NormalizeScriptWord(input);
    if (arg == L"ON" || arg == L"1" || arg == L"YES" || arg == L"TRUE") {
        value = true;
        return true;
    }
    if (arg == L"OFF" || arg == L"0" || arg == L"NO" || arg == L"FALSE") {
        value = false;
        return true;
    }
    return false;
}

bool IsAllSelectionToken(const CString& input)
{
    CString arg = NormalizeScriptWord(input);
    return arg == L"ALL" || arg == L"*";
}

void SelectAllEntities(CLargeHWDoc* pDoc)
{
    if (!pDoc) return;
    for (auto* p : pDoc->GetEntities())
        p->m_bSelected = true;
}

CString FormatOnOff(bool value)
{
    return value ? L"ON" : L"OFF";
}

CString QuoteScriptToken(const CString& value)
{
    if (value.FindOneOf(L" \t;#\"") < 0)
        return value;

    CString escaped = value;
    escaped.Replace(L"\"", L"\\\"");
    return L"\"" + escaped + L"\"";
}

double GetModelUnitScale(const CLargeHWDoc* pDoc)
{
    if (!pDoc || pDoc->m_dModelUnitScale < 1.0)
        return 1.0;
    return pDoc->m_dModelUnitScale;
}

CString FormatModelNumber(double value)
{
    CString text;
    text.Format(L"%.5f", value);
    while (text.Find(L'.') >= 0 && text.Right(1) == L"0")
        text = text.Left(text.GetLength() - 1);
    if (text.Right(1) == L".")
        text = text.Left(text.GetLength() - 1);
    if (text == L"-0")
        text = L"0";
    return text;
}

CString FormatModelPoint(CPoint pt, double modelUnitScale)
{
    CString text;
    text.Format(L"%s,%s",
                (LPCTSTR)FormatModelNumber(pt.x / modelUnitScale),
                (LPCTSTR)FormatModelNumber(pt.y / modelUnitScale));
    return text;
}

void ApplyDocumentModelUnitScale(CLargeHWDoc* pDoc, double requestedScale)
{
    if (!pDoc || requestedScale <= pDoc->m_dModelUnitScale)
        return;

    double previousScale = GetModelUnitScale(pDoc);
    double factor = requestedScale / previousScale;
    if (factor <= 1.0)
        return;

    for (auto* pEntity : pDoc->GetEntities()) {
        if (pEntity)
            pEntity->Scale(CPoint(0, 0), factor);
    }

    pDoc->m_dScale /= factor;
    pDoc->m_nGridSpacing = max(1, ScriptRound(pDoc->m_nGridSpacing * factor));
    pDoc->m_dModelUnitScale = requestedScale;
}

double ClampViewScale(const CLargeHWDoc* pDoc, double scale)
{
    double modelUnitScale = GetModelUnitScale(pDoc);
    double minScale = 0.001 / modelUnitScale;
    double maxScale = 10000.0 / modelUnitScale;

    if (scale < minScale)
        return minScale;
    if (scale > maxScale)
        return maxScale;
    return scale;
}

void FitViewToWorldBounds(CLargeHWDoc* pDoc, CRect bounds, const CRect& rcClient, int marginPx)
{
    if (!pDoc || rcClient.Width() <= 0 || rcClient.Height() <= 0)
        return;

    bounds.NormalizeRect();
    int minHalfExtent = max(1, ScriptRound(GetModelUnitScale(pDoc)));
    if (bounds.Width() <= 0)
        bounds.InflateRect(minHalfExtent, 0);
    if (bounds.Height() <= 0)
        bounds.InflateRect(0, minHalfExtent);

    int usableWidth = max(1, rcClient.Width() - marginPx * 2);
    int usableHeight = max(1, rcClient.Height() - marginPx * 2);
    double sx = (double)usableWidth / max(1, bounds.Width());
    double sy = (double)usableHeight / max(1, bounds.Height());

    pDoc->m_dScale = ClampViewScale(pDoc, min(sx, sy));

    double contentWidth = bounds.Width() * pDoc->m_dScale;
    double contentHeight = bounds.Height() * pDoc->m_dScale;
    double xMargin = (rcClient.Width() - contentWidth) / 2.0;
    double yMargin = (rcClient.Height() - contentHeight) / 2.0;

    pDoc->m_ptOffset = CPoint(
        ScriptRound(xMargin - bounds.left * pDoc->m_dScale),
        ScriptRound(yMargin + bounds.bottom * pDoc->m_dScale)
    );
}

bool IntersectInfiniteLines(CPoint a1, CPoint a2, CPoint b1, CPoint b2, double& ix, double& iy)
{
    double x1 = a1.x, y1 = a1.y;
    double x2 = a2.x, y2 = a2.y;
    double x3 = b1.x, y3 = b1.y;
    double x4 = b2.x, y4 = b2.y;
    double den = (x1 - x2) * (y3 - y4) - (y1 - y2) * (x3 - x4);
    if (fabs(den) < 1e-9)
        return false;

    double det1 = x1 * y2 - y1 * x2;
    double det2 = x3 * y4 - y3 * x4;
    ix = (det1 * (x3 - x4) - (x1 - x2) * det2) / den;
    iy = (det1 * (y3 - y4) - (y1 - y2) * det2) / den;
    return true;
}

CPoint PointFromIntersectionTowardEndpoint(double ix, double iy, CPoint endpoint, double distance)
{
    double vx = endpoint.x - ix;
    double vy = endpoint.y - iy;
    double len = sqrt(vx * vx + vy * vy);
    if (len < 1e-9)
        return CPoint(ScriptRound(ix), ScriptRound(iy));

    double d = min(max(0.0, distance), len);
    return CPoint(ScriptRound(ix + vx / len * d), ScriptRound(iy + vy / len * d));
}

CPoint ClosestLineEndpointToPoint(const CLineEntity* pLine, double x, double y)
{
    double sx = (double)pLine->m_ptStart.x - x;
    double sy = (double)pLine->m_ptStart.y - y;
    double ex = (double)pLine->m_ptEnd.x - x;
    double ey = (double)pLine->m_ptEnd.y - y;
    double dStart = sqrt(sx * sx + sy * sy);
    double dEnd = sqrt(ex * ex + ey * ey);
    return dStart <= dEnd ? pLine->m_ptStart : pLine->m_ptEnd;
}

CPoint ChamferPointOnSegment(CPoint start, CPoint end, double ix, double iy,
                             double distance, CPoint& endpointToTrim)
{
    CLineEntity temp(start, end);
    endpointToTrim = ClosestLineEndpointToPoint(&temp, ix, iy);
    CPoint directionEndpoint = (endpointToTrim == start) ? end : start;
    return PointFromIntersectionTowardEndpoint(ix, iy, directionEndpoint, distance);
}

bool ComputeFilletGeometry(CPoint s1, CPoint e1, CPoint s2, CPoint e2, double radius,
                           CPoint& trim1, CPoint& trim2, CPoint& tan1, CPoint& tan2,
                           CPoint& center, int& actualRadius)
{
    double ix = 0.0, iy = 0.0;
    if (!IntersectInfiniteLines(s1, e1, s2, e2, ix, iy))
        return false;

    CLineEntity line1(s1, e1);
    CLineEntity line2(s2, e2);
    trim1 = ClosestLineEndpointToPoint(&line1, ix, iy);
    trim2 = ClosestLineEndpointToPoint(&line2, ix, iy);
    CPoint dirEnd1 = (trim1 == s1) ? e1 : s1;
    CPoint dirEnd2 = (trim2 == s2) ? e2 : s2;

    double u1x = dirEnd1.x - ix, u1y = dirEnd1.y - iy;
    double u2x = dirEnd2.x - ix, u2y = dirEnd2.y - iy;
    double len1 = sqrt(u1x * u1x + u1y * u1y);
    double len2 = sqrt(u2x * u2x + u2y * u2y);
    if (len1 < 1e-6 || len2 < 1e-6 || radius <= 0.0)
        return false;

    u1x /= len1; u1y /= len1;
    u2x /= len2; u2y /= len2;
    double dot = max(-1.0, min(1.0, u1x * u2x + u1y * u2y));
    double theta = acos(dot);
    if (theta < 1e-3 || fabs(theta - M_PI) < 1e-3)
        return false;

    double tangent = radius / tan(theta / 2.0);
    tangent = min(tangent, min(len1, len2));
    double r = tangent * tan(theta / 2.0);
    if (r < 1.0)
        return false;

    tan1 = CPoint(ScriptRound(ix + u1x * tangent), ScriptRound(iy + u1y * tangent));
    tan2 = CPoint(ScriptRound(ix + u2x * tangent), ScriptRound(iy + u2y * tangent));

    double bx = u1x + u2x, by = u1y + u2y;
    double bl = sqrt(bx * bx + by * by);
    if (bl < 1e-6)
        return false;
    bx /= bl; by /= bl;
    double centerDist = r / sin(theta / 2.0);
    center = CPoint(ScriptRound(ix + bx * centerDist), ScriptRound(iy + by * centerDist));
    actualRadius = max(1, ScriptRound(r));
    return true;
}

void AppendArcApprox(std::vector<CPoint>& out, CPoint center, int radius, CPoint from, CPoint to)
{
    double a1 = atan2((double)(from.y - center.y), (double)(from.x - center.x));
    double a2 = atan2((double)(to.y - center.y), (double)(to.x - center.x));
    double sweep = a2 - a1;
    while (sweep > M_PI) sweep -= 2.0 * M_PI;
    while (sweep < -M_PI) sweep += 2.0 * M_PI;

    int segments = max(12, min(48, radius / 4));
    for (int i = 0; i <= segments; ++i) {
        double t = (double)i / segments;
        double a = a1 + sweep * t;
        CPoint p(ScriptRound(center.x + radius * cos(a)),
                 ScriptRound(center.y + radius * sin(a)));
        if (out.empty() || out.back() != p)
            out.push_back(p);
    }
}

void AppendPolylineArcApprox(std::vector<CPoint>& vertices, CPoint start, CPoint end)
{
    double dx = end.x - start.x;
    double dy = end.y - start.y;
    double chord = sqrt(dx * dx + dy * dy);
    if (chord < 1.0)
        return;

    CPoint center((start.x + end.x) / 2, (start.y + end.y) / 2);
    int radius = max(1, ScriptRound(chord / 2.0));
    std::vector<CPoint> arcPts;
    AppendArcApprox(arcPts, center, radius, start, end);
    for (size_t i = 1; i < arcPts.size(); ++i)
        vertices.push_back(arcPts[i]);
}

static bool FitCircle3P(CPoint p1, CPoint p2, CPoint p3, CPoint& center, int& radius)
{
    double x1 = p1.x, y1 = p1.y;
    double x2 = p2.x, y2 = p2.y;
    double x3 = p3.x, y3 = p3.y;
    double d = 2.0 * (x1*(y2-y3) + x2*(y3-y1) + x3*(y1-y2));
    if (fabs(d) < 1e-6) return false;
    double cx = ((x1*x1 + y1*y1)*(y2-y3) + (x2*x2 + y2*y2)*(y3-y1) + (x3*x3 + y3*y3)*(y1-y2)) / d;
    double cy = ((x1*x1 + y1*y1)*(x3-x2) + (x2*x2 + y2*y2)*(x1-x3) + (x3*x3 + y3*y3)*(x2-x1)) / d;
    center = CPoint((int)cx, (int)cy);
    radius = (int)Distance(center, p1);
    return radius >= 1;
}

static bool DetectArcInPolyline(const CPolylineEntity* poly, CPoint screenPt,
                                double scale, CPoint offset,
                                CPoint& center, int& radius,
                                CPoint& ptStart, CPoint& ptMid, CPoint& ptEnd,
                                int& idxStart, int& idxEnd)
{
    const auto& v = poly->m_vertices;
    int n = (int)v.size();
    if (n < 5) return false;

    int nearestSeg = -1;
    double bestDist = 1e9;
    for (int i = 0; i < n - 1; ++i) {
        CPoint p1((int)(v[i].x * scale + offset.x), (int)(-v[i].y * scale + offset.y));
        CPoint p2((int)(v[i+1].x * scale + offset.x), (int)(-v[i+1].y * scale + offset.y));
        double d = PointToLineDistance(screenPt, p1, p2);
        if (d < bestDist) { bestDist = d; nearestSeg = i; }
    }
    if (nearestSeg < 0 || bestDist > 15.0) return false;

    int left = nearestSeg;
    int right = nearestSeg + 1;
    if (right - left < 2) {
        if (left > 0) left--;
        if (right < n - 1) right++;
    }
    if (right - left < 2) return false;

    CPoint refCenter;
    int refRadius;
    int mid = (left + right) / 2;
    if (!FitCircle3P(v[left], v[mid], v[right], refCenter, refRadius))
        return false;
    if (refRadius < 1 || refRadius > 50000)
        return false;

    const int TOL = 5;
    for (int i = left; i <= right; ++i) {
        int d = (int)Distance(v[i], refCenter);
        if (abs(d - refRadius) > TOL) return false;
    }

    while (left > 0) {
        int d = (int)Distance(v[left-1], refCenter);
        if (abs(d - refRadius) > TOL) break;
        left--;
    }
    while (right < n - 1) {
        int d = (int)Distance(v[right+1], refCenter);
        if (abs(d - refRadius) > TOL) break;
        right++;
    }

    if (right - left < 3) return false;

    if (!FitCircle3P(v[left], v[(left+right)/2], v[right], refCenter, refRadius))
        return false;

    center = refCenter;
    radius = refRadius;
    ptStart = v[left];
    ptEnd = v[right];
    idxStart = left;
    idxEnd = right;

    double angS = atan2((double)(ptStart.y - center.y), (double)(ptStart.x - center.x));
    double angE = atan2((double)(ptEnd.y - center.y), (double)(ptEnd.x - center.x));
    double sweep = angE - angS;
    if (sweep < 0) sweep += 2.0 * M_PI;
    double angMid = angS + sweep / 2.0;
    ptMid = CPoint((int)(center.x + radius * cos(angMid) + 0.5),
                   (int)(center.y + radius * sin(angMid) + 0.5));
    return true;
}
}

IMPLEMENT_DYNCREATE(CLargeHWView, CView)

// Text input dialog
class CTextInputDlg : public CDialog
{
public:
    CString m_strText;
    int     m_nHeight;

    CTextInputDlg(CWnd* pParent = nullptr)
        : CDialog(IDD_TEXT_INPUT, pParent)
        , m_strText(_T(""))
        , m_nHeight(20)
    {}

protected:
    virtual BOOL OnInitDialog() override
    {
        CDialog::OnInitDialog();
        SetDlgItemText(IDC_TEXT_CONTENT, L"");
        SetDlgItemInt(IDC_TEXT_HEIGHT, 20, FALSE);
        GetDlgItem(IDC_TEXT_CONTENT)->SetFocus();
        return FALSE;
    }

    virtual void OnOK() override
    {
        GetDlgItemText(IDC_TEXT_CONTENT, m_strText);
        m_nHeight = GetDlgItemInt(IDC_TEXT_HEIGHT, NULL, FALSE);
        if (m_nHeight < 1) m_nHeight = 20;
        if (m_strText.IsEmpty())
            m_strText = L"Text";
        CDialog::OnOK();
    }

    DECLARE_MESSAGE_MAP()
};

BEGIN_MESSAGE_MAP(CTextInputDlg, CDialog)
END_MESSAGE_MAP()

BEGIN_MESSAGE_MAP(CLargeHWView, CView)
    ON_COMMAND(ID_FILE_PRINT, &CView::OnFilePrint)
    ON_COMMAND(ID_FILE_PRINT_DIRECT, &CView::OnFilePrint)
    ON_COMMAND(ID_FILE_PRINT_PREVIEW, &CView::OnFilePrintPreview)

    ON_WM_LBUTTONDOWN()
    ON_WM_LBUTTONUP()
    ON_WM_MOUSEMOVE()
    ON_WM_RBUTTONDOWN()
    ON_WM_MBUTTONDOWN()
    ON_WM_MBUTTONUP()
    ON_WM_MOUSEWHEEL()
    ON_WM_KEYDOWN()
    ON_WM_SETCURSOR()
    ON_WM_SIZE()
    ON_WM_CONTEXTMENU()

    ON_COMMAND(ID_DRAW_LINE,       &CLargeHWView::OnDrawLine)
    ON_COMMAND(ID_DRAW_POLYLINE,   &CLargeHWView::OnDrawPolyline)
    ON_COMMAND(ID_DRAW_CIRCLE,     &CLargeHWView::OnDrawCircle)
    ON_COMMAND(ID_DRAW_ARC,        &CLargeHWView::OnDrawArc)
    ON_COMMAND(ID_DRAW_RECTANGLE,  &CLargeHWView::OnDrawRectangle)
    ON_COMMAND(ID_DRAW_POLYGON,    &CLargeHWView::OnDrawPolygon)
    ON_COMMAND(ID_DRAW_ELLIPSE,    &CLargeHWView::OnDrawEllipse)
    ON_COMMAND(ID_DRAW_TEXT,       &CLargeHWView::OnDrawText)
    ON_COMMAND(ID_DRAW_DIMENSION_LENGTH, &CLargeHWView::OnDrawDimLength)
    ON_COMMAND(ID_DRAW_DIMENSION_ANGLE,  &CLargeHWView::OnDrawDimAngle)
    ON_COMMAND(ID_DRAW_DIM_LENGTH_ALIGNED, &CLargeHWView::OnDrawDimLengthAligned)
    ON_COMMAND(ID_DRAW_DIM_LENGTH_HORIZ, &CLargeHWView::OnDrawDimLengthHoriz)
    ON_COMMAND(ID_DRAW_DIM_LENGTH_VERT, &CLargeHWView::OnDrawDimLengthVert)
    ON_COMMAND(ID_DRAW_DIMENSION_RADIUS, &CLargeHWView::OnDrawDimRadius)
    ON_COMMAND(ID_DRAW_DIMENSION_DIAMETER, &CLargeHWView::OnDrawDimDiameter)
    ON_COMMAND(ID_DRAW_DIMENSION_ARCLENGTH, &CLargeHWView::OnDrawDimArcLength)
    ON_COMMAND(ID_DRAW_DIMENSION_COORDINATE, &CLargeHWView::OnDrawDimCoordinate)

    ON_COMMAND(ID_MODIFY_MOVE,     &CLargeHWView::OnModifyMove)
    ON_COMMAND(ID_MODIFY_COPY,     &CLargeHWView::OnModifyCopy)
    ON_COMMAND(ID_MODIFY_ROTATE,   &CLargeHWView::OnModifyRotate)
    ON_COMMAND(ID_MODIFY_SCALE,    &CLargeHWView::OnModifyScale)
    ON_COMMAND(ID_MODIFY_DELETE,   &CLargeHWView::OnModifyDelete)
    ON_COMMAND(ID_MODIFY_MIRROR,   &CLargeHWView::OnModifyMirror)
    ON_COMMAND(ID_MODIFY_OFFSET,   &CLargeHWView::OnModifyOffset)
    ON_COMMAND(ID_MODIFY_CHAMFER,  &CLargeHWView::OnModifyChamfer)
    ON_COMMAND(ID_MODIFY_FILLET,   &CLargeHWView::OnModifyFillet)
    ON_COMMAND(ID_MODIFY_ARRAY,    &CLargeHWView::OnModifyArray)

    ON_COMMAND(ID_EDIT_UNDO,       &CLargeHWView::OnEditUndo)
    ON_COMMAND(ID_EDIT_REDO,       &CLargeHWView::OnEditRedo)
    ON_COMMAND(ID_CAD_UNDO,        &CLargeHWView::OnEditUndo)
    ON_COMMAND(ID_CAD_REDO,        &CLargeHWView::OnEditRedo)
    ON_COMMAND(ID_EDIT_SELECTALL,  &CLargeHWView::OnEditSelectAll)

    ON_UPDATE_COMMAND_UI(ID_EDIT_UNDO, &CLargeHWView::OnUpdateEditUndo)
    ON_UPDATE_COMMAND_UI(ID_EDIT_REDO, &CLargeHWView::OnUpdateEditRedo)

    ON_COMMAND(ID_VIEW_ZOOM_EXTENTS,&CLargeHWView::OnViewZoomExtents)
    ON_COMMAND(ID_VIEW_ZOOM_WINDOW, &CLargeHWView::OnViewZoomWindow)
    ON_COMMAND(ID_VIEW_PAN,         &CLargeHWView::OnViewPan)
    ON_COMMAND(ID_VIEW_GRID,        &CLargeHWView::OnViewGrid)
    ON_COMMAND(ID_VIEW_SNAP,        &CLargeHWView::OnViewSnap)
    ON_COMMAND(ID_VIEW_ORTHO,       &CLargeHWView::OnViewOrtho)

    ON_COMMAND(ID_COLOR_RED,       &CLargeHWView::OnColorRed)
    ON_COMMAND(ID_COLOR_YELLOW,    &CLargeHWView::OnColorYellow)
    ON_COMMAND(ID_COLOR_GREEN,     &CLargeHWView::OnColorGreen)
    ON_COMMAND(ID_COLOR_CYAN,      &CLargeHWView::OnColorCyan)
    ON_COMMAND(ID_COLOR_BLUE,      &CLargeHWView::OnColorBlue)
    ON_COMMAND(ID_COLOR_MAGENTA,   &CLargeHWView::OnColorMagenta)
    ON_COMMAND(ID_COLOR_WHITE,     &CLargeHWView::OnColorWhite)
    ON_COMMAND(ID_LINETYPE_SOLID,  &CLargeHWView::OnLinetypeSolid)
    ON_COMMAND(ID_LINETYPE_DASH,   &CLargeHWView::OnLinetypeDash)
    ON_COMMAND(ID_LINETYPE_DOT,    &CLargeHWView::OnLinetypeDot)
    ON_COMMAND(ID_LINETYPE_DASHDOT,&CLargeHWView::OnLinetypeDashDot)
    ON_COMMAND(ID_LINEWEIGHT_1,    &CLargeHWView::OnLineweight1)
    ON_COMMAND(ID_LINEWEIGHT_2,    &CLargeHWView::OnLineweight2)
    ON_COMMAND(ID_LINEWEIGHT_3,    &CLargeHWView::OnLineweight3)
    ON_COMMAND(ID_LINEWEIGHT_4,    &CLargeHWView::OnLineweight4)

    ON_COMMAND(ID_CONTEXT_CANCEL,  &CLargeHWView::OnCancelCommand)
    ON_COMMAND(ID_CONTEXT_REPEAT,  &CLargeHWView::OnContextRepeat)
    ON_COMMAND(ID_FORMAT_LAYER,    &CLargeHWView::OnFormatLayer)
    ON_COMMAND(ID_SCRIPT_RUN,      &CLargeHWView::OnScriptRun)
    ON_COMMAND(ID_SCRIPT_RECORD_START, &CLargeHWView::OnScriptRecordStart)
    ON_COMMAND(ID_SCRIPT_RECORD_STOP,  &CLargeHWView::OnScriptRecordStop)
    ON_UPDATE_COMMAND_UI(ID_SCRIPT_RUN, &CLargeHWView::OnUpdateScriptRun)
    ON_UPDATE_COMMAND_UI(ID_SCRIPT_RECORD_START, &CLargeHWView::OnUpdateScriptRecordStart)
    ON_UPDATE_COMMAND_UI(ID_SCRIPT_RECORD_STOP,  &CLargeHWView::OnUpdateScriptRecordStop)
END_MESSAGE_MAP()

// Constructor/Destructor
CLargeHWView::CLargeHWView() noexcept
    : m_bDrawing(false)
    , m_bDragging(false)
    , m_ptDragStart(0, 0)
    , m_nGripIndex(-1)
    , m_pGripEntity(nullptr)
    , m_nPolygonSides(6)
    , m_bArcAltHalf(false)
    , m_bPolylineClose(false)
    , m_bPolylineArcMode(false)
    , m_nPolylineWidth(1)
    , m_nPolylineStartWidth(1)
    , m_nPolylineEndWidth(1)
    , m_pActivePolyline(nullptr)
    , m_pChamferFirst(nullptr)
    , m_chamferFirstSegment()
    , m_dChamferDistance(20.0)
    , m_filletFirstSegment()
    , m_dFilletRadius(20.0)
    , m_nArrayRows(2)
    , m_nArrayColumns(2)
    , m_dArrayRowSpacing(50.0)
    , m_dArrayColumnSpacing(50.0)
    , m_bPanning(false)
    , m_ptPanStart(0, 0)
    , m_ptPanOffsetStart(0, 0)
    , m_ptSnapped(0, 0)
    , m_bSnapActive(false)
    , m_nSnapType(SNAP_NONE)
    , m_nLastCommandID(0)
    , m_currentColor(RGB(0, 255, 209))
    , m_currentLineStyle(PS_SOLID)
    , m_currentLineWidth(1)
    , m_pDimEnt1(nullptr)
    , m_pDimEnt2(nullptr)
    , m_pDimRadiusSrcEnt(nullptr)
    , m_pDimDiamSrcEnt(nullptr)
    , m_pDimArcLenSrcEnt(nullptr)
    , m_bDimRadiusSrcTemp(false)
    , m_bDimDiamSrcTemp(false)
    , m_bDimArcLenSrcTemp(false)
    , m_bCoordDimMode(false)
    , m_ptCoordPoint(0,0)
    , m_bScriptRecording(false)
    , m_bRunningScript(false)
    , m_bSubmittingCommandLine(false)
    , m_dScriptCoordinateScale(1.0)
{
}

CLargeHWView::~CLargeHWView()
{
    if (m_bScriptRecording) {
        m_scriptRecordFile.Close();
        m_bScriptRecording = false;
    }
}

BOOL CLargeHWView::PreCreateWindow(CREATESTRUCT& cs)
{
    return CView::PreCreateWindow(cs);
}

// ============================================================
// OnDraw - Main render (AutoCAD-style dark model space)
// ============================================================
void CLargeHWView::OnDraw(CDC* pDC)
{
    CLargeHWDoc* pDoc = GetDocument();
    ASSERT_VALID(pDoc);
    if (!pDoc) return;

    CRect rcClient;
    GetClientRect(&rcClient);

    // Dark background (AutoCAD model space: #1A1A1A)
    COLORREF bgColor = RGB(26, 26, 26);
    pDC->FillSolidRect(&rcClient, bgColor);

    // Grid
    if (pDoc->m_bShowGrid) DrawGrid(pDC);

    // Border
    CPen borderPen(PS_SOLID, 1, RGB(60, 60, 60));
    CPen* pOldPen = pDC->SelectObject(&borderPen);
    CBrush* pOldBrush = (CBrush*)pDC->SelectStockObject(NULL_BRUSH);
    pDC->Rectangle(&rcClient);

    // Entities
    DrawEntities(pDC);

    static int drawCount = 0;
    drawCount++;
    if (drawCount % 30 == 1)
        TRACE(L"[DEBUG] OnDraw #%d: entCount=%d, state=%d, scale=%.2f, offset=(%d,%d)\n",
              drawCount, (int)pDoc->GetEntities().size(), (int)pDoc->m_drawState, pDoc->m_dScale,
              pDoc->m_ptOffset.x, pDoc->m_ptOffset.y);

    // Preview (rubber band)
    if (m_bDrawing) DrawPreview(pDC);

    // UCS icon
    DrawUCSIcon(pDC);

    // Snap marker
    if (m_bSnapActive)
        DrawSnapMarker(pDC);

    // Crosshair cursor
    CPoint cursorPt;
    GetCursorPos(&cursorPt);
    ScreenToClient(&cursorPt);
    if (rcClient.PtInRect(cursorPt))
        DrawCrosshair(pDC, cursorPt);

    pDC->SelectObject(pOldPen);
    pDC->SelectObject(pOldBrush);
}

// ============================================================
// Draw grid (AutoCAD-style light gray dotted grid)
// ============================================================
void CLargeHWView::DrawGrid(CDC* pDC)
{
    CLargeHWDoc* pDoc = GetDocument();
    CRect rcClient;
    GetClientRect(&rcClient);

    int spacing = pDoc->m_nGridSpacing;
    int scaledSpacing = (int)(spacing * pDoc->m_dScale);
    if (scaledSpacing < 10) scaledSpacing = spacing;

    int offX = pDoc->m_ptOffset.x % scaledSpacing;
    int offY = pDoc->m_ptOffset.y % scaledSpacing;

    if (scaledSpacing < 5) return;

    CPen gridPen(PS_DOT, 0, RGB(40, 40, 40));
    CPen* pOldPen = pDC->SelectObject(&gridPen);

    for (int x = offX; x < rcClient.right; x += scaledSpacing) {
        pDC->MoveTo(x, 0);
        pDC->LineTo(x, rcClient.bottom);
    }
    for (int y = offY; y < rcClient.bottom; y += scaledSpacing) {
        pDC->MoveTo(0, y);
        pDC->LineTo(rcClient.right, y);
    }

    pDC->SelectObject(pOldPen);
}

// ============================================================
// Crosshair cursor (AutoCAD-style)
// ============================================================
void CLargeHWView::DrawCrosshair(CDC* pDC, CPoint pt)
{
    CRect rcClient;
    GetClientRect(&rcClient);

    CPen crossPen(PS_SOLID, 1, RGB(200, 200, 200));
    CPen* pOldPen = pDC->SelectObject(&crossPen);
    int oldROP = pDC->SetROP2(R2_XORPEN);

    pDC->MoveTo(0, pt.y);
    pDC->LineTo(rcClient.right, pt.y);
    pDC->MoveTo(pt.x, 0);
    pDC->LineTo(pt.x, rcClient.bottom);

    // Center box (5x5)
    pDC->MoveTo(pt.x - 5, pt.y - 5);
    pDC->LineTo(pt.x + 5, pt.y - 5);
    pDC->LineTo(pt.x + 5, pt.y + 5);
    pDC->LineTo(pt.x - 5, pt.y + 5);
    pDC->LineTo(pt.x - 5, pt.y - 5);

    pDC->SetROP2(oldROP);
    pDC->SelectObject(pOldPen);
}

// ============================================================
// Draw all entities
// ============================================================
void CLargeHWView::DrawEntities(CDC* pDC)
{
    CLargeHWDoc* pDoc = GetDocument();
    const auto& ents = pDoc->GetEntities();
    static int entTraceCount = 0;
    entTraceCount++;
    if (entTraceCount % 30 == 1)
        TRACE(L"[DEBUG] DrawEntities #%d: entCount=%d\n", entTraceCount, (int)ents.size());
    for (size_t i = 0; i < ents.size(); ++i) {
        CEntity* p = const_cast<CEntity*>(ents[i]);
        p->Draw(pDC, pDoc->m_dScale, pDoc->m_ptOffset);
        if (p->m_bSelected) {
            CRect bounds = p->GetBounds();
            // Radius/Diameter/Arc-length dimension: no selection highlight needed
            CadDrawState st = pDoc->m_drawState;
            if (st == STATE_DRAW_DIM_RADIUS_SELECT || st == STATE_DRAW_DIM_RADIUS_POS ||
                st == STATE_DRAW_DIM_DIAMETER_SELECT || st == STATE_DRAW_DIM_DIAMETER_POS ||
                st == STATE_DRAW_DIM_ARCLEN_SELECT || st == STATE_DRAW_DIM_ARCLEN_POS) {
                // skip
            } else if (st >= STATE_DRAW_DIM_LENGTH_P1 && st <= STATE_DRAW_DIM_COORD_PICK) {
        // draw selection overlay for different entity types (thick cyan stroke)
        for (size_t j = 0; j < ents.size(); ++j) {
            CEntity* e2 = const_cast<CEntity*>(ents[j]);
            if (!e2->m_bSelected) continue;
            // line
            if (auto ln = dynamic_cast<CLineEntity*>(e2)) {
                CPoint p1 = WorldToScreen(ln->m_ptStart);
                CPoint p2 = WorldToScreen(ln->m_ptEnd);
                CPen selPen(PS_SOLID, max(2, ln->m_nLineWidth + 2), RGB(0,255,255));
                CPen* pOldPen2 = pDC->SelectObject(&selPen);
                pDC->MoveTo(p1); pDC->LineTo(p2);
                pDC->SelectObject(pOldPen2);
                continue;
            }
            // polyline
            if (auto pl = dynamic_cast<CPolylineEntity*>(e2)) {
                CPen selPen(PS_SOLID, max(2, pl->m_nLineWidth + 2), RGB(0,255,255));
                CPen* pOldPen2 = pDC->SelectObject(&selPen);
                if (!pl->m_vertices.empty()) {
                    pDC->MoveTo(WorldToScreen(pl->m_vertices[0]));
                    for (size_t k = 1; k < pl->m_vertices.size(); ++k)
                        pDC->LineTo(WorldToScreen(pl->m_vertices[k]));
                }
                pDC->SelectObject(pOldPen2);
                continue;
            }
            // circle
            if (auto cir = dynamic_cast<CCircleEntity*>(e2)) {
                CPoint c = WorldToScreen(cir->m_ptCenter);
                int r2 = (int)(cir->m_nRadius * pDoc->m_dScale);
                if (r2 < 1) r2 = 1;
                CPen selPen(PS_SOLID, max(2, cir->m_nLineWidth + 2), RGB(0,255,255));
                CPen* pOldPen2 = pDC->SelectObject(&selPen);
                CBrush* pOldBr2 = (CBrush*)pDC->SelectStockObject(NULL_BRUSH);
                pDC->Ellipse(c.x - r2, c.y - r2, c.x + r2, c.y + r2);
                pDC->SelectObject(pOldPen2);
                pDC->SelectObject(pOldBr2);
                continue;
            }
            // polygon
            if (auto poly = dynamic_cast<CPolygonEntity*>(e2)) {
                std::vector<CPoint> verts; poly->GetVertices(verts);
                if (!verts.empty()) {
                    CPen selPen(PS_SOLID, max(2, poly->m_nLineWidth + 2), RGB(0,255,255));
                    CPen* pOldPen2 = pDC->SelectObject(&selPen);
                    pDC->MoveTo(WorldToScreen(verts[0]));
                    for (size_t k = 1; k < verts.size(); ++k) pDC->LineTo(WorldToScreen(verts[k]));
                    pDC->LineTo(WorldToScreen(verts[0]));
                    pDC->SelectObject(pOldPen2);
                }
                continue;
            }
            // rectangle
            if (auto rect = dynamic_cast<CRectangleEntity*>(e2)) {
                CPoint p1 = WorldToScreen(rect->m_ptCorner1);
                CPoint p2 = WorldToScreen(rect->m_ptCorner2);
                int l = min(p1.x, p2.x), r3 = max(p1.x, p2.x);
                int t = min(p1.y, p2.y), b = max(p1.y, p2.y);
                CPen selPen(PS_SOLID, max(2, rect->m_nLineWidth + 2), RGB(0,255,255));
                CPen* pOldPen2 = pDC->SelectObject(&selPen);
                CBrush* pOldBr2 = (CBrush*)pDC->SelectStockObject(NULL_BRUSH);
                pDC->Rectangle(l, t, r3, b);
                pDC->SelectObject(pOldPen2);
                pDC->SelectObject(pOldBr2);
                continue;
            }
            // ellipse
            if (auto el = dynamic_cast<CEllipseEntity*>(e2)) {
                CPoint c = WorldToScreen(el->m_ptCenter);
                int rx = (int)(el->m_nRadiusX * pDoc->m_dScale);
                int ry = (int)(el->m_nRadiusY * pDoc->m_dScale);
                CPen selPen(PS_SOLID, max(2, el->m_nLineWidth + 2), RGB(0,255,255));
                CPen* pOldPen2 = pDC->SelectObject(&selPen);
                CBrush* pOldBr2 = (CBrush*)pDC->SelectStockObject(NULL_BRUSH);
                pDC->Ellipse(c.x - rx, c.y - ry, c.x + rx, c.y + ry);
                pDC->SelectObject(pOldPen2);
                pDC->SelectObject(pOldBr2);
                continue;
            }
            // arc (approximate by sampling)
            if (auto ar = dynamic_cast<CArcEntity*>(e2)) {
                CPoint c = WorldToScreen(ar->m_ptCenter);
                int r2 = (int)(ar->m_nRadius * pDoc->m_dScale);
                if (r2 < 1) r2 = 1;
                double angStart = atan2((double)(ar->m_ptStart.y - ar->m_ptCenter.y), (double)(ar->m_ptStart.x - ar->m_ptCenter.x));
                double angEnd   = atan2((double)(ar->m_ptEnd.y - ar->m_ptCenter.y),   (double)(ar->m_ptEnd.x - ar->m_ptCenter.x));
                double sweep = angEnd - angStart; if (sweep < 0) sweep += 2*M_PI;
                int steps = max(8, (int)ceil(sweep / (M_PI/36.0)));
                CPen selPen(PS_SOLID, max(2, ar->m_nLineWidth + 2), RGB(0,255,255));
                CPen* pOldPen2 = pDC->SelectObject(&selPen);
                for (int k = 0; k <= steps; ++k) {
                    double ang = angStart + sweep * (double)k / (double)steps;
                    CPoint p((int)floor(c.x + r2 * cos(ang) + 0.5), (int)floor(c.y + r2 * sin(ang) + 0.5));
                    if (k == 0) pDC->MoveTo(p); else pDC->LineTo(p);
                }
                pDC->SelectObject(pOldPen2);
                continue;
            }
        }
            } else {
            CRect screenBounds = p->ToScreenRect(bounds, pDoc->m_dScale, pDoc->m_ptOffset);
            screenBounds.NormalizeRect();
            screenBounds.InflateRect(3, 3);
            CPen hlPen(PS_SOLID, 2, RGB(0, 255, 255));
            CPen* pOld = pDC->SelectObject(&hlPen);
            CBrush* pOldBr = (CBrush*)pDC->SelectStockObject(NULL_BRUSH);
            pDC->Rectangle(&screenBounds);
            pDC->SelectObject(pOld);
            pDC->SelectObject(pOldBr);
            }
        }
    }
}

// ============================================================
// Draw preview (rubber band)
// ============================================================
void CLargeHWView::DrawPreview(CDC* pDC)
{
    CLargeHWDoc* pDoc = GetDocument();
    TRACE(L"[TRACE] Enter DrawPreview: state=%d, m_bDrawing=%d, tempPts=%d, pDim1=%p pDim2=%p\n",
          (int)pDoc->m_drawState, (int)m_bDrawing, (int)m_tempPts.size(), m_pDimEnt1, m_pDimEnt2);
    if (m_tempPts.empty()) {
        // still allow preview/highlight when in angular-dimension selection states
        if (!(pDoc->m_drawState == STATE_DRAW_DIM_ANGLE_SELECT_E1 ||
              pDoc->m_drawState == STATE_DRAW_DIM_ANGLE_SELECT_E2 ||
              pDoc->m_drawState == STATE_DRAW_DIM_ANGLE_POS ||
              pDoc->m_drawState == STATE_DRAW_DIM_RADIUS_SELECT ||
              pDoc->m_drawState == STATE_DRAW_DIM_RADIUS_POS ||
              pDoc->m_drawState == STATE_DRAW_DIM_DIAMETER_SELECT ||
              pDoc->m_drawState == STATE_DRAW_DIM_DIAMETER_POS ||
              pDoc->m_drawState == STATE_DRAW_DIM_ARCLEN_SELECT ||
              pDoc->m_drawState == STATE_DRAW_DIM_ARCLEN_POS ||
              (pDoc->m_drawState == STATE_DRAW_TEXT_POS && m_pPendingDim)))
            return;
    }

    CPen previewPen(PS_DASH, 1, RGB(150, 150, 150));
    CPen* pOldPen = pDC->SelectObject(&previewPen);
    int oldROP = pDC->SetROP2(R2_NOTXORPEN);

    CPoint curWorld = m_bSnapActive ? ScreenToWorld(m_ptSnapped) : ScreenToWorld(m_ptCurrent);
    CPoint cursorPt = m_bSnapActive ? m_ptSnapped : m_ptCurrent;

    switch (pDoc->m_drawState) {
    case STATE_DRAW_LINE_P2:
    case STATE_DRAW_RECT_P2:
        if (m_tempPts.size() >= 1) {
            CPoint p1 = WorldToScreen(
                pDoc->m_drawState == STATE_DRAW_LINE_P2 ? m_tempPts.back() : m_tempPts[0]);
            pDC->MoveTo(p1);
            pDC->LineTo(cursorPt);
        }
        break;

    case STATE_DRAW_DIM_LENGTH_P1:
        // waiting for first point; nothing to preview
        break;
    case STATE_DRAW_DIM_LENGTH_P2:
        if (m_tempPts.size() >= 1) {
            CPoint p1 = WorldToScreen(m_tempPts[0]);
            pDC->MoveTo(p1);
            pDC->LineTo(cursorPt);
        }
        break;

    case STATE_DRAW_DIM_LENGTH_POS:
        // Rubber-band preview for linear dimension placement: show extension lines,
        // main dim line and arrowheads based on current mouse position
        if (m_tempPts.size() >= 2) {
            CPoint a = m_tempPts[0];
            CPoint b = m_tempPts[1];
            // determine mode
            int mode = m_nLastDimMode; // 0=aligned,1=horizontal,2=vertical
            CPoint aDimWorld = a, bDimWorld = b;
            double wx = (double)(b.x - a.x), wy = (double)(b.y - a.y);
            double wlen = sqrt(wx*wx + wy*wy);
            // determine placement (current mouse world)
            CPoint place = m_bSnapActive ? ScreenToWorld(m_ptSnapped) : ScreenToWorld(m_ptCurrent);
            // compute offset in world units
            double screenOffset = 15.0; double offWorld = 1.0;
            CLargeHWDoc* pDocLocal = GetDocument();
            if (pDocLocal && pDocLocal->m_dScale > 0.0) offWorld = screenOffset / pDocLocal->m_dScale;

            if (wlen > 1e-6) {
                if (mode == 1) {
                    // horizontal: y = place.y
                    int left = min(a.x, b.x), right = max(a.x, b.x);
                    aDimWorld = CPoint(left, place.y);
                    bDimWorld = CPoint(right, place.y);
                } else if (mode == 2) {
                    int top = min(a.y, b.y), bottom = max(a.y, b.y);
                    aDimWorld = CPoint(place.x, top);
                    bDimWorld = CPoint(place.x, bottom);
                } else {
                    // aligned mode: compute signed offset from original line to current mouse position
                    double ux = wx, uy = wy;
                    double wlen_d = sqrt(ux*ux + uy*uy);
                    double nx = -uy / wlen_d; double ny = ux / wlen_d; // unit normal
                    CPoint place = m_bSnapActive ? ScreenToWorld(m_ptSnapped) : ScreenToWorld(m_ptCurrent);
                    double pax = (double)(place.x - a.x), pay = (double)(place.y - a.y);
                    double signedOff = pax * nx + pay * ny;
                    aDimWorld = CPoint((int)floor((double)a.x + nx * signedOff + 0.5), (int)floor((double)a.y + ny * signedOff + 0.5));
                    bDimWorld = CPoint((int)floor((double)b.x + nx * signedOff + 0.5), (int)floor((double)b.y + ny * signedOff + 0.5));
                }
            } else {
                // degenerate: small offset from a
                aDimWorld = a; bDimWorld = b;
                aDimWorld.y += (int)offWorld; bDimWorld.y += (int)offWorld;
            }

            // screen coords
            CPoint sA = WorldToScreen(a);
            CPoint sB = WorldToScreen(b);
            CPoint sAD = WorldToScreen(aDimWorld);
            CPoint sBD = WorldToScreen(bDimWorld);

            // draw extension/connection lines
            if (mode == 1) {
                CPoint conn1(a.x, aDimWorld.y); CPoint conn2(b.x, bDimWorld.y);
                pDC->MoveTo(sA); pDC->LineTo(WorldToScreen(conn1));
                pDC->MoveTo(sB); pDC->LineTo(WorldToScreen(conn2));
            } else if (mode == 2) {
                CPoint conn1(aDimWorld.x, a.y); CPoint conn2(bDimWorld.x, b.y);
                pDC->MoveTo(sA); pDC->LineTo(WorldToScreen(conn1));
                pDC->MoveTo(sB); pDC->LineTo(WorldToScreen(conn2));
            } else {
                pDC->MoveTo(sA); pDC->LineTo(sAD);
                pDC->MoveTo(sB); pDC->LineTo(sBD);
            }

            // draw main dimension line
            pDC->MoveTo(sAD); pDC->LineTo(sBD);

            // arrowheads
            auto drawArrow = [&](CPoint pt, CPoint toward) {
                double dx = (double)(toward.x - pt.x);
                double dy = (double)(toward.y - pt.y);
                double d = sqrt(dx*dx + dy*dy);
                if (d < 1e-6) return;
                double ux = dx / d, uy = dy / d;
                double ang = atan2(uy, ux);
                double ang1 = ang + M_PI * 3.0 / 8.0;
                double ang2 = ang - M_PI * 3.0 / 8.0;
                int arrowLen = max(8, (int)(8 * pDocLocal->m_dScale));
                CPoint p1((int)floor(pt.x + cos(ang1) * arrowLen + 0.5), (int)floor(pt.y + sin(ang1) * arrowLen + 0.5));
                CPoint p2((int)floor(pt.x + cos(ang2) * arrowLen + 0.5), (int)floor(pt.y + sin(ang2) * arrowLen + 0.5));
                pDC->MoveTo(pt); pDC->LineTo(p1);
                pDC->MoveTo(pt); pDC->LineTo(p2);
            };

            drawArrow(sAD, sBD);
            drawArrow(sBD, sAD);

            // draw length text near middle
            double lenVal;
            if (mode == 1) lenVal = fabs((double)(b.x - a.x));
            else if (mode == 2) lenVal = fabs((double)(b.y - a.y));
            else lenVal = Distance(a, b);
            CString txt; txt.Format(L"%.2f", lenVal);
            CPoint mid((sAD.x + sBD.x)/2, (sAD.y + sBD.y)/2);
            int textOffset = max(6, (int)(6 * pDocLocal->m_dScale));
            double sx = (double)(sBD.x - sAD.x); double sy = (double)(sBD.y - sAD.y);
            double slen = sqrt(sx*sx + sy*sy);
            if (slen > 1e-6) {
                double nxs = -sy / slen; double nys = sx / slen;
                mid.x = (int)floor(mid.x + nxs * textOffset + 0.5);
                mid.y = (int)floor(mid.y + nys * textOffset + 0.5);
            }
            pDC->TextOutW(mid.x+4, mid.y+4, txt);
        }
        break;

    case STATE_DRAW_DIM_ANGLE_SELECT_E1:
        // waiting for first entity selection - highlight under cursor optionally
        // If a polyline segment was identified, draw that segment highlighted
        if (m_pDimEnt1Orig && m_nDimSegIndex1 >= 0) {
            if (auto pl = dynamic_cast<CPolylineEntity*>(m_pDimEnt1Orig)) {
                int i = m_nDimSegIndex1;
                CPoint s = WorldToScreen(pl->m_vertices[i]);
                CPoint e = WorldToScreen(pl->m_vertices[i+1]);
                CPen hl(PS_SOLID, 3, RGB(0,255,255));
                CPen* pOld = pDC->SelectObject(&hl);
                pDC->MoveTo(s); pDC->LineTo(e);
                pDC->SelectObject(pOld);
            }
        }
        break;
    case STATE_DRAW_DIM_ANGLE_SELECT_E2:
        // waiting for second entity selection
        // always draw highlight for first selected segment (if any)
        if (m_pDimEnt1Orig && m_nDimSegIndex1 >= 0) {
            if (auto pl = dynamic_cast<CPolylineEntity*>(m_pDimEnt1Orig)) {
                int i = m_nDimSegIndex1;
                CPoint s = WorldToScreen(pl->m_vertices[i]);
                CPoint e = WorldToScreen(pl->m_vertices[i+1]);
                CPen hl(PS_SOLID, 3, RGB(0,255,255));
                CPen* pOld = pDC->SelectObject(&hl);
                pDC->MoveTo(s); pDC->LineTo(e);
                pDC->SelectObject(pOld);
            }
        }
        if (m_pDimEnt2Orig && m_nDimSegIndex2 >= 0) {
            if (auto pl = dynamic_cast<CPolylineEntity*>(m_pDimEnt2Orig)) {
                int i = m_nDimSegIndex2;
                CPoint s = WorldToScreen(pl->m_vertices[i]);
                CPoint e = WorldToScreen(pl->m_vertices[i+1]);
                CPen hl(PS_SOLID, 3, RGB(0,255,255));
                CPen* pOld = pDC->SelectObject(&hl);
                pDC->MoveTo(s); pDC->LineTo(e);
                pDC->SelectObject(pOld);
            }
        }
        break;
    case STATE_DRAW_DIM_ANGLE_POS:
        // preview arc using direct rubber-band drawing (match length-dimension preview style)
        if (m_pDimEnt1 && m_pDimEnt2) {
            // compute center (intersection or midpoint fallback)
            CPoint center;
            CLineEntity* l1 = dynamic_cast<CLineEntity*>(m_pDimEnt1);
            CLineEntity* l2 = dynamic_cast<CLineEntity*>(m_pDimEnt2);
            if (l1 && l2) {
                CPoint a1 = l1->m_ptStart, a2 = l1->m_ptEnd;
                CPoint b1 = l2->m_ptStart, b2 = l2->m_ptEnd;
                double x1 = a1.x, y1 = a1.y, x2 = a2.x, y2 = a2.y;
                double x3 = b1.x, y3 = b1.y, x4 = b2.x, y4 = b2.y;
                double den = (x1 - x2)*(y3 - y4) - (y1 - y2)*(x3 - x4);
                if (fabs(den) < 1e-6) {
                    CPoint cp1 = ClosestPointOnSegment(a1, a2, b1);
                    CPoint cp2 = ClosestPointOnSegment(b1, b2, cp1);
                    center = MidPoint(cp1, cp2);
                }
                else {
                    double px = ((x1*y2 - y1*x2)*(x3 - x4) - (x1 - x2)*(x3*y4 - y3*x4)) / den;
                    double py = ((x1*y2 - y1*x2)*(y3 - y4) - (y1 - y2)*(x3*y4 - y3*x4)) / den;
                    center = CPoint((int)px, (int)py);
                }
            } else {
                if (l1) center = MidPoint(l1->m_ptStart, l1->m_ptEnd);
                else if (l2) center = MidPoint(l2->m_ptStart, l2->m_ptEnd);
                else center = CPoint(0,0);
            }

            // compute base angles from segment midpoints to avoid zero-angle when center lies on the lines
            auto vec = [](CPoint a, CPoint b){ return CPoint(b.x - a.x, b.y - a.y); };
            auto dot = [](CPoint u, CPoint v){ return (double)u.x * v.x + (double)u.y * v.y; };
            auto len = [](CPoint u){ return sqrt((double)u.x*u.x + (double)u.y*u.y); };
            auto normalize = [](double ang){ while (ang < 0) ang += 2*M_PI; while (ang >= 2*M_PI) ang -= 2*M_PI; return ang; };

            CPoint A1, A2;
            if (auto le1 = dynamic_cast<CLineEntity*>(m_pDimEnt1)) {
                A1 = MidPoint(le1->m_ptStart, le1->m_ptEnd);
            } else if (auto pl = dynamic_cast<CPolylineEntity*>(m_pDimEnt1Orig)) {
                int idx = max(0, m_nDimSegIndex1);
                A1 = MidPoint(pl->m_vertices[idx], pl->m_vertices[idx+1]);
            } else {
                A1 = center;
            }
            if (auto le2 = dynamic_cast<CLineEntity*>(m_pDimEnt2)) {
                A2 = MidPoint(le2->m_ptStart, le2->m_ptEnd);
            } else if (auto pl = dynamic_cast<CPolylineEntity*>(m_pDimEnt2Orig)) {
                int idx = max(0, m_nDimSegIndex2);
                A2 = MidPoint(pl->m_vertices[idx], pl->m_vertices[idx+1]);
            } else {
                A2 = center;
            }

            CPoint curWorld = m_bSnapActive ? ScreenToWorld(m_ptSnapped) : ScreenToWorld(m_ptCurrent);
            CPoint OA1 = vec(center, A1);
            CPoint OA2 = vec(center, A2);
            CPoint OB = vec(center, curWorld);
            double lenOA1 = len(OA1), lenOA2 = len(OA2), lenOB = len(OB);
            bool isInner = false;
            if (lenOA1 > 1e-6 && lenOA2 > 1e-6 && lenOB > 1e-6) {
                double cos1 = dot(OA1, OB) / (lenOA1 * lenOB);
                double cos2 = dot(OA2, OB) / (lenOA2 * lenOB);
                if (cos1 > 1.0) cos1 = 1.0; if (cos1 < -1.0) cos1 = -1.0;
                if (cos2 > 1.0) cos2 = 1.0; if (cos2 < -1.0) cos2 = -1.0;
                double ang1 = acos(cos1);
                double ang2 = acos(cos2);
                if (ang1 < (M_PI/2.0) && ang2 < (M_PI/2.0)) isInner = true;
            }

            double angA1 = normalize(atan2((double)OA1.y, (double)OA1.x));
            double angA2 = normalize(atan2((double)OA2.y, (double)OA2.x));
            double s1 = angA1, e1 = angA2; double sweep1 = e1 - s1; if (sweep1 < 0) sweep1 += 2*M_PI;
            double s2 = angA2, e2 = angA1; double sweep2 = e2 - s2; if (sweep2 < 0) sweep2 += 2*M_PI;
            double start = 0, end = 0, sweep = 0;
            if (isInner) {
                if (sweep1 <= M_PI) { start = s1; end = e1; sweep = sweep1; }
                else { start = s2; end = e2; sweep = sweep2; }
            } else {
                if (sweep1 > M_PI) { start = s1; end = e1; sweep = sweep1; }
                else { start = s2; end = e2; sweep = sweep2; }
            }

            // compute radius in screen pixels from cursor world distance
            double worldRad = Distance(center, curWorld);
            CLargeHWDoc* pDocLocal = GetDocument();
            int scrR = max(12, (int)floor(worldRad * pDocLocal->m_dScale + 0.5));
            if (scrR < 8) scrR = max(8, (int)(20 * pDocLocal->m_dScale));

            // draw rays and arc in screen coordinates
            CPoint sc = WorldToScreen(center);
            CPoint scrStart((int)floor(sc.x + scrR * cos(start) + 0.5), (int)floor(sc.y - scrR * sin(start) + 0.5));
            CPoint scrEnd((int)floor(sc.x + scrR * cos(end) + 0.5),   (int)floor(sc.y - scrR * sin(end) + 0.5));
            // draw with preview pen and sample the arc (Arc can be affected by ROP modes)
            pDC->MoveTo(sc); pDC->LineTo(scrStart);
            pDC->MoveTo(sc); pDC->LineTo(scrEnd);
            // sample arc into small line segments for reliable preview rendering
            int steps = max(12, (int)ceil(fabs(sweep) / (M_PI/36.0)));
            for (int k = 0; k <= steps; ++k) {
                double ang = start + sweep * (double)k / (double)steps;
                CPoint pp((int)floor(sc.x + scrR * cos(ang) + 0.5), (int)floor(sc.y - scrR * sin(ang) + 0.5));
                if (k == 0) pDC->MoveTo(pp); else pDC->LineTo(pp);
            }
        }
        break;

    case STATE_DRAW_DIM_RADIUS_SELECT:
        if (m_pDimRadiusSrcEnt) {
            CPoint center;
            int radius = 0;
            if (auto pCirc = dynamic_cast<CCircleEntity*>(m_pDimRadiusSrcEnt)) {
                center = pCirc->m_ptCenter; radius = pCirc->m_nRadius;
            } else if (auto pArc = dynamic_cast<CArcEntity*>(m_pDimRadiusSrcEnt)) {
                center = pArc->m_ptCenter; radius = pArc->m_nRadius;
            }
            if (radius > 0) {
                CPoint sc = WorldToScreen(center);
                int r = (int)(radius * pDoc->m_dScale);
                CBrush* pB = (CBrush*)pDC->SelectStockObject(NULL_BRUSH);
                CPen hl(PS_SOLID, 2, RGB(0,255,255));
                CPen* pOld = pDC->SelectObject(&hl);
                pDC->Ellipse(sc.x - r, sc.y - r, sc.x + r, sc.y + r);
                pDC->SelectObject(pOld); pDC->SelectObject(pB);
            }
        }
        break;

    case STATE_DRAW_DIM_RADIUS_POS:
        if (m_pDimRadiusSrcEnt) {
            CPoint center;
            int radius = 0;
            if (auto pCirc = dynamic_cast<CCircleEntity*>(m_pDimRadiusSrcEnt)) {
                center = pCirc->m_ptCenter; radius = pCirc->m_nRadius;
            } else if (auto pArc = dynamic_cast<CArcEntity*>(m_pDimRadiusSrcEnt)) {
                center = pArc->m_ptCenter; radius = pArc->m_nRadius;
            }
            if (radius > 0) {
                CPoint curWorld = m_bSnapActive ? ScreenToWorld(m_ptSnapped) : ScreenToWorld(m_ptCurrent);
                double dx = (double)(curWorld.x - center.x);
                double dy = (double)(curWorld.y - center.y);
                double dist = sqrt(dx*dx + dy*dy);
                if (dist < 1e-6) dist = 1.0;
                CPoint endPt((int)floor(center.x + radius * dx / dist + 0.5),
                             (int)floor(center.y + radius * dy / dist + 0.5));
                CPoint sc = WorldToScreen(center);
                CPoint se = WorldToScreen(endPt);
                pDC->MoveTo(sc); pDC->LineTo(se);
                double sdx = (double)(se.x - sc.x), sdy = (double)(se.y - sc.y);
                double sd = sqrt(sdx*sdx + sdy*sdy);
                if (sd > 1e-6) {
                    double ux = sdx / sd, uy = sdy / sd;
                    int arrowLen = max(8, (int)(8 * pDoc->m_dScale));
                    double arrowAng = atan2(uy, ux);
                    double a1 = arrowAng + M_PI * 3.0 / 8.0;
                    double a2 = arrowAng - M_PI * 3.0 / 8.0;
                    CPoint p1((int)floor(se.x - cos(a1) * arrowLen + 0.5), (int)floor(se.y - sin(a1) * arrowLen + 0.5));
                    CPoint p2((int)floor(se.x - cos(a2) * arrowLen + 0.5), (int)floor(se.y - sin(a2) * arrowLen + 0.5));
                    pDC->MoveTo(se); pDC->LineTo(p1);
                    pDC->MoveTo(se); pDC->LineTo(p2);
                }
                CString txt; txt.Format(L"%.2f", (double)radius);
                CPoint txtPt = m_bSnapActive ? WorldToScreen(m_ptSnapped) : m_ptCurrent;
                pDC->TextOutW(txtPt.x+4, txtPt.y+4, txt);
            }
        }
        break;

    case STATE_DRAW_DIM_DIAMETER_SELECT:
        if (m_pDimDiamSrcEnt) {
            CPoint center; int radius = 0;
            if (auto pCirc = dynamic_cast<CCircleEntity*>(m_pDimDiamSrcEnt)) { center = pCirc->m_ptCenter; radius = pCirc->m_nRadius; }
            else if (auto pArc = dynamic_cast<CArcEntity*>(m_pDimDiamSrcEnt)) { center = pArc->m_ptCenter; radius = pArc->m_nRadius; }
            if (radius > 0) {
                CPoint sc = WorldToScreen(center); int r = (int)(radius * pDoc->m_dScale);
                CBrush* pB = (CBrush*)pDC->SelectStockObject(NULL_BRUSH);
                CPen hl(PS_SOLID, 2, RGB(0,255,255)); CPen* pOld = pDC->SelectObject(&hl);
                pDC->Ellipse(sc.x - r, sc.y - r, sc.x + r, sc.y + r);
                pDC->SelectObject(pOld); pDC->SelectObject(pB);
            }
        }
        break;

    case STATE_DRAW_DIM_DIAMETER_POS:
        if (m_pDimDiamSrcEnt) {
            CPoint center; int radius = 0;
            if (auto pCirc = dynamic_cast<CCircleEntity*>(m_pDimDiamSrcEnt)) { center = pCirc->m_ptCenter; radius = pCirc->m_nRadius; }
            else if (auto pArc = dynamic_cast<CArcEntity*>(m_pDimDiamSrcEnt)) { center = pArc->m_ptCenter; radius = pArc->m_nRadius; }
            if (radius > 0) {
                CPoint curWorld = m_bSnapActive ? ScreenToWorld(m_ptSnapped) : ScreenToWorld(m_ptCurrent);
                double dx = (double)(curWorld.x - center.x), dy = (double)(curWorld.y - center.y);
                double dist = sqrt(dx*dx + dy*dy); if (dist < 1e-6) dist = 1.0;
                CPoint end1((int)floor(center.x - radius * dx / dist + 0.5), (int)floor(center.y - radius * dy / dist + 0.5));
                CPoint end2((int)floor(center.x + radius * dx / dist + 0.5), (int)floor(center.y + radius * dy / dist + 0.5));
                CPoint se1 = WorldToScreen(end1), se2 = WorldToScreen(end2);
                pDC->MoveTo(se1); pDC->LineTo(se2);
                double sdx = (double)(se2.x - se1.x), sdy = (double)(se2.y - se1.y);
                double sd = sqrt(sdx*sdx + sdy*sdy);
                if (sd > 1e-6) {
                    double ux = sdx / sd, uy = sdy / sd; int arrowLen = max(8, (int)(8 * pDoc->m_dScale));
                    double arrowAng = atan2(uy, ux);
                    double a1 = arrowAng + M_PI * 3.0 / 8.0, a2 = arrowAng - M_PI * 3.0 / 8.0;
                    CPoint p1((int)floor(se1.x + cos(a1) * arrowLen + 0.5), (int)floor(se1.y + sin(a1) * arrowLen + 0.5));
                    CPoint p2((int)floor(se1.x + cos(a2) * arrowLen + 0.5), (int)floor(se1.y + sin(a2) * arrowLen + 0.5));
                    pDC->MoveTo(se1); pDC->LineTo(p1); pDC->MoveTo(se1); pDC->LineTo(p2);
                    a1 = arrowAng + M_PI + M_PI * 3.0 / 8.0; a2 = arrowAng + M_PI - M_PI * 3.0 / 8.0;
                    p1 = CPoint((int)floor(se2.x + cos(a1) * arrowLen + 0.5), (int)floor(se2.y + sin(a1) * arrowLen + 0.5));
                    p2 = CPoint((int)floor(se2.x + cos(a2) * arrowLen + 0.5), (int)floor(se2.y + sin(a2) * arrowLen + 0.5));
                    pDC->MoveTo(se2); pDC->LineTo(p1); pDC->MoveTo(se2); pDC->LineTo(p2);
                }
                CString txt; txt.Format(L"%.2f", (double)(radius*2));
                CPoint txtPt = m_bSnapActive ? WorldToScreen(m_ptSnapped) : m_ptCurrent;
                pDC->TextOutW(txtPt.x+4, txtPt.y+4, txt);
            }
        }
        break;

    case STATE_DRAW_DIM_ARCLEN_SELECT:
        if (m_pDimArcLenSrcEnt) {
            if (auto pArc = dynamic_cast<CArcEntity*>(m_pDimArcLenSrcEnt)) {
                CPoint sc = WorldToScreen(pArc->m_ptCenter);
                int r = (int)(pArc->m_nRadius * pDoc->m_dScale);
                CBrush* pB = (CBrush*)pDC->SelectStockObject(NULL_BRUSH);
                CPen hl(PS_SOLID, 2, RGB(0,255,255)); CPen* pOld = pDC->SelectObject(&hl);
                pDC->Ellipse(sc.x - r, sc.y - r, sc.x + r, sc.y + r);
                pDC->SelectObject(pOld); pDC->SelectObject(pB);
            }
        }
        break;

    case STATE_DRAW_DIM_ARCLEN_POS:
        if (m_pDimArcLenSrcEnt) {
            auto pArc = dynamic_cast<CArcEntity*>(m_pDimArcLenSrcEnt);
            if (!pArc) break;
            CPoint center = pArc->m_ptCenter;
            int arcRadius = pArc->m_nRadius;
            double angStart = atan2((double)(pArc->m_ptStart.y - center.y), (double)(pArc->m_ptStart.x - center.x));
            double angEnd = atan2((double)(pArc->m_ptEnd.y - center.y), (double)(pArc->m_ptEnd.x - center.x));
            double sweep = angEnd - angStart;
            if (sweep < 0) sweep += 2*M_PI;
            double angMid = atan2((double)(pArc->m_ptMid.y - center.y), (double)(pArc->m_ptMid.x - center.x));
            double midSweep = angMid - angStart;
            if (midSweep < 0) midSweep += 2*M_PI;
            if (midSweep > sweep) { double tmp = angStart; angStart = angEnd; angEnd = tmp; sweep = 2*M_PI - sweep; }

            CPoint curWorld = m_bSnapActive ? ScreenToWorld(m_ptSnapped) : ScreenToWorld(m_ptCurrent);
            double dist = Distance(center, curWorld);
            if (dist <= arcRadius + 1) {
                CPoint txtPt = m_bSnapActive ? m_ptSnapped : m_ptCurrent;
                pDC->TextOutW(txtPt.x, txtPt.y, L"this position is illegal");
                // Still show extension lines
                CPoint arcS((int)floor(center.x + arcRadius*cos(angStart)+0.5), (int)floor(center.y + arcRadius*sin(angStart)+0.5));
                CPoint arcE((int)floor(center.x + arcRadius*cos(angEnd)+0.5), (int)floor(center.y + arcRadius*sin(angEnd)+0.5));
                CPoint sS = WorldToScreen(arcS), sE = WorldToScreen(arcE);
                pDC->MoveTo(sS); pDC->LineTo(sS.x + (int)((sS.x-WorldToScreen(center).x)*0.3), sS.y + (int)((sS.y-WorldToScreen(center).y)*0.3));
                pDC->MoveTo(sE); pDC->LineTo(sE.x + (int)((sE.x-WorldToScreen(center).x)*0.3), sE.y + (int)((sE.y-WorldToScreen(center).y)*0.3));
                break;
            }

            // Compute dimension arc endpoints
            CPoint dimS((int)floor(center.x + dist*cos(angStart)+0.5), (int)floor(center.y + dist*sin(angStart)+0.5));
            CPoint dimE((int)floor(center.x + dist*cos(angEnd)+0.5), (int)floor(center.y + dist*sin(angEnd)+0.5));
            CPoint arcS((int)floor(center.x + arcRadius*cos(angStart)+0.5), (int)floor(center.y + arcRadius*sin(angStart)+0.5));
            CPoint arcE((int)floor(center.x + arcRadius*cos(angEnd)+0.5), (int)floor(center.y + arcRadius*sin(angEnd)+0.5));
            CPoint sc = WorldToScreen(center), sDS = WorldToScreen(dimS), sDE = WorldToScreen(dimE);
            CPoint sAS = WorldToScreen(arcS), sAE = WorldToScreen(arcE);
            int sdr = (int)(dist * pDoc->m_dScale);

            // Extension lines
            pDC->MoveTo(sAS); pDC->LineTo(sDS);
            pDC->MoveTo(sAE); pDC->LineTo(sDE);

            // Dimension arc - use Arc() for exact match with final rendering
            CRect rcE(sc.x - sdr, sc.y - sdr, sc.x + sdr, sc.y + sdr);
            pDC->Arc(rcE, sDS, sDE);

            // Arrowheads at arc ends
            int arrowLen = max(8, (int)(8 * pDoc->m_dScale));
            double sweepDir = sweep > 0 ? M_PI/2.0 : -M_PI/2.0;
            auto drawArrow = [&](CPoint pt, double angle, bool isStart) {
                double tanAng = angle + (isStart ? sweepDir - M_PI : sweepDir);
                double a1 = tanAng + M_PI * 3.0 / 8.0, a2 = tanAng - M_PI * 3.0 / 8.0;
                CPoint p1((int)floor(pt.x + cos(a1)*arrowLen + 0.5), (int)floor(pt.y + sin(a1)*arrowLen + 0.5));
                CPoint p2((int)floor(pt.x + cos(a2)*arrowLen + 0.5), (int)floor(pt.y + sin(a2)*arrowLen + 0.5));
                pDC->MoveTo(pt); pDC->LineTo(p1); pDC->MoveTo(pt); pDC->LineTo(p2);
            };
            drawArrow(sDS, angStart, true);
            drawArrow(sDE, angEnd, false);

            // Text preview
            double arcLen = (double)arcRadius * fabs(sweep);
            CString txt; txt.Format(L"%.2f", arcLen);
            CPoint txtPt = m_bSnapActive ? m_ptSnapped : m_ptCurrent;
            pDC->TextOutW(txtPt.x+4, txtPt.y+4, txt);
        }
        break;

    case STATE_DRAW_TEXT_POS:
        if (m_pPendingDim) {
            if (auto pL = dynamic_cast<CDimLinearEntity*>(m_pPendingDim)) {
                CPoint place = m_bSnapActive ? ScreenToWorld(m_ptSnapped) : ScreenToWorld(m_ptCurrent);
                CPoint mid = WorldToScreen(place);
                double lenVal;
                if (pL->m_mode == CDimLinearEntity::DIM_HORIZONTAL) lenVal = fabs((double)(pL->m_pt2.x - pL->m_pt1.x));
                else if (pL->m_mode == CDimLinearEntity::DIM_VERTICAL) lenVal = fabs((double)(pL->m_pt2.y - pL->m_pt1.y));
                else lenVal = Distance(pL->m_pt1, pL->m_pt2);
                CString txt; txt.Format(L"%.2f", lenVal);
                pDC->TextOutW(mid.x+4, mid.y+4, txt);
            } else if (auto pA = dynamic_cast<CDimAngularEntity*>(m_pPendingDim)) {
                CPoint place = m_bSnapActive ? ScreenToWorld(m_ptSnapped) : ScreenToWorld(m_ptCurrent);
                double a1 = pA->m_ang1; double a2 = pA->m_ang2;
                auto norm = [](double ang){ while (ang < 0) ang += 2*M_PI; while (ang >= 2*M_PI) ang -= 2*M_PI; return ang; };
                double start = norm(a1), end = norm(a2);
                double sweep = end - start; if (sweep < 0) sweep += 2*M_PI;
                double angle = sweep; if (angle > 2*M_PI) angle = fmod(angle, 2*M_PI);
                double deg = angle * 180.0 / M_PI;
                CString txt; txt.Format(L"%.1f deg", deg);

                CPoint txtPt = WorldToScreen(place);
                pDC->TextOutW(txtPt.x, txtPt.y, txt);
            } else if (auto pR = dynamic_cast<CDimRadiusEntity*>(m_pPendingDim)) {
                CPoint place = m_bSnapActive ? ScreenToWorld(m_ptSnapped) : ScreenToWorld(m_ptCurrent);
                CString txt; txt.Format(L"%.2f", (double)pR->m_nRadius);
                CPoint txtPt = WorldToScreen(place);
                pDC->TextOutW(txtPt.x+4, txtPt.y+4, txt);
            } else if (auto pD = dynamic_cast<CDimDiamEntity*>(m_pPendingDim)) {
                CPoint place = m_bSnapActive ? ScreenToWorld(m_ptSnapped) : ScreenToWorld(m_ptCurrent);
                CString txt; txt.Format(L"%.2f", (double)(pD->m_nRadius*2));
                CPoint txtPt = WorldToScreen(place);
                pDC->TextOutW(txtPt.x+4, txtPt.y+4, txt);
            } else if (auto pAL = dynamic_cast<CDimArcLengthEntity*>(m_pPendingDim)) {
                CPoint place = m_bSnapActive ? ScreenToWorld(m_ptSnapped) : ScreenToWorld(m_ptCurrent);
                double arcLen = (double)pAL->m_nArcRadius * fabs(pAL->m_dSweep);
                CString txt; txt.Format(L"%.2f", arcLen);
                CPoint txtPt = WorldToScreen(place);
                pDC->TextOutW(txtPt.x+4, txtPt.y+4, txt);
            }
        }
        if (m_bCoordDimMode && m_tempPts.size() >= 1) {
            CPoint screenPt = WorldToScreen(m_ptCoordPoint);
            int crossLen = 4;
            pDC->MoveTo(screenPt.x - crossLen, screenPt.y);
            pDC->LineTo(screenPt.x + crossLen, screenPt.y);
            pDC->MoveTo(screenPt.x, screenPt.y - crossLen);
            pDC->LineTo(screenPt.x, screenPt.y + crossLen);
            CPoint place = m_bSnapActive ? ScreenToWorld(m_ptSnapped) : ScreenToWorld(m_ptCurrent);
            CString txt; txt.Format(L"(%.2f, %.2f)", (double)m_ptCoordPoint.x, (double)m_ptCoordPoint.y);
            CPoint txtPt = WorldToScreen(place);
            pDC->TextOutW(txtPt.x+4, txtPt.y+4, txt);
        }
        break;

    case STATE_DRAW_CIRCLE_RADIUS:
        if (m_tempPts.size() >= 1) {
            CPoint c = WorldToScreen(m_tempPts[0]);
            int r = (int)(Distance(m_tempPts[0], curWorld) * pDoc->m_dScale);
            CBrush* pB = (CBrush*)pDC->SelectStockObject(NULL_BRUSH);
            pDC->Ellipse(c.x - r, c.y - r, c.x + r, c.y + r);
            pDC->SelectObject(pB);
        }
        break;

    case STATE_DRAW_ELLIPSE_RADIUS:
        if (m_tempPts.size() >= 1) {
            CPoint c = WorldToScreen(m_tempPts[0]);
            int rx = (int)(abs(curWorld.x - m_tempPts[0].x) * pDoc->m_dScale);
            int ry = (int)(abs(curWorld.y - m_tempPts[0].y) * pDoc->m_dScale);
            CBrush* pB = (CBrush*)pDC->SelectStockObject(NULL_BRUSH);
            pDC->Ellipse(c.x - rx, c.y - ry, c.x + rx, c.y + ry);
            pDC->SelectObject(pB);
        }
        break;

    case STATE_DRAW_POLYGON_RADIUS:
        if (m_tempPts.size() >= 1) {
            int r = (int)(Distance(m_tempPts[0], curWorld) * pDoc->m_dScale);
            CPoint c = WorldToScreen(m_tempPts[0]);
            double step = 2.0 * M_PI / m_nPolygonSides;
            for (int i = 0; i < m_nPolygonSides; ++i) {
                double a1 = -M_PI/2 + step * i;
                double a2 = -M_PI/2 + step * (i+1);
                pDC->MoveTo(c.x + (int)(r*cos(a1)), c.y + (int)(r*sin(a1)));
                pDC->LineTo(c.x + (int)(r*cos(a2)), c.y + (int)(r*sin(a2)));
            }
        }
        break;

    case STATE_DRAW_POLYLINE_POINT:
        // Draw all placed segments + rubber band to cursor
        if (!m_pActivePolyline && m_tempPts.size() >= 2) {
            for (size_t i = 0; i < m_tempPts.size() - 1; ++i) {
                CPoint p1 = WorldToScreen(m_tempPts[i]);
                CPoint p2 = WorldToScreen(m_tempPts[i+1]);
                pDC->MoveTo(p1);
                pDC->LineTo(p2);
            }
        }
        if (m_tempPts.size() >= 1) {
            CPoint last = WorldToScreen(m_tempPts.back());
            if (m_bPolylineArcMode) {
                std::vector<CPoint> previewPts;
                previewPts.push_back(m_tempPts.back());
                AppendPolylineArcApprox(previewPts, m_tempPts.back(), curWorld);
                for (size_t i = 1; i < previewPts.size(); ++i) {
                    pDC->MoveTo(WorldToScreen(previewPts[i - 1]));
                    pDC->LineTo(WorldToScreen(previewPts[i]));
                }
            } else {
                pDC->MoveTo(last);
                pDC->LineTo(cursorPt);
            }
        }
        // Closed mode: dashed line from last point back to first
        if (m_bPolylineClose && m_tempPts.size() >= 2) {
            CPoint first = WorldToScreen(m_tempPts.front());
            CPoint last  = WorldToScreen(m_tempPts.back());
            pDC->MoveTo(last);
            pDC->LineTo(first);
        }
        break;

    case STATE_DRAW_ARC_P3:
        if (m_tempPts.size() >= 2) {
            CPoint ptStart  = m_tempPts[0];
            CPoint ptCenter = m_tempPts[1];
            int r = (int)Distance(ptCenter, ptStart);
            if (r > 0) {
                CPoint c = WorldToScreen(ptCenter);
                int scrR = (int)(r * pDoc->m_dScale);
                double angS = atan2((double)(ptStart.y - ptCenter.y),
                                    (double)(ptStart.x - ptCenter.x));
                double angE = atan2((double)(curWorld.y - ptCenter.y),
                                    (double)(curWorld.x - ptCenter.x));

                CPoint scrStart(c.x + (int)(scrR * cos(angS)),
                                c.y - (int)(scrR * sin(angS)));
                CPoint scrEnd(c.x + (int)(scrR * cos(angE)),
                              c.y - (int)(scrR * sin(angE)));

                pDC->Arc(c.x - scrR, c.y - scrR, c.x + scrR, c.y + scrR,
                         scrStart.x, scrStart.y, scrEnd.x, scrEnd.y);
            }
        }
        break;

    case STATE_DRAW_ARC_PREVIEW:
        if (m_tempPts.size() >= 3) {
            CPoint ptStart  = m_tempPts[0];
            CPoint ptCenter = m_tempPts[1];
            CPoint ptEnd    = m_tempPts[2];
            int r = (int)Distance(ptCenter, ptStart);
            if (r > 0) {
                CPoint c = WorldToScreen(ptCenter);
                int scrR = (int)(r * pDoc->m_dScale);
                double angS = atan2((double)(ptStart.y - ptCenter.y),
                                    (double)(ptStart.x - ptCenter.x));
                double angE = atan2((double)(ptEnd.y - ptCenter.y),
                                    (double)(ptEnd.x - ptCenter.x));

                CPoint scrStart(c.x + (int)(scrR * cos(angS)),
                                c.y - (int)(scrR * sin(angS)));
                CPoint scrEnd(c.x + (int)(scrR * cos(angE)),
                              c.y - (int)(scrR * sin(angE)));

                if (!m_bArcAltHalf) {
                    pDC->Arc(c.x - scrR, c.y - scrR, c.x + scrR, c.y + scrR,
                             scrStart.x, scrStart.y, scrEnd.x, scrEnd.y);
                } else {
                    // Draw complementary arc: swap start/end
                    pDC->Arc(c.x - scrR, c.y - scrR, c.x + scrR, c.y + scrR,
                             scrEnd.x, scrEnd.y, scrStart.x, scrStart.y);
                }
            }
        }
        break;

    case STATE_MOVE_DEST:
    case STATE_COPY_DEST:
        if (m_tempPts.size() >= 1) {
            CPoint base = WorldToScreen(m_tempPts.back());
            TRACE(L"[DEBUG] DrawPreview MOVE/COPY_DEST: base=(%d,%d), cursor=(%d,%d), tempPts=%d\n",
                  base.x, base.y, cursorPt.x, cursorPt.y, (int)m_tempPts.size());
            pDC->MoveTo(base);
            pDC->LineTo(cursorPt);
        }
        break;

    case STATE_WINDOW_SELECT: {
        CBrush* pB = (CBrush*)pDC->SelectStockObject(NULL_BRUSH);
        pDC->Rectangle(m_ptDragStart.x, m_ptDragStart.y, m_ptCurrent.x, m_ptCurrent.y);
        pDC->SelectObject(pB);
        break;
    }

    // dimension preview handled above; interactive creation occurs in OnLButtonDown

    case STATE_ROTATE_ANGLE:
        if (m_tempPts.size() >= 1) {
            CPoint base = WorldToScreen(m_tempPts[0]);
            pDC->MoveTo(base);
            pDC->LineTo(cursorPt);
        }
        break;

    case STATE_SCALE_FACTOR:
        if (m_tempPts.size() >= 1) {
            CPoint base = WorldToScreen(m_tempPts[0]);
            pDC->MoveTo(base);
            pDC->LineTo(cursorPt);
        }
        break;

    case STATE_MIRROR_P1:
        // No preview needed - first click sets mirror line start
        break;

    case STATE_MIRROR_P2:
        if (m_tempPts.size() >= 1) {
            CPoint p1 = WorldToScreen(m_tempPts[0]);
            CPoint cursorPt = m_bSnapActive ? m_ptSnapped : m_ptCurrent;
            pDC->MoveTo(p1);
            pDC->LineTo(cursorPt);
        }
        break;

    case STATE_ZOOM_WINDOW_P1:
        if (m_bDragging) {
            CBrush* pB = (CBrush*)pDC->SelectStockObject(NULL_BRUSH);
            pDC->Rectangle(m_ptDragStart.x, m_ptDragStart.y, m_ptCurrent.x, m_ptCurrent.y);
            pDC->SelectObject(pB);
        } else {
            pDC->MoveTo(m_ptDragStart);
            pDC->LineTo(m_ptCurrent);
            pDC->MoveTo(m_ptCurrent);
            pDC->LineTo(m_ptDragStart.x, m_ptCurrent.y);
            pDC->MoveTo(m_ptCurrent);
            pDC->LineTo(m_ptCurrent.x, m_ptDragStart.y);
        }
        break;

    case STATE_ZOOM_WINDOW_P2: {
        CBrush* pB = (CBrush*)pDC->SelectStockObject(NULL_BRUSH);
        pDC->Rectangle(m_ptDragStart.x, m_ptDragStart.y, m_ptCurrent.x, m_ptCurrent.y);
        pDC->SelectObject(pB);
        break;
    }
    }

    pDC->SetROP2(oldROP);
    pDC->SelectObject(pOldPen);
}

// ============================================================
// Draw UCS icon (bottom-left X/Y arrows)
// ============================================================
void CLargeHWView::DrawUCSIcon(CDC* pDC)
{
    CRect rcClient;
    GetClientRect(&rcClient);

    int cx = 40, cy = rcClient.bottom - 40;
    CPen xPen(PS_SOLID, 2, RGB(255, 0, 0));
    CPen yPen(PS_SOLID, 2, RGB(0, 255, 0));
    CPen* pOld = pDC->SelectObject(&xPen);

    // X axis
    pDC->MoveTo(cx, cy);
    pDC->LineTo(cx + 30, cy);
    pDC->LineTo(cx + 25, cy - 5);
    pDC->MoveTo(cx + 30, cy);
    pDC->LineTo(cx + 25, cy + 5);
    pDC->SetTextColor(RGB(255, 100, 100));
    pDC->TextOutW(cx + 32, cy - 8, L"X");

    // Y axis
    pDC->SelectObject(&yPen);
    pDC->MoveTo(cx, cy);
    pDC->LineTo(cx, cy - 30);
    pDC->LineTo(cx - 5, cy - 25);
    pDC->MoveTo(cx, cy - 30);
    pDC->LineTo(cx + 5, cy - 25);
    pDC->SetTextColor(RGB(100, 255, 100));
    pDC->TextOutW(cx - 15, cy - 32, L"Y");

    pDC->SelectObject(pOld);
}

// ============================================================
// Draw snap marker
// ============================================================
void CLargeHWView::DrawSnapMarker(CDC* pDC)
{
    CPoint scr = m_ptSnapped;
    COLORREF markerColor;
    switch (m_nSnapType) {
    case SNAP_ENDPOINT:  markerColor = RGB(255, 128, 0); break;
    case SNAP_MIDPOINT:  markerColor = RGB(0, 255, 128); break;
    case SNAP_CENTER:    markerColor = RGB(255, 255, 0); break;
    case SNAP_QUADRANT:  markerColor = RGB(0, 255, 255); break;
    case SNAP_NEAREST:   markerColor = RGB(200, 200, 200); break;
    default:             markerColor = RGB(128, 255, 0); break;
    }

    CPen snapPen(PS_SOLID, 2, markerColor);
    CPen* pOldPen = pDC->SelectObject(&snapPen);
    CBrush* pOldBrush = (CBrush*)pDC->SelectStockObject(NULL_BRUSH);

    int sz = 8;
    pDC->Rectangle(scr.x - sz, scr.y - sz, scr.x + sz, scr.y + sz);
    pDC->MoveTo(scr.x - sz - 2, scr.y);
    pDC->LineTo(scr.x + sz + 2, scr.y);
    pDC->MoveTo(scr.x, scr.y - sz - 2);
    pDC->LineTo(scr.x, scr.y + sz + 2);

    pDC->SelectObject(pOldPen);
    pDC->SelectObject(pOldBrush);
}

// ============================================================
// Coordinate transforms
// ============================================================
CPoint CLargeHWView::WorldToScreen(CPoint world) const
{
    CLargeHWDoc* pDoc = GetDocument();
    double s = pDoc ? pDoc->m_dScale : 1.0;
    CPoint off = pDoc ? pDoc->m_ptOffset : CPoint(0, 0);
    return CPoint((int)(world.x * s + off.x), (int)(off.y - world.y * s));
}

CPoint CLargeHWView::ScreenToWorld(CPoint screen) const
{
    CLargeHWDoc* pDoc = GetDocument();
    double s = pDoc ? pDoc->m_dScale : 1.0;
    if (s <= 0) s = 1;
    CPoint off = pDoc ? pDoc->m_ptOffset : CPoint(0, 0);
    return CPoint((int)((screen.x - off.x) / s), (int)((off.y - screen.y) / s));
}

CPoint CLargeHWView::SnapToGrid(CPoint pt) const
{
    CLargeHWDoc* pDoc = GetDocument();
    if (!pDoc || !pDoc->m_bSnapToGrid) return pt;
    int gs = pDoc->m_nGridSpacing;
    int x = ((pt.x + gs / 2) / gs) * gs;
    int y = ((pt.y + gs / 2) / gs) * gs;
    return CPoint(x, y);
}

CPoint CLargeHWView::SnapToObjects(CPoint pt)
{
    CLargeHWDoc* pDoc = GetDocument();
    if (!pDoc || !pDoc->m_bObjectSnap) return pt;

    std::vector<CPoint> snapPts;
    std::vector<SnapType> snapTypes;
    pDoc->CollectSnapPoints(snapPts, snapTypes);

    double screenPt = 12.0 / pDoc->m_dScale;
    if (screenPt < 1.0) screenPt = 1.0;
    double bestDist = screenPt;
    CPoint bestPt = pt;
    SnapType bestType = SNAP_NONE;

    for (size_t i = 0; i < snapPts.size(); ++i) {
        SnapType t = snapTypes[i];
        if (!pDoc->m_bSnapEndpoint && t == SNAP_ENDPOINT) continue;
        if (!pDoc->m_bSnapMidpoint && t == SNAP_MIDPOINT) continue;
        if (!pDoc->m_bSnapCenter && t == SNAP_CENTER) continue;
        if (!pDoc->m_bSnapQuadrant && t == SNAP_QUADRANT) continue;
        if (!pDoc->m_bSnapNearest && t == SNAP_NEAREST) continue;

        double d = Distance(pt, snapPts[i]);
        if (d < bestDist) {
            bestDist = d;
            bestPt = snapPts[i];
            bestType = t;
        }
    }

    if (bestType != SNAP_NONE) {
        m_nSnapType = bestType;
        return bestPt;
    }

    return pt;
}

CPoint CLargeHWView::ApplyOrtho(CPoint pt, CPoint ref) const
{
    CLargeHWDoc* pDoc = GetDocument();
    if (!pDoc || !pDoc->m_bOrthoMode) return pt;
    double dx = fabs((double)(pt.x - ref.x));
    double dy = fabs((double)(pt.y - ref.y));
    if (dx >= dy)
        return CPoint(pt.x, ref.y);
    else
        return CPoint(ref.x, pt.y);
}

// ============================================================
// Update status bar (coordinates + command prompt)
// ============================================================
void CLargeHWView::UpdateStatusBar()
{
    CLargeHWDoc* pDoc = GetDocument();
    CMainFrame* pFrame = (CMainFrame*)AfxGetMainWnd();
    if (!pFrame || !pDoc) return;

    CPoint world = ScreenToWorld(m_ptCurrent);
    CString strCoord;
    double modelUnitScale = GetModelUnitScale(pDoc);
    CString xText = FormatModelNumber(world.x / modelUnitScale);
    CString yText = FormatModelNumber(world.y / modelUnitScale);
    strCoord.Format(L"X: %s  Y: %s  | Zoom: %.2f  |  %s  SNAP=%s GRID=%s ORTHO=%s OSNAP=%s",
                    (LPCTSTR)xText, (LPCTSTR)yText, pDoc->m_dScale * modelUnitScale,
                    (LPCTSTR)pDoc->m_strCommandPrompt,
                    pDoc->m_bSnapToGrid ? L"ON" : L"OFF",
                    pDoc->m_bShowGrid ? L"ON" : L"OFF",
                    pDoc->m_bOrthoMode ? L"ON" : L"OFF",
                    pDoc->m_bObjectSnap ? L"ON" : L"OFF");

    pFrame->SetStatusBarText(strCoord);
}

// ============================================================
// Set drawing state
// ============================================================
void CLargeHWView::SetDrawState(CadDrawState state)
{
    CLargeHWDoc* pDoc = GetDocument();
    pDoc->m_drawState = state;
    m_bDrawing = (state != STATE_IDLE);

    // Only deselect for drawing commands, not modify/select/view operations
    if (state >= STATE_DRAW_LINE_P1 && state <= STATE_DRAW_TEXT_POS)
        pDoc->DeselectAll();

    TRACE(L"[DEBUG] SetDrawState: state=%d, m_bDrawing=%d, tempPts=%d\n", (int)state, m_bDrawing, (int)m_tempPts.size());

    switch (state) {
    case STATE_DRAW_LINE_P1:         pDoc->m_strCommandPrompt = L"LINE Specify first point: "; break;
    case STATE_DRAW_LINE_P2:         pDoc->m_strCommandPrompt = L"LINE Specify next point: "; break;
    case STATE_DRAW_CIRCLE_CENTER:   pDoc->m_strCommandPrompt = L"CIRCLE Specify center: "; break;
    case STATE_DRAW_CIRCLE_RADIUS:   pDoc->m_strCommandPrompt = L"CIRCLE Specify radius: "; break;
    case STATE_DRAW_ARC_P1:          pDoc->m_strCommandPrompt = L"ARC Specify start point: "; break;
    case STATE_DRAW_ARC_P2:          pDoc->m_strCommandPrompt = L"ARC Specify center point: "; break;
    case STATE_DRAW_ARC_P3:          pDoc->m_strCommandPrompt = L"ARC Specify end point: "; break;
    case STATE_DRAW_ARC_PREVIEW:     pDoc->m_strCommandPrompt = L"ARC Press ENTER to confirm, Ctrl to flip half: "; break;
    case STATE_DRAW_RECT_P1:         pDoc->m_strCommandPrompt = L"RECTANG Specify first corner: "; break;
    case STATE_DRAW_RECT_P2:         pDoc->m_strCommandPrompt = L"RECTANG Specify other corner: "; break;
    case STATE_DRAW_POLYGON_CENTER:  pDoc->m_strCommandPrompt = L"POLYGON Specify center: "; break;
    case STATE_DRAW_POLYGON_RADIUS:  pDoc->m_strCommandPrompt = L"POLYGON Specify radius: "; break;
    case STATE_DRAW_ELLIPSE_CENTER:  pDoc->m_strCommandPrompt = L"ELLIPSE Specify center: "; break;
    case STATE_DRAW_ELLIPSE_RADIUS:  pDoc->m_strCommandPrompt = L"ELLIPSE Specify axis endpoint: "; break;
    case STATE_DRAW_POLYLINE_POINT:  pDoc->m_strCommandPrompt.Format(L"PLINE Specify next point [Arc/Line/Width/Close] mode=%s width=%d: ", m_bPolylineArcMode ? L"ARC" : L"LINE", m_nPolylineWidth); break;
    case STATE_DRAW_POLYLINE_START_WIDTH: pDoc->m_strCommandPrompt.Format(L"PLINE Specify start width <%d>: ", m_nPolylineWidth); break;
    case STATE_DRAW_POLYLINE_END_WIDTH:   pDoc->m_strCommandPrompt.Format(L"PLINE Specify end width <%d>: ", m_nPolylineWidth); break;
    case STATE_DRAW_DIM_LENGTH_P1:   pDoc->m_strCommandPrompt = L"DIMENSION Specify first extension point: "; break;
    case STATE_DRAW_DIM_LENGTH_P2:   pDoc->m_strCommandPrompt = L"DIMENSION Specify second extension point: "; break;
    case STATE_DRAW_DIM_LENGTH_POS:  pDoc->m_strCommandPrompt = L"DIMENSION Click to place dimension line and text: "; break;
    case STATE_DRAW_DIM_ANGLE_SELECT_E1: pDoc->m_strCommandPrompt = L"ANGDIM Select first line: "; break;
    case STATE_DRAW_DIM_ANGLE_SELECT_E2: pDoc->m_strCommandPrompt = L"ANGDIM Select second line: "; break;
    case STATE_DRAW_DIM_ANGLE_POS:       pDoc->m_strCommandPrompt = L"ANGDIM Pick position on arc for dimension: "; break;
    case STATE_DRAW_DIM_RADIUS_SELECT:   pDoc->m_strCommandPrompt = L"RADDIM Select circle or arc: "; break;
    case STATE_DRAW_DIM_RADIUS_POS:      pDoc->m_strCommandPrompt = L"RADDIM Specify leader line position: "; break;
    case STATE_DRAW_DIM_DIAMETER_SELECT: pDoc->m_strCommandPrompt = L"DIADIM Select circle or arc: "; break;
    case STATE_DRAW_DIM_DIAMETER_POS:    pDoc->m_strCommandPrompt = L"DIADIM Specify diameter line position: "; break;
    case STATE_DRAW_DIM_ARCLEN_SELECT:   pDoc->m_strCommandPrompt = L"ARCLENDIM Select arc: "; break;
    case STATE_DRAW_DIM_ARCLEN_POS:      pDoc->m_strCommandPrompt = L"ARCLENDIM Specify dimension arc position: "; break;
    case STATE_DRAW_DIM_COORD_PICK:      pDoc->m_strCommandPrompt = L"COORD Specify point: "; break;
    case STATE_DRAW_TEXT_POS:        pDoc->m_strCommandPrompt = L"TEXT Specify position: "; break;
    case STATE_MOVE_SELECT:          pDoc->m_strCommandPrompt = L"MOVE Select objects: "; break;
    case STATE_MOVE_BASE:            pDoc->m_strCommandPrompt = L"MOVE Specify base point: "; break;
    case STATE_MOVE_DEST:            pDoc->m_strCommandPrompt = L"MOVE Specify destination: "; break;
    case STATE_COPY_SELECT:          pDoc->m_strCommandPrompt = L"COPY Select objects: "; break;
    case STATE_COPY_BASE:            pDoc->m_strCommandPrompt = L"COPY Specify base point: "; break;
    case STATE_COPY_DEST:            pDoc->m_strCommandPrompt = L"COPY Specify destination: "; break;
    case STATE_ROTATE_SELECT:        pDoc->m_strCommandPrompt = L"ROTATE Select objects: "; break;
    case STATE_ROTATE_CENTER:        pDoc->m_strCommandPrompt = L"ROTATE Specify base point: "; break;
    case STATE_ROTATE_ANGLE:         pDoc->m_strCommandPrompt = L"ROTATE Specify rotation angle or second point: "; break;
    case STATE_SCALE_SELECT:         pDoc->m_strCommandPrompt = L"SCALE Select objects: "; break;
    case STATE_SCALE_BASE:           pDoc->m_strCommandPrompt = L"SCALE Specify base point: "; break;
    case STATE_SCALE_FACTOR:         pDoc->m_strCommandPrompt = L"SCALE Specify scale factor or second point: "; break;
    case STATE_MIRROR_SELECT:        pDoc->m_strCommandPrompt = L"MIRROR Select objects: "; break;
    case STATE_MIRROR_P1:            pDoc->m_strCommandPrompt = L"MIRROR Specify first point of mirror line: "; break;
    case STATE_MIRROR_P2:            pDoc->m_strCommandPrompt = L"MIRROR Specify second point of mirror line: "; break;
    case STATE_OFFSET_SELECT:        pDoc->m_strCommandPrompt = L"OFFSET Select entity to offset: "; break;
    case STATE_OFFSET_DIST:          pDoc->m_strCommandPrompt = L"OFFSET Specify offset distance (click point on side): "; break;
    case STATE_CHAMFER_SELECT_FIRST: pDoc->m_strCommandPrompt.Format(L"CHAMFER Select first line or enter distance <%.0f>: ", m_dChamferDistance); break;
    case STATE_CHAMFER_SELECT_SECOND:pDoc->m_strCommandPrompt = L"CHAMFER Select second line: "; break;
    case STATE_FILLET_SELECT_FIRST:  pDoc->m_strCommandPrompt.Format(L"FILLET Select first edge or enter radius <%.0f>: ", m_dFilletRadius); break;
    case STATE_FILLET_SELECT_SECOND: pDoc->m_strCommandPrompt = L"FILLET Select second adjacent edge: "; break;
    case STATE_ARRAY_SELECT:         pDoc->m_strCommandPrompt = L"ARRAY Select objects (ENTER to finish): "; break;
    case STATE_ARRAY_ROWS:           pDoc->m_strCommandPrompt.Format(L"ARRAY Enter number of rows <%d>: ", m_nArrayRows); break;
    case STATE_ARRAY_COLUMNS:        pDoc->m_strCommandPrompt.Format(L"ARRAY Enter number of columns <%d>: ", m_nArrayColumns); break;
    case STATE_ARRAY_ROW_SPACING:    pDoc->m_strCommandPrompt.Format(L"ARRAY Enter row item spacing <%.0f>: ", m_dArrayRowSpacing); break;
    case STATE_ARRAY_COLUMN_SPACING: pDoc->m_strCommandPrompt.Format(L"ARRAY Enter column item spacing <%.0f>: ", m_dArrayColumnSpacing); break;
    case STATE_ZOOM_WINDOW_P1:       pDoc->m_strCommandPrompt = L"ZOOM Window: Specify first corner: "; break;
    case STATE_ZOOM_WINDOW_P2:       pDoc->m_strCommandPrompt = L"ZOOM Window: Specify opposite corner: "; break;
    case STATE_IDLE:
    default:                         pDoc->m_strCommandPrompt = L"Command: "; m_bDrawing = false; break;
    }
    
    // Update command line edit (only on state change, not every mouse move)
    CMainFrame* pFrame = (CMainFrame*)AfxGetMainWnd();
    if (pFrame && pFrame->m_wndCmdLine.GetSafeHwnd()) {
        pFrame->m_wndCmdLine.SetWindowText(pDoc->m_strCommandPrompt);
        int nLen = pDoc->m_strCommandPrompt.GetLength();
        pFrame->m_wndCmdLine.SetSel(nLen, nLen);
    }
    
    UpdateStatusBar();
    Invalidate(FALSE);
}

void CLargeHWView::CompleteDrawCommand()
{
    TRACE(L"[DEBUG] CompleteDrawCommand called\n");
    m_tempPts.clear();
    m_pActivePolyline = nullptr;
    if (m_bDimRadiusSrcTemp) { delete m_pDimRadiusSrcEnt; m_bDimRadiusSrcTemp = false; }
    m_pDimRadiusSrcEnt = nullptr;
    if (m_bDimDiamSrcTemp) { delete m_pDimDiamSrcEnt; m_bDimDiamSrcTemp = false; }
    m_pDimDiamSrcEnt = nullptr;
    if (m_bDimArcLenSrcTemp) { delete m_pDimArcLenSrcEnt; m_bDimArcLenSrcTemp = false; }
    m_pDimArcLenSrcEnt = nullptr;
    SetDrawState(STATE_IDLE);
}

// ============================================================
// LButtonDown - Interaction state machine core
// ============================================================
void CLargeHWView::OnLButtonDown(UINT nFlags, CPoint point)
{
    CLargeHWDoc* pDoc = GetDocument();
    if (!pDoc) return;

    CPoint world = ScreenToWorld(point);

    // Apply snap (grid + object)
    CPoint worldSnapped = SnapToGrid(world);
    if (pDoc->m_bObjectSnap) {
        worldSnapped = SnapToObjects(worldSnapped);
    }

    // Apply ortho if in a state that requires it
    CadDrawState state = pDoc->m_drawState;
    TRACE(L"[DEBUG] OnLButtonDown: screen=(%d,%d), world=(%d,%d), state=%d, tempPts=%d, selected=%d\n",
          point.x, point.y, world.x, world.y, (int)state, (int)m_tempPts.size(), pDoc->GetSelectedCount());
    if (pDoc->m_bOrthoMode && !m_tempPts.empty() &&
        (state == STATE_DRAW_LINE_P2 || state == STATE_MOVE_DEST || state == STATE_COPY_DEST ||
         state == STATE_ROTATE_ANGLE || state == STATE_SCALE_FACTOR || state == STATE_MIRROR_P2 ||
         state == STATE_OFFSET_DIST || state == STATE_DRAW_RECT_P2)) {
        worldSnapped = ApplyOrtho(worldSnapped, m_tempPts.back());
    }

    world = worldSnapped;

    if (m_bScriptRecording && !m_bRunningScript && !m_bSubmittingCommandLine &&
        state != STATE_DRAW_TEXT_POS && ShouldRecordPointForState(state)) {
        RecordScriptInput(FormatScriptPoint(world));
    }

    // Check grip hit first (only in IDLE state)
    if (state == STATE_IDLE) {
        std::vector<CEntity*> hitEntities = pDoc->HitTestEntities(point, pDoc->m_dScale, pDoc->m_ptOffset);
        for (auto* hitEntity : hitEntities) {
            if (!hitEntity || !hitEntity->m_bSelected)
                continue;

            int gripIdx = hitEntity->HitTestGrip(point, pDoc->m_dScale, pDoc->m_ptOffset);
            if (gripIdx >= 0) {
                pDoc->RecordGripUndo(hitEntity);
                m_nGripIndex = gripIdx;
                m_pGripEntity = hitEntity;
                m_bDragging = true;
                m_ptDragStart = point;
                return;
            }
        }
    }

    switch (state) {
    case STATE_IDLE: {
        std::vector<CEntity*> hits = pDoc->HitTestEntities(point, pDoc->m_dScale, pDoc->m_ptOffset);
        if (!hits.empty()) {
            CEntity* hit = hits.front();
            if (nFlags & MK_CONTROL) {
                hit->m_bSelected = true;
            } else {
                for (size_t i = 0; i < hits.size(); ++i) {
                    if (hits[i]->m_bSelected) {
                        hit = hits[(i + 1) % hits.size()];
                        break;
                    }
                }
                pDoc->DeselectAll();
                hit->m_bSelected = true;
            }
        } else {
            // Ctrl+click on empty space starts zoom window drag
            if (nFlags & MK_CONTROL) {
                pDoc->m_drawState = STATE_ZOOM_WINDOW_P1;
                m_bDrawing = true;
                m_ptDragStart = point;
                m_bDragging = true;
                pDoc->m_strCommandPrompt = L"ZOOM Window: Specify first corner: ";
                UpdateStatusBar();
            } else {
                pDoc->DeselectAll();
                pDoc->m_drawState = STATE_WINDOW_SELECT;
                m_ptDragStart = point;
                m_bDragging = true;
            }
        }
        Invalidate(FALSE);
        break;
    }

    case STATE_DRAW_LINE_P1:
        m_tempPts.push_back(world);
        SetDrawState(STATE_DRAW_LINE_P2);
        break;

    case STATE_DRAW_LINE_P2: {
        CLineEntity* pLine = new CLineEntity(m_tempPts.back(), world);
        pDoc->AddEntity(pLine);
        CPoint firstPt = m_tempPts.front();
        m_tempPts.clear();
        m_tempPts.push_back(firstPt);
        m_tempPts.push_back(world);
        SetDrawState(STATE_DRAW_LINE_P2);
        break;
    }

    case STATE_DRAW_CIRCLE_CENTER:
        m_tempPts.push_back(world);
        SetDrawState(STATE_DRAW_CIRCLE_RADIUS);
        break;

    case STATE_DRAW_CIRCLE_RADIUS: {
        int r = (int)Distance(m_tempPts[0], world);
        CCircleEntity* pCircle = new CCircleEntity(m_tempPts[0], r);
        pDoc->AddEntity(pCircle);
        CompleteDrawCommand();
        break;
    }

    case STATE_DRAW_ARC_P1:
        m_tempPts.push_back(world);
        SetDrawState(STATE_DRAW_ARC_P2);
        break;

    case STATE_DRAW_ARC_P2:
        m_tempPts.push_back(world);
        SetDrawState(STATE_DRAW_ARC_P3);
        break;

    case STATE_DRAW_ARC_P3: {
        m_tempPts.push_back(world);
        m_bArcAltHalf = false;
        SetDrawState(STATE_DRAW_ARC_PREVIEW);
        break;
    }

    case STATE_DRAW_RECT_P1:
        m_tempPts.push_back(world);
        SetDrawState(STATE_DRAW_RECT_P2);
        break;

    case STATE_DRAW_RECT_P2: {
        m_tempPts.push_back(world);
        CRectangleEntity* pRect = new CRectangleEntity(m_tempPts[0], m_tempPts[1]);
        pDoc->AddEntity(pRect);
        CompleteDrawCommand();
        break;
    }

    case STATE_DRAW_POLYGON_CENTER:
        m_tempPts.push_back(world);
        SetDrawState(STATE_DRAW_POLYGON_RADIUS);
        break;

    case STATE_DRAW_POLYGON_RADIUS: {
        int r = (int)Distance(m_tempPts[0], world);
        CPolygonEntity* pPoly = new CPolygonEntity(m_tempPts[0], r, m_nPolygonSides);
        pDoc->AddEntity(pPoly);
        CompleteDrawCommand();
        break;
    }

    case STATE_DRAW_ELLIPSE_CENTER:
        m_tempPts.push_back(world);
        SetDrawState(STATE_DRAW_ELLIPSE_RADIUS);
        break;

    case STATE_DRAW_ELLIPSE_RADIUS: {
        int rx = abs(world.x - m_tempPts[0].x);
        int ry = abs(world.y - m_tempPts[0].y);
        CEllipseEntity* pEllipse = new CEllipseEntity(m_tempPts[0], rx, ry);
        pDoc->AddEntity(pEllipse);
        CompleteDrawCommand();
        break;
    }

    case STATE_DRAW_POLYLINE_POINT:
        AddPolylinePoint(world);
        SetDrawState(STATE_DRAW_POLYLINE_POINT);
        break;

    case STATE_DRAW_TEXT_POS: {
        // If we have a pending dimension entity, use this click as text placement
        if (m_pPendingDim) {
            if (auto pL = dynamic_cast<CDimLinearEntity*>(m_pPendingDim)) {
                pL->m_ptText = world;
                pL->m_bTextPlaced = true;
                pL->m_bSelected = false;
                m_pPendingDim = nullptr;
                CompleteDrawCommand();
            } else if (auto pA = dynamic_cast<CDimAngularEntity*>(m_pPendingDim)) {
                // set text position
                pA->m_ptText = world;
                pA->m_bTextPlaced = true;
                pA->m_bSelected = false;
                // restore any temporary splits created during angle selection
                if (!m_tempSplitNewIDs.empty()) {
                    RestoreTemporarySplits(pDoc, m_tempSplitNewIDs);
                    m_tempSplitNewIDs.clear();
                }
                // clear selection so any previously highlighted (thick) entities return to normal
                pDoc->DeselectAll();
                // cleanup temp segment entities if created
                if (m_bDimEnt1Temp && m_pDimEnt1) delete m_pDimEnt1;
                if (m_bDimEnt2Temp && m_pDimEnt2) delete m_pDimEnt2;
                m_pDimEnt1 = m_pDimEnt2 = nullptr;
                m_pDimEnt1Orig = m_pDimEnt2Orig = nullptr;
                m_bDimEnt1Temp = m_bDimEnt2Temp = false;
                m_pPendingDim = nullptr;
                CompleteDrawCommand();
            } else if (auto pR = dynamic_cast<CDimRadiusEntity*>(m_pPendingDim)) {
                pR->m_ptText = world;
                pR->m_bTextPlaced = true;
                pR->m_bSelected = false;
                m_pPendingDim = nullptr;
                if (m_bDimRadiusSrcTemp) { delete m_pDimRadiusSrcEnt; m_bDimRadiusSrcTemp = false; }
                m_pDimRadiusSrcEnt = nullptr;
                pDoc->DeselectAll();
                CompleteDrawCommand();
            } else if (auto pD = dynamic_cast<CDimDiamEntity*>(m_pPendingDim)) {
                pD->m_ptText = world;
                pD->m_bTextPlaced = true;
                pD->m_bSelected = false;
                m_pPendingDim = nullptr;
                if (m_bDimDiamSrcTemp) { delete m_pDimDiamSrcEnt; m_bDimDiamSrcTemp = false; }
                m_pDimDiamSrcEnt = nullptr;
                pDoc->DeselectAll();
                CompleteDrawCommand();
            } else if (auto pAL = dynamic_cast<CDimArcLengthEntity*>(m_pPendingDim)) {
                pAL->m_ptText = world;
                pAL->m_bTextPlaced = true;
                pAL->m_bSelected = false;
                m_pPendingDim = nullptr;
                if (m_bDimArcLenSrcTemp) { delete m_pDimArcLenSrcEnt; m_bDimArcLenSrcTemp = false; }
                m_pDimArcLenSrcEnt = nullptr;
                pDoc->DeselectAll();
                CompleteDrawCommand();
            }
        } else if (m_bCoordDimMode) {
            CString txt;
            txt.Format(L"(%.2f, %.2f)", (double)m_ptCoordPoint.x, (double)m_ptCoordPoint.y);
            CTextEntity* pText = new CTextEntity(world, txt, 20);
            pDoc->AddEntity(pText);
            // Draw cross marker at the coordinate point (4 screen pixels)
            double halfW = (pDoc->m_dScale > 0.0) ? (2.0 / pDoc->m_dScale) : 2.0;
            CLineEntity* pLineH = new CLineEntity(
                CPoint((int)(m_ptCoordPoint.x - halfW + 0.5), m_ptCoordPoint.y),
                CPoint((int)(m_ptCoordPoint.x + halfW + 0.5), m_ptCoordPoint.y));
            CLineEntity* pLineV = new CLineEntity(
                CPoint(m_ptCoordPoint.x, (int)(m_ptCoordPoint.y - halfW + 0.5)),
                CPoint(m_ptCoordPoint.x, (int)(m_ptCoordPoint.y + halfW + 0.5)));
            pDoc->AddEntity(pLineH);
            pDoc->AddEntity(pLineV);
            m_bCoordDimMode = false;
            CompleteDrawCommand();
        } else {
            // fallback: normal text input dialog
            CTextInputDlg dlg(this);
            if (dlg.DoModal() == IDOK) {
                CTextEntity* pText = new CTextEntity(world, dlg.m_strText, dlg.m_nHeight);
                pDoc->AddEntity(pText);
            }
            if (m_bScriptRecording && !m_bRunningScript && !m_bSubmittingCommandLine) {
                CString strTextCommand;
                double modelUnitScale = GetModelUnitScale(pDoc);
                strTextCommand.Format(L"TEXT %s %s \"%s\"",
                                      (LPCTSTR)FormatModelPoint(world, modelUnitScale),
                                      (LPCTSTR)FormatModelNumber(dlg.m_nHeight / modelUnitScale),
                                      (LPCTSTR)EscapeScriptText(dlg.m_strText));
                RecordScriptInput(strTextCommand);
            }
            CompleteDrawCommand();
        }
        break;
    }

    case STATE_DRAW_DIM_LENGTH_P1:
        m_tempPts.push_back(world);
        SetDrawState(STATE_DRAW_DIM_LENGTH_P2);
        break;

    case STATE_DRAW_DIM_LENGTH_P2: {
        // push second point and move to placement state so user can click to place offset
        m_tempPts.push_back(world);
        // reserve a third temp slot to hold offset world point chosen by user (initially compute default offset)
        CPoint a = m_tempPts[0], b = m_tempPts[1];
        double wx = (double)(b.x - a.x), wy = (double)(b.y - a.y);
        double wlen = sqrt(wx*wx + wy*wy);
        double screenOffset = 15.0; double offWorld = 1.0; CLargeHWDoc* pDocLocal = GetDocument();
        if (pDocLocal && pDocLocal->m_dScale > 0.0) offWorld = screenOffset / pDocLocal->m_dScale;
        CPoint offsetPt = a;
        if (wlen > 1e-6) {
            double ux = wx / wlen, uy = wy / wlen;
            double nx = -uy, ny = ux;
            offsetPt.x = (int)floor(a.x + nx * offWorld + 0.5);
            offsetPt.y = (int)floor(a.y + ny * offWorld + 0.5);
        } else {
            offsetPt.y = a.y + (int)offWorld;
        }
        m_tempPts.push_back(offsetPt);
        SetDrawState(STATE_DRAW_DIM_LENGTH_POS);
        break;
    }

    case STATE_DRAW_DIM_LENGTH_POS: {
        // First click: create the dimension graphics (without text placement). Next click will place the text.
        if (m_tempPts.size() >= 3) {
            CPoint a = m_tempPts[0], b = m_tempPts[1];
            CPoint place = world; // click position
            // compute signed offset along normal from original line to placement point (in world units)
            double wx = (double)(b.x - a.x), wy = (double)(b.y - a.y);
            double wlen = sqrt(wx*wx + wy*wy);
            double signedOff = 0.0;
            if (wlen > 1e-6) {
                double nx = -(wy / wlen), ny = (wx / wlen);
                double pax = (double)(place.x - a.x), pay = (double)(place.y - a.y);
                signedOff = pax * nx + pay * ny; // projection onto normal
            } else {
                signedOff = (double)(place.y - a.y);
            }
            CDimLinearEntity::DimMode mode = CDimLinearEntity::DIM_ALIGNED;
            if (m_nLastDimMode == 1) mode = CDimLinearEntity::DIM_HORIZONTAL;
            else if (m_nLastDimMode == 2) mode = CDimLinearEntity::DIM_VERTICAL;
            CDimLinearEntity* pDim = new CDimLinearEntity(a, b, mode, signedOff, place);
            pDim->m_bTextPlaced = false;
            // add to document so graphics appear immediately, keep as pending for text placement
            pDoc->AddEntity(pDim);
            pDim->m_bSelected = true;
            if (m_pPendingDim && m_pPendingDim != pDim) m_pPendingDim = nullptr;
            m_pPendingDim = pDim;
            // move to a dedicated text placement state so next click places text
            SetDrawState(STATE_DRAW_TEXT_POS);
        } else {
            CompleteDrawCommand();
        }
        break;
    }



    case STATE_DRAW_DIM_ANGLE_SELECT_E1: {
        CEntity* hit = pDoc->HitTestEntity(point, pDoc->m_dScale, pDoc->m_ptOffset);
        TRACE(L"[DEBUG] STATE_DRAW_DIM_ANGLE_SELECT_E1: hit=%p\n", hit);
        if (hit) {
            // If it's a polyline, determine which segment was clicked and create a temp CLineEntity
            m_pDimEnt1Orig = hit;
            m_pDimEnt1 = hit;
            m_bDimEnt1Temp = false;
            m_nDimSegIndex1 = -1;
            if (auto pl = dynamic_cast<CPolylineEntity*>(hit)) {
                // find nearest segment
                double best = 1e9; int bestIdx = -1;
                CPoint sp = point;
                for (int i = 0; i < pl->GetVertexCount() - 1; ++i) {
                    CPoint p1 = WorldToScreen(pl->m_vertices[i]);
                    CPoint p2 = WorldToScreen(pl->m_vertices[i+1]);
                    double d = PointToLineDistance(sp, p1, p2);
                    if (d < best) { best = d; bestIdx = i; }
                }
                if (bestIdx >= 0) {
                    // always record which segment is nearest for highlighting
                    m_nDimSegIndex1 = bestIdx;
                }
                if (bestIdx >= 0 && best <= 8.0) {
                    // try to find an existing temporary split segment created at command start
                    CPoint sPt = pl->m_vertices[bestIdx];
                    CPoint ePt = pl->m_vertices[bestIdx+1];
                    CEntity* found = nullptr;
                    for (int id : m_tempSplitNewIDs) {
                        CEntity* ee = pDoc->FindEntityByID(id);
                        if (!ee) continue;
                        if (auto lne = dynamic_cast<CLineEntity*>(ee)) {
                            if ((lne->m_ptStart == sPt && lne->m_ptEnd == ePt) ||
                                (lne->m_ptStart == ePt && lne->m_ptEnd == sPt)) {
                                found = ee; break;
                            }
                        }
                    }
                    if (found) {
                        m_pDimEnt1 = found; m_bDimEnt1Temp = false; m_nDimSegIndex1 = bestIdx;
                    } else {
                        // not found among pre-created splits -> create and add as temporary doc entity
                        CLineEntity* ln = new CLineEntity(sPt, ePt);
                        pDoc->AddEntity(ln);
                        m_tempSplitNewIDs.push_back(ln->m_nID);
                        m_pDimEnt1 = ln; m_bDimEnt1Temp = false; m_nDimSegIndex1 = bestIdx;
                    }
                }
            }
            pDoc->DeselectAll();
            m_pDimEnt1->m_bSelected = true;
            Invalidate(FALSE);
            SetDrawState(STATE_DRAW_DIM_ANGLE_SELECT_E2);
        }
        break;
    }

    case STATE_DRAW_DIM_ANGLE_SELECT_E2: {
        CEntity* hit = pDoc->HitTestEntity(point, pDoc->m_dScale, pDoc->m_ptOffset);
        TRACE(L"[DEBUG] STATE_DRAW_DIM_ANGLE_SELECT_E2: hit=%p\n", hit);
        if (hit && hit != m_pDimEnt1) {
            m_pDimEnt2Orig = hit;
            m_pDimEnt2 = hit;
            m_bDimEnt2Temp = false;
            m_nDimSegIndex2 = -1;
            if (auto pl = dynamic_cast<CPolylineEntity*>(hit)) {
                double best = 1e9; int bestIdx = -1;
                CPoint sp = point;
                for (int i = 0; i < pl->GetVertexCount() - 1; ++i) {
                    CPoint p1 = WorldToScreen(pl->m_vertices[i]);
                    CPoint p2 = WorldToScreen(pl->m_vertices[i+1]);
                    double d = PointToLineDistance(sp, p1, p2);
                    if (d < best) { best = d; bestIdx = i; }
                }
                if (bestIdx >= 0) {
                    m_nDimSegIndex2 = bestIdx;
                }
                if (bestIdx >= 0 && best <= 8.0) {
                    // try to reuse existing temporary split segment if present
                    CPoint sPt = pl->m_vertices[bestIdx];
                    CPoint ePt = pl->m_vertices[bestIdx+1];
                    CEntity* found = nullptr;
                    for (int id : m_tempSplitNewIDs) {
                        CEntity* ee = pDoc->FindEntityByID(id);
                        if (!ee) continue;
                        if (auto lne = dynamic_cast<CLineEntity*>(ee)) {
                            if ((lne->m_ptStart == sPt && lne->m_ptEnd == ePt) ||
                                (lne->m_ptStart == ePt && lne->m_ptEnd == sPt)) {
                                found = ee; break;
                            }
                        }
                    }
                    if (found) {
                        m_pDimEnt2 = found; m_bDimEnt2Temp = false; m_nDimSegIndex2 = bestIdx;
                    } else {
                        // not found among pre-created splits -> create and add as temporary doc entity
                        CLineEntity* ln = new CLineEntity(sPt, ePt);
                        pDoc->AddEntity(ln);
                        m_tempSplitNewIDs.push_back(ln->m_nID);
                        m_pDimEnt2 = ln; m_bDimEnt2Temp = false; m_nDimSegIndex2 = bestIdx;
                    }
                }
            }
            pDoc->DeselectAll();
            m_pDimEnt2->m_bSelected = true;
            Invalidate(FALSE);
            pDoc->m_strCommandPrompt = L"Pick position on arc for dimension (inside/outside): ";
            TRACE(L"[DEBUG] OnLButtonDown: entering STATE_DRAW_DIM_ANGLE_POS, pDim1=%p pDim2=%p\n", m_pDimEnt1, m_pDimEnt2);
            SetDrawState(STATE_DRAW_DIM_ANGLE_POS);
        }
        break;
    }

    case STATE_DRAW_DIM_ANGLE_POS: {
        // The user clicked a point that indicates where the arc should be drawn (chooses inner/outer)
        // We'll compute two direction vectors from the intersection/center and create the CDimAngularEntity
        if (!m_pDimEnt1 || !m_pDimEnt2) { CompleteDrawCommand(); break; }

        // Determine reference points on each entity nearest to clicked point
        CPoint pA, pB, center;
        // For simplicity, if both are lines, use their intersection as center; otherwise use first entity center
        CLineEntity* l1 = dynamic_cast<CLineEntity*>(m_pDimEnt1);
        CLineEntity* l2 = dynamic_cast<CLineEntity*>(m_pDimEnt2);
        if (l1 && l2) {
            // compute intersection of two lines (in world coords)
            CPoint a1 = l1->m_ptStart, a2 = l1->m_ptEnd;
            CPoint b1 = l2->m_ptStart, b2 = l2->m_ptEnd;
            double x1 = a1.x, y1 = a1.y, x2 = a2.x, y2 = a2.y;
            double x3 = b1.x, y3 = b1.y, x4 = b2.x, y4 = b2.y;
            double den = (x1 - x2)*(y3 - y4) - (y1 - y2)*(x3 - x4);
            if (fabs(den) < 1e-6) {
                CPoint cp1 = ClosestPointOnSegment(a1, a2, b1);
                CPoint cp2 = ClosestPointOnSegment(b1, b2, cp1);
                center = MidPoint(cp1, cp2);
            } else {
                double px = ((x1*y2 - y1*x2)*(x3 - x4) - (x1 - x2)*(x3*y4 - y3*x4)) / den;
                double py = ((x1*y2 - y1*x2)*(y3 - y4) - (y1 - y2)*(x3*y4 - y3*x4)) / den;
                center = CPoint((int)px, (int)py);
            }
            // pick directions as nearest points on each line from center
            pA = a1; pB = b1;
        } else {
            // fallback: pick centers or midpoints
            if (l1) { center = MidPoint(l1->m_ptStart, l1->m_ptEnd); pA = l1->m_ptStart; }
            else if (l2) { center = MidPoint(l2->m_ptStart, l2->m_ptEnd); pB = l2->m_ptStart; }
            else { CompleteDrawCommand(); break; }
        }

        // Compute midpoints A1/A2 for each selected segment
        auto vec = [](CPoint a, CPoint b){ return CPoint(b.x - a.x, b.y - a.y); };
        auto dot = [](CPoint u, CPoint v){ return (double)u.x * v.x + (double)u.y * v.y; };
        auto len = [](CPoint u){ return sqrt((double)u.x*u.x + (double)u.y*u.y); };
        auto normalize = [](double ang){ while (ang < 0) ang += 2*M_PI; while (ang >= 2*M_PI) ang -= 2*M_PI; return ang; };

        CPoint A1, A2;
        if (auto le1 = dynamic_cast<CLineEntity*>(m_pDimEnt1)) A1 = MidPoint(le1->m_ptStart, le1->m_ptEnd);
        else if (auto pl = dynamic_cast<CPolylineEntity*>(m_pDimEnt1Orig)) {
            int idx = (m_nDimSegIndex1 >= 0) ? m_nDimSegIndex1 : 0;
            A1 = MidPoint(pl->m_vertices[idx], pl->m_vertices[idx+1]);
        } else A1 = pA;
        if (auto le2 = dynamic_cast<CLineEntity*>(m_pDimEnt2)) A2 = MidPoint(le2->m_ptStart, le2->m_ptEnd);
        else if (auto pl = dynamic_cast<CPolylineEntity*>(m_pDimEnt2Orig)) {
            int idx = (m_nDimSegIndex2 >= 0) ? m_nDimSegIndex2 : 0;
            A2 = MidPoint(pl->m_vertices[idx], pl->m_vertices[idx+1]);
        } else A2 = pB;

        CPoint OA1 = vec(center, A1);
        CPoint OA2 = vec(center, A2);
        CPoint OB  = vec(center, world);
        double lenOA1 = len(OA1), lenOA2 = len(OA2), lenOB = len(OB);
        bool isInner = false;
        if (lenOA1 > 1e-6 && lenOA2 > 1e-6 && lenOB > 1e-6) {
            double cos1 = dot(OA1, OB) / (lenOA1 * lenOB);
            double cos2 = dot(OA2, OB) / (lenOA2 * lenOB);
            if (cos1 > 1.0) cos1 = 1.0; if (cos1 < -1.0) cos1 = -1.0;
            if (cos2 > 1.0) cos2 = 1.0; if (cos2 < -1.0) cos2 = -1.0;
            double ang1 = acos(cos1);
            double ang2 = acos(cos2);
            // if both angles < 90deg -> inner
            if (ang1 < (M_PI/2.0) && ang2 < (M_PI/2.0)) isInner = true;
        }

        // compute angular positions for A1/A2
        double angA1 = atan2((double)OA1.y, (double)OA1.x);
        double angA2 = atan2((double)OA2.y, (double)OA2.x);
        angA1 = normalize(angA1); angA2 = normalize(angA2);

        // determine explicit candidate arcs and pick according to isInner
        double s1 = angA1, e1 = angA2; double sweep1 = e1 - s1; if (sweep1 < 0) sweep1 += 2*M_PI;
        double s2 = angA2, e2 = angA1; double sweep2 = e2 - s2; if (sweep2 < 0) sweep2 += 2*M_PI;
        double start, end;
        if (isInner) {
            // inner -> pick the smaller (minor) sweep
            if (sweep1 <= M_PI) { start = s1; end = e1; } else { start = s2; end = e2; }
        } else {
            // outer -> pick the larger (major) sweep
            if (sweep1 > M_PI) { start = s1; end = e1; } else { start = s2; end = e2; }
        }

        // compute safe radius based on nearest points on the two segments so arc stays between rays
        double d1 = 10000.0, d2 = 10000.0;
        if (auto l1p2 = dynamic_cast<CLineEntity*>(m_pDimEnt1)) {
            CPoint cp = ClosestPointOnSegment(l1p2->m_ptStart, l1p2->m_ptEnd, center);
            d1 = Distance(center, cp);
        } else if (auto pl = dynamic_cast<CPolylineEntity*>(m_pDimEnt1Orig)) {
            int idx = (m_nDimSegIndex1 >= 0) ? m_nDimSegIndex1 : 0;
            CPoint s = pl->m_vertices[idx], e = pl->m_vertices[idx+1];
            CPoint cp = ClosestPointOnSegment(s, e, center);
            d1 = Distance(center, cp);
        }
        if (auto l2p2 = dynamic_cast<CLineEntity*>(m_pDimEnt2)) {
            CPoint cp = ClosestPointOnSegment(l2p2->m_ptStart, l2p2->m_ptEnd, center);
            d2 = Distance(center, cp);
        } else if (auto pl = dynamic_cast<CPolylineEntity*>(m_pDimEnt2Orig)) {
            int idx = (m_nDimSegIndex2 >= 0) ? m_nDimSegIndex2 : 0;
            CPoint s = pl->m_vertices[idx], e = pl->m_vertices[idx+1];
            CPoint cp = ClosestPointOnSegment(s, e, center);
            d2 = Distance(center, cp);
        }
        double rad = min(d1, d2);
        if (rad < 8.0) rad = max(8.0, min(20.0, max(d1, d2)));
        int r = (int)rad;
        CPoint rp1((int)(center.x + r * cos(start)), (int)(center.y + r * sin(start)));
        CPoint rp2((int)(center.x + r * cos(end)),   (int)(center.y + r * sin(end)));
        // use 'world' (user click) as placement point and pass exact start/end angles
        CDimAngularEntity* pDimA = new CDimAngularEntity(center, rp1, rp2, world, start, end);
        pDimA->m_bTextPlaced = false;
        // add to document so it's visible; keep reference as pending for text placement
        pDoc->AddEntity(pDimA);
        pDimA->m_bSelected = true;
        if (m_pPendingDim && m_pPendingDim != pDimA) m_pPendingDim = nullptr;
        m_pPendingDim = pDimA;
        // prompt for text placement and keep temporary splits until text is placed
        pDoc->m_strCommandPrompt = L"Place dimension text: ";
        SetDrawState(STATE_DRAW_TEXT_POS);
        break;
    }

    case STATE_DRAW_DIM_RADIUS_SELECT: {
        CEntity* hit = pDoc->HitTestEntity(point, pDoc->m_dScale, pDoc->m_ptOffset);
        if (hit) {
            if (dynamic_cast<CCircleEntity*>(hit) || dynamic_cast<CArcEntity*>(hit)) {
                if (m_bDimRadiusSrcTemp) { delete m_pDimRadiusSrcEnt; m_bDimRadiusSrcTemp = false; }
                m_pDimRadiusSrcEnt = hit;
                pDoc->DeselectAll();
                hit->m_bSelected = true;
                Invalidate(FALSE);
                pDoc->m_strCommandPrompt = L"Specify leader line position: ";
                SetDrawState(STATE_DRAW_DIM_RADIUS_POS);
            } else if (auto pPoly = dynamic_cast<CPolylineEntity*>(hit)) {
                CPoint center, ptStart, ptMid, ptEnd;
                int radius, idxS, idxE;
                if (DetectArcInPolyline(pPoly, point, pDoc->m_dScale, pDoc->m_ptOffset,
                                        center, radius, ptStart, ptMid, ptEnd, idxS, idxE)) {
                    if (m_bDimRadiusSrcTemp) delete m_pDimRadiusSrcEnt;
                    double angS = atan2((double)(ptStart.y - center.y), (double)(ptStart.x - center.x));
                    double angE = atan2((double)(ptEnd.y - center.y),   (double)(ptEnd.x - center.x));
                    { double sw = angE - angS; while (sw > M_PI) sw -= 2.0*M_PI; while (sw < -M_PI) sw += 2.0*M_PI; if (sw < 0) { double t=angS; angS=angE; angE=t; } }
                    CArcEntity* pTemp = new CArcEntity();
                    pTemp->SetArcByCenter(center, radius, angS, angE);
                    m_pDimRadiusSrcEnt = pTemp;
                    m_bDimRadiusSrcTemp = true;
                    pDoc->DeselectAll();
                    Invalidate(FALSE);
                    pDoc->m_strCommandPrompt = L"Specify leader line position: ";
                    SetDrawState(STATE_DRAW_DIM_RADIUS_POS);
                } else {
                    pDoc->m_strCommandPrompt = L"Not a circle or arc. Select circle or arc: ";
                    CMainFrame* pFrame = (CMainFrame*)AfxGetMainWnd();
                    if (pFrame && pFrame->m_wndCmdLine.GetSafeHwnd()) {
                        pFrame->m_wndCmdLine.SetWindowText(pDoc->m_strCommandPrompt);
                        int nLen = pDoc->m_strCommandPrompt.GetLength();
                        pFrame->m_wndCmdLine.SetSel(nLen, nLen);
                    }
                }
            } else {
                pDoc->m_strCommandPrompt = L"Not a circle or arc. Select circle or arc: ";
                CMainFrame* pFrame = (CMainFrame*)AfxGetMainWnd();
                if (pFrame && pFrame->m_wndCmdLine.GetSafeHwnd()) {
                    pFrame->m_wndCmdLine.SetWindowText(pDoc->m_strCommandPrompt);
                    int nLen = pDoc->m_strCommandPrompt.GetLength();
                    pFrame->m_wndCmdLine.SetSel(nLen, nLen);
                }
            }
        }
        break;
    }

    case STATE_DRAW_DIM_RADIUS_POS: {
        if (m_pDimRadiusSrcEnt) {
            CPoint center;
            int radius = 0;
            if (auto pCirc = dynamic_cast<CCircleEntity*>(m_pDimRadiusSrcEnt)) {
                center = pCirc->m_ptCenter; radius = pCirc->m_nRadius;
            } else if (auto pArc = dynamic_cast<CArcEntity*>(m_pDimRadiusSrcEnt)) {
                center = pArc->m_ptCenter; radius = pArc->m_nRadius;
            }
            if (radius > 0) {
                double dx = (double)(world.x - center.x);
                double dy = (double)(world.y - center.y);
                double dist = sqrt(dx*dx + dy*dy);
                if (dist < 1e-6) dist = 1.0;
                double angle = atan2(dy, dx);
                CDimRadiusEntity* pDim = new CDimRadiusEntity(center, radius, angle);
                pDim->m_bTextPlaced = false;
                pDim->m_color = m_currentColor;
                pDim->m_nLineStyle = m_currentLineStyle;
                pDim->m_nLineWidth = m_currentLineWidth;
                pDoc->AddEntity(pDim);
                pDim->m_bSelected = true;
                m_pPendingDim = pDim;
                if (m_bDimRadiusSrcTemp) { delete m_pDimRadiusSrcEnt; m_bDimRadiusSrcTemp = false; }
                m_pDimRadiusSrcEnt = nullptr;
                pDoc->m_strCommandPrompt = L"Place dimension text: ";
                SetDrawState(STATE_DRAW_TEXT_POS);
            }
        } else {
            CompleteDrawCommand();
        }
        break;
    }

    case STATE_DRAW_DIM_DIAMETER_SELECT: {
        CEntity* hit = pDoc->HitTestEntity(point, pDoc->m_dScale, pDoc->m_ptOffset);
        if (hit) {
            if (dynamic_cast<CCircleEntity*>(hit) || dynamic_cast<CArcEntity*>(hit)) {
                if (m_bDimDiamSrcTemp) { delete m_pDimDiamSrcEnt; m_bDimDiamSrcTemp = false; }
                m_pDimDiamSrcEnt = hit;
                pDoc->DeselectAll();
                hit->m_bSelected = true;
                Invalidate(FALSE);
                pDoc->m_strCommandPrompt = L"Specify diameter line position: ";
                SetDrawState(STATE_DRAW_DIM_DIAMETER_POS);
            } else if (auto pPoly = dynamic_cast<CPolylineEntity*>(hit)) {
                CPoint center, ptStart, ptMid, ptEnd;
                int radius, idxS, idxE;
                if (DetectArcInPolyline(pPoly, point, pDoc->m_dScale, pDoc->m_ptOffset,
                                        center, radius, ptStart, ptMid, ptEnd, idxS, idxE)) {
                    if (m_bDimDiamSrcTemp) delete m_pDimDiamSrcEnt;
                    double angS = atan2((double)(ptStart.y - center.y), (double)(ptStart.x - center.x));
                    double angE = atan2((double)(ptEnd.y - center.y),   (double)(ptEnd.x - center.x));
                    { double sw = angE - angS; while (sw > M_PI) sw -= 2.0*M_PI; while (sw < -M_PI) sw += 2.0*M_PI; if (sw < 0) { double t=angS; angS=angE; angE=t; } }
                    CArcEntity* pTemp = new CArcEntity();
                    pTemp->SetArcByCenter(center, radius, angS, angE);
                    m_pDimDiamSrcEnt = pTemp;
                    m_bDimDiamSrcTemp = true;
                    pDoc->DeselectAll();
                    Invalidate(FALSE);
                    pDoc->m_strCommandPrompt = L"Specify diameter line position: ";
                    SetDrawState(STATE_DRAW_DIM_DIAMETER_POS);
                } else {
                    pDoc->m_strCommandPrompt = L"Not a circle or arc. Select circle or arc: ";
                    CMainFrame* pFrame = (CMainFrame*)AfxGetMainWnd();
                    if (pFrame && pFrame->m_wndCmdLine.GetSafeHwnd()) {
                        pFrame->m_wndCmdLine.SetWindowText(pDoc->m_strCommandPrompt);
                        int nLen = pDoc->m_strCommandPrompt.GetLength();
                        pFrame->m_wndCmdLine.SetSel(nLen, nLen);
                    }
                }
            } else {
                pDoc->m_strCommandPrompt = L"Not a circle or arc. Select circle or arc: ";
                CMainFrame* pFrame = (CMainFrame*)AfxGetMainWnd();
                if (pFrame && pFrame->m_wndCmdLine.GetSafeHwnd()) {
                    pFrame->m_wndCmdLine.SetWindowText(pDoc->m_strCommandPrompt);
                    int nLen = pDoc->m_strCommandPrompt.GetLength();
                    pFrame->m_wndCmdLine.SetSel(nLen, nLen);
                }
            }
        }
        break;
    }

    case STATE_DRAW_DIM_DIAMETER_POS: {
        if (m_pDimDiamSrcEnt) {
            CPoint center; int radius = 0;
            if (auto pCirc = dynamic_cast<CCircleEntity*>(m_pDimDiamSrcEnt)) { center = pCirc->m_ptCenter; radius = pCirc->m_nRadius; }
            else if (auto pArc = dynamic_cast<CArcEntity*>(m_pDimDiamSrcEnt)) { center = pArc->m_ptCenter; radius = pArc->m_nRadius; }
            if (radius > 0) {
                double dx = (double)(world.x - center.x), dy = (double)(world.y - center.y);
                double dist = sqrt(dx*dx + dy*dy); if (dist < 1e-6) dist = 1.0;
                double angle = atan2(dy, dx);
                CDimDiamEntity* pDim = new CDimDiamEntity(center, radius, angle);
                pDim->m_bTextPlaced = false;
                pDim->m_color = m_currentColor;
                pDim->m_nLineStyle = m_currentLineStyle;
                pDim->m_nLineWidth = m_currentLineWidth;
                pDoc->AddEntity(pDim);
                pDim->m_bSelected = true;
                m_pPendingDim = pDim;
                if (m_bDimDiamSrcTemp) { delete m_pDimDiamSrcEnt; m_bDimDiamSrcTemp = false; }
                m_pDimDiamSrcEnt = nullptr;
                pDoc->m_strCommandPrompt = L"Place dimension text: ";
                SetDrawState(STATE_DRAW_TEXT_POS);
            }
        } else {
            CompleteDrawCommand();
        }
        break;
    }

    case STATE_DRAW_DIM_ARCLEN_SELECT: {
        CEntity* hit = pDoc->HitTestEntity(point, pDoc->m_dScale, pDoc->m_ptOffset);
        if (hit) {
            if (dynamic_cast<CArcEntity*>(hit)) {
                if (m_bDimArcLenSrcTemp) { delete m_pDimArcLenSrcEnt; m_bDimArcLenSrcTemp = false; }
                m_pDimArcLenSrcEnt = hit;
                pDoc->DeselectAll();
                hit->m_bSelected = true;
                Invalidate(FALSE);
                pDoc->m_strCommandPrompt = L"Specify dimension arc position: ";
                SetDrawState(STATE_DRAW_DIM_ARCLEN_POS);
            } else if (auto pPoly = dynamic_cast<CPolylineEntity*>(hit)) {
                CPoint center, ptStart, ptMid, ptEnd;
                int radius, idxS, idxE;
                if (DetectArcInPolyline(pPoly, point, pDoc->m_dScale, pDoc->m_ptOffset,
                                        center, radius, ptStart, ptMid, ptEnd, idxS, idxE)) {
                    if (m_bDimArcLenSrcTemp) delete m_pDimArcLenSrcEnt;
                    double angS = atan2((double)(ptStart.y - center.y), (double)(ptStart.x - center.x));
                    double angE = atan2((double)(ptEnd.y - center.y),   (double)(ptEnd.x - center.x));
                    { double sw = angE - angS; while (sw > M_PI) sw -= 2.0*M_PI; while (sw < -M_PI) sw += 2.0*M_PI; if (sw < 0) { double t=angS; angS=angE; angE=t; } }
                    CArcEntity* pTemp = new CArcEntity();
                    pTemp->SetArcByCenter(center, radius, angS, angE);
                    m_pDimArcLenSrcEnt = pTemp;
                    m_bDimArcLenSrcTemp = true;
                    pDoc->DeselectAll();
                    Invalidate(FALSE);
                    pDoc->m_strCommandPrompt = L"Specify dimension arc position: ";
                    SetDrawState(STATE_DRAW_DIM_ARCLEN_POS);
                } else {
                    pDoc->m_strCommandPrompt = L"Not a arc. Select arc: ";
                    CMainFrame* pFrame = (CMainFrame*)AfxGetMainWnd();
                    if (pFrame && pFrame->m_wndCmdLine.GetSafeHwnd()) {
                        pFrame->m_wndCmdLine.SetWindowText(pDoc->m_strCommandPrompt);
                        int nLen = pDoc->m_strCommandPrompt.GetLength();
                        pFrame->m_wndCmdLine.SetSel(nLen, nLen);
                    }
                }
            } else {
                pDoc->m_strCommandPrompt = L"Not a arc. Select arc: ";
                CMainFrame* pFrame = (CMainFrame*)AfxGetMainWnd();
                if (pFrame && pFrame->m_wndCmdLine.GetSafeHwnd()) {
                    pFrame->m_wndCmdLine.SetWindowText(pDoc->m_strCommandPrompt);
                    int nLen = pDoc->m_strCommandPrompt.GetLength();
                    pFrame->m_wndCmdLine.SetSel(nLen, nLen);
                }
            }
        }
        break;
    }

    case STATE_DRAW_DIM_ARCLEN_POS: {
        if (m_pDimArcLenSrcEnt) {
            auto pArc = dynamic_cast<CArcEntity*>(m_pDimArcLenSrcEnt);
            if (!pArc) { CompleteDrawCommand(); break; }
            CPoint center = pArc->m_ptCenter;
            int arcRadius = pArc->m_nRadius;
            double dist = Distance(center, world);
            if (dist <= arcRadius + 1) {
                pDoc->m_strCommandPrompt = L"this position is illegal";
                Invalidate(FALSE);
                break;
            }
            double angStart = atan2((double)(pArc->m_ptStart.y - center.y), (double)(pArc->m_ptStart.x - center.x));
            double angEnd = atan2((double)(pArc->m_ptEnd.y - center.y), (double)(pArc->m_ptEnd.x - center.x));
            double sweep = angEnd - angStart;
            if (sweep < 0) sweep += 2*M_PI;
            double angMid = atan2((double)(pArc->m_ptMid.y - center.y), (double)(pArc->m_ptMid.x - center.x));
            double midSweep = angMid - angStart;
            if (midSweep < 0) midSweep += 2*M_PI;
            TRACE(L"[ARC DIM] angS=%.2f angE=%.2f angM=%.2f sweep=%.2f midSweep=%.2f swap=%d rad=%d\n",
                  angStart*180/M_PI, angEnd*180/M_PI, angMid*180/M_PI,
                  sweep*180/M_PI, midSweep*180/M_PI, (midSweep > sweep) ? 1 : 0, arcRadius);
            if (midSweep > sweep) {
                double tmp = angStart; angStart = angEnd; angEnd = tmp;
                sweep = 2*M_PI - sweep;
            }
            CDimArcLengthEntity* pDim = new CDimArcLengthEntity(center, arcRadius, (int)dist, angStart, angEnd, sweep);
            pDim->m_bTextPlaced = false;
            pDim->m_color = m_currentColor;
            pDim->m_nLineStyle = m_currentLineStyle;
            pDim->m_nLineWidth = m_currentLineWidth;
            pDoc->AddEntity(pDim);
            pDim->m_bSelected = true;
            m_pPendingDim = pDim;
            if (m_bDimArcLenSrcTemp) { delete m_pDimArcLenSrcEnt; m_bDimArcLenSrcTemp = false; }
            m_pDimArcLenSrcEnt = nullptr;
            pDoc->m_strCommandPrompt = L"Place dimension text: ";
            SetDrawState(STATE_DRAW_TEXT_POS);
        } else {
            CompleteDrawCommand();
        }
        break;
    }

    case STATE_DRAW_DIM_COORD_PICK: {
        m_ptCoordPoint = world;
        m_tempPts.push_back(world);
        m_bCoordDimMode = true;
        pDoc->m_strCommandPrompt = L"Place coordinate text: ";
        SetDrawState(STATE_DRAW_TEXT_POS);
        break;
    }

    case STATE_MOVE_SELECT: {
        CEntity* hit = pDoc->HitTestEntity(point, pDoc->m_dScale, pDoc->m_ptOffset);
        TRACE(L"[DEBUG] STATE_MOVE_SELECT: hit=%p, world=(%d,%d)\n", hit, world.x, world.y);
        if (hit) {
            pDoc->DeselectAll();
            hit->m_bSelected = true;
            m_tempPts.push_back(world);
            SetDrawState(STATE_MOVE_BASE);
        }
        break;
    }

    case STATE_MOVE_BASE:
        TRACE(L"[DEBUG] STATE_MOVE_BASE: world=(%d,%d), tempPts=%d\n", world.x, world.y, (int)m_tempPts.size());
        m_tempPts.push_back(world);
        SetDrawState(STATE_MOVE_DEST);
        break;

    case STATE_MOVE_DEST: {
        pDoc->RecordModifyUndo();
        double dx = world.x - m_tempPts[1].x;
        double dy = world.y - m_tempPts[1].y;
        int nCount = pDoc->GetSelectedCount();
        TRACE(L"[DEBUG] STATE_MOVE_DEST: world=(%d,%d), base=(%d,%d), dx=%.0f, dy=%.0f, nSel=%d, tempPts=%d\n",
              world.x, world.y, m_tempPts[1].x, m_tempPts[1].y, dx, dy, nCount, (int)m_tempPts.size());
        pDoc->MoveSelected(dx, dy);
        pDoc->DeselectAll();
        CompleteDrawCommand();
        pDoc->m_strCommandPrompt.Format(L"Moved %d entities (dx=%d, dy=%d)", nCount, (int)dx, (int)dy);
        UpdateStatusBar();
        break;
    }

    case STATE_COPY_SELECT: {
        CEntity* hit = pDoc->HitTestEntity(point, pDoc->m_dScale, pDoc->m_ptOffset);
        TRACE(L"[DEBUG] STATE_COPY_SELECT: hit=%p, world=(%d,%d)\n", hit, world.x, world.y);
        if (hit) {
            pDoc->DeselectAll();
            hit->m_bSelected = true;
            m_tempPts.push_back(world);
            SetDrawState(STATE_COPY_BASE);
        }
        break;
    }

    case STATE_COPY_BASE:
        TRACE(L"[DEBUG] STATE_COPY_BASE: world=(%d,%d), tempPts=%d\n", world.x, world.y, (int)m_tempPts.size());
        m_tempPts.push_back(world);
        SetDrawState(STATE_COPY_DEST);
        break;

    case STATE_COPY_DEST: {
        double dx = world.x - m_tempPts[1].x;
        double dy = world.y - m_tempPts[1].y;
        TRACE(L"[DEBUG] STATE_COPY_DEST: world=(%d,%d), base=(%d,%d), dx=%.0f, dy=%.0f, tempPts=%d\n",
              world.x, world.y, m_tempPts[1].x, m_tempPts[1].y, dx, dy, (int)m_tempPts.size());
        auto selectedEnts = pDoc->GetSelectedEntities();
        int nCount = (int)selectedEnts.size();
        std::vector<int> newIDs;
        for (auto* pEnt : selectedEnts) {
            CEntity* pCopy = pEnt->Clone();
            pCopy->Move(dx, dy);
            pDoc->AddEntity(pCopy, false);
            newIDs.push_back(pCopy->m_nID);
        }
        pDoc->RecordCreateUndo(newIDs);
        pDoc->DeselectAll();
        CompleteDrawCommand();
        pDoc->m_strCommandPrompt.Format(L"Copied %d entities (dx=%d, dy=%d)", nCount, (int)dx, (int)dy);
        UpdateStatusBar();
        break;
    }

    case STATE_ROTATE_SELECT: {
        CEntity* hit = pDoc->HitTestEntity(point, pDoc->m_dScale, pDoc->m_ptOffset);
        TRACE(L"[DEBUG] STATE_ROTATE_SELECT: hit=%p\n", hit);
        if (hit) {
            pDoc->DeselectAll();
            hit->m_bSelected = true;
            SetDrawState(STATE_ROTATE_CENTER);
        }
        break;
    }

    case STATE_ROTATE_CENTER:
        TRACE(L"[DEBUG] STATE_ROTATE_CENTER: world=(%d,%d)\n", world.x, world.y);
        m_tempPts.push_back(world);
        SetDrawState(STATE_ROTATE_ANGLE);
        break;

    case STATE_ROTATE_ANGLE: {
        MessageBeep(MB_ICONASTERISK);
        pDoc->RecordModifyUndo();
        CPoint base = m_tempPts[0];
        double angle = atan2((double)(world.y - base.y), (double)(world.x - base.x));
        int nCount = pDoc->GetSelectedCount();
        TRACE(L"[DEBUG] STATE_ROTATE_ANGLE: world=(%d,%d), base=(%d,%d), angle=%.3f rad (%.1f deg), nSel=%d\n",
              world.x, world.y, base.x, base.y, angle, angle * 180.0 / M_PI, nCount);
        if (nCount == 0) {
            pDoc->m_strCommandPrompt = L"ROTATE: No entities selected! Select objects first.";
        } else {
            pDoc->RotateSelected(base, angle);
            pDoc->m_strCommandPrompt.Format(L"Rotated %d entities (angle=%.1f deg)", nCount, angle * 180.0 / M_PI);
        }
        pDoc->DeselectAll();
        CompleteDrawCommand();
        UpdateStatusBar();
        break;
    }

    case STATE_SCALE_SELECT: {
        CEntity* hit = pDoc->HitTestEntity(point, pDoc->m_dScale, pDoc->m_ptOffset);
        TRACE(L"[DEBUG] STATE_SCALE_SELECT: hit=%p\n", hit);
        if (hit) {
            pDoc->DeselectAll();
            hit->m_bSelected = true;
            SetDrawState(STATE_SCALE_BASE);
        }
        break;
    }

    case STATE_SCALE_BASE:
        TRACE(L"[DEBUG] STATE_SCALE_BASE: world=(%d,%d)\n", world.x, world.y);
        m_tempPts.push_back(world);
        SetDrawState(STATE_SCALE_FACTOR);
        break;

    case STATE_SCALE_FACTOR: {
        MessageBeep(MB_ICONASTERISK);
        pDoc->RecordModifyUndo();
        CPoint base = m_tempPts[0];
        double dist = Distance(base, world);
        if (dist < 1.0) dist = 1.0;
        double factor = dist / 100.0;
        int nCount = pDoc->GetSelectedCount();
        TRACE(L"[DEBUG] STATE_SCALE_FACTOR: world=(%d,%d), base=(%d,%d), dist=%.0f, factor=%.3f, nSel=%d\n",
              world.x, world.y, base.x, base.y, dist, factor, nCount);
        if (nCount == 0) {
            pDoc->m_strCommandPrompt = L"SCALE: No entities selected!";
        } else {
            pDoc->ScaleSelected(base, factor);
            pDoc->m_strCommandPrompt.Format(L"Scaled %d entities (factor=%.2f)", nCount, factor);
        }
        pDoc->DeselectAll();
        CompleteDrawCommand();
        UpdateStatusBar();
        break;
    }

    case STATE_MIRROR_SELECT: {
        CEntity* hit = pDoc->HitTestEntity(point, pDoc->m_dScale, pDoc->m_ptOffset);
        TRACE(L"[DEBUG] STATE_MIRROR_SELECT: hit=%p\n", hit);
        if (hit) {
            pDoc->DeselectAll();
            hit->m_bSelected = true;
            SetDrawState(STATE_MIRROR_P1);
        }
        break;
    }

    case STATE_MIRROR_P1:
        TRACE(L"[DEBUG] STATE_MIRROR_P1: world=(%d,%d)\n", world.x, world.y);
        m_tempPts.push_back(world);
        SetDrawState(STATE_MIRROR_P2);
        break;

    case STATE_MIRROR_P2: {
        CPoint p1 = m_tempPts[0];
        CPoint p2 = world;
        double distP1P2 = Distance(p1, p2);
        int nCount = pDoc->GetSelectedCount();
        TRACE(L"[DEBUG] STATE_MIRROR_P2: p1=(%d,%d), p2=(%d,%d), dist=%.0f, nSel=%d\n",
              p1.x, p1.y, p2.x, p2.y, distP1P2, nCount);
        if (distP1P2 > 0) {
            pDoc->RecordModifyUndo();
            if (nCount == 0) {
                pDoc->m_strCommandPrompt = L"MIRROR: No entities selected!";
            } else {
                pDoc->MirrorSelected(p1, p2);
                pDoc->m_strCommandPrompt.Format(L"Mirrored %d entities", nCount);
            }
        } else {
            pDoc->m_strCommandPrompt = L"Mirror: line too short, cancelled";
        }
        pDoc->DeselectAll();
        CompleteDrawCommand();
        UpdateStatusBar();
        break;
    }

    case STATE_OFFSET_SELECT: {
        CEntity* hit = pDoc->HitTestEntity(point, pDoc->m_dScale, pDoc->m_ptOffset);
        TRACE(L"[DEBUG] STATE_OFFSET_SELECT: hit=%p\n", hit);
        if (hit) {
            pDoc->DeselectAll();
            hit->m_bSelected = true;
            m_tempPts.push_back(world);
            SetDrawState(STATE_OFFSET_DIST);
        }
        break;
    }

    case STATE_OFFSET_DIST: {
        CPoint ref = m_tempPts[0];
        double dx = world.x - ref.x;
        double dy = world.y - ref.y;
        TRACE(L"[DEBUG] STATE_OFFSET_DIST: world=(%d,%d), ref=(%d,%d), dx=%.0f, dy=%.0f\n",
              world.x, world.y, ref.x, ref.y, dx, dy);
        auto selectedEnts = pDoc->GetSelectedEntities();
        int nCount = (int)selectedEnts.size();
        std::vector<int> newIDs;
        for (auto* pEnt : selectedEnts) {
            CEntity* pCopy = pEnt->Clone();
            pCopy->Move(dx, dy);
            pDoc->AddEntity(pCopy, false);
            newIDs.push_back(pCopy->m_nID);
        }
        pDoc->RecordCreateUndo(newIDs);
        pDoc->DeselectAll();
        CompleteDrawCommand();
        pDoc->m_strCommandPrompt.Format(L"Offset %d entities (dx=%d, dy=%d)", nCount, (int)dx, (int)dy);
        UpdateStatusBar();
        break;
    }

    case STATE_CHAMFER_SELECT_FIRST: {
        ChamferSegmentRef seg = HitTestChamferSegment(point);
        if (seg.IsValid()) {
            pDoc->DeselectAll();
            m_chamferFirstSegment = seg;
            m_pChamferFirst = (seg.pEntity->m_Type == ENT_LINE)
                ? static_cast<CLineEntity*>(seg.pEntity)
                : nullptr;
            SetDrawState(STATE_CHAMFER_SELECT_SECOND);
        } else {
            pDoc->m_strCommandPrompt = L"CHAMFER Select a line, rectangle edge, or polyline segment: ";
            UpdateStatusBar();
        }
        break;
    }

    case STATE_CHAMFER_SELECT_SECOND: {
        ChamferSegmentRef seg = HitTestChamferSegment(point);
        if (seg.IsValid() &&
            (seg.pEntity != m_chamferFirstSegment.pEntity ||
             seg.segmentIndex != m_chamferFirstSegment.segmentIndex)) {
            ApplyChamfer(m_chamferFirstSegment, seg, m_dChamferDistance);
        } else {
            pDoc->m_strCommandPrompt = L"CHAMFER Select a different second segment: ";
            UpdateStatusBar();
        }
        break;
    }

    case STATE_FILLET_SELECT_FIRST: {
        ChamferSegmentRef seg = HitTestChamferSegment(point);
        if (seg.IsValid()) {
            pDoc->DeselectAll();
            m_filletFirstSegment = seg;
            SetDrawState(STATE_FILLET_SELECT_SECOND);
        } else {
            pDoc->m_strCommandPrompt = L"FILLET Select a line, rectangle edge, or polyline segment: ";
            UpdateStatusBar();
        }
        break;
    }

    case STATE_FILLET_SELECT_SECOND: {
        ChamferSegmentRef seg = HitTestChamferSegment(point);
        if (seg.IsValid() &&
            (seg.pEntity != m_filletFirstSegment.pEntity ||
             seg.segmentIndex != m_filletFirstSegment.segmentIndex)) {
            ApplyFillet(m_filletFirstSegment, seg, m_dFilletRadius);
        } else {
            pDoc->m_strCommandPrompt = L"FILLET Select a different second segment: ";
            UpdateStatusBar();
        }
        break;
    }

    case STATE_ARRAY_SELECT: {
        CEntity* hit = pDoc->HitTestEntity(point, pDoc->m_dScale, pDoc->m_ptOffset);
        if (hit) {
            hit->m_bSelected = true;
            pDoc->m_strCommandPrompt.Format(L"ARRAY Select objects (ENTER to finish): %d selected",
                                            pDoc->GetSelectedCount());
            SyncCommandLinePrompt();
            UpdateStatusBar();
        }
        break;
    }

    case STATE_ZOOM_WINDOW_P1:
        m_ptDragStart = point;
        SetDrawState(STATE_ZOOM_WINDOW_P2);
        break;

    case STATE_ZOOM_WINDOW_P2: {
        CRect rc(m_ptDragStart, point);
        rc.NormalizeRect();
        if (rc.Width() > 5 && rc.Height() > 5) {
            CPoint topLeft = ScreenToWorld(rc.TopLeft());
            CPoint bottomRight = ScreenToWorld(rc.BottomRight());
            CRect rcWorld(topLeft, bottomRight);
            rcWorld.NormalizeRect();
            rcWorld.InflateRect(10, 10);

            CRect rcClient;
            GetClientRect(&rcClient);

            if (rcWorld.Width() > 0 && rcWorld.Height() > 0) {
                FitViewToWorldBounds(pDoc, rcWorld, rcClient, 0);
            }
        }
        CompleteDrawCommand();
        UpdateStatusBar();
        Invalidate(FALSE);
        break;
    }

    default:
        break;
    }

    m_ptCurrent = point;
    UpdateStatusBar();
    Invalidate(FALSE);

    CView::OnLButtonDown(nFlags, point);
}

// ============================================================
// LButtonUp
// ============================================================
void CLargeHWView::OnLButtonUp(UINT nFlags, CPoint point)
{
    CLargeHWDoc* pDoc = GetDocument();

    if (pDoc->m_drawState == STATE_WINDOW_SELECT && m_bDragging) {
        CRect rc(m_ptDragStart, point);
        rc.NormalizeRect();
        if (rc.Width() > 5 && rc.Height() > 5) {
            pDoc->SelectByWindow(rc, pDoc->m_dScale, pDoc->m_ptOffset);
        } else {
            pDoc->SelectByPoint(point, pDoc->m_dScale, pDoc->m_ptOffset);
        }
        pDoc->m_drawState = STATE_IDLE;
        m_bDragging = false;
        m_bDrawing = false;
        UpdateStatusBar();
        Invalidate(FALSE);
    }

    if (pDoc->m_drawState == STATE_ZOOM_WINDOW_P1 && m_bDragging) {
        CRect rc(m_ptDragStart, point);
        rc.NormalizeRect();
        if (rc.Width() > 5 && rc.Height() > 5) {
            CPoint topLeft = ScreenToWorld(rc.TopLeft());
            CPoint bottomRight = ScreenToWorld(rc.BottomRight());
            CRect rcWorld(topLeft, bottomRight);
            rcWorld.NormalizeRect();
            rcWorld.InflateRect(10, 10);

            CRect rcClient;
            GetClientRect(&rcClient);

            if (rcWorld.Width() > 0 && rcWorld.Height() > 0) {
                FitViewToWorldBounds(pDoc, rcWorld, rcClient, 0);
                pDoc->m_strCommandPrompt.Format(L"Zoom Window: scale=%.2f",
                                                pDoc->m_dScale * GetModelUnitScale(pDoc));
            }
        } else {
            pDoc->m_strCommandPrompt = L"Zoom Window: area too small, cancelled";
        }
        pDoc->m_drawState = STATE_IDLE;
        m_bDragging = false;
        m_bDrawing = false;
        UpdateStatusBar();
        Invalidate(FALSE);
    }

    if (m_bDragging && m_nGripIndex >= 0 && m_pGripEntity) {
        CPoint world = ScreenToWorld(point);
        m_pGripEntity->SetGrip(m_nGripIndex, world);
        m_bDragging = false;
        m_nGripIndex = -1;
        m_pGripEntity = nullptr;
        pDoc->SetModified(true);
        Invalidate(FALSE);
    }

    CView::OnLButtonUp(nFlags, point);
}

// ============================================================
// MouseMove - Update coordinates + preview
// ============================================================
void CLargeHWView::OnMouseMove(UINT nFlags, CPoint point)
{
    CLargeHWDoc* pDoc = GetDocument();

    // Pan with middle button
    if (m_bPanning) {
        CPoint delta(point.x - m_ptPanStart.x, point.y - m_ptPanStart.y);
        pDoc->m_ptOffset = CPoint(m_ptPanOffsetStart.x + delta.x, m_ptPanOffsetStart.y + delta.y);
        m_ptCurrent = point;
        UpdateStatusBar();
        Invalidate(FALSE);
        return;
    }

    // Always track raw mouse for smooth crosshair
    m_ptCurrent = point;

    // Compute snapped world point
    CPoint world = ScreenToWorld(point);
    world = SnapToGrid(world);
    CPoint worldBeforeSnap = world;
    if (pDoc->m_bObjectSnap) {
        world = SnapToObjects(world);
    }
    m_bSnapActive = (worldBeforeSnap.x != world.x || worldBeforeSnap.y != world.y);
    if (m_bSnapActive) {
        m_ptSnapped = WorldToScreen(world);
    }

    // Grip drag with ortho
    if (m_bDragging && m_nGripIndex >= 0 && m_pGripEntity) {
        CPoint refGrip = m_pGripEntity->GetGrip(0);
        world = ApplyOrtho(world, refGrip);
        m_pGripEntity->SetGrip(m_nGripIndex, world);
        pDoc->SetModified(true);
        UpdateStatusBar();
        Invalidate(FALSE);
        return;
    }

    // Apply ortho for drawing/modify commands
    CadDrawState state = pDoc->m_drawState;
    if ((state == STATE_DRAW_LINE_P2 || state == STATE_MOVE_DEST || state == STATE_COPY_DEST ||
         state == STATE_ROTATE_ANGLE || state == STATE_SCALE_FACTOR || state == STATE_MIRROR_P2 ||
         state == STATE_OFFSET_DIST) && !m_tempPts.empty()) {
        world = ApplyOrtho(world, m_tempPts.back());
        m_ptSnapped = WorldToScreen(world);
        m_bSnapActive = true;
    }

    UpdateStatusBar();

    // Always invalidate to keep crosshair tracking and snap marker live
    Invalidate(FALSE);

    CView::OnMouseMove(nFlags, point);
}

// ============================================================
// RButtonDown - Context menu / End polyline
// ============================================================
void CLargeHWView::OnRButtonDown(UINT nFlags, CPoint point)
{
    CLargeHWDoc* pDoc = GetDocument();

    CView::OnRButtonDown(nFlags, point);
}

// ============================================================
// MouseWheel - Zoom
// ============================================================
BOOL CLargeHWView::OnMouseWheel(UINT nFlags, short zDelta, CPoint pt)
{
    CLargeHWDoc* pDoc = GetDocument();
    if (!pDoc)
        return CView::OnMouseWheel(nFlags, zDelta, pt);

    ScreenToClient(&pt);
    m_ptCurrent = pt;
    CPoint worldBefore = ScreenToWorld(pt);

    double zoomFactor = (zDelta > 0) ? 1.1 : 1.0 / 1.1;
    double newScale = ClampViewScale(pDoc, pDoc->m_dScale * zoomFactor);

    pDoc->m_dScale = newScale;
    pDoc->m_ptOffset.x = ScriptRound(pt.x - worldBefore.x * pDoc->m_dScale);
    pDoc->m_ptOffset.y = ScriptRound(pt.y + worldBefore.y * pDoc->m_dScale);

    UpdateStatusBar();
    Invalidate(FALSE);

    return TRUE;
}

// ============================================================
// MButton Down/Up - Pan with middle mouse
// ============================================================
void CLargeHWView::OnMButtonDown(UINT nFlags, CPoint point)
{
    CLargeHWDoc* pDoc = GetDocument();
    m_bPanning = true;
    m_ptPanStart = point;
    m_ptPanOffsetStart = pDoc->m_ptOffset;
    SetCapture();
    CView::OnMButtonDown(nFlags, point);
}

void CLargeHWView::OnMButtonUp(UINT nFlags, CPoint point)
{
    m_bPanning = false;
    ReleaseCapture();
    UpdateStatusBar();
    Invalidate(FALSE);
    CView::OnMButtonUp(nFlags, point);
}

// ============================================================
// Keyboard
// ============================================================
void CLargeHWView::OnKeyDown(UINT nChar, UINT nRepCnt, UINT nFlags)
{
    CLargeHWDoc* pDoc = GetDocument();

    switch (nChar) {
    case VK_ESCAPE:
        OnCancelCommand();
        break;

    case 'C':
        if (pDoc->m_drawState == STATE_DRAW_LINE_P2 && m_tempPts.size() >= 2) {
            if (m_bScriptRecording && !m_bRunningScript && !m_bSubmittingCommandLine)
                RecordScriptInput(L"C");
            CloseLineCommand();
            return;
        }
        if (pDoc->m_drawState == STATE_DRAW_POLYLINE_POINT && m_tempPts.size() >= 2) {
            if (m_bScriptRecording && !m_bRunningScript && !m_bSubmittingCommandLine)
                RecordScriptInput(L"C");
            FinishPolylineCommand(true);
            return;
        }
        break;

    case VK_RETURN:
        if (pDoc->m_drawState == STATE_DRAW_LINE_P2 && !m_tempPts.empty()) {
            if (m_bScriptRecording && !m_bRunningScript && !m_bSubmittingCommandLine)
                RecordScriptInput(L"");
            CompleteDrawCommand();
            Invalidate(FALSE);
        }
        if (pDoc->m_drawState == STATE_DRAW_POLYLINE_POINT && m_tempPts.size() >= 2) {
            if (m_bScriptRecording && !m_bRunningScript && !m_bSubmittingCommandLine)
                RecordScriptInput(L"");
            FinishPolylineCommand(false);
        }
        if (pDoc->m_drawState == STATE_DRAW_ARC_PREVIEW && m_tempPts.size() >= 3) {
            if (m_bScriptRecording && !m_bRunningScript && !m_bSubmittingCommandLine)
                RecordScriptInput(L"");
            // Build arc: tempPts[0]=start, [1]=center, [2]=end
            CPoint ptStart  = m_tempPts[0];
            CPoint ptCenter = m_tempPts[1];
            CPoint ptEnd    = m_tempPts[2];
            int r = (int)Distance(ptCenter, ptStart);
            double angS = atan2(ptStart.y - ptCenter.y, ptStart.x - ptCenter.x);
            double angE = atan2(ptEnd.y - ptCenter.y, ptEnd.x - ptCenter.x);
            double sweep = angE - angS;
            if (sweep < 0) sweep += 2.0 * M_PI;

            CArcEntity* pArc = new CArcEntity();
            if (!m_bArcAltHalf) {
                pArc->SetArcByCenter(ptCenter, r, angS, angE);
            } else {
                pArc->SetArcByCenter(ptCenter, r, angE, angS);
            }
            pDoc->AddEntity(pArc);
            CompleteDrawCommand();
            Invalidate(FALSE);
        }
        if (pDoc->m_drawState == STATE_ARRAY_SELECT) {
            if (pDoc->GetSelectedCount() > 0) {
                UpdateArrayDefaultSpacingFromSelection();
                SetDrawState(STATE_ARRAY_ROWS);
            } else {
                pDoc->m_strCommandPrompt = L"ARRAY: No objects selected";
            }
            UpdateStatusBar();
            Invalidate(FALSE);
        } else if (pDoc->m_drawState == STATE_ARRAY_ROWS) {
            SetDrawState(STATE_ARRAY_COLUMNS);
        } else if (pDoc->m_drawState == STATE_ARRAY_COLUMNS) {
            SetDrawState(STATE_ARRAY_ROW_SPACING);
        } else if (pDoc->m_drawState == STATE_ARRAY_ROW_SPACING) {
            SetDrawState(STATE_ARRAY_COLUMN_SPACING);
        } else if (pDoc->m_drawState == STATE_ARRAY_COLUMN_SPACING) {
            CreateRectangularArray(m_nArrayRows, m_nArrayColumns,
                                   m_dArrayRowSpacing, m_dArrayColumnSpacing);
        }
        break;

    case VK_DELETE:
        if (m_bScriptRecording && !m_bRunningScript && !m_bSubmittingCommandLine)
            RecordScriptInput(L"ERASE");
        pDoc->DeleteSelected();
        Invalidate(FALSE);
        break;

    case 'Z':
        if (GetKeyState(VK_CONTROL) & 0x8000) {
            pDoc->Undo();
            UpdateStatusBar();
            Invalidate(FALSE);
        } else if (pDoc->m_drawState == STATE_IDLE) {
            OnViewZoomWindow();
        }
        break;

    case 'E':
        if (pDoc->m_drawState == STATE_ZOOM_WINDOW_P1 ||
            pDoc->m_drawState == STATE_ZOOM_WINDOW_P2) {
            OnViewZoomExtents();
            return;
        }
        break;

    case 'Y':
        if (GetKeyState(VK_CONTROL) & 0x8000) {
            pDoc->Redo();
            UpdateStatusBar();
            Invalidate(FALSE);
        }
        break;

    case 'A':
        if (GetKeyState(VK_CONTROL) & 0x8000) {
            for (auto* p : pDoc->GetEntities())
                const_cast<CEntity*>(p)->m_bSelected = true;
            Invalidate(FALSE);
        }
        break;

    case VK_F7:
        pDoc->m_bShowGrid = !pDoc->m_bShowGrid;
        if (m_bScriptRecording && !m_bRunningScript && !m_bSubmittingCommandLine) {
            CString script;
            script.Format(L"GRID %s", (LPCTSTR)FormatOnOff(pDoc->m_bShowGrid));
            RecordScriptInput(script);
        }
        UpdateStatusBar();
        Invalidate(FALSE);
        break;

    case VK_F9:
        pDoc->m_bSnapToGrid = !pDoc->m_bSnapToGrid;
        if (m_bScriptRecording && !m_bRunningScript && !m_bSubmittingCommandLine) {
            CString script;
            script.Format(L"SNAP %s", (LPCTSTR)FormatOnOff(pDoc->m_bSnapToGrid));
            RecordScriptInput(script);
        }
        UpdateStatusBar();
        Invalidate(FALSE);
        break;

    case VK_F8:
        pDoc->m_bOrthoMode = !pDoc->m_bOrthoMode;
        if (m_bScriptRecording && !m_bRunningScript && !m_bSubmittingCommandLine) {
            CString script;
            script.Format(L"ORTHO %s", (LPCTSTR)FormatOnOff(pDoc->m_bOrthoMode));
            RecordScriptInput(script);
        }
        UpdateStatusBar();
        Invalidate(FALSE);
        break;

    case VK_F3:
        pDoc->m_bObjectSnap = !pDoc->m_bObjectSnap;
        if (m_bScriptRecording && !m_bRunningScript && !m_bSubmittingCommandLine) {
            CString script;
            script.Format(L"OSNAP %s", (LPCTSTR)FormatOnOff(pDoc->m_bObjectSnap));
            RecordScriptInput(script);
        }
        UpdateStatusBar();
        Invalidate(FALSE);
        break;

    case VK_CONTROL:
        if (pDoc->m_drawState == STATE_DRAW_ARC_PREVIEW && !(nFlags & 0x4000)) {
            // Toggle arc half (only on keydown, not repeat)
            m_bArcAltHalf = !m_bArcAltHalf;
            Invalidate(FALSE);
            return;
        }
        break;
    }

    CView::OnKeyDown(nChar, nRepCnt, nFlags);
}

// ============================================================
// Cursor - Show crosshair on client area
// ============================================================
BOOL CLargeHWView::OnSetCursor(CWnd* pWnd, UINT nHitTest, UINT message)
{
    if (nHitTest == HTCLIENT) {
        HCURSOR hCross = AfxGetApp()->LoadStandardCursor(IDC_CROSS);
        if (hCross) {
            SetCursor(hCross);
            return TRUE;
        }
    }
    return CView::OnSetCursor(pWnd, nHitTest, message);
}

// ============================================================
// OnSize
// ============================================================
void CLargeHWView::OnSize(UINT nType, int cx, int cy)
{
    CView::OnSize(nType, cx, cy);
    Invalidate(FALSE);
}

// ============================================================
// Context menu
// ============================================================
void CLargeHWView::OnContextMenu(CWnd* pWnd, CPoint pt)
{
    CMenu menu;
    menu.CreatePopupMenu();

    menu.AppendMenu(MF_STRING, ID_CONTEXT_ENTER,  L"Enter");
    menu.AppendMenu(MF_STRING, ID_CONTEXT_CANCEL, L"Cancel (Esc)");
    menu.AppendMenu(MF_SEPARATOR);
    menu.AppendMenu(MF_STRING, ID_CONTEXT_REPEAT, L"Repeat last command");
    menu.AppendMenu(MF_SEPARATOR);
    menu.AppendMenu(MF_STRING, ID_VIEW_ZOOM_WINDOW,  L"Zoom Window");
    menu.AppendMenu(MF_STRING, ID_VIEW_ZOOM_EXTENTS, L"Zoom Extents");

    CLargeHWDoc* pDoc = GetDocument();
    int nSel = pDoc->GetSelectedCount();
    if (nSel > 0) {
        menu.AppendMenu(MF_SEPARATOR);
        CString strSel;
        strSel.Format(L"Selected %d objects", nSel);
        menu.AppendMenu(MF_STRING | MF_GRAYED, 0, strSel);
        menu.AppendMenu(MF_STRING, ID_MODIFY_DELETE, L"Delete");
        menu.AppendMenu(MF_STRING, ID_MODIFY_MOVE,   L"Move");
        menu.AppendMenu(MF_STRING, ID_MODIFY_COPY,   L"Copy");
        menu.AppendMenu(MF_STRING, ID_MODIFY_ROTATE, L"Rotate");
        menu.AppendMenu(MF_STRING, ID_MODIFY_SCALE,  L"Scale");
        menu.AppendMenu(MF_STRING, ID_MODIFY_MIRROR, L"Mirror");
        menu.AppendMenu(MF_STRING, ID_MODIFY_CHAMFER, L"Chamfer");
        menu.AppendMenu(MF_STRING, ID_MODIFY_FILLET, L"Fillet");
        menu.AppendMenu(MF_STRING, ID_MODIFY_ARRAY,  L"Array");
    }

    if (pt.x == -1 && pt.y == -1) {
        CRect rc; GetClientRect(&rc);
        pt = rc.CenterPoint();
        ClientToScreen(&pt);
    }

    menu.TrackPopupMenu(TPM_LEFTALIGN | TPM_RIGHTBUTTON, pt.x, pt.y, this);
}

// ============================================================
// Draw command handlers
// ============================================================
void CLargeHWView::OnDrawLine()       { if (m_bScriptRecording && !m_bRunningScript && !m_bSubmittingCommandLine) RecordScriptInput(L"LINE"); m_tempPts.clear(); m_nLastCommandID = ID_DRAW_LINE; SetDrawState(STATE_DRAW_LINE_P1); }
void CLargeHWView::OnDrawCircle()     { if (m_bScriptRecording && !m_bRunningScript && !m_bSubmittingCommandLine) RecordScriptInput(L"CIRCLE"); m_tempPts.clear(); m_nLastCommandID = ID_DRAW_CIRCLE; SetDrawState(STATE_DRAW_CIRCLE_CENTER); }
void CLargeHWView::OnDrawArc()        { if (m_bScriptRecording && !m_bRunningScript && !m_bSubmittingCommandLine) RecordScriptInput(L"ARC"); m_tempPts.clear(); m_nLastCommandID = ID_DRAW_ARC; SetDrawState(STATE_DRAW_ARC_P1); }
void CLargeHWView::OnDrawRectangle()  { if (m_bScriptRecording && !m_bRunningScript && !m_bSubmittingCommandLine) RecordScriptInput(L"RECTANGLE"); m_tempPts.clear(); m_nLastCommandID = ID_DRAW_RECTANGLE; SetDrawState(STATE_DRAW_RECT_P1); }
void CLargeHWView::OnDrawEllipse()    { if (m_bScriptRecording && !m_bRunningScript && !m_bSubmittingCommandLine) RecordScriptInput(L"ELLIPSE"); m_tempPts.clear(); m_nLastCommandID = ID_DRAW_ELLIPSE; SetDrawState(STATE_DRAW_ELLIPSE_CENTER); }
void CLargeHWView::OnDrawPolyline()   { if (m_bScriptRecording && !m_bRunningScript && !m_bSubmittingCommandLine) RecordScriptInput(L"PLINE"); m_tempPts.clear(); m_pActivePolyline = nullptr; m_bPolylineClose = false; m_bPolylineArcMode = false; m_nPolylineWidth = max(1, GetDocument()->GetCurrentLineWidth()); m_nPolylineStartWidth = m_nPolylineWidth; m_nPolylineEndWidth = m_nPolylineWidth; m_nLastCommandID = ID_DRAW_POLYLINE; SetDrawState(STATE_DRAW_POLYLINE_POINT); }
void CLargeHWView::OnDrawText()       { m_tempPts.clear(); m_nLastCommandID = ID_DRAW_TEXT; SetDrawState(STATE_DRAW_TEXT_POS); }
void CLargeHWView::OnDrawPolygon()    { if (m_bScriptRecording && !m_bRunningScript && !m_bSubmittingCommandLine) RecordScriptInput(L"POLYGON"); m_tempPts.clear(); m_nPolygonSides = 6; m_nLastCommandID = ID_DRAW_POLYGON; SetDrawState(STATE_DRAW_POLYGON_CENTER); }
void CLargeHWView::OnDrawDimLength()  { OnDrawDimLengthAligned(); }
void CLargeHWView::OnDrawDimLengthAligned() { m_tempPts.clear(); m_nLastCommandID = ID_DRAW_DIMENSION_LENGTH; m_nLastDimMode = 0; SetDrawState(STATE_DRAW_DIM_LENGTH_P1); }
void CLargeHWView::OnDrawDimLengthHoriz() { m_tempPts.clear(); m_nLastCommandID = ID_DRAW_DIMENSION_LENGTH; m_nLastDimMode = 1; SetDrawState(STATE_DRAW_DIM_LENGTH_P1); }
void CLargeHWView::OnDrawDimLengthVert()  { m_tempPts.clear(); m_nLastCommandID = ID_DRAW_DIMENSION_LENGTH; m_nLastDimMode = 2; SetDrawState(STATE_DRAW_DIM_LENGTH_P1); }

void CLargeHWView::OnDrawDimAngle()
{
    m_tempPts.clear();
    m_nLastCommandID = ID_DRAW_DIMENSION_ANGLE;
    m_pDimEnt1 = nullptr; m_pDimEnt2 = nullptr;
    // create temporary split segments up-front so user can click/select split pieces immediately
    CLargeHWDoc* pDoc = GetDocument();
    if (pDoc) {
        // if there are old temp splits, remove them first
        if (!m_tempSplitNewIDs.empty()) {
            RestoreTemporarySplits(pDoc, m_tempSplitNewIDs);
            m_tempSplitNewIDs.clear();
        }
        // create temporary split segments but do NOT select them; selection should occur on user clicks
        CreateTemporarySplits(pDoc, m_tempSplitNewIDs);
    }
    SetDrawState(STATE_DRAW_DIM_ANGLE_SELECT_E1);
}

void CLargeHWView::OnDrawDimRadius()
{
    m_tempPts.clear();
    m_nLastCommandID = ID_DRAW_DIMENSION_RADIUS;
    if (m_bDimRadiusSrcTemp) { delete m_pDimRadiusSrcEnt; m_bDimRadiusSrcTemp = false; }
    m_pDimRadiusSrcEnt = nullptr;
    m_pPendingDim = nullptr;
    CLargeHWDoc* pDoc = GetDocument();
    if (pDoc) {
        pDoc->DeselectAll();
        pDoc->m_strCommandPrompt = L"Select circle or arc: ";
    }
    SetDrawState(STATE_DRAW_DIM_RADIUS_SELECT);
}

void CLargeHWView::OnDrawDimDiameter()
{
    m_tempPts.clear();
    m_nLastCommandID = ID_DRAW_DIMENSION_DIAMETER;
    if (m_bDimDiamSrcTemp) { delete m_pDimDiamSrcEnt; m_bDimDiamSrcTemp = false; }
    m_pDimDiamSrcEnt = nullptr;
    m_pPendingDim = nullptr;
    CLargeHWDoc* pDoc = GetDocument();
    if (pDoc) {
        pDoc->DeselectAll();
        pDoc->m_strCommandPrompt = L"Select circle or arc: ";
    }
    SetDrawState(STATE_DRAW_DIM_DIAMETER_SELECT);
}

void CLargeHWView::OnDrawDimArcLength()
{
    m_tempPts.clear();
    m_nLastCommandID = ID_DRAW_DIMENSION_ARCLENGTH;
    if (m_bDimArcLenSrcTemp) { delete m_pDimArcLenSrcEnt; m_bDimArcLenSrcTemp = false; }
    m_pDimArcLenSrcEnt = nullptr;
    m_pPendingDim = nullptr;
    CLargeHWDoc* pDoc = GetDocument();
    if (pDoc) {
        pDoc->DeselectAll();
        pDoc->m_strCommandPrompt = L"Select arc: ";
    }
    SetDrawState(STATE_DRAW_DIM_ARCLEN_SELECT);
}

void CLargeHWView::OnDrawDimCoordinate()
{
    m_tempPts.clear();
    m_nLastCommandID = ID_DRAW_DIMENSION_COORDINATE;
    m_bCoordDimMode = true;
    m_ptCoordPoint = CPoint(0, 0);
    m_pPendingDim = nullptr;
    CLargeHWDoc* pDoc = GetDocument();
    if (pDoc) {
        pDoc->m_strCommandPrompt = L"Specify point: ";
    }
    SetDrawState(STATE_DRAW_DIM_COORD_PICK);
}

// ============================================================
// Modify command handlers
// ============================================================
void CLargeHWView::OnModifyMove()
{
    if (m_bScriptRecording && !m_bRunningScript && !m_bSubmittingCommandLine)
        RecordScriptInput(L"MOVE");
    m_tempPts.clear();
    CLargeHWDoc* pDoc = GetDocument();
    m_nLastCommandID = ID_MODIFY_MOVE;
    int nSel = pDoc->GetSelectedCount();
    TRACE(L"[DEBUG] OnModifyMove called, selected=%d, entCount=%d\n", nSel, (int)pDoc->GetEntities().size());
    if (nSel > 0) {
        m_tempPts.push_back(CPoint(0, 0));
        SetDrawState(STATE_MOVE_BASE);
        TRACE(L"[DEBUG] OnModifyMove: entities pre-selected, state=STATE_MOVE_BASE\n");
    } else {
        SetDrawState(STATE_MOVE_SELECT);
        TRACE(L"[DEBUG] OnModifyMove: no selection, state=STATE_MOVE_SELECT\n");
    }
}

void CLargeHWView::OnModifyCopy()
{
    if (m_bScriptRecording && !m_bRunningScript && !m_bSubmittingCommandLine)
        RecordScriptInput(L"COPY");
    m_tempPts.clear();
    CLargeHWDoc* pDoc = GetDocument();
    m_nLastCommandID = ID_MODIFY_COPY;
    int nSel = pDoc->GetSelectedCount();
    TRACE(L"[DEBUG] OnModifyCopy called, selected=%d, entCount=%d\n", nSel, (int)pDoc->GetEntities().size());
    if (nSel > 0) {
        m_tempPts.push_back(CPoint(0, 0));
        SetDrawState(STATE_COPY_BASE);
        TRACE(L"[DEBUG] OnModifyCopy: entities pre-selected, state=STATE_COPY_BASE\n");
    } else {
        SetDrawState(STATE_COPY_SELECT);
        TRACE(L"[DEBUG] OnModifyCopy: no selection, state=STATE_COPY_SELECT\n");
    }
}

void CLargeHWView::OnModifyDelete()
{
    if (m_bScriptRecording && !m_bRunningScript && !m_bSubmittingCommandLine)
        RecordScriptInput(L"ERASE");
    CLargeHWDoc* pDoc = GetDocument();
    int nSel = pDoc->GetSelectedCount();
    TRACE(L"[DEBUG] OnModifyDelete called, selected=%d\n", nSel);
    pDoc->DeleteSelected();
    Invalidate(FALSE);
}

void CLargeHWView::OnModifyRotate()
{
    if (m_bScriptRecording && !m_bRunningScript && !m_bSubmittingCommandLine)
        RecordScriptInput(L"ROTATE");
    m_tempPts.clear();
    CLargeHWDoc* pDoc = GetDocument();
    m_nLastCommandID = ID_MODIFY_ROTATE;
    int nSel = pDoc->GetSelectedCount();
    TRACE(L"[DEBUG] OnModifyRotate called, selected=%d, entCount=%d\n", nSel, (int)pDoc->GetEntities().size());
    if (nSel > 0) {
        SetDrawState(STATE_ROTATE_CENTER);
        TRACE(L"[DEBUG] OnModifyRotate: entities pre-selected, state=STATE_ROTATE_CENTER\n");
    } else {
        SetDrawState(STATE_ROTATE_SELECT);
        TRACE(L"[DEBUG] OnModifyRotate: no selection, state=STATE_ROTATE_SELECT\n");
    }
}

void CLargeHWView::OnModifyScale()
{
    if (m_bScriptRecording && !m_bRunningScript && !m_bSubmittingCommandLine)
        RecordScriptInput(L"SCALE");
    m_tempPts.clear();
    CLargeHWDoc* pDoc = GetDocument();
    m_nLastCommandID = ID_MODIFY_SCALE;
    int nSel = pDoc->GetSelectedCount();
    TRACE(L"[DEBUG] OnModifyScale called, selected=%d, entCount=%d\n", nSel, (int)pDoc->GetEntities().size());
    if (nSel > 0) {
        SetDrawState(STATE_SCALE_BASE);
        TRACE(L"[DEBUG] OnModifyScale: entities pre-selected, state=STATE_SCALE_BASE\n");
    } else {
        SetDrawState(STATE_SCALE_SELECT);
        TRACE(L"[DEBUG] OnModifyScale: no selection, state=STATE_SCALE_SELECT\n");
    }
}

void CLargeHWView::OnModifyMirror()
{
    if (m_bScriptRecording && !m_bRunningScript && !m_bSubmittingCommandLine)
        RecordScriptInput(L"MIRROR");
    m_tempPts.clear();
    CLargeHWDoc* pDoc = GetDocument();
    m_nLastCommandID = ID_MODIFY_MIRROR;
    int nSel = pDoc->GetSelectedCount();
    TRACE(L"[DEBUG] OnModifyMirror called, selected=%d, entCount=%d\n", nSel, (int)pDoc->GetEntities().size());
    if (nSel > 0) {
        SetDrawState(STATE_MIRROR_P1);
        TRACE(L"[DEBUG] OnModifyMirror: entities pre-selected, state=STATE_MIRROR_P1\n");
    } else {
        SetDrawState(STATE_MIRROR_SELECT);
        TRACE(L"[DEBUG] OnModifyMirror: no selection, state=STATE_MIRROR_SELECT\n");
    }
}

void CLargeHWView::OnModifyOffset()
{
    if (m_bScriptRecording && !m_bRunningScript && !m_bSubmittingCommandLine)
        RecordScriptInput(L"OFFSET");
    m_tempPts.clear();
    m_nLastCommandID = ID_MODIFY_OFFSET;
    TRACE(L"[DEBUG] OnModifyOffset called, state=STATE_OFFSET_SELECT\n");
    SetDrawState(STATE_OFFSET_SELECT);
}

void CLargeHWView::OnModifyChamfer()
{
    if (m_bScriptRecording && !m_bRunningScript && !m_bSubmittingCommandLine)
        RecordScriptInput(L"CHAMFER");
    m_tempPts.clear();
    m_pChamferFirst = nullptr;
    m_chamferFirstSegment = ChamferSegmentRef();
    m_nLastCommandID = ID_MODIFY_CHAMFER;
    SetDrawState(STATE_CHAMFER_SELECT_FIRST);
}

void CLargeHWView::OnModifyFillet()
{
    if (m_bScriptRecording && !m_bRunningScript && !m_bSubmittingCommandLine)
        RecordScriptInput(L"FILLET");
    m_tempPts.clear();
    m_filletFirstSegment = ChamferSegmentRef();
    m_nLastCommandID = ID_MODIFY_FILLET;
    SetDrawState(STATE_FILLET_SELECT_FIRST);
}

void CLargeHWView::OnModifyArray()
{
    if (m_bScriptRecording && !m_bRunningScript && !m_bSubmittingCommandLine)
        RecordScriptInput(L"ARRAY");
    m_tempPts.clear();
    m_nLastCommandID = ID_MODIFY_ARRAY;
    CLargeHWDoc* pDoc = GetDocument();
    if (pDoc && pDoc->GetSelectedCount() > 0) {
        UpdateArrayDefaultSpacingFromSelection();
        SetDrawState(STATE_ARRAY_ROWS);
    } else {
        SetDrawState(STATE_ARRAY_SELECT);
    }
}

CLineEntity* CLargeHWView::HitTestLineEntity(CPoint point) const
{
    CLargeHWDoc* pDoc = GetDocument();
    if (!pDoc) return nullptr;

    CLineEntity* pBest = nullptr;
    double bestDist = DBL_MAX;

    const auto& ents = pDoc->GetEntities();
    for (auto it = ents.rbegin(); it != ents.rend(); ++it) {
        CEntity* pEnt = *it;
        if (!pEnt || !pEnt->m_bVisible || pEnt->m_Type != ENT_LINE)
            continue;

        CLineEntity* pLine = static_cast<CLineEntity*>(pEnt);
        CPoint p1 = pLine->ToScreen(pLine->m_ptStart, pDoc->m_dScale, pDoc->m_ptOffset);
        CPoint p2 = pLine->ToScreen(pLine->m_ptEnd, pDoc->m_dScale, pDoc->m_ptOffset);
        double dist = PointToLineDistance(point, p1, p2);
        double tolerance = max(12.0, (double)pLine->m_nLineWidth + 8.0);
        if (dist <= tolerance && dist < bestDist) {
            bestDist = dist;
            pBest = pLine;
        }
    }

    return pBest;
}

CLargeHWView::ChamferSegmentRef CLargeHWView::HitTestChamferSegment(CPoint point) const
{
    CLargeHWDoc* pDoc = GetDocument();
    ChamferSegmentRef best;
    if (!pDoc) return best;

    double bestDist = DBL_MAX;
    auto considerSegment = [&](CEntity* pEnt, int segmentIndex, CPoint start, CPoint end) {
        if (!pEnt || !pEnt->m_bVisible)
            return;

        CPoint p1 = pEnt->ToScreen(start, pDoc->m_dScale, pDoc->m_ptOffset);
        CPoint p2 = pEnt->ToScreen(end, pDoc->m_dScale, pDoc->m_ptOffset);
        double dist = PointToLineDistance(point, p1, p2);
        double tolerance = max(12.0, (double)pEnt->m_nLineWidth + 8.0);
        if (dist <= tolerance && dist < bestDist) {
            bestDist = dist;
            best.pEntity = pEnt;
            best.segmentIndex = segmentIndex;
            best.start = start;
            best.end = end;
        }
    };

    const auto& ents = pDoc->GetEntities();
    for (auto it = ents.rbegin(); it != ents.rend(); ++it) {
        CEntity* pEnt = *it;
        if (!pEnt || !pEnt->m_bVisible)
            continue;

        if (pEnt->m_Type == ENT_LINE) {
            CLineEntity* pLine = static_cast<CLineEntity*>(pEnt);
            considerSegment(pEnt, 0, pLine->m_ptStart, pLine->m_ptEnd);
        } else if (pEnt->m_Type == ENT_RECTANGLE) {
            CRectangleEntity* pRect = static_cast<CRectangleEntity*>(pEnt);
            CRect rc(pRect->m_ptCorner1, pRect->m_ptCorner2);
            rc.NormalizeRect();
            CPoint vertices[4] = {
                rc.TopLeft(),
                CPoint(rc.right, rc.top),
                rc.BottomRight(),
                CPoint(rc.left, rc.bottom)
            };
            for (int i = 0; i < 4; ++i)
                considerSegment(pEnt, i, vertices[i], vertices[(i + 1) % 4]);
        } else if (pEnt->m_Type == ENT_POLYLINE) {
            CPolylineEntity* pPoly = static_cast<CPolylineEntity*>(pEnt);
            int n = (int)pPoly->m_vertices.size();
            for (int i = 0; i < n - 1; ++i)
                considerSegment(pEnt, i, pPoly->m_vertices[i], pPoly->m_vertices[i + 1]);
            if (pPoly->m_bClosed && n > 2)
                considerSegment(pEnt, n - 1, pPoly->m_vertices[n - 1], pPoly->m_vertices[0]);
        }
    }

    return best;
}

bool CLargeHWView::ApplyChamfer(CLineEntity* pFirst, CLineEntity* pSecond, double distance)
{
    CLargeHWDoc* pDoc = GetDocument();
    if (!pDoc || !pFirst || !pSecond || pFirst == pSecond)
        return false;

    double ix = 0.0;
    double iy = 0.0;
    if (!IntersectInfiniteLines(pFirst->m_ptStart, pFirst->m_ptEnd,
                                pSecond->m_ptStart, pSecond->m_ptEnd, ix, iy)) {
        pDoc->m_strCommandPrompt = L"CHAMFER: Lines are parallel, cancelled";
        pDoc->DeselectAll();
        CompleteDrawCommand();
        UpdateStatusBar();
        return false;
    }

    CPoint firstTrimEnd;
    CPoint secondTrimEnd;
    CPoint pChamfer1 = ChamferPointOnSegment(pFirst->m_ptStart, pFirst->m_ptEnd,
                                             ix, iy, distance, firstTrimEnd);
    CPoint pChamfer2 = ChamferPointOnSegment(pSecond->m_ptStart, pSecond->m_ptEnd,
                                             ix, iy, distance, secondTrimEnd);
    if (Distance(pChamfer1, pChamfer2) <= 0.5) {
        pDoc->m_strCommandPrompt = L"CHAMFER: distance too small or segment too short";
        pDoc->DeselectAll();
        CompleteDrawCommand();
        UpdateStatusBar();
        return false;
    }

    pFirst->m_bSelected = true;
    pSecond->m_bSelected = true;
    pDoc->RecordModifyUndo();

    if (firstTrimEnd == pFirst->m_ptStart) pFirst->m_ptStart = pChamfer1;
    else pFirst->m_ptEnd = pChamfer1;

    if (secondTrimEnd == pSecond->m_ptStart) pSecond->m_ptStart = pChamfer2;
    else pSecond->m_ptEnd = pChamfer2;

    std::vector<int> newIDs;
    if (Distance(pChamfer1, pChamfer2) > 0.5) {
        CLineEntity* pChamfer = new CLineEntity(pChamfer1, pChamfer2);
        pDoc->AddEntity(pChamfer, false);
        newIDs.push_back(pChamfer->m_nID);
        pDoc->RecordCreateUndo(newIDs);
    } else {
        pDoc->SetModified(true);
    }

    pDoc->DeselectAll();
    CompleteDrawCommand();
    pDoc->m_strCommandPrompt.Format(L"Chamfer created (distance=%.0f)", distance);
    UpdateStatusBar();
    Invalidate(FALSE);
    return true;
}

bool CLargeHWView::ApplyChamfer(const ChamferSegmentRef& first, const ChamferSegmentRef& second, double distance)
{
    CLargeHWDoc* pDoc = GetDocument();
    if (!pDoc || !first.IsValid() || !second.IsValid())
        return false;

    double ix = 0.0;
    double iy = 0.0;
    if (!IntersectInfiniteLines(first.start, first.end, second.start, second.end, ix, iy)) {
        pDoc->m_strCommandPrompt = L"CHAMFER: Segments are parallel, cancelled";
        pDoc->DeselectAll();
        CompleteDrawCommand();
        UpdateStatusBar();
        return false;
    }

    CPoint firstTrimEnd;
    CPoint secondTrimEnd;
    CPoint pChamfer1 = ChamferPointOnSegment(first.start, first.end,
                                             ix, iy, distance, firstTrimEnd);
    CPoint pChamfer2 = ChamferPointOnSegment(second.start, second.end,
                                             ix, iy, distance, secondTrimEnd);
    if (Distance(pChamfer1, pChamfer2) <= 0.5) {
        pDoc->m_strCommandPrompt = L"CHAMFER: distance too small or segment too short";
        pDoc->DeselectAll();
        CompleteDrawCommand();
        UpdateStatusBar();
        return false;
    }

    auto copyProps = [](CEntity* dst, const CEntity* src) {
        if (!dst || !src) return;
        dst->m_color = src->m_color;
        dst->m_nLineStyle = src->m_nLineStyle;
        dst->m_nLineWidth = src->m_nLineWidth;
        dst->m_strLayer = src->m_strLayer;
        dst->m_bVisible = src->m_bVisible;
        dst->m_bUseLayerColor = src->m_bUseLayerColor;
    };

    auto rectangleVertices = [](CRectangleEntity* pRect) {
        std::vector<CPoint> vertices;
        CRect rc(pRect->m_ptCorner1, pRect->m_ptCorner2);
        rc.NormalizeRect();
        vertices.push_back(rc.TopLeft());
        vertices.push_back(CPoint(rc.right, rc.top));
        vertices.push_back(rc.BottomRight());
        vertices.push_back(CPoint(rc.left, rc.bottom));
        return vertices;
    };

    auto buildChamferedVertices = [&](const std::vector<CPoint>& vertices, bool closed,
                                      int segA, int segB, CPoint chamferA, CPoint chamferB,
                                      std::vector<CPoint>& newVertices) -> bool {
        int n = (int)vertices.size();
        if (n < 3) return false;

        int aNext = closed ? (segA + 1) % n : segA + 1;
        int bNext = closed ? (segB + 1) % n : segB + 1;
        int corner = -1;
        CPoint firstPoint;
        CPoint secondPoint;

        if (aNext == segB) {
            corner = segB;
            firstPoint = chamferA;
            secondPoint = chamferB;
        } else if (bNext == segA) {
            corner = segA;
            firstPoint = chamferB;
            secondPoint = chamferA;
        } else {
            return false;
        }

        newVertices.clear();
        for (int i = 0; i < n; ++i) {
            if (i == corner) {
                newVertices.push_back(firstPoint);
                newVertices.push_back(secondPoint);
            } else {
                newVertices.push_back(vertices[i]);
            }
        }
        return true;
    };

    if (first.pEntity == second.pEntity &&
        (first.pEntity->m_Type == ENT_RECTANGLE || first.pEntity->m_Type == ENT_POLYLINE)) {
        std::vector<CPoint> vertices;
        bool closed = true;
        if (first.pEntity->m_Type == ENT_RECTANGLE) {
            vertices = rectangleVertices(static_cast<CRectangleEntity*>(first.pEntity));
            closed = true;
        } else {
            CPolylineEntity* pPoly = static_cast<CPolylineEntity*>(first.pEntity);
            vertices = pPoly->m_vertices;
            closed = pPoly->m_bClosed;
        }

        std::vector<CPoint> newVertices;
        if (buildChamferedVertices(vertices, closed,
                                   first.segmentIndex, second.segmentIndex,
                                   pChamfer1, pChamfer2, newVertices)) {
            if (first.pEntity->m_Type == ENT_POLYLINE) {
                first.pEntity->m_bSelected = true;
                pDoc->RecordModifyUndo();
                first.pEntity->m_bSelected = false;
                CPolylineEntity* pPoly = static_cast<CPolylineEntity*>(first.pEntity);
                pPoly->m_vertices = newVertices;
                pPoly->m_bClosed = closed;
                pDoc->SetModified(true);
            } else {
                CPolylineEntity* pNew = new CPolylineEntity(newVertices);
                pNew->m_bClosed = true;
                copyProps(pNew, first.pEntity);
                pDoc->ReplaceEntity(first.pEntity->m_nID, pNew, true);
            }

            pDoc->DeselectAll();
            CompleteDrawCommand();
            pDoc->m_strCommandPrompt.Format(L"Chamfered composite corner (distance=%.0f)", distance);
            UpdateStatusBar();
            Invalidate(FALSE);
            return true;
        }
    }

    if (first.pEntity->m_Type == ENT_RECTANGLE || first.pEntity->m_Type == ENT_POLYLINE ||
        second.pEntity->m_Type == ENT_RECTANGLE || second.pEntity->m_Type == ENT_POLYLINE) {
        pDoc->m_strCommandPrompt = L"CHAMFER: Rectangle/polyline segments must be adjacent";
        pDoc->DeselectAll();
        CompleteDrawCommand();
        UpdateStatusBar();
        return false;
    }

    first.pEntity->m_bSelected = true;
    second.pEntity->m_bSelected = true;
    pDoc->RecordModifyUndo();

    auto trimSegmentEndpoint = [](const ChamferSegmentRef& seg, CPoint trimEnd, CPoint newPt) {
        if (seg.pEntity->m_Type == ENT_LINE) {
            CLineEntity* pLine = static_cast<CLineEntity*>(seg.pEntity);
            if (trimEnd == seg.start) pLine->m_ptStart = newPt;
            else pLine->m_ptEnd = newPt;
        } else if (seg.pEntity->m_Type == ENT_POLYLINE) {
            CPolylineEntity* pPoly = static_cast<CPolylineEntity*>(seg.pEntity);
            int n = (int)pPoly->m_vertices.size();
            if (n <= 0) return;
            int startIndex = seg.segmentIndex;
            int endIndex = (seg.segmentIndex + 1) % n;
            if (trimEnd == seg.start) pPoly->m_vertices[startIndex] = newPt;
            else pPoly->m_vertices[endIndex] = newPt;
        }
    };

    trimSegmentEndpoint(first, firstTrimEnd, pChamfer1);
    trimSegmentEndpoint(second, secondTrimEnd, pChamfer2);

    std::vector<int> newIDs;
    if (Distance(pChamfer1, pChamfer2) > 0.5) {
        CLineEntity* pChamfer = new CLineEntity(pChamfer1, pChamfer2);
        copyProps(pChamfer, first.pEntity);
        pDoc->AddEntity(pChamfer, false);
        newIDs.push_back(pChamfer->m_nID);
        pDoc->RecordCreateUndo(newIDs);
    }

    pDoc->DeselectAll();
    CompleteDrawCommand();
    pDoc->m_strCommandPrompt.Format(L"Chamfer created (distance=%.0f)", distance);
    UpdateStatusBar();
    Invalidate(FALSE);
    return true;
}

bool CLargeHWView::ApplyFillet(const ChamferSegmentRef& first, const ChamferSegmentRef& second, double radius)
{
    CLargeHWDoc* pDoc = GetDocument();
    if (!pDoc || !first.IsValid() || !second.IsValid())
        return false;

    CPoint trim1, trim2, tan1, tan2, center;
    int actualRadius = 0;
    if (!ComputeFilletGeometry(first.start, first.end, second.start, second.end,
                               radius, trim1, trim2, tan1, tan2, center, actualRadius)) {
        pDoc->m_strCommandPrompt = L"FILLET: invalid radius or parallel segments";
        pDoc->DeselectAll();
        CompleteDrawCommand();
        UpdateStatusBar();
        return false;
    }

    auto copyProps = [](CEntity* dst, const CEntity* src) {
        if (!dst || !src) return;
        dst->m_color = src->m_color;
        dst->m_nLineStyle = src->m_nLineStyle;
        dst->m_nLineWidth = src->m_nLineWidth;
        dst->m_strLayer = src->m_strLayer;
        dst->m_bVisible = src->m_bVisible;
        dst->m_bUseLayerColor = src->m_bUseLayerColor;
    };

    auto rectangleVertices = [](CRectangleEntity* pRect) {
        std::vector<CPoint> vertices;
        CRect rc(pRect->m_ptCorner1, pRect->m_ptCorner2);
        rc.NormalizeRect();
        vertices.push_back(rc.TopLeft());
        vertices.push_back(CPoint(rc.right, rc.top));
        vertices.push_back(rc.BottomRight());
        vertices.push_back(CPoint(rc.left, rc.bottom));
        return vertices;
    };

    auto buildFilletedVertices = [&](const std::vector<CPoint>& vertices, bool closed,
                                     int segA, int segB, CPoint arcStart, CPoint arcEnd,
                                     std::vector<CPoint>& newVertices) -> bool {
        int n = (int)vertices.size();
        if (n < 3) return false;

        int aNext = closed ? (segA + 1) % n : segA + 1;
        int bNext = closed ? (segB + 1) % n : segB + 1;
        int corner = -1;
        CPoint firstArcPoint, secondArcPoint;

        if (aNext == segB) {
            corner = segB;
            firstArcPoint = arcStart;
            secondArcPoint = arcEnd;
        } else if (bNext == segA) {
            corner = segA;
            firstArcPoint = arcEnd;
            secondArcPoint = arcStart;
        } else {
            return false;
        }

        newVertices.clear();
        for (int i = 0; i < n; ++i) {
            if (i == corner) {
                AppendArcApprox(newVertices, center, actualRadius, firstArcPoint, secondArcPoint);
            } else {
                newVertices.push_back(vertices[i]);
            }
        }
        return true;
    };

    if (first.pEntity == second.pEntity &&
        (first.pEntity->m_Type == ENT_RECTANGLE || first.pEntity->m_Type == ENT_POLYLINE)) {
        std::vector<CPoint> vertices;
        bool closed = true;
        if (first.pEntity->m_Type == ENT_RECTANGLE) {
            vertices = rectangleVertices(static_cast<CRectangleEntity*>(first.pEntity));
            closed = true;
        } else {
            CPolylineEntity* pPoly = static_cast<CPolylineEntity*>(first.pEntity);
            vertices = pPoly->m_vertices;
            closed = pPoly->m_bClosed;
        }

        std::vector<CPoint> newVertices;
        if (buildFilletedVertices(vertices, closed, first.segmentIndex, second.segmentIndex,
                                  tan1, tan2, newVertices)) {
            if (first.pEntity->m_Type == ENT_POLYLINE) {
                first.pEntity->m_bSelected = true;
                pDoc->RecordModifyUndo();
                first.pEntity->m_bSelected = false;
                CPolylineEntity* pPoly = static_cast<CPolylineEntity*>(first.pEntity);
                pPoly->m_vertices = newVertices;
                pPoly->m_bClosed = closed;
                pDoc->SetModified(true);
            } else {
                CPolylineEntity* pNew = new CPolylineEntity(newVertices);
                pNew->m_bClosed = true;
                copyProps(pNew, first.pEntity);
                pDoc->ReplaceEntity(first.pEntity->m_nID, pNew, true);
            }

            pDoc->DeselectAll();
            CompleteDrawCommand();
            pDoc->m_strCommandPrompt.Format(L"Fillet created (radius=%.0f)", radius);
            UpdateStatusBar();
            Invalidate(FALSE);
            return true;
        }
    }

    if (first.pEntity->m_Type != ENT_LINE || second.pEntity->m_Type != ENT_LINE) {
        pDoc->m_strCommandPrompt = L"FILLET: Rectangle/polyline segments must be adjacent";
        pDoc->DeselectAll();
        CompleteDrawCommand();
        UpdateStatusBar();
        return false;
    }

    first.pEntity->m_bSelected = true;
    second.pEntity->m_bSelected = true;
    pDoc->RecordModifyUndo();
    first.pEntity->m_bSelected = false;
    second.pEntity->m_bSelected = false;

    CLineEntity* line1 = static_cast<CLineEntity*>(first.pEntity);
    CLineEntity* line2 = static_cast<CLineEntity*>(second.pEntity);
    if (trim1 == first.start) line1->m_ptStart = tan1;
    else line1->m_ptEnd = tan1;
    if (trim2 == second.start) line2->m_ptStart = tan2;
    else line2->m_ptEnd = tan2;

    double a1 = atan2((double)(tan1.y - center.y), (double)(tan1.x - center.x));
    double a2 = atan2((double)(tan2.y - center.y), (double)(tan2.x - center.x));
    CArcEntity* arc = new CArcEntity();
    arc->SetArcByCenter(center, actualRadius, a1, a2);
    copyProps(arc, first.pEntity);
    pDoc->AddEntity(arc, false);
    std::vector<int> ids;
    ids.push_back(arc->m_nID);
    pDoc->RecordCreateUndo(ids);

    pDoc->DeselectAll();
    CompleteDrawCommand();
    pDoc->m_strCommandPrompt.Format(L"Fillet created (radius=%.0f)", radius);
    UpdateStatusBar();
    Invalidate(FALSE);
    return true;
}

bool CLargeHWView::UpdateArrayDefaultSpacingFromSelection()
{
    CLargeHWDoc* pDoc = GetDocument();
    if (!pDoc || pDoc->GetSelectedCount() == 0)
        return false;

    CRect bounds(INT_MAX, INT_MAX, INT_MIN, INT_MIN);
    for (auto* pEnt : pDoc->GetSelectedEntities()) {
        if (!pEnt)
            continue;

        CRect eb = pEnt->GetBounds();
        if (eb.left < bounds.left) bounds.left = eb.left;
        if (eb.top < bounds.top) bounds.top = eb.top;
        if (eb.right > bounds.right) bounds.right = eb.right;
        if (eb.bottom > bounds.bottom) bounds.bottom = eb.bottom;
    }

    if (bounds.left == INT_MAX)
        return false;

    int gap = max(pDoc->m_nGridSpacing, ScriptRound(GetModelUnitScale(pDoc)));
    m_dArrayColumnSpacing = max(1, bounds.Width() + gap);
    m_dArrayRowSpacing = max(1, bounds.Height() + gap);
    return true;
}

void CLargeHWView::CreateRectangularArray(int rows, int columns, double rowSpacing, double columnSpacing)
{
    CLargeHWDoc* pDoc = GetDocument();
    if (!pDoc) return;

    rows = max(1, rows);
    columns = max(1, columns);
    auto selectedEnts = pDoc->GetSelectedEntities();
    if (selectedEnts.empty() || (rows == 1 && columns == 1)) {
        pDoc->m_strCommandPrompt = L"ARRAY: No copies created";
        CompleteDrawCommand();
        UpdateStatusBar();
        return;
    }

    std::vector<int> newIDs;
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < columns; ++c) {
            if (r == 0 && c == 0)
                continue;

            double dx = c * columnSpacing;
            double dy = r * rowSpacing;
            for (auto* pEnt : selectedEnts) {
                CEntity* pCopy = pEnt->Clone();
                if (!pCopy) continue;
                pCopy->m_bSelected = false;
                pCopy->Move(dx, dy);
                pDoc->AddEntity(pCopy, false);
                newIDs.push_back(pCopy->m_nID);
            }
        }
    }

    pDoc->RecordCreateUndo(newIDs);
    pDoc->DeselectAll();
    CompleteDrawCommand();
    pDoc->m_strCommandPrompt.Format(L"Array created: %d rows x %d columns, %d copies",
                                    rows, columns, (int)newIDs.size());
    UpdateStatusBar();
    Invalidate(FALSE);
}

bool CLargeHWView::CloseLineCommand()
{
    CLargeHWDoc* pDoc = GetDocument();
    if (!pDoc || pDoc->m_drawState != STATE_DRAW_LINE_P2 || m_tempPts.size() < 2)
        return false;

    CPoint firstPt = m_tempPts.front();
    CPoint lastPt = m_tempPts.back();
    if (Distance(firstPt, lastPt) <= 0.5)
        return false;

    CLineEntity* pLine = new CLineEntity(lastPt, firstPt);
    pDoc->AddEntity(pLine);
    CompleteDrawCommand();
    pDoc->m_strCommandPrompt = L"LINE closed";
    UpdateStatusBar();
    Invalidate(FALSE);
    return true;
}

void CLargeHWView::AddPolylinePoint(CPoint world)
{
    CLargeHWDoc* pDoc = GetDocument();
    if (!pDoc) return;

    if (m_tempPts.empty()) {
        m_tempPts.push_back(world);
        return;
    }

    CPoint start = m_tempPts.back();
    std::vector<CPoint> newPts;
    newPts.push_back(start);
    if (m_bPolylineArcMode)
        AppendPolylineArcApprox(newPts, start, world);
    else
        newPts.push_back(world);

    if (!m_pActivePolyline) {
        std::vector<CPoint> initPts;
        initPts.push_back(start);
        m_pActivePolyline = new CPolylineEntity(initPts);
        m_pActivePolyline->m_vertexWidths.assign(1, max(1, m_nPolylineStartWidth));
        pDoc->AddEntity(m_pActivePolyline);
    }

    if ((int)m_pActivePolyline->m_vertexWidths.size() < (int)m_pActivePolyline->m_vertices.size())
        m_pActivePolyline->m_vertexWidths.resize(m_pActivePolyline->m_vertices.size(), max(1, m_nPolylineWidth));

    if (!m_pActivePolyline->m_vertices.empty())
        m_pActivePolyline->SetVertexWidth((int)m_pActivePolyline->m_vertices.size() - 1, m_nPolylineStartWidth);

    int countToAdd = (int)newPts.size() - 1;
    for (int i = 1; i < (int)newPts.size(); ++i) {
        double t = countToAdd <= 1 ? 1.0 : (double)i / countToAdd;
        int w = max(1, (int)(m_nPolylineStartWidth +
                             (m_nPolylineEndWidth - m_nPolylineStartWidth) * t + 0.5));
        m_pActivePolyline->AddVertex(newPts[i], w);
        m_tempPts.push_back(newPts[i]);
    }

    m_nPolylineWidth = max(1, m_nPolylineEndWidth);
    m_nPolylineStartWidth = m_nPolylineWidth;
    m_nPolylineEndWidth = m_nPolylineWidth;
    pDoc->SetModified(true);
    Invalidate(FALSE);
}

bool CLargeHWView::FinishPolylineCommand(bool close)
{
    CLargeHWDoc* pDoc = GetDocument();
    if (!pDoc || pDoc->m_drawState != STATE_DRAW_POLYLINE_POINT)
        return false;

    if (close && m_pActivePolyline && m_pActivePolyline->GetVertexCount() >= 3)
        m_pActivePolyline->m_bClosed = true;

    m_bPolylineClose = false;
    m_bPolylineArcMode = false;
    CompleteDrawCommand();
    pDoc->m_strCommandPrompt = close ? L"PLINE closed" : L"PLINE finished";
    UpdateStatusBar();
    Invalidate(FALSE);
    return true;
}

bool CLargeHWView::ProcessArrayParameterInput(const CString& strInput)
{
    CLargeHWDoc* pDoc = GetDocument();
    if (!pDoc) return false;

    CString s = strInput;
    s.Trim();
    if (s.IsEmpty()) return false;

    CadDrawState state = pDoc->m_drawState;
    if (state != STATE_ARRAY_ROWS &&
        state != STATE_ARRAY_COLUMNS &&
        state != STATE_ARRAY_ROW_SPACING &&
        state != STATE_ARRAY_COLUMN_SPACING) {
        return false;
    }

    double modelUnitScale = GetModelUnitScale(pDoc);
    double value = 0.0;
    CPoint pt;
    bool hasNumber = TryParseDoubleStrict(s, value);
    bool hasPoint = TryParseScriptPoint(s, CPoint(0, 0), pt, modelUnitScale);

    switch (state) {
    case STATE_ARRAY_ROWS:
        if (!hasNumber) return false;
        m_nArrayRows = max(1, abs((int)value));
        SetDrawState(STATE_ARRAY_COLUMNS);
        return true;

    case STATE_ARRAY_COLUMNS:
        if (!hasNumber) return false;
        m_nArrayColumns = max(1, abs((int)value));
        SetDrawState(STATE_ARRAY_ROW_SPACING);
        return true;

    case STATE_ARRAY_ROW_SPACING:
        if (hasNumber) {
            m_dArrayRowSpacing = value * modelUnitScale;
        } else if (hasPoint) {
            m_dArrayRowSpacing = pt.y;
        } else {
            return false;
        }
        SetDrawState(STATE_ARRAY_COLUMN_SPACING);
        return true;

    case STATE_ARRAY_COLUMN_SPACING:
        if (hasNumber) {
            m_dArrayColumnSpacing = value * modelUnitScale;
        } else if (hasPoint) {
            m_dArrayColumnSpacing = pt.x;
        } else {
            return false;
        }
        CreateRectangularArray(m_nArrayRows, m_nArrayColumns,
                               m_dArrayRowSpacing, m_dArrayColumnSpacing);
        return true;

    default:
        return false;
    }
}

// ============================================================
// Edit commands
// ============================================================
void CLargeHWView::OnEditUndo()
{
    if (m_bScriptRecording && !m_bRunningScript && !m_bSubmittingCommandLine)
        RecordScriptInput(L"UNDO");
    CLargeHWDoc* pDoc = GetDocument();
    pDoc->Undo();
    UpdateStatusBar();
    Invalidate(FALSE);
}

void CLargeHWView::OnEditRedo()
{
    if (m_bScriptRecording && !m_bRunningScript && !m_bSubmittingCommandLine)
        RecordScriptInput(L"REDO");
    CLargeHWDoc* pDoc = GetDocument();
    pDoc->Redo();
    UpdateStatusBar();
    Invalidate(FALSE);
}

void CLargeHWView::OnUpdateEditUndo(CCmdUI* pCmdUI)
{
    CLargeHWDoc* pDoc = GetDocument();
    pCmdUI->Enable(pDoc && pDoc->CanUndo());
}

void CLargeHWView::OnUpdateEditRedo(CCmdUI* pCmdUI)
{
    CLargeHWDoc* pDoc = GetDocument();
    pCmdUI->Enable(pDoc && pDoc->CanRedo());
}

void CLargeHWView::OnEditSelectAll()
{
    CLargeHWDoc* pDoc = GetDocument();
    for (auto* p : pDoc->GetEntities())
        const_cast<CEntity*>(p)->m_bSelected = true;
    Invalidate(FALSE);
}

// ============================================================
// View commands
// ============================================================
void CLargeHWView::OnViewZoomExtents()
{
    if (m_bScriptRecording && !m_bRunningScript && !m_bSubmittingCommandLine)
        RecordScriptInput(L"ZOOME");
    CLargeHWDoc* pDoc = GetDocument();
    if (!pDoc) return;
    m_tempPts.clear();
    m_bDrawing = false;
    m_bDragging = false;
    pDoc->m_drawState = STATE_IDLE;
    const auto& ents = pDoc->GetEntities();
    if (ents.empty()) return;

    CRect bounds(INT_MAX, INT_MAX, INT_MIN, INT_MIN);
    for (const auto* p : ents) {
        CRect eb = const_cast<CEntity*>(p)->GetBounds();
        if (eb.left < bounds.left) bounds.left = eb.left;
        if (eb.top < bounds.top) bounds.top = eb.top;
        if (eb.right > bounds.right) bounds.right = eb.right;
        if (eb.bottom > bounds.bottom) bounds.bottom = eb.bottom;
    }
    if (bounds.left == INT_MAX) return;

    CRect rcClient;
    GetClientRect(&rcClient);

    FitViewToWorldBounds(pDoc, bounds, rcClient, 24);
    UpdateStatusBar();
    Invalidate(FALSE);
}

void CLargeHWView::OnViewZoomWindow()
{
    if (m_bScriptRecording && !m_bRunningScript && !m_bSubmittingCommandLine)
        RecordScriptInput(L"ZOOM");
    m_tempPts.clear();
    m_nLastCommandID = ID_VIEW_ZOOM_WINDOW;
    SetDrawState(STATE_ZOOM_WINDOW_P1);
}

void CLargeHWView::OnViewPan()
{
    if (m_bScriptRecording && !m_bRunningScript && !m_bSubmittingCommandLine)
        RecordScriptInput(L"PAN");
    m_nLastCommandID = ID_VIEW_PAN;
    CLargeHWDoc* pDoc = GetDocument();
    pDoc->m_strCommandPrompt = L"PAN: Hold and drag middle mouse button to pan. Press Esc to cancel.";
    UpdateStatusBar();
}

void CLargeHWView::OnViewGrid()
{
    CLargeHWDoc* pDoc = GetDocument();
    pDoc->m_bShowGrid = !pDoc->m_bShowGrid;
    if (m_bScriptRecording && !m_bRunningScript && !m_bSubmittingCommandLine) {
        CString script;
        script.Format(L"GRID %s", (LPCTSTR)FormatOnOff(pDoc->m_bShowGrid));
        RecordScriptInput(script);
    }
    UpdateStatusBar();
    Invalidate(FALSE);
}

void CLargeHWView::OnViewSnap()
{
    CLargeHWDoc* pDoc = GetDocument();
    pDoc->m_bSnapToGrid = !pDoc->m_bSnapToGrid;
    if (m_bScriptRecording && !m_bRunningScript && !m_bSubmittingCommandLine) {
        CString script;
        script.Format(L"SNAP %s", (LPCTSTR)FormatOnOff(pDoc->m_bSnapToGrid));
        RecordScriptInput(script);
    }
    UpdateStatusBar();
}

void CLargeHWView::OnViewOrtho()
{
    CLargeHWDoc* pDoc = GetDocument();
    pDoc->m_bOrthoMode = !pDoc->m_bOrthoMode;
    if (m_bScriptRecording && !m_bRunningScript && !m_bSubmittingCommandLine) {
        CString script;
        script.Format(L"ORTHO %s", (LPCTSTR)FormatOnOff(pDoc->m_bOrthoMode));
        RecordScriptInput(script);
    }
    UpdateStatusBar();
}

// ============================================================
// Property commands
// ============================================================
void CLargeHWView::OnColorRed()     { if (m_bScriptRecording && !m_bRunningScript && !m_bSubmittingCommandLine) RecordScriptInput(L"COLOR RED");     CLargeHWDoc* p = GetDocument(); p->SetCurrentColor(RGB(255, 50, 50));   m_currentColor = RGB(255, 50, 50);   UpdateStatusBar(); }
void CLargeHWView::OnColorYellow()  { if (m_bScriptRecording && !m_bRunningScript && !m_bSubmittingCommandLine) RecordScriptInput(L"COLOR YELLOW");  CLargeHWDoc* p = GetDocument(); p->SetCurrentColor(RGB(255, 255, 50));  m_currentColor = RGB(255, 255, 50);  UpdateStatusBar(); }
void CLargeHWView::OnColorGreen()   { if (m_bScriptRecording && !m_bRunningScript && !m_bSubmittingCommandLine) RecordScriptInput(L"COLOR GREEN");   CLargeHWDoc* p = GetDocument(); p->SetCurrentColor(RGB(50, 255, 50));   m_currentColor = RGB(50, 255, 50);   UpdateStatusBar(); }
void CLargeHWView::OnColorCyan()    { if (m_bScriptRecording && !m_bRunningScript && !m_bSubmittingCommandLine) RecordScriptInput(L"COLOR CYAN");    CLargeHWDoc* p = GetDocument(); p->SetCurrentColor(RGB(0, 255, 209));   m_currentColor = RGB(0, 255, 209);   UpdateStatusBar(); }
void CLargeHWView::OnColorBlue()    { if (m_bScriptRecording && !m_bRunningScript && !m_bSubmittingCommandLine) RecordScriptInput(L"COLOR BLUE");    CLargeHWDoc* p = GetDocument(); p->SetCurrentColor(RGB(50, 100, 255));  m_currentColor = RGB(50, 100, 255);  UpdateStatusBar(); }
void CLargeHWView::OnColorMagenta() { if (m_bScriptRecording && !m_bRunningScript && !m_bSubmittingCommandLine) RecordScriptInput(L"COLOR MAGENTA"); CLargeHWDoc* p = GetDocument(); p->SetCurrentColor(RGB(255, 0, 127));   m_currentColor = RGB(255, 0, 127);   UpdateStatusBar(); }
void CLargeHWView::OnColorWhite()   { if (m_bScriptRecording && !m_bRunningScript && !m_bSubmittingCommandLine) RecordScriptInput(L"COLOR WHITE");   CLargeHWDoc* p = GetDocument(); p->SetCurrentColor(RGB(240, 240, 240)); m_currentColor = RGB(240, 240, 240); UpdateStatusBar(); }

void CLargeHWView::OnLinetypeSolid()   { if (m_bScriptRecording && !m_bRunningScript && !m_bSubmittingCommandLine) RecordScriptInput(L"LINETYPE SOLID");   CLargeHWDoc* p = GetDocument(); p->SetCurrentLineStyle(PS_SOLID);   m_currentLineStyle = PS_SOLID;   UpdateStatusBar(); }
void CLargeHWView::OnLinetypeDash()    { if (m_bScriptRecording && !m_bRunningScript && !m_bSubmittingCommandLine) RecordScriptInput(L"LINETYPE DASH");    CLargeHWDoc* p = GetDocument(); p->SetCurrentLineStyle(PS_DASH);    m_currentLineStyle = PS_DASH;    UpdateStatusBar(); }
void CLargeHWView::OnLinetypeDot()     { if (m_bScriptRecording && !m_bRunningScript && !m_bSubmittingCommandLine) RecordScriptInput(L"LINETYPE DOT");     CLargeHWDoc* p = GetDocument(); p->SetCurrentLineStyle(PS_DOT);     m_currentLineStyle = PS_DOT;     UpdateStatusBar(); }
void CLargeHWView::OnLinetypeDashDot() { if (m_bScriptRecording && !m_bRunningScript && !m_bSubmittingCommandLine) RecordScriptInput(L"LINETYPE DASHDOT"); CLargeHWDoc* p = GetDocument(); p->SetCurrentLineStyle(PS_DASHDOT); m_currentLineStyle = PS_DASHDOT; UpdateStatusBar(); }

void CLargeHWView::OnLineweight1()     { if (m_bScriptRecording && !m_bRunningScript && !m_bSubmittingCommandLine) RecordScriptInput(L"LINEWEIGHT 1"); CLargeHWDoc* p = GetDocument(); p->SetCurrentLineWidth(1); m_currentLineWidth = 1; UpdateStatusBar(); }
void CLargeHWView::OnLineweight2()     { if (m_bScriptRecording && !m_bRunningScript && !m_bSubmittingCommandLine) RecordScriptInput(L"LINEWEIGHT 2"); CLargeHWDoc* p = GetDocument(); p->SetCurrentLineWidth(2); m_currentLineWidth = 2; UpdateStatusBar(); }
void CLargeHWView::OnLineweight3()     { if (m_bScriptRecording && !m_bRunningScript && !m_bSubmittingCommandLine) RecordScriptInput(L"LINEWEIGHT 3"); CLargeHWDoc* p = GetDocument(); p->SetCurrentLineWidth(3); m_currentLineWidth = 3; UpdateStatusBar(); }
void CLargeHWView::OnLineweight4()     { if (m_bScriptRecording && !m_bRunningScript && !m_bSubmittingCommandLine) RecordScriptInput(L"LINEWEIGHT 4"); CLargeHWDoc* p = GetDocument(); p->SetCurrentLineWidth(4); m_currentLineWidth = 4; UpdateStatusBar(); }

void CLargeHWView::OnCancelCommand()
{
    if (m_bScriptRecording && !m_bRunningScript && !m_bSubmittingCommandLine)
        RecordScriptInput(L"ESC");
    m_tempPts.clear();
    m_bCoordDimMode = false;
    m_pChamferFirst = nullptr;
    CLargeHWDoc* pDoc = GetDocument();
    pDoc->DeselectAll();
    CompleteDrawCommand();
    Invalidate(FALSE);
}

// ============================================================
// Format commands
// ============================================================
void CLargeHWView::OnFormatLayer()
{
    CLargeHWDoc* pDoc = GetDocument();
    const auto& layers = pDoc->GetLayers();

    CMenu menu;
    menu.CreatePopupMenu();
    for (size_t i = 0; i < layers.size(); ++i) {
        UINT flags = MF_STRING;
        if (layers[i] == pDoc->GetCurrentLayer())
            flags |= MF_CHECKED;
        menu.AppendMenu(flags, 10000 + (UINT)i, layers[i]);
    }
    menu.AppendMenu(MF_SEPARATOR);
    menu.AppendMenu(MF_STRING, 10099, L"New Layer...");

    CPoint pt;
    GetCursorPos(&pt);

    int nSel = menu.TrackPopupMenu(TPM_LEFTALIGN | TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, this);
    menu.DestroyMenu();

    if (nSel >= 10000 && nSel < 10099) {
        int idx = nSel - 10000;
        if (idx < (int)layers.size()) {
            pDoc->SetCurrentLayer(layers[idx]);
            pDoc->m_strCommandPrompt.Format(L"Current layer: %s", (LPCTSTR)layers[idx]);
            if (m_bScriptRecording && !m_bRunningScript && !m_bSubmittingCommandLine) {
                CString script;
                script.Format(L"LAYER SET %s", (LPCTSTR)QuoteScriptToken(layers[idx]));
                RecordScriptInput(script);
            }
        }
    } else if (nSel == 10099) {
        CString strName;
        strName.Format(L"Layer%d", (int)layers.size() + 1);
        pDoc->AddLayer(strName);
        pDoc->SetCurrentLayer(strName);
        pDoc->m_strCommandPrompt.Format(L"New layer created: %s", (LPCTSTR)strName);
        if (m_bScriptRecording && !m_bRunningScript && !m_bSubmittingCommandLine) {
            CString script;
            script.Format(L"LAYER MAKE %s", (LPCTSTR)QuoteScriptToken(strName));
            RecordScriptInput(script);
        }
    }
    UpdateStatusBar();
}

// ============================================================
// SCR script commands
// ============================================================
void CLargeHWView::OnScriptRun()
{
    CFileDialog dlg(TRUE, L"scr", nullptr,
                    OFN_FILEMUSTEXIST | OFN_HIDEREADONLY,
                    L"SCR Script Files (*.scr)|*.scr|Text Files (*.txt)|*.txt|All Files (*.*)|*.*||",
                    this);
    if (dlg.DoModal() != IDOK)
        return;

    if (!ExecuteScriptFile(dlg.GetPathName())) {
        AfxMessageBox(L"Failed to read SCR script file.", MB_ICONERROR);
    }
}

void CLargeHWView::OnScriptRecordStart()
{
    if (m_bScriptRecording)
        return;

    CFileDialog dlg(FALSE, L"scr", L"recording.scr",
                    OFN_OVERWRITEPROMPT | OFN_HIDEREADONLY,
                    L"SCR Script Files (*.scr)|*.scr|Text Files (*.txt)|*.txt|All Files (*.*)|*.*||",
                    this);
    if (dlg.DoModal() != IDOK)
        return;

    CString strPath = dlg.GetPathName();
    if (!m_scriptRecordFile.Open(strPath,
                                 CFile::modeCreate | CFile::modeWrite | CFile::shareDenyWrite)) {
        AfxMessageBox(L"Failed to create SCR script file.", MB_ICONERROR);
        return;
    }

    m_strScriptRecordPath = strPath;
    m_bScriptRecording = true;

    CLargeHWDoc* pDoc = GetDocument();
    if (pDoc)
        pDoc->m_strCommandPrompt = L"SCR recording started. Use Script > Stop Recording to finish.";
    SyncCommandLinePrompt();
    UpdateStatusBar();
}

void CLargeHWView::OnScriptRecordStop()
{
    if (!m_bScriptRecording)
        return;

    m_scriptRecordFile.Close();
    m_bScriptRecording = false;

    CLargeHWDoc* pDoc = GetDocument();
    if (pDoc)
        pDoc->m_strCommandPrompt.Format(L"SCR recording saved: %s", (LPCTSTR)m_strScriptRecordPath);
    SyncCommandLinePrompt();
    UpdateStatusBar();
}

void CLargeHWView::OnUpdateScriptRun(CCmdUI* pCmdUI)
{
    pCmdUI->Enable(!m_bRunningScript);
}

void CLargeHWView::OnUpdateScriptRecordStart(CCmdUI* pCmdUI)
{
    pCmdUI->Enable(!m_bScriptRecording && !m_bRunningScript);
}

void CLargeHWView::OnUpdateScriptRecordStop(CCmdUI* pCmdUI)
{
    pCmdUI->Enable(m_bScriptRecording);
}

void CLargeHWView::OnContextRepeat()
{
    RepeatLastCommand();
}

void CLargeHWView::RepeatLastCommand()
{
    if (m_nLastCommandID == 0) return;
    switch (m_nLastCommandID) {
    case ID_DRAW_LINE:       OnDrawLine(); break;
    case ID_DRAW_POLYLINE:   OnDrawPolyline(); break;
    case ID_DRAW_CIRCLE:     OnDrawCircle(); break;
    case ID_DRAW_ARC:        OnDrawArc(); break;
    case ID_DRAW_RECTANGLE:  OnDrawRectangle(); break;
    case ID_DRAW_POLYGON:    OnDrawPolygon(); break;
    case ID_DRAW_ELLIPSE:    OnDrawEllipse(); break;
    case ID_DRAW_TEXT:       OnDrawText(); break;
    case ID_MODIFY_MOVE:     OnModifyMove(); break;
    case ID_MODIFY_COPY:     OnModifyCopy(); break;
    case ID_MODIFY_ROTATE:   OnModifyRotate(); break;
    case ID_MODIFY_SCALE:    OnModifyScale(); break;
    case ID_MODIFY_MIRROR:   OnModifyMirror(); break;
    case ID_MODIFY_OFFSET:   OnModifyOffset(); break;
    case ID_MODIFY_CHAMFER:  OnModifyChamfer(); break;
    case ID_MODIFY_FILLET:   OnModifyFillet(); break;
    case ID_MODIFY_ARRAY:    OnModifyArray(); break;
    case ID_VIEW_ZOOM_WINDOW: OnViewZoomWindow(); break;
    default: break;
    }
}

// ============================================================
// SCR script helpers
// ============================================================
CString CLargeHWView::FormatScriptPoint(CPoint pt) const
{
    return FormatModelPoint(pt, GetModelUnitScale(GetDocument()));
}

CString CLargeHWView::EscapeScriptText(const CString& strText) const
{
    CString str = strText;
    str.Replace(L"\"", L"\\\"");
    return str;
}

void CLargeHWView::RecordScriptInput(const CString& strInput)
{
    if (!m_bScriptRecording || m_bRunningScript)
        return;

    CString strLine = strInput;
    strLine.TrimRight(L"\r\n");

    CStringA utf8 = WideToUtf8(strLine);
    if (!utf8.IsEmpty())
        m_scriptRecordFile.Write((LPCSTR)utf8, (UINT)utf8.GetLength());
    m_scriptRecordFile.Write("\r\n", 2);
    m_scriptRecordFile.Flush();
}

bool CLargeHWView::IsCoordinateInput(const CString& strInput) const
{
    CString str = strInput;
    str.Trim();
    if (str.IsEmpty()) return false;

    if (str.Find(L',') >= 0 || str[0] == L'@' || str.Find(L'<') >= 0)
        return true;

    CLargeHWDoc* pDoc = GetDocument();
    if (!pDoc || pDoc->m_drawState == STATE_IDLE)
        return false;

    wchar_t* pEnd = nullptr;
    wcstod(str, &pEnd);
    while (pEnd && *pEnd != L'\0' && iswspace(*pEnd))
        ++pEnd;
    return pEnd && *pEnd == L'\0';
}

void CLargeHWView::SyncCommandLinePrompt()
{
    CLargeHWDoc* pDoc = GetDocument();
    CMainFrame* pFrame = (CMainFrame*)AfxGetMainWnd();
    if (!pDoc || !pFrame || !pFrame->m_wndCmdLine.GetSafeHwnd())
        return;

    CString strPrompt = pDoc->m_strCommandPrompt;
    if (strPrompt.IsEmpty()) strPrompt = L"Command: ";
    pFrame->m_wndCmdLine.SetWindowText(strPrompt);
    pFrame->m_wndCmdLine.SetSel(strPrompt.GetLength(), strPrompt.GetLength());
}

void CLargeHWView::SubmitCommandLineInput(const CString& strInput, bool bRecord)
{
    CLargeHWDoc* pDoc = GetDocument();
    if (!pDoc) return;

    CString strCmd = strInput;
    strCmd.Trim();

    if (strCmd.IsEmpty()) {
        if (bRecord) RecordScriptInput(L"");
        m_bSubmittingCommandLine = true;
        OnKeyDown(VK_RETURN, 1, 0);
        m_bSubmittingCommandLine = false;
    } else {
        CString strUpper = NormalizeScriptWord(strCmd);

        if (pDoc->m_drawState == STATE_DRAW_LINE_P2 &&
            (strUpper == L"C" || strUpper == L"CLOSE")) {
            if (bRecord) RecordScriptInput(strUpper);
            CloseLineCommand();
        } else if (pDoc->m_drawState == STATE_DRAW_POLYLINE_POINT &&
            (strUpper == L"C" || strUpper == L"CLOSE")) {
            if (bRecord) RecordScriptInput(strUpper);
            FinishPolylineCommand(true);
        } else if (pDoc->m_drawState == STATE_DRAW_POLYLINE_POINT &&
                   (strUpper == L"A" || strUpper == L"ARC")) {
            if (bRecord) RecordScriptInput(strUpper);
            m_bPolylineArcMode = true;
            SetDrawState(STATE_DRAW_POLYLINE_POINT);
        } else if (pDoc->m_drawState == STATE_DRAW_POLYLINE_POINT &&
                   (strUpper == L"L" || strUpper == L"LINE")) {
            if (bRecord) RecordScriptInput(strUpper);
            m_bPolylineArcMode = false;
            SetDrawState(STATE_DRAW_POLYLINE_POINT);
        } else if (pDoc->m_drawState == STATE_DRAW_POLYLINE_POINT &&
                   (strUpper == L"W" || strUpper == L"WIDTH")) {
            if (bRecord) RecordScriptInput(strUpper);
            SetDrawState(STATE_DRAW_POLYLINE_START_WIDTH);
        } else if (pDoc->m_drawState == STATE_ARRAY_SELECT &&
                   (strUpper == L"ALL" || strUpper == L"*")) {
            if (bRecord) RecordScriptInput(strUpper);
            pDoc->DeselectAll();
            SelectAllEntities(pDoc);
            UpdateArrayDefaultSpacingFromSelection();
            SetDrawState(STATE_ARRAY_ROWS);
        } else if (ProcessArrayParameterInput(strCmd)) {
            if (bRecord)
                RecordScriptInput(strCmd);
        } else if ((pDoc->m_drawState == STATE_ZOOM_WINDOW_P1 ||
                    pDoc->m_drawState == STATE_ZOOM_WINDOW_P2) &&
                   (strUpper == L"E" || strUpper == L"EXTENTS")) {
            if (bRecord) RecordScriptInput(L"ZOOM E");
            m_bSubmittingCommandLine = true;
            OnViewZoomExtents();
            m_bSubmittingCommandLine = false;
        } else if (IsCoordinateInput(strCmd)) {
            if (bRecord && pDoc->m_drawState != STATE_DRAW_TEXT_POS)
                RecordScriptInput(strCmd);
            m_bSubmittingCommandLine = true;
            ProcessCoordinateInput(strCmd);
            m_bSubmittingCommandLine = false;
        } else if (pDoc->m_drawState != STATE_IDLE) {
            pDoc->m_strCommandPrompt.Format(L"Invalid option for current command: %s. Press Esc to cancel.", (LPCTSTR)strCmd);
            UpdateStatusBar();
        } else {
            bool bTextStart =
                (strUpper == L"T" || strUpper == L"TEXT" || strUpper == L"DT" || strUpper == L"DTEXT");
            bool bScriptControl =
                (strUpper == L"SCRIPT" || strUpper == L"SCR" ||
                 strUpper == L"SCRIPTREC" || strUpper == L"RECORDSCRIPT" || strUpper == L"SCRREC" ||
                 strUpper == L"SCRIPTSTOP" || strUpper == L"STOPSCRIPT" || strUpper == L"SCRSTOP");
            if (bRecord && !bTextStart && !bScriptControl)
                RecordScriptInput(strCmd);

            m_bSubmittingCommandLine = true;
            ExecuteCommand(strCmd);
            m_bSubmittingCommandLine = false;
        }
    }

    if (pDoc->m_drawState == STATE_IDLE)
        pDoc->m_strCommandPrompt = L"Command: ";
    SyncCommandLinePrompt();
}

bool CLargeHWView::ShouldRecordPointForState(CadDrawState state) const
{
    switch (state) {
    case STATE_DRAW_LINE_P1:
    case STATE_DRAW_LINE_P2:
    case STATE_DRAW_CIRCLE_CENTER:
    case STATE_DRAW_CIRCLE_RADIUS:
    case STATE_DRAW_ARC_P1:
    case STATE_DRAW_ARC_P2:
    case STATE_DRAW_ARC_P3:
    case STATE_DRAW_RECT_P1:
    case STATE_DRAW_RECT_P2:
    case STATE_DRAW_POLYGON_CENTER:
    case STATE_DRAW_POLYGON_RADIUS:
    case STATE_DRAW_ELLIPSE_CENTER:
    case STATE_DRAW_ELLIPSE_RADIUS:
    case STATE_DRAW_POLYLINE_POINT:
    case STATE_DRAW_POLYLINE_START_WIDTH:
    case STATE_DRAW_POLYLINE_END_WIDTH:
    case STATE_MOVE_SELECT:
    case STATE_MOVE_BASE:
    case STATE_MOVE_DEST:
    case STATE_COPY_SELECT:
    case STATE_COPY_BASE:
    case STATE_COPY_DEST:
    case STATE_ROTATE_SELECT:
    case STATE_ROTATE_CENTER:
    case STATE_ROTATE_ANGLE:
    case STATE_SCALE_SELECT:
    case STATE_SCALE_BASE:
    case STATE_SCALE_FACTOR:
    case STATE_MIRROR_SELECT:
    case STATE_MIRROR_P1:
    case STATE_MIRROR_P2:
    case STATE_OFFSET_SELECT:
    case STATE_OFFSET_DIST:
    case STATE_CHAMFER_SELECT_FIRST:
    case STATE_CHAMFER_SELECT_SECOND:
    case STATE_FILLET_SELECT_FIRST:
    case STATE_FILLET_SELECT_SECOND:
    case STATE_ARRAY_SELECT:
    case STATE_ZOOM_WINDOW_P1:
    case STATE_ZOOM_WINDOW_P2:
        return true;
    default:
        return false;
    }
}

CString CLargeHWView::StripScriptComment(const CString& strLine) const
{
    bool bInQuote = false;
    for (int i = 0; i < strLine.GetLength(); ++i) {
        wchar_t ch = strLine[i];
        if (ch == L'"') {
            bInQuote = !bInQuote;
        } else if (!bInQuote && (ch == L';' || ch == L'#')) {
            return strLine.Left(i);
        }
    }
    return strLine;
}

void CLargeHWView::TokenizeScriptLine(const CString& strLine, std::vector<CString>& tokens) const
{
    tokens.clear();
    CString token;
    bool bInQuote = false;

    for (int i = 0; i < strLine.GetLength(); ++i) {
        wchar_t ch = strLine[i];
        if (bInQuote && ch == L'\\' && i + 1 < strLine.GetLength() && strLine[i + 1] == L'"') {
            token += L'"';
            ++i;
        } else if (ch == L'"') {
            bInQuote = !bInQuote;
        } else if (!bInQuote && iswspace(ch)) {
            if (!token.IsEmpty()) {
                tokens.push_back(token);
                token.Empty();
            }
        } else {
            token += ch;
        }
    }

    if (!token.IsEmpty())
        tokens.push_back(token);
}

bool CLargeHWView::ExecuteDirectScriptCommand(const CString& strLine)
{
    std::vector<CString> tokens;
    TokenizeScriptLine(strLine, tokens);
    if (tokens.empty())
        return true;

    CString cmd = NormalizeScriptWord(tokens[0]);

    CLargeHWDoc* pDoc = GetDocument();
    if (!pDoc) return true;

    const double coordScale = max(max(1.0, m_dScriptCoordinateScale), GetModelUnitScale(pDoc));
    auto parsePoint = [coordScale](const CString& token, CPoint ref, CPoint& pt) {
        return TryParseScriptPoint(token, ref, pt, coordScale);
    };
    auto scaleLength = [coordScale](double value) {
        return ScriptRound(value * coordScale);
    };

    if ((cmd == L"LINE" || cmd == L"L") && tokens.size() >= 3) {
        std::vector<CPoint> points;
        bool bClosed = false;
        CPoint ref(0, 0);
        for (size_t i = 1; i < tokens.size(); ++i) {
            CString option = NormalizeScriptWord(tokens[i]);
            if (option == L"C" || option == L"CLOSE") {
                bClosed = true;
                continue;
            }

            CPoint pt;
            if (!parsePoint(tokens[i], ref, pt))
                return false;
            points.push_back(pt);
            ref = pt;
        }

        if (points.size() < 2)
            return false;

        if (pDoc->m_drawState != STATE_IDLE)
            OnCancelCommand();

        for (size_t i = 1; i < points.size(); ++i)
            pDoc->AddEntity(new CLineEntity(points[i - 1], points[i]));
        if (bClosed && points.size() > 2)
            pDoc->AddEntity(new CLineEntity(points.back(), points.front()));

        pDoc->m_strCommandPrompt.Format(L"Script LINE added: %d segment(s)",
                                        (int)points.size() - 1 + (bClosed && points.size() > 2 ? 1 : 0));
        UpdateStatusBar();
        Invalidate(FALSE);
        return true;
    }

    if ((cmd == L"CIRCLE" || cmd == L"C") && tokens.size() >= 3) {
        CPoint center;
        if (!parsePoint(tokens[1], CPoint(0, 0), center))
            return false;

        int radius = 0;
        double radiusValue = 0.0;
        if (TryParseDoubleStrict(tokens[2], radiusValue)) {
            radius = abs(scaleLength(radiusValue));
        } else {
            CPoint radiusPoint;
            if (!parsePoint(tokens[2], center, radiusPoint))
                return false;
            radius = ScriptRound(Distance(center, radiusPoint));
        }
        if (radius < 1)
            radius = 1;

        if (pDoc->m_drawState != STATE_IDLE)
            OnCancelCommand();

        pDoc->AddEntity(new CCircleEntity(center, radius));
        pDoc->m_strCommandPrompt.Format(L"Script CIRCLE added: center %s radius %s",
                                        (LPCTSTR)FormatModelPoint(center, GetModelUnitScale(pDoc)),
                                        (LPCTSTR)FormatModelNumber(radius / GetModelUnitScale(pDoc)));
        UpdateStatusBar();
        Invalidate(FALSE);
        return true;
    }

    if ((cmd == L"RECTANGLE" || cmd == L"RECTANG" || cmd == L"RECT" || cmd == L"REC" || cmd == L"BOX") &&
        tokens.size() >= 3) {
        CPoint p1;
        CPoint p2;
        if (!parsePoint(tokens[1], CPoint(0, 0), p1) ||
            !parsePoint(tokens[2], p1, p2)) {
            return false;
        }

        if (pDoc->m_drawState != STATE_IDLE)
            OnCancelCommand();

        pDoc->AddEntity(new CRectangleEntity(p1, p2));
        pDoc->m_strCommandPrompt = (cmd == L"BOX") ? L"Script BOX projected to 2D RECTANGLE"
                                                    : L"Script RECTANGLE added";
        UpdateStatusBar();
        Invalidate(FALSE);
        return true;
    }

    if ((cmd == L"POINT" || cmd == L"PO") && tokens.size() >= 2) {
        CPoint pos;
        if (!parsePoint(tokens[1], CPoint(0, 0), pos))
            return false;

        if (pDoc->m_drawState != STATE_IDLE)
            OnCancelCommand();

        pDoc->AddEntity(new CPointEntity(pos));
        pDoc->m_strCommandPrompt.Format(L"Script POINT added: %s",
                                        (LPCTSTR)FormatModelPoint(pos, GetModelUnitScale(pDoc)));
        UpdateStatusBar();
        Invalidate(FALSE);
        return true;
    }

    if ((cmd == L"ELLIPSE" || cmd == L"EL") && tokens.size() >= 3) {
        CPoint center;
        if (!parsePoint(tokens[1], CPoint(0, 0), center))
            return false;

        int rx = 0;
        int ry = 0;
        double rxValue = 0.0;
        double ryValue = 0.0;

        if (tokens.size() >= 4 &&
            TryParseDoubleStrict(tokens[2], rxValue) &&
            TryParseDoubleStrict(tokens[3], ryValue)) {
            rx = abs(scaleLength(rxValue));
            ry = abs(scaleLength(ryValue));
        } else {
            CPoint radiusPoint;
            if (!parsePoint(tokens[2], center, radiusPoint))
                return false;
            rx = abs(radiusPoint.x - center.x);
            ry = abs(radiusPoint.y - center.y);
        }

        if (rx < 1) rx = 1;
        if (ry < 1) ry = 1;

        if (pDoc->m_drawState != STATE_IDLE)
            OnCancelCommand();

        pDoc->AddEntity(new CEllipseEntity(center, rx, ry));
        pDoc->m_strCommandPrompt.Format(L"Script ELLIPSE added: rx=%s ry=%s",
                                        (LPCTSTR)FormatModelNumber(rx / GetModelUnitScale(pDoc)),
                                        (LPCTSTR)FormatModelNumber(ry / GetModelUnitScale(pDoc)));
        UpdateStatusBar();
        Invalidate(FALSE);
        return true;
    }

    if ((cmd == L"POLYGON" || cmd == L"POL") && tokens.size() >= 3) {
        size_t nIndex = 1;
        int nSides = 6;
        double sideValue = 0.0;
        if (tokens.size() >= 4 && TryParseDoubleStrict(tokens[1], sideValue)) {
            nSides = max(3, min(12, abs(ScriptRound(sideValue))));
            nIndex = 2;
        }

        CPoint center;
        if (!parsePoint(tokens[nIndex], CPoint(0, 0), center))
            return false;

        int radius = 0;
        double radiusValue = 0.0;
        if (TryParseDoubleStrict(tokens[nIndex + 1], radiusValue)) {
            radius = abs(scaleLength(radiusValue));
        } else {
            CPoint radiusPoint;
            if (!parsePoint(tokens[nIndex + 1], center, radiusPoint))
                return false;
            radius = ScriptRound(Distance(center, radiusPoint));
        }
        if (radius < 1)
            radius = 1;

        if (pDoc->m_drawState != STATE_IDLE)
            OnCancelCommand();

        pDoc->AddEntity(new CPolygonEntity(center, radius, nSides));
        pDoc->m_strCommandPrompt.Format(L"Script POLYGON added: %d sides", nSides);
        UpdateStatusBar();
        Invalidate(FALSE);
        return true;
    }

    if ((cmd == L"ARC" || cmd == L"A") && tokens.size() >= 4) {
        if (NormalizeScriptWord(tokens[1]) == L"C") {
            if (tokens.size() < 5)
                return false;

            CPoint center;
            CPoint start;
            CPoint end;
            if (!parsePoint(tokens[2], CPoint(0, 0), center) ||
                !parsePoint(tokens[3], center, start) ||
                !parsePoint(tokens[4], start, end)) {
                return false;
            }

            int radius = ScriptRound(Distance(center, start));
            if (radius < 1)
                return false;

            double angStart = atan2((double)(start.y - center.y), (double)(start.x - center.x));
            double angEnd = atan2((double)(end.y - center.y), (double)(end.x - center.x));

            if (pDoc->m_drawState != STATE_IDLE)
                OnCancelCommand();

            CArcEntity* pArc = new CArcEntity();
            pArc->SetArcByCenter(center, radius, angStart, angEnd);
            pDoc->AddEntity(pArc);
            pDoc->m_strCommandPrompt = L"Script ARC added";
            UpdateStatusBar();
            Invalidate(FALSE);
            return true;
        }

        CPoint start;
        CPoint mid;
        CPoint end;
        if (!parsePoint(tokens[1], CPoint(0, 0), start) ||
            !parsePoint(tokens[2], start, mid) ||
            !parsePoint(tokens[3], mid, end)) {
            return false;
        }

        if (pDoc->m_drawState != STATE_IDLE)
            OnCancelCommand();

        pDoc->AddEntity(new CArcEntity(start, mid, end));
        pDoc->m_strCommandPrompt = L"Script ARC added";
        UpdateStatusBar();
        Invalidate(FALSE);
        return true;
    }

    if ((cmd == L"PLINE" || cmd == L"POLYLINE" || cmd == L"PL") && tokens.size() >= 3) {
        std::vector<CPoint> points;
        bool bClosed = false;
        CPoint ref(0, 0);

        for (size_t i = 1; i < tokens.size(); ++i) {
            CString option = NormalizeScriptWord(tokens[i]);
            if (option == L"C" || option == L"CLOSE") {
                bClosed = true;
                continue;
            }

            CPoint pt;
            if (!parsePoint(tokens[i], ref, pt))
                return false;
            points.push_back(pt);
            ref = pt;
        }

        if (points.size() < 2)
            return false;

        if (pDoc->m_drawState != STATE_IDLE)
            OnCancelCommand();

        CPolylineEntity* pPolyline = new CPolylineEntity(points);
        pPolyline->m_bClosed = bClosed;
        pDoc->AddEntity(pPolyline);
        pDoc->m_strCommandPrompt.Format(L"Script PLINE added: %d vertices", (int)points.size());
        UpdateStatusBar();
        Invalidate(FALSE);
        return true;
    }

    if (cmd == L"TEXT" || cmd == L"T" || cmd == L"DT" || cmd == L"DTEXT") {
        if (tokens.size() < 4)
            return false;

        if (pDoc->m_drawState != STATE_IDLE)
            OnCancelCommand();

        CPoint pos;
        if (!parsePoint(tokens[1], CPoint(0, 0), pos))
            return false;

        double height = 0.0;
        int nHeight = TryParseDoubleStrict(tokens[2], height) ? abs(scaleLength(height)) : _wtoi(tokens[2]);
        if (nHeight < 1) nHeight = 20;

        CString strText = tokens[3];
        for (size_t i = 4; i < tokens.size(); ++i) {
            strText += L" ";
            strText += tokens[i];
        }

        CTextEntity* pText = new CTextEntity(pos, strText, nHeight);
        pDoc->AddEntity(pText);
        pDoc->m_strCommandPrompt.Format(L"Script TEXT added: %s", (LPCTSTR)strText);
        UpdateStatusBar();
        Invalidate(FALSE);
        return true;
    }

    if ((cmd == L"LAYER" || cmd == L"LA" || cmd == L"CLAYER") && tokens.size() >= 2) {
        size_t nameIndex = 1;
        CString action = L"MAKE";

        if (cmd != L"CLAYER") {
            CString maybeAction = NormalizeScriptWord(tokens[1]);
            if (maybeAction == L"SET" || maybeAction == L"S" ||
                maybeAction == L"MAKE" || maybeAction == L"M" ||
                maybeAction == L"NEW" || maybeAction == L"N") {
                action = maybeAction;
                nameIndex = 2;
            }
        } else {
            action = L"SET";
        }

        if (nameIndex >= tokens.size())
            return false;

        CString layerName = tokens[nameIndex];
        layerName.Trim();
        if (layerName.IsEmpty())
            return false;

        pDoc->AddLayer(layerName);
        if (action != L"NEW" && action != L"N")
            pDoc->SetCurrentLayer(layerName);

        pDoc->m_strCommandPrompt.Format(
            (action == L"NEW" || action == L"N") ? L"Script LAYER new: %s" : L"Script LAYER current: %s",
            (LPCTSTR)layerName);
        UpdateStatusBar();
        Invalidate(FALSE);
        return true;
    }

    if (cmd == L"SELECT" && tokens.size() >= 2 && IsAllSelectionToken(tokens[1])) {
        pDoc->DeselectAll();
        SelectAllEntities(pDoc);
        pDoc->m_strCommandPrompt.Format(L"Script SELECT ALL: %d selected", pDoc->GetSelectedCount());
        UpdateStatusBar();
        Invalidate(FALSE);
        return true;
    }

    if ((cmd == L"ERASE" || cmd == L"DELETE" || cmd == L"DEL" || cmd == L"E") &&
        tokens.size() >= 2 && IsAllSelectionToken(tokens[1])) {
        pDoc->DeselectAll();
        SelectAllEntities(pDoc);
        int nCount = pDoc->GetSelectedCount();
        pDoc->DeleteSelected();
        pDoc->m_strCommandPrompt.Format(L"Script ERASE ALL: %d entities", nCount);
        UpdateStatusBar();
        Invalidate(FALSE);
        return true;
    }

    if ((cmd == L"MOVE" || cmd == L"M") && tokens.size() >= 3) {
        size_t nIndex = 1;
        if (IsAllSelectionToken(tokens[nIndex])) {
            pDoc->DeselectAll();
            SelectAllEntities(pDoc);
            ++nIndex;
        }
        if (tokens.size() < nIndex + 2 || pDoc->GetSelectedCount() == 0)
            return false;

        CPoint base;
        CPoint dest;
        if (!parsePoint(tokens[nIndex], CPoint(0, 0), base) ||
            !parsePoint(tokens[nIndex + 1], base, dest)) {
            return false;
        }

        pDoc->RecordModifyUndo();
        int nCount = pDoc->GetSelectedCount();
        pDoc->MoveSelected(dest.x - base.x, dest.y - base.y);
        pDoc->DeselectAll();
        pDoc->m_strCommandPrompt.Format(L"Script MOVE: %d entities", nCount);
        UpdateStatusBar();
        Invalidate(FALSE);
        return true;
    }

    if ((cmd == L"COPY" || cmd == L"CO" || cmd == L"CP") && tokens.size() >= 3) {
        size_t nIndex = 1;
        if (IsAllSelectionToken(tokens[nIndex])) {
            pDoc->DeselectAll();
            SelectAllEntities(pDoc);
            ++nIndex;
        }
        if (tokens.size() < nIndex + 2 || pDoc->GetSelectedCount() == 0)
            return false;

        CPoint base;
        CPoint dest;
        if (!parsePoint(tokens[nIndex], CPoint(0, 0), base) ||
            !parsePoint(tokens[nIndex + 1], base, dest)) {
            return false;
        }

        double dx = dest.x - base.x;
        double dy = dest.y - base.y;
        auto selectedEnts = pDoc->GetSelectedEntities();
        std::vector<int> newIDs;
        for (auto* pEnt : selectedEnts) {
            CEntity* pCopy = pEnt->Clone();
            if (!pCopy) continue;
            pCopy->Move(dx, dy);
            pDoc->AddEntity(pCopy, false);
            newIDs.push_back(pCopy->m_nID);
        }
        pDoc->RecordCreateUndo(newIDs);
        pDoc->DeselectAll();
        pDoc->m_strCommandPrompt.Format(L"Script COPY: %d entities", (int)newIDs.size());
        UpdateStatusBar();
        Invalidate(FALSE);
        return true;
    }

    if ((cmd == L"ROTATE" || cmd == L"RO") && tokens.size() >= 3) {
        size_t nIndex = 1;
        if (IsAllSelectionToken(tokens[nIndex])) {
            pDoc->DeselectAll();
            SelectAllEntities(pDoc);
            ++nIndex;
        }
        if (tokens.size() < nIndex + 2 || pDoc->GetSelectedCount() == 0)
            return false;

        CPoint base;
        if (!parsePoint(tokens[nIndex], CPoint(0, 0), base))
            return false;

        double angleDeg = 0.0;
        if (!TryParseDoubleStrict(tokens[nIndex + 1], angleDeg)) {
            CPoint anglePoint;
            if (!parsePoint(tokens[nIndex + 1], base, anglePoint))
                return false;
            angleDeg = atan2((double)(anglePoint.y - base.y), (double)(anglePoint.x - base.x)) * 180.0 / M_PI;
        }

        pDoc->RecordModifyUndo();
        int nCount = pDoc->GetSelectedCount();
        pDoc->RotateSelected(base, angleDeg * M_PI / 180.0);
        pDoc->DeselectAll();
        pDoc->m_strCommandPrompt.Format(L"Script ROTATE: %d entities", nCount);
        UpdateStatusBar();
        Invalidate(FALSE);
        return true;
    }

    if ((cmd == L"SCALE" || cmd == L"SC") && tokens.size() >= 3) {
        size_t nIndex = 1;
        if (IsAllSelectionToken(tokens[nIndex])) {
            pDoc->DeselectAll();
            SelectAllEntities(pDoc);
            ++nIndex;
        }
        if (tokens.size() < nIndex + 2 || pDoc->GetSelectedCount() == 0)
            return false;

        CPoint base;
        double factor = 0.0;
        if (!parsePoint(tokens[nIndex], CPoint(0, 0), base) ||
            !TryParseDoubleStrict(tokens[nIndex + 1], factor)) {
            return false;
        }
        if (fabs(factor) < 1e-9)
            return false;

        pDoc->RecordModifyUndo();
        int nCount = pDoc->GetSelectedCount();
        pDoc->ScaleSelected(base, factor);
        pDoc->DeselectAll();
        pDoc->m_strCommandPrompt.Format(L"Script SCALE: %d entities", nCount);
        UpdateStatusBar();
        Invalidate(FALSE);
        return true;
    }

    if ((cmd == L"MIRROR" || cmd == L"MI") && tokens.size() >= 3) {
        size_t nIndex = 1;
        if (IsAllSelectionToken(tokens[nIndex])) {
            pDoc->DeselectAll();
            SelectAllEntities(pDoc);
            ++nIndex;
        }
        if (tokens.size() < nIndex + 2 || pDoc->GetSelectedCount() == 0)
            return false;

        CPoint p1;
        CPoint p2;
        if (!parsePoint(tokens[nIndex], CPoint(0, 0), p1) ||
            !parsePoint(tokens[nIndex + 1], p1, p2) ||
            Distance(p1, p2) < 1.0) {
            return false;
        }

        pDoc->RecordModifyUndo();
        int nCount = pDoc->GetSelectedCount();
        pDoc->MirrorSelected(p1, p2);
        pDoc->DeselectAll();
        pDoc->m_strCommandPrompt.Format(L"Script MIRROR: %d entities", nCount);
        UpdateStatusBar();
        Invalidate(FALSE);
        return true;
    }

    if ((cmd == L"OFFSET" || cmd == L"O") && tokens.size() >= 3) {
        size_t nIndex = 1;
        if (IsAllSelectionToken(tokens[nIndex])) {
            pDoc->DeselectAll();
            SelectAllEntities(pDoc);
            ++nIndex;
        }
        if (tokens.size() < nIndex + 2 || pDoc->GetSelectedCount() == 0)
            return false;

        CPoint base;
        CPoint dest;
        if (!parsePoint(tokens[nIndex], CPoint(0, 0), base) ||
            !parsePoint(tokens[nIndex + 1], base, dest)) {
            return false;
        }

        double dx = dest.x - base.x;
        double dy = dest.y - base.y;
        auto selectedEnts = pDoc->GetSelectedEntities();
        std::vector<int> newIDs;
        for (auto* pEnt : selectedEnts) {
            CEntity* pCopy = pEnt->Clone();
            if (!pCopy) continue;
            pCopy->Move(dx, dy);
            pDoc->AddEntity(pCopy, false);
            newIDs.push_back(pCopy->m_nID);
        }
        pDoc->RecordCreateUndo(newIDs);
        pDoc->DeselectAll();
        pDoc->m_strCommandPrompt.Format(L"Script OFFSET: %d entities", (int)newIDs.size());
        UpdateStatusBar();
        Invalidate(FALSE);
        return true;
    }

    if ((cmd == L"ARRAY" || cmd == L"ARRAYRECT" || cmd == L"AR") && tokens.size() >= 5) {
        size_t nIndex = 1;
        if (IsAllSelectionToken(tokens[nIndex])) {
            pDoc->DeselectAll();
            SelectAllEntities(pDoc);
            ++nIndex;
        }
        if (tokens.size() < nIndex + 4 || pDoc->GetSelectedCount() == 0)
            return false;

        double rowsValue = 0.0;
        double colsValue = 0.0;
        double rowSpacingValue = 0.0;
        double colSpacingValue = 0.0;
        if (!TryParseDoubleStrict(tokens[nIndex], rowsValue) ||
            !TryParseDoubleStrict(tokens[nIndex + 1], colsValue) ||
            !TryParseDoubleStrict(tokens[nIndex + 2], rowSpacingValue) ||
            !TryParseDoubleStrict(tokens[nIndex + 3], colSpacingValue)) {
            return false;
        }

        CreateRectangularArray(max(1, abs((int)rowsValue)),
                               max(1, abs((int)colsValue)),
                               scaleLength(rowSpacingValue),
                               scaleLength(colSpacingValue));
        return true;
    }

    if ((cmd == L"CHAMFER" || cmd == L"CHA") && tokens.size() >= 2) {
        double distanceValue = 0.0;
        if (!TryParseDoubleStrict(tokens[1], distanceValue))
            return false;

        std::vector<CLineEntity*> selectedLines;
        for (auto* pEnt : pDoc->GetSelectedEntities()) {
            if (pEnt && pEnt->m_Type == ENT_LINE)
                selectedLines.push_back(static_cast<CLineEntity*>(pEnt));
        }
        if (selectedLines.size() < 2)
            return false;

        ApplyChamfer(selectedLines[0], selectedLines[1], fabs(scaleLength(distanceValue)));
        return true;
    }

    if (cmd == L"UNDO" || cmd == L"U") {
        OnEditUndo();
        return true;
    }

    if (cmd == L"REDO") {
        OnEditRedo();
        return true;
    }

    if (cmd == L"ZOOME" ||
        (cmd == L"ZOOM" && tokens.size() >= 2 &&
         (NormalizeScriptWord(tokens[1]) == L"E" || NormalizeScriptWord(tokens[1]) == L"EXTENTS"))) {
        OnViewZoomExtents();
        return true;
    }

    if ((cmd == L"GRID" || cmd == L"SNAP" || cmd == L"ORTHO" || cmd == L"OSNAP" || cmd == L"F7" ||
         cmd == L"F8" || cmd == L"F9" || cmd == L"F3") && tokens.size() >= 1) {
        bool* pFlag = nullptr;
        if (cmd == L"GRID" || cmd == L"F7") pFlag = &pDoc->m_bShowGrid;
        else if (cmd == L"SNAP" || cmd == L"F9") pFlag = &pDoc->m_bSnapToGrid;
        else if (cmd == L"ORTHO" || cmd == L"F8") pFlag = &pDoc->m_bOrthoMode;
        else if (cmd == L"OSNAP" || cmd == L"F3") pFlag = &pDoc->m_bObjectSnap;

        if (!pFlag)
            return false;

        if (tokens.size() >= 2) {
            bool flagValue = false;
            double numericValue = 0.0;
            if (TryParseOnOff(tokens[1], flagValue)) {
                *pFlag = flagValue;
            } else if ((cmd == L"GRID" || cmd == L"SNAP") &&
                       TryParseDoubleStrict(tokens[1], numericValue)) {
                pDoc->m_nGridSpacing = max(1, abs(scaleLength(numericValue)));
                *pFlag = true;
            } else {
                return false;
            }
        } else {
            *pFlag = !*pFlag;
        }

        pDoc->m_strCommandPrompt.Format(L"Script %s %s", (LPCTSTR)cmd, (LPCTSTR)FormatOnOff(*pFlag));
        UpdateStatusBar();
        Invalidate(FALSE);
        return true;
    }

    if ((cmd == L"GRIDSPACING" || cmd == L"GRIDUNIT") && tokens.size() >= 2) {
        double spacing = 0.0;
        if (!TryParseDoubleStrict(tokens[1], spacing))
            return false;

        pDoc->m_nGridSpacing = max(1, abs(scaleLength(spacing)));
        pDoc->m_strCommandPrompt.Format(L"Script GRIDSPACING %s",
                                        (LPCTSTR)FormatModelNumber(pDoc->m_nGridSpacing / GetModelUnitScale(pDoc)));
        UpdateStatusBar();
        Invalidate(FALSE);
        return true;
    }

    if (cmd == L"COLOR" && tokens.size() >= 2) {
        CString arg = NormalizeScriptWord(tokens[1]);

        if (arg == L"RED") OnColorRed();
        else if (arg == L"YELLOW") OnColorYellow();
        else if (arg == L"GREEN") OnColorGreen();
        else if (arg == L"CYAN") OnColorCyan();
        else if (arg == L"BLUE") OnColorBlue();
        else if (arg == L"MAGENTA") OnColorMagenta();
        else if (arg == L"WHITE") OnColorWhite();
        else if (arg.Find(L',') > 0) {
            int nFirst = arg.Find(L',');
            int nSecond = arg.Find(L",", nFirst + 1);
            if (nSecond > nFirst) {
                int r = max(0, min(255, _wtoi(arg.Left(nFirst))));
                int g = max(0, min(255, _wtoi(arg.Mid(nFirst + 1, nSecond - nFirst - 1))));
                int b = max(0, min(255, _wtoi(arg.Mid(nSecond + 1))));
                pDoc->SetCurrentColor(RGB(r, g, b));
                m_currentColor = RGB(r, g, b);
                UpdateStatusBar();
            }
        }
        return true;
    }

    if ((cmd == L"LINETYPE" || cmd == L"LTYPE") && tokens.size() >= 2) {
        CString arg = NormalizeScriptWord(tokens[1]);
        if (arg == L"SOLID") OnLinetypeSolid();
        else if (arg == L"DASH") OnLinetypeDash();
        else if (arg == L"DOT") OnLinetypeDot();
        else if (arg == L"DASHDOT") OnLinetypeDashDot();
        return true;
    }

    if ((cmd == L"LINEWEIGHT" || cmd == L"LWEIGHT") && tokens.size() >= 2) {
        double width = 0.0;
        int nWidth = TryParseDoubleStrict(tokens[1], width) ? abs(ScriptRound(width)) : _wtoi(tokens[1]);
        if (nWidth <= 1) OnLineweight1();
        else if (nWidth == 2) OnLineweight2();
        else if (nWidth == 3) OnLineweight3();
        else OnLineweight4();
        return true;
    }

    return false;
}

void CLargeHWView::ExecuteScriptLine(const CString& strLine)
{
    CString line = strLine;
    line.TrimRight(L"\r\n");

    CString trimmed = line;
    trimmed.Trim();
    if (trimmed.IsEmpty()) {
        SubmitCommandLineInput(L"", false);
        return;
    }

    CString withoutComment = StripScriptComment(line);
    withoutComment.Trim();
    if (withoutComment.IsEmpty())
        return;

    if (ExecuteDirectScriptCommand(withoutComment))
        return;

    std::vector<CString> tokens;
    TokenizeScriptLine(withoutComment, tokens);
    for (size_t i = 0; i < tokens.size(); ++i) {
        CString token = (i == 0) ? NormalizeScriptWord(tokens[i]) : tokens[i];
        SubmitCommandLineInput(token, false);
    }
}

bool CLargeHWView::ExecuteScriptFile(const CString& strPath)
{
    CString strContent;
    if (!LoadTextFile(strPath, strContent))
        return false;

    bool bPrevRunning = m_bRunningScript;
    double dPrevScriptCoordinateScale = m_dScriptCoordinateScale;
    CLargeHWDoc* pDoc = GetDocument();
    if (pDoc)
        ApplyDocumentModelUnitScale(pDoc, DetermineScriptCoordinateScale(strContent));
    m_bRunningScript = true;
    m_dScriptCoordinateScale = pDoc ? GetModelUnitScale(pDoc) : DetermineScriptCoordinateScale(strContent);

    int nLineCount = 0;
    int nStart = 0;
    while (nStart <= strContent.GetLength()) {
        int nEnd = nStart;
        while (nEnd < strContent.GetLength() &&
               strContent[nEnd] != L'\r' &&
               strContent[nEnd] != L'\n') {
            ++nEnd;
        }

        CString strLine = strContent.Mid(nStart, nEnd - nStart);
        ExecuteScriptLine(strLine);
        ++nLineCount;

        if (nEnd >= strContent.GetLength())
            break;

        nStart = nEnd + 1;
        if (strContent[nEnd] == L'\r' &&
            nStart < strContent.GetLength() &&
            strContent[nStart] == L'\n') {
            ++nStart;
        }
    }

    m_bRunningScript = bPrevRunning;
    m_dScriptCoordinateScale = dPrevScriptCoordinateScale;

    if (pDoc) {
        if (pDoc->m_drawState == STATE_IDLE)
            pDoc->m_strCommandPrompt.Format(L"Script loaded: %d lines", nLineCount);
        else
            pDoc->m_strCommandPrompt.Format(L"Script paused: command needs more input after %d lines", nLineCount);
    }
    SyncCommandLinePrompt();
    UpdateStatusBar();
    Invalidate(FALSE);
    return true;
}

// ============================================================
// Parse coordinate from command line input
//   "100,200"  -> absolute (100, 200)
//   "@50,100"  -> relative to ref point (ref+50, ref+100)
//   "100<45"   -> polar: distance 100 at 45 degrees from ref
// ============================================================
CPoint CLargeHWView::ParseCoordinate(const CString& str, CPoint ref) const
{
    CString s = str;
    s.Trim();

    if (s.IsEmpty()) return ref;

    CPoint pt;
    double modelUnitScale = GetModelUnitScale(GetDocument());
    if (TryParseScriptPoint(s, ref, pt, modelUnitScale))
        return pt;

    bool bRelative = false;
    CString numeric = s;
    if (!numeric.IsEmpty() && numeric[0] == L'@') {
        bRelative = true;
        numeric = numeric.Mid(1);
        numeric.Trim();
    }

    double val = 0.0;
    if (TryParseDoubleStrict(numeric, val)) {
        int nVal = ScriptRound(val * modelUnitScale);
        if (bRelative) return CPoint(ref.x + nVal, ref.y);
        return CPoint(nVal, ref.y);
    }

    return ref;
}

// ============================================================
// Process coordinate input - convert world coords to screen click
// ============================================================
void CLargeHWView::ProcessCoordinateInput(const CString& strInput)
{
    CLargeHWDoc* pDoc = GetDocument();
    if (!pDoc) return;

    CadDrawState state = pDoc->m_drawState;

    // Single-number input for distance/angle states
    if (!strInput.IsEmpty() && strInput.Find(L',') < 0 && strInput[0] != L'@' && strInput.Find(L'<') < 0) {
        double modelUnitScale = GetModelUnitScale(pDoc);
        double inputValue = 0.0;
        bool bHasNumericValue = TryParseDoubleStrict(strInput, inputValue);
        int distanceVal = bHasNumericValue ? ScriptRound(inputValue * modelUnitScale) : _wtoi(strInput);
        double angleVal = bHasNumericValue ? inputValue : (double)_wtoi(strInput);
        double factorVal = bHasNumericValue ? inputValue : (double)_wtoi(strInput);

        switch (state) {
        case STATE_DRAW_CIRCLE_RADIUS:
            if (!m_tempPts.empty()) {
                int r = abs(distanceVal);
                CCircleEntity* pCir = new CCircleEntity(m_tempPts[0], r);
                pDoc->AddEntity(pCir);
                CompleteDrawCommand();
            }
            return;

        case STATE_DRAW_POLYGON_RADIUS:
            if (!m_tempPts.empty()) {
                int r = abs(distanceVal);
                CPolygonEntity* pPoly = new CPolygonEntity(m_tempPts[0], r, m_nPolygonSides);
                pDoc->AddEntity(pPoly);
                CompleteDrawCommand();
            }
            return;

        case STATE_DRAW_ELLIPSE_RADIUS:
            if (!m_tempPts.empty()) {
                int r = abs(distanceVal);
                CEllipseEntity* pEll = new CEllipseEntity(m_tempPts[0], r, r);
                pDoc->AddEntity(pEll);
                CompleteDrawCommand();
            }
            return;

        case STATE_ROTATE_ANGLE:
            if (!m_tempPts.empty()) {
                double angle = angleVal * M_PI / 180.0;
                int nCount = pDoc->GetSelectedCount();
                if (nCount > 0) {
                    pDoc->RecordModifyUndo();
                    pDoc->RotateSelected(m_tempPts.back(), angle);
                    pDoc->m_strCommandPrompt.Format(L"Rotated %d entities (angle=%.1f deg)", nCount, angleVal);
                }
                CompleteDrawCommand();
            }
            return;

        case STATE_SCALE_FACTOR:
            if (!m_tempPts.empty()) {
                double factor = factorVal;
                if (factor == 0) factor = 1.0;
                int nCount = pDoc->GetSelectedCount();
                if (nCount > 0) {
                    pDoc->RecordModifyUndo();
                    pDoc->ScaleSelected(m_tempPts.back(), factor);
                    pDoc->m_strCommandPrompt.Format(L"Scaled %d entities (factor=%.2f)", nCount, factor);
                }
                CompleteDrawCommand();
            }
            return;

        case STATE_CHAMFER_SELECT_FIRST:
            m_dChamferDistance = max(0.0, fabs(inputValue * modelUnitScale));
            SetDrawState(STATE_CHAMFER_SELECT_FIRST);
            return;

        case STATE_DRAW_POLYLINE_START_WIDTH:
            m_nPolylineStartWidth = max(1, abs(distanceVal));
            SetDrawState(STATE_DRAW_POLYLINE_END_WIDTH);
            return;

        case STATE_DRAW_POLYLINE_END_WIDTH:
            m_nPolylineEndWidth = max(1, abs(distanceVal));
            m_nPolylineWidth = m_nPolylineEndWidth;
            SetDrawState(STATE_DRAW_POLYLINE_POINT);
            return;

        case STATE_FILLET_SELECT_FIRST:
            m_dFilletRadius = max(0.0, fabs(inputValue * modelUnitScale));
            SetDrawState(STATE_FILLET_SELECT_FIRST);
            return;

        case STATE_ARRAY_ROWS:
            m_nArrayRows = max(1, abs((int)factorVal));
            SetDrawState(STATE_ARRAY_COLUMNS);
            return;

        case STATE_ARRAY_COLUMNS:
            m_nArrayColumns = max(1, abs((int)factorVal));
            SetDrawState(STATE_ARRAY_ROW_SPACING);
            return;

        case STATE_ARRAY_ROW_SPACING:
            m_dArrayRowSpacing = inputValue * modelUnitScale;
            SetDrawState(STATE_ARRAY_COLUMN_SPACING);
            return;

        case STATE_ARRAY_COLUMN_SPACING:
            m_dArrayColumnSpacing = inputValue * modelUnitScale;
            CreateRectangularArray(m_nArrayRows, m_nArrayColumns,
                                   m_dArrayRowSpacing, m_dArrayColumnSpacing);
            return;
        }
    }

    // Coordinate input (comma / @ / polar) - convert world -> screen -> click
    CPoint refPt(0, 0);
    if (!m_tempPts.empty()) refPt = m_tempPts.back();

    CPoint world = ParseCoordinate(strInput, refPt);

    // Apply snap
    world = SnapToGrid(world);
    if (pDoc->m_bObjectSnap) world = SnapToObjects(world);

    // Convert to screen and inject as click
    CPoint screen = WorldToScreen(world);
    m_ptCurrent = screen;
    OnLButtonDown(0, screen);
}

// ============================================================
// Execute command from command line (AutoCAD-style aliases)
// ============================================================
void CLargeHWView::ExecuteCommand(const CString& strCmd)
{
    if (strCmd.IsEmpty()) return;

    CString cmd = NormalizeScriptWord(strCmd);

    // Cancel current command first if in progress
    CLargeHWDoc* pDoc = GetDocument();
    if (!pDoc) return;

    // Commands that cancel ongoing operations
    if (cmd == L"ESC" || cmd == L"CANCEL" || cmd == L"*CANCEL*") {
        OnCancelCommand();
        return;
    }

    // If currently in a command, cancel first before starting new one
    if (pDoc->m_drawState != STATE_IDLE) {
        OnCancelCommand();
    }

    // --- Draw commands ---
    if (cmd == L"L" || cmd == L"LINE") {
        OnDrawLine();
    } else if (cmd == L"C" || cmd == L"CIRCLE") {
        OnDrawCircle();
    } else if (cmd == L"A" || cmd == L"ARC") {
        OnDrawArc();
    } else if (cmd == L"REC" || cmd == L"RECTANG" || cmd == L"RECTANGLE") {
        OnDrawRectangle();
    } else if (cmd == L"PL" || cmd == L"PLINE" || cmd == L"POLYLINE") {
        OnDrawPolyline();
    } else if (cmd == L"EL" || cmd == L"ELLIPSE") {
        OnDrawEllipse();
    } else if (cmd == L"POL" || cmd == L"POLYGON") {
        OnDrawPolygon();
    } else if (cmd == L"T" || cmd == L"TEXT" || cmd == L"DT" || cmd == L"DTEXT") {
        OnDrawText();

    // --- Dimension commands ---
    } else if (cmd == L"DIMRAD" || cmd == L"DIMRADIUS" || cmd == L"DRA") {
        OnDrawDimRadius();
    } else if (cmd == L"DIMDIA" || cmd == L"DIMDIAMETER" || cmd == L"DDI") {
        OnDrawDimDiameter();
    } else if (cmd == L"DIMARC" || cmd == L"DIMARCLEN" || cmd == L"DAR") {
        OnDrawDimArcLength();
    } else if (cmd == L"DIMCOORD" || cmd == L"DIMCOORDINATE" || cmd == L"DOR") {
        OnDrawDimCoordinate();

    // --- Modify commands ---
    } else if (cmd == L"M" || cmd == L"MOVE") {
        OnModifyMove();
    } else if (cmd == L"CO" || cmd == L"CP" || cmd == L"COPY") {
        OnModifyCopy();
    } else if (cmd == L"RO" || cmd == L"ROTATE") {
        OnModifyRotate();
    } else if (cmd == L"SC" || cmd == L"SCALE") {
        OnModifyScale();
    } else if (cmd == L"MI" || cmd == L"MIRROR") {
        OnModifyMirror();
    } else if (cmd == L"O" || cmd == L"OFFSET") {
        OnModifyOffset();
    } else if (cmd == L"CHA" || cmd == L"CHAMFER") {
        OnModifyChamfer();
    } else if (cmd == L"F" || cmd == L"FILLET") {
        OnModifyFillet();
    } else if (cmd == L"AR" || cmd == L"ARRAY" || cmd == L"ARRAYRECT") {
        OnModifyArray();
    } else if (cmd == L"E" || cmd == L"ERASE" || cmd == L"DEL" || cmd == L"DELETE") {
        OnModifyDelete();

    // --- View commands ---
    } else if (cmd == L"Z" || cmd == L"ZOOM") {
        OnViewZoomWindow();
    } else if (cmd == L"ZE" || cmd == L"ZOOME") {
        OnViewZoomExtents();
    } else if (cmd == L"P" || cmd == L"PAN") {
        OnViewPan();

    // --- Edit commands ---
    } else if (cmd == L"U" || cmd == L"UNDO") {
        OnEditUndo();
    } else if (cmd == L"REDO") {
        OnEditRedo();

    // --- Scripts ---
    } else if (cmd == L"SCRIPT" || cmd == L"SCR") {
        OnScriptRun();
    } else if (cmd == L"SCRIPTREC" || cmd == L"RECORDSCRIPT" || cmd == L"SCRREC") {
        OnScriptRecordStart();
    } else if (cmd == L"SCRIPTSTOP" || cmd == L"STOPSCRIPT" || cmd == L"SCRSTOP") {
        OnScriptRecordStop();

    // --- Toggles ---
    } else if (cmd == L"GRID" || cmd == L"F7") {
        pDoc->m_bShowGrid = !pDoc->m_bShowGrid;
        UpdateStatusBar();
        Invalidate(FALSE);
    } else if (cmd == L"SNAP" || cmd == L"F9") {
        pDoc->m_bSnapToGrid = !pDoc->m_bSnapToGrid;
        UpdateStatusBar();
        Invalidate(FALSE);
    } else if (cmd == L"ORTHO" || cmd == L"F8") {
        pDoc->m_bOrthoMode = !pDoc->m_bOrthoMode;
        UpdateStatusBar();
        Invalidate(FALSE);
    } else if (cmd == L"OSNAP" || cmd == L"F3") {
        pDoc->m_bObjectSnap = !pDoc->m_bObjectSnap;
        UpdateStatusBar();
        Invalidate(FALSE);

    } else if (ExecuteDirectScriptCommand(strCmd)) {
        return;

    // --- Unknown command ---
    } else {
        pDoc->m_strCommandPrompt.Format(L"Unknown command: \"%s\". Press F1 for help.", (LPCTSTR)strCmd);
        CMainFrame* pFrame = (CMainFrame*)AfxGetMainWnd();
        if (pFrame && pFrame->m_wndCmdLine.GetSafeHwnd()) {
            pFrame->m_wndCmdLine.SetWindowText(pDoc->m_strCommandPrompt);
            int nLen = pDoc->m_strCommandPrompt.GetLength();
            pFrame->m_wndCmdLine.SetSel(nLen, nLen);
        }
    }
}

// ============================================================
// Print support
// ============================================================
BOOL CLargeHWView::OnPreparePrinting(CPrintInfo* pInfo)
{
    return DoPreparePrinting(pInfo);
}

void CLargeHWView::OnBeginPrinting(CDC* pDC, CPrintInfo* pInfo)
{
    CLargeHWDoc* pDoc = GetDocument();
    if (!pDoc) return;

    CRect rcClient;
    GetClientRect(&rcClient);

    const auto& ents = pDoc->GetEntities();
    if (ents.empty()) return;

    CRect bounds(INT_MAX, INT_MAX, INT_MIN, INT_MIN);
    for (const auto* p : ents) {
        CRect eb = const_cast<CEntity*>(p)->GetBounds();
        if (eb.left < bounds.left) bounds.left = eb.left;
        if (eb.top < bounds.top) bounds.top = eb.top;
        if (eb.right > bounds.right) bounds.right = eb.right;
        if (eb.bottom > bounds.bottom) bounds.bottom = eb.bottom;
    }
    if (bounds.left == INT_MAX) return;
    bounds.InflateRect(50, 50);

    int pageW = pDC->GetDeviceCaps(HORZRES);
    int pageH = pDC->GetDeviceCaps(VERTRES);

    double sx = (double)pageW / bounds.Width();
    double sy = (double)pageH / bounds.Height();
    double printScale = min(sx, sy);

    CPoint printOffset(
        (int)(-bounds.left * printScale),
        (int)(bounds.bottom * printScale)
    );

    double saveScale = pDoc->m_dScale;
    CPoint saveOffset = pDoc->m_ptOffset;

    pDoc->m_dScale = printScale;
    pDoc->m_ptOffset = printOffset;

    OnDraw(pDC);

    pDoc->m_dScale = saveScale;
    pDoc->m_ptOffset = saveOffset;
}

void CLargeHWView::OnEndPrinting(CDC* /*pDC*/, CPrintInfo* /*pInfo*/) {}

// ============================================================
// Diagnostics
// ============================================================
#ifdef _DEBUG
void CLargeHWView::AssertValid() const { CView::AssertValid(); }
void CLargeHWView::Dump(CDumpContext& dc) const { CView::Dump(dc); }

CLargeHWDoc* CLargeHWView::GetDocument() const
{
    ASSERT(m_pDocument->IsKindOf(RUNTIME_CLASS(CLargeHWDoc)));
    return (CLargeHWDoc*)m_pDocument;
}
#endif
